/*
 * Copyright (c) 2025, Kinara, Inc. All rights reserved.
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: multistream_yolo.h
 */

#ifndef MULTISTREAM_YOLO_H
#define MULTISTREAM_YOLO_H

// System headers
#include <cairo.h>
#include <glib.h>
#include <gst/gst.h>

// Standard C++ headers
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <functional>

// Project headers
#include "stream_config.h"
#include "byte_tracker.hpp"
#include "reid_crop_store.h"
#include "ara_reid_embedder.hpp"
#include "ara_runtime.hpp"
#include "identity_manager.hpp"
#include "reid_crop_store.h"
#include "ara_reid_embedder.hpp"
#include "ara_runtime.hpp"
#include "identity_manager.hpp"
#include "reid_crop_store.h"
#include "ara_reid_embedder.hpp"
#include "ara_runtime.hpp"
#include "identity_manager.hpp"

// Forward declaration - avoids pulling Boost headers into every TU that
// includes multistream_yolo.h.
class DetectionWebSocketServer;

// Constants
namespace {

constexpr guint64 TIMESTAMP_TOLERANCE = 150000000;  // ~150ms in nanoseconds

// Default bounded wait for the pre-flight RTSP resolution probe (override with
// the RTSP_PROBE_TIMEOUT_SEC environment variable, 1-60). A reachable camera
// answers well inside this window; an unreachable one must not be allowed to
// stall startup, so the probe gives up and the caller keeps its fallback.
// All RTSP streams are probed in parallel, so this bounds the TOTAL wait, not
// the wait per stream.
constexpr int kRtspProbeTimeoutSec = 5;
}  // namespace

// Read the frame size of a video FILE by prerolling filesrc ! decodebin to
// PAUSED. Returns false (leaving width/height untouched) if no video pad with
// width/height caps appears.
bool get_video_resolution_caps(const std::string& filepath,
                               int& width, int& height);

// Read the frame size of a LIVE RTSP source. rtspsrc is a live source with
// dynamic pads, so unlike a file it cannot be prerolled in PAUSED -- hence a
// separate helper rather than a flag on the above.
//
// For "h264"/"h265" (the codec from the SDP) it runs rtspsrc ! depay ! parse !
// appsink and reads the size from the parser's caps WITHOUT decoding: no
// keyframe wait, no hardware decoder instantiated. Any other codec falls back
// to rtspsrc ! decodebin ! appsink.
//
// url / latency_ms / use_tcp / transport_timeout_us MUST mirror the real
// pipeline's rtspsrc settings; probing a TCP-only camera over the default UDP
// transport just times out and reports a wrong (fallback) resolution.
//
// Returns false on any failure, on a bus ERROR (connect/auth/404, reported
// immediately rather than after the full wait), or after timeout_sec, leaving
// width/height untouched so the caller keeps its own fallback. When reason is
// non-null it receives a short human-readable cause on failure.
bool get_rtsp_resolution_caps(const std::string& url, const std::string& codec,
                              int latency_ms, bool use_tcp,
                              guint64 transport_timeout_us, int timeout_sec,
                              int& width, int& height,
                              std::string* reason = nullptr);

// Scale src_w x src_h to fit inside model_w x model_h, preserving aspect ratio
// (exact integer arithmetic), rounded down to even values (required by the
// g2d converter and the subsampled YUV formats downstream). Not clamped to
// 1.0: a source smaller than the model is scaled up, as video files always
// were. Shared by every source type so the per-source caps and the reported
// "Final Resolution" can never diverge.
void fit_dimensions_to_model(int src_w, int src_h, int model_w, int model_h,
                             int& out_w, int& out_h);

// COCO class names (80 classes)
const std::vector<std::string> COCO_CLASSES = {
    "person",        "bicycle",      "car",
    "motorcycle",    "airplane",     "bus",
    "train",         "truck",        "boat",
    "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench",        "bird",
    "cat",           "dog",          "horse",
    "sheep",         "cow",          "elephant",
    "bear",          "zebra",        "giraffe",
    "backpack",      "umbrella",     "handbag",
    "tie",           "suitcase",     "frisbee",
    "skis",          "snowboard",    "sports ball",
    "kite",          "baseball bat", "baseball glove",
    "skateboard",    "surfboard",    "tennis racket",
    "bottle",        "wine glass",   "cup",
    "fork",          "knife",        "spoon",
    "bowl",          "banana",       "apple",
    "sandwich",      "orange",       "broccoli",
    "carrot",        "hot dog",      "pizza",
    "donut",         "cake",         "chair",
    "couch",         "potted plant", "bed",
    "dining table",  "toilet",       "tv",
    "laptop",        "mouse",        "remote",
    "keyboard",      "cell phone",   "microwave",
    "oven",          "toaster",      "sink",
    "refrigerator",  "book",         "clock",
    "vase",          "scissors",     "teddy bear",
    "hair drier",    "toothbrush"};

// Helper function to get class name
inline std::string get_class_name(int class_id) {
  if (class_id >= 0 && class_id < static_cast<int>(COCO_CLASSES.size())) {
    return COCO_CLASSES[class_id];
  }
  return "unknown";
}

// Structure to hold detection box data for rendering
struct DetectionBox {
  float x1;
  float y1;
  float x2;
  float y2;
  float score;
  int class_id;
  int track_id;
  // Persistent ReID identity (>0) resolved from the appearance gallery, or -1
  // when ReID is disabled / has not committed an id for this track yet. Stamped
  // once in new_detection() from the shared track_to_person_ map.
  int person_id = -1;
};

struct ModelResolution {
  int width;
  int height;
  bool success;
};

// Per-stream mapping from a decoded frame's PTS (GST_BUFFER_PTS, ns) to the
// 32-bit RTP timestamp (90 kHz) that the pre-decode RTP buffer carried.
//
// Rationale: the RTP timestamp only exists on the buffer BEFORE decoding
// (between rtspsrc and decodebin3). A custom GstMeta would be dropped by the
// hardware decoder, but PTS is a core GstBuffer field that survives the whole
// pipeline. So we capture (pts -> rtp_ts) at a pre-decode pad probe and, in the
// WebSocket publish path, recover the RTP timestamp by looking up the surviving
// PTS. The RTP timestamp becomes the browser-facing sync key (it is the same
// clock the browser observes via WebRTC / requestVideoFrameCallback).
//
// decodebin3 can reorder frames (B-frames), so lookup uses nearest-PTS within a
// tolerance rather than exact equality. The map is bounded to ~2 s of history.
class RtpTimestampMap {
 public:
  void insert(guint64 pts_ns, guint32 rtp_ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[pts_ns] = rtp_ts;
    // Prune entries older than ~2 s relative to the newest PTS.
    constexpr guint64 kMaxAgeNs = 2000000000ULL;
    if (pts_ns > kMaxAgeNs) {
      const guint64 cutoff = pts_ns - kMaxAgeNs;
      auto it = map_.begin();
      while (it != map_.end() && it->first < cutoff) {
        it = map_.erase(it);
      }
    }
  }

  // Look up the RTP timestamp for a PTS using nearest-match within tolerance.
  // Returns true and sets out_rtp_ts on success; false when the map is empty or
  // no entry is within tolerance.
  bool lookup_nearest(guint64 pts_ns, guint64 tolerance_ns,
                      guint32* out_rtp_ts) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (map_.empty()) return false;

    // Candidate at or after pts_ns.
    auto hi = map_.lower_bound(pts_ns);
    guint64 best_diff = UINT64_MAX;
    guint32 best_val = 0;

    if (hi != map_.end()) {
      const guint64 diff = hi->first - pts_ns;
      if (diff < best_diff) { best_diff = diff; best_val = hi->second; }
    }
    if (hi != map_.begin()) {
      auto lo = std::prev(hi);
      const guint64 diff = pts_ns - lo->first;
      if (diff < best_diff) { best_diff = diff; best_val = lo->second; }
    }

    if (best_diff <= tolerance_ns) {
      *out_rtp_ts = best_val;
      return true;
    }
    return false;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::map<guint64, guint32> map_;
};


// Structure matching ara_detection_t from ara_inference_api.h
// NOTE: class_name pointer is not valid after serialization, so we ignore it
struct AraDetection {
  float xmin;               // offset 0, 4 bytes
  float ymin;               // offset 4, 4 bytes
  float xmax;               // offset 8, 4 bytes
  float ymax;               // offset 12, 4 bytes
  float confidence;         // offset 16, 4 bytes
  int32_t class_id;         // offset 20, 4 bytes
  void* class_name_ptr;     // offset 24, 8 bytes (ignored - pointer not valid)
} __attribute__((packed));  // Total: 32 bytes on 64-bit systems

//Camera configuration structure
struct CameraConfig {
  std::string device;
  int width;
  int height;
  int fps;
};

/**
 * @brief Handles detection visualization for a video stream
 */
class Detection {
 public:
  explicit Detection(int stream_id);
  ~Detection();

  // Non-copyable and non-movable: Detection owns std::mutex members (which
  // are not movable) and is always held via std::unique_ptr<Detection> in
  // Demo, so the object itself is never moved. Deleting the move operations
  // makes this explicit and prevents an ill-formed defaulted move.
  Detection(const Detection&) = delete;
  Detection& operator=(const Detection&) = delete;
  Detection(Detection&&) = delete;
  Detection& operator=(Detection&&) = delete;


  /**
   * @brief Start detection processing for the given pipeline
   * @param pipeline GStreamer pipeline element
   */
  void start(GstElement* pipeline);
  void set_no_bbox(bool no_bbox);
  void set_no_osd_stats(bool no_osd_stats);
  void set_only_bbox(bool only_bbox);
  void set_headless(bool headless);
  void set_model_name(const std::string& name) { model_name_ = name; }
  // Enable/disable ByteTrack object tracking. frame_rate is the stream's
  // nominal fps and scales the tracker's lost-track buffer so occlusion
  // handling stays time-consistent for non-30fps streams.
  void set_tracking(bool enabled, int track_class, int frame_rate = 30);
  void set_bt_debug(bool on) { bt_debug_ = on; }
  void set_console_interval(int console_interval);
  // Detection callback receives (stream_id, boxes, pts_ns) where pts_ns is the
  // source buffer PTS (GST_BUFFER_PTS) in nanoseconds. pts_ns is the internal
  // join key used to recover the RTP timestamp for A/V sync (see
  // RtpTimestampMap); it is not itself sent to clients.
  void set_detection_callback(
      std::function<void(int, const std::vector<DetectionBox>&, guint64)> cb);

  void print_stats_to_console(void);

  // ReID: attach a shared OSNet embedder and set the per-track process cadence.
  // Must be called before start() so the crop store picks them up.
  void set_reid(AraReIdEmbedder* embedder, int min_interval_ms);

  // Minimum person-box size (px, this branch's resolution) to embed for ReID.
  // Boxes smaller than w x h are skipped so tiny crops are never upscaled.
  void set_reid_min_crop(int w, int h) { reid_min_crop_w_ = w; reid_min_crop_h_ = h; }

  // Proximity gate: skip ReID embedding for a track when its box overlaps
  // another person box with IoU >= v (0..1; >=1 disables). Prevents ID swaps
  // during crossings. <0 keeps the ReidCropStore default.
  void set_reid_proximity_iou(float v) { reid_proximity_iou_ = v; }

  // Heavy-merge threshold (IoMin): on strong overlap, drop tracks + re-run ReID
  // and show "?" until re-identified. <0 keeps the ReidCropStore default (0.50).
  void set_reid_merge_iomin(float v) { reid_merge_iomin_ = v; }

  // Post-overlap re-verification window in frames (see ReidCropStore). <0 keeps
  // the default. Larger = re-verify identity from the gallery for longer after
  // a crossing, correcting ByteTrack ID swaps.
  void set_reid_reverify_window(int f) { reid_reverify_frames_ = f; }

  // Enable saving person crops to /tmp/reid_crops/ for debugging. Default: off.
  // Do NOT leave on in production; it fills /tmp over time.
  void set_reid_save_crops(bool on) { reid_save_crops_ = on; }

  // ReID: attach the shared appearance-gallery identity manager. When set,
  // fresh embeddings are assigned persistent person IDs, displayed instead of
  // raw track IDs. Not owned.
  void set_identity_manager(IdentityManager* mgr) { identity_manager_ = mgr; }

  void set_stats_callback(std::function < void() > callback);
  double get_avg_fps(void) const
  {
    std::lock_guard<std::mutex> lock(fps_mutex_);
    if (stream_dead_) return 0.0;
    return avg_fps_;
  }
  void reset_fps() {
    std::lock_guard<std::mutex> lock(fps_mutex_);
    avg_fps_  = 0.0;
    curr_fps_ = 0.0;
    fps_samples_.clear();
    frame_count_ = 0;
    last_frame_time_ = std::chrono::steady_clock::now();
  }
  void set_stream_dead(bool dead) { stream_dead_.store(dead); }
  // Returns seconds elapsed since the last display frame arrived.
  // Used by Demo to detect silently stalled streams.
  double get_last_frame_age_s() const {
    std::lock_guard<std::mutex> lock(fps_mutex_);
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - last_frame_time_).count();
  }
  double get_avg_ips(void)
  {
    std::lock_guard < std::mutex > lock(ips_mutex_);
    if (stream_dead_) return 0.0;
    return avg_ips_;
  }

  // ReID per-stream stats (safe to call from any thread).
  struct ReidStreamStats {
    uint64_t reidRuns      = 0;
    uint64_t cropsSkipped  = 0;
    int      activeTracks  = 0;
    int      identified    = 0;
    int      unidentified  = 0;
    int      inOverlap     = 0;
  };
  ReidStreamStats getReidStats() const {
    ReidStreamStats s;
    if (reid_crop_store_) {
      s.reidRuns     = reid_crop_store_->embeddingsMade();
      s.cropsSkipped = reid_frame_seq_ > 0 ?
          (reid_frame_seq_ - reid_crop_store_->embeddingsMade()) : 0;
    }
    {
      std::lock_guard<std::mutex> lock(person_mutex_);
      s.activeTracks  = static_cast<int>(track_to_person_.size()) +
                        static_cast<int>(merged_track_set_.size());
      s.identified    = 0;
      for (const auto& kv : track_to_person_)
        if (kv.second > 0) s.identified++;
      s.unidentified  = s.activeTracks - s.identified -
                        static_cast<int>(merged_track_set_.size());
      s.inOverlap     = static_cast<int>(merged_track_set_.size());
    }
    return s;
  }


 private:
  // Stream identification
  int stream_id_;
  std::string model_name_;  // human-readable model name shown on the overlay
  bool no_bbox_;
  bool no_osd_stats_;
  bool only_bbox_;
  bool headless_;
  int console_interval_;
  std::atomic<bool> stream_dead_{false};  // true when stream fails; get_avg_fps/ips return 0.0

  std::chrono::steady_clock::time_point last_console_print_;
  std::function<void()> stats_callback_;
  std::function<void(int, const std::vector<DetectionBox>&, guint64)>
      detection_callback_;

  // GStreamer elements
  GstElement* dv_post_sink_;
  GstElement* draw_detections_;

  // ReID crop capture: dedicated appsink delivering raw RGB frames, plus the
  // store that crops person boxes out of them and writes them to disk.
  GstElement* reid_frame_sink_ = nullptr;
  std::unique_ptr<ReidCropStore> reid_crop_store_;
  uint64_t reid_frame_seq_ = 0;
  AraReIdEmbedder* reid_embedder_ = nullptr;  // shared, owned by Demo
  int reid_min_interval_ms_ = 200;
  int reid_min_crop_w_ = -1;   // <=0 = keep ReidCropStore default (128)
  int reid_min_crop_h_ = -1;   // <=0 = keep ReidCropStore default (256)
  float reid_proximity_iou_ = -1.0f;  // <0 = keep ReidCropStore default (0.30)
  float reid_merge_iomin_ = -1.0f;    // <0 = keep ReidCropStore default (0.50)
  int reid_reverify_frames_ = -1;     // <0 = keep ReidCropStore default (30)
  bool reid_save_crops_ = false;      // off by default; enable with --save-reid-crops

  // ReID identity: shared gallery (owned by Demo) + latest trackId->personId
  // map for this stream, produced by assignBatch and read by draw_overlay.
  IdentityManager* identity_manager_ = nullptr;
  std::unordered_map<int, int> track_to_person_;  // trackId -> personId
  std::unordered_map<int, char> merged_track_set_;  // trackId in overlap -> draw "?"
  mutable std::mutex person_mutex_;

  // Rendering parameters
  int line_width_;
  int text_size_;

  // FPS tracking
  double curr_fps_;
  double avg_fps_;
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_fps_update_;
  std::chrono::steady_clock::time_point last_frame_time_;
  uint64_t frame_count_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>>
      fps_samples_;
  mutable std::mutex fps_mutex_;

  // IPS (Inferences Per Second) tracking
  double curr_ips_;
  double avg_ips_;
  uint64_t inference_count_;
  std::chrono::steady_clock::time_point last_ips_update_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>>
      ips_samples_;
  std::mutex ips_mutex_;

  // Detection data
  std::vector<std::vector<DetectionBox>> online_targets_;
  std::map<guint64, std::vector<DetectionBox>> timestamped_detections_;
  std::mutex targets_mutex_;

  // Object tracking
  bool tracking_enabled_;
  int tracking_class_;  // -1 = all classes, otherwise COCO class id
  bool bt_debug_ = false;  // verbose ByteTrack logging (--bt-debug)
  std::unique_ptr<bytetrack::ByteTracker> byte_tracker_;


  // Video capabilities
  GstCaps* video_caps_;


  // Static members
  static bool display_warning_;
  static std::mutex warning_mutex_;

  // Private methods
  void calculate_fps();
  void calculate_ips();
  GstFlowReturn new_detection(GstElement* sink);

  // ReID crop frame handler: maps the RGB buffer, matches this frame's tracks
  // by PTS, and hands them to reid_crop_store_ for cropping/saving.
  GstFlowReturn new_reid_frame(GstElement* sink);
  void prepare_overlay(GstCaps* caps);
  void draw_overlay(cairo_t* cr, guint64 timestamp);
  static int get_terminal_columns();

  // GStreamer callback functions
  static GstFlowReturn new_detection_callback(GstElement* sink,
                                              gpointer user_data);
  static GstFlowReturn new_reid_frame_callback(GstElement* sink,
                                               gpointer user_data);
  static void prepare_overlay_callback(GstElement* overlay, GstCaps* caps,
                                       gpointer user_data);
  static void draw_overlay_callback(GstElement* overlay, cairo_t* cr,
                                    guint64 timestamp, guint64 duration,
                                    gpointer user_data);
};


/**
  * @brief Main demo application class managing multiple video streams
  */
class Demo {
  public:
  Demo(int streams, int endpoint, const std::string& group, bool enable_perf_stats = false, bool sync = false, const std::string& model = "/usr/share/cnn/detection/yolov8n/model.dvm", int console_interval = 2);
  ~Demo();

  // Non-copyable
  Demo(const Demo&) = delete;
  Demo& operator=(const Demo&) = delete;

  // Movable
  Demo(Demo&&) = default;
  Demo& operator=(Demo&&) = default;

  /**
    * @brief Start the demo application
    * @return true if started successfully, false otherwise
    */
  bool start();
  void set_no_bbox(bool no_bbox);
  void set_no_osd_stats(bool no_osd_stats);
  void set_only_bbox(bool only_bbox);
  void set_headless(bool headless);
  void set_tracking(bool enabled, int track_class);
  void set_bt_debug(bool on) { bt_debug_ = on; }
  void set_reid_stats(bool on) { reid_stats_ = on; }
  void print_total_stats(void);
  void add_camera(const std::string& device);

  void set_camera_params(int width, int height, int fps);
  static void list_cameras();
  void set_stream_config(std::unique_ptr<StreamConfigManager> config);
  void add_stream_source(std::unique_ptr<StreamSourceConfig> config);
  bool load_stream_config(const std::string& filepath);
  void set_debug(bool enable);

  int get_video_width(int stream_id) const { return video_widths_[stream_id]; }
  int get_video_height(int stream_id) const { return video_heights_[stream_id]; }
  int get_original_width(int stream_id) const { return original_widths_[stream_id]; }
  int get_original_height(int stream_id) const { return original_heights_[stream_id]; }
  int get_streams_count(void) const { return video_widths_.size(); }
  ModelResolution get_model_resolution(const std::string& model_path);

  void set_window_size(int width, int height);

  // ReID: set the OSNet model path (empty = ReID embedding disabled, cropping
  // only) and the per-track process cadence. Call before start().
  void set_reid(const std::string& reid_model_path, int min_interval_ms);

  // Minimum person-box size (px) to embed for ReID; smaller boxes are skipped
  // so tiny crops are never upscaled. <=0 keeps the ReidCropStore default.
  void set_reid_min_crop(int w, int h) { reid_min_crop_w_ = w; reid_min_crop_h_ = h; }

  // Proximity gate for ReID (see Detection). <0 keeps ReidCropStore default.
  void set_reid_proximity_iou(float v) { reid_proximity_iou_ = v; }

  // Heavy-merge threshold (IoMin): drop tracks + re-run ReID on strong overlap.
  void set_reid_merge_iomin(float v) { reid_merge_iomin_ = v; }

  // Post-overlap re-verification window in frames. <0 keeps ReidCropStore default.
  void set_reid_reverify_window(int f) { reid_reverify_frames_ = f; }

  // Enable saving person crops to /tmp/reid_crops/ for debugging. Default: off.
  // Do NOT leave on in production; it fills /tmp over time.
  void set_reid_save_crops(bool on) { reid_save_crops_ = on; }

  // ReID gallery tuning (negative = keep IdentityManager default). Call before
  // start(). match: acquire threshold; sticky: retain threshold; commit_crops:
  // good crops before committing an id.
  void set_reid_thresholds(float match, float sticky, int commit_crops);

  // ReID gallery capacity (max identities). When full, the least-recently-used
  // identity is evicted (LRU). <=0 keeps the IdentityManager default (1000).
  void set_reid_max_gallery(int n) { reid_max_gallery_ = n; }

  // ReID multi-view gallery size: templates kept per person (default 2). More
  // templates capture more viewpoints -> better cross-camera matching. <=0 keeps
  // the IdentityManager default. Call before start().
  void set_reid_gallery_k(int k) { reid_gallery_k_ = k; }

  // Enable verbose ReID decision logging (per-frame + per-track). Call before
  // start(). Useful for debugging cross-camera matching; noisy, so off by default.
  void set_reid_debug(bool on) { reid_debug_ = on; }

  // Enable the per-interval CPU consumption readout in the console TOTAL block
  // (process CPU% in core units + per-stream average, plus ReID gallery size).
  void set_cpu_stats(bool enable) { cpu_stats_ = enable; }
  // Enable WebSocket detection publishing
  void set_ws_detections(bool enable) { ws_detections_enabled_ = enable; }
  void set_ws_port(uint16_t port)     { ws_port_ = port; }
  private:
  // Configuration
  int streams_;
  int endpoint_;
  std::string group_;
  bool sync_;
  std::string model_;
  std::string sock_;
  std::vector<int> video_widths_;
  std::vector<int> video_heights_;
  std::vector<int> original_widths_;
  std::vector<int> original_heights_;
  bool enable_perf_stats_;
  bool no_bbox_;
  bool no_osd_stats_;
  bool only_bbox_;
  bool headless_;
  bool tracking_enabled_;
  int tracking_class_;
  bool bt_debug_ = false;  // verbose ByteTrack logging (--bt-debug)
  int console_interval_;
  std::unique_ptr<StreamConfigManager> stream_config_;

  int model_width_;
  int model_height_;
  bool debug_pipeline_;
  int window_width;
  int window_height;

  // WebSocket detection server (null when --ws-detections is not set).
  bool ws_detections_enabled_{false};
  uint16_t ws_port_{8765};
  std::unique_ptr<DetectionWebSocketServer> ws_server_;

  // GStreamer components
  GstElement* pipeline_;
  GMainLoop* main_loop_;

  // Detection handlers
  std::vector<std::unique_ptr<Detection>> detections_;

  // ReID: one shared Ara-2 runtime + OSNet embedder for all streams (the NPU
  // proxy is shared; embedding is synchronous). Empty model path => disabled.
  std::string reid_model_path_;
  int reid_min_interval_ms_ = 200;
  int reid_min_crop_w_ = -1;          // <=0 = keep ReidCropStore default (128)
  int reid_min_crop_h_ = -1;          // <=0 = keep ReidCropStore default (256)
  float reid_proximity_iou_ = -1.0f;  // <0 = keep ReidCropStore default (0.30)
  float reid_merge_iomin_ = -1.0f;    // <0 = keep ReidCropStore default (0.50)
  int reid_reverify_frames_ = -1;     // <0 = keep ReidCropStore default (30)
  bool reid_save_crops_ = false;      // off by default; enable with --save-reid-crops
  float reid_match_thresh_ = -1.0f;   // <0 = keep IdentityManager default
  float reid_sticky_thresh_ = -1.0f;
  int reid_commit_crops_ = -1;
  int reid_max_gallery_ = -1;         // <=0 = keep IdentityManager default (1000)
  int reid_gallery_k_ = -1;           // <=0 = keep IdentityManager default (2)
  bool reid_debug_ = false;           // verbose ReID decision logging
  std::unique_ptr<AraRuntime> reid_runtime_;
  std::unique_ptr<AraReIdEmbedder> reid_embedder_;
  // Shared appearance gallery across all streams (cross-stream identities).
  std::unique_ptr<IdentityManager> identity_manager_;
  // Per-stream PTS -> RTP-timestamp maps, populated by a pre-decode pad probe
  // on each RTSP stream's watchdog src pad (see attach_rtp_probes()). Held via
  // unique_ptr so the objects (which own a mutex) are never moved even if the
  // vector reallocates. Non-RTSP streams leave their entry null.
  std::vector<std::unique_ptr<RtpTimestampMap>> rtp_maps_;

  // Attach the pre-decode RTP-timestamp pad probes for every RTSP stream.
  // Called from start() once the pipeline exists.
  void attach_rtp_probes();

  // Pad-probe trampoline: reads the RTP timestamp and PTS from a pre-decode
  // buffer and stores (pts -> rtp_ts) into the RtpTimestampMap passed as
  // user_data.
  static GstPadProbeReturn rtp_probe_cb(GstPad* pad, GstPadProbeInfo* info,
                                        gpointer user_data);

  // Private methods
  std::string build_compositor(int number_of_streams);

  void on_bus_message(GstMessage* message);
  void set_fullscreen_when_ready();
  void cleanup();
  std::string build_source_pipeline(int stream_id);

  // Record a source's discovered size and derive the model-fitted size the
  // pipeline caps, dvInf orig-width/height and "Final Resolution" all use.
  // The single place every source type goes through, so they cannot drift.
  void set_stream_resolution(int stream_id, int src_w, int src_h);

  // Probe every RTSP stream's real frame size concurrently (one thread per
  // stream) and cache the results in rtsp_probe_results_. Called once before
  // the per-stream pipelines are built, so N unreachable cameras cost one
  // timeout in total instead of N sequential ones.
  void probe_rtsp_resolutions(int number_of_streams);
  struct RtspProbeResult {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::string reason;  // set when !ok
  };
  std::map<int, RtspProbeResult> rtsp_probe_results_;

  // Derive the per-stream nominal frame rate used to size the tracker's
  // time-based buffers. Camera (V4L2) sources carry an explicit fps; every
  // other source type (and any out-of-range index) defaults to 30. Shared by
  // set_tracking() and start() so the two paths cannot drift.
  int derive_frame_rate(int stream_index) const;


  // GStreamer callback function
  static gboolean on_bus_message_callback(GstBus* bus, GstMessage* message,
                                          gpointer user_data);

  // Failsafe: per-stream black fallback.
  // selector_[i]  : input-selector element for stream i
  // black_pad_[i] : sink_0 of selector_[i]  (videotestsrc black)
  // rtsp_pad_[i]  : sink_1 of selector_[i]  (live branch)
  std::vector<GstElement*> selector_;
  std::vector<GstPad*>     black_pad_;
  std::vector<GstPad*>     rtsp_pad_;

  // Runtime stream state: true = live, false = black fallback active.
  // Written from the bus thread, read from inference/stats callbacks;
  // use atomic elements to avoid data races without a separate lock.
  std::vector<std::atomic<bool>> stream_alive_;

  // Stats print guard — prevents duplicate prints when multiple stream
  // callbacks fire within the same console_interval_ window.
  std::mutex stats_mutex_;
  std::chrono::steady_clock::time_point last_total_stats_print_;
  std::chrono::steady_clock::time_point demo_start_time_;

  // ---- CPU consumption sampling (Linux /proc based) -------------------------
  // Sampled once per print_total_stats() tick to report how much CPU the app
  // is burning to drive N streams (+ ReID). Reported as a percentage of a
  // single core (e.g. 250% = 2.5 cores busy) and normalized by online cores.
  // Prev counters are jiffies from the previous sample; 0 = not yet sampled.
  unsigned long long cpu_prev_proc_jiffies_ = 0;   // utime+stime of this process
  unsigned long long cpu_prev_total_jiffies_ = 0;  // total system jiffies
  unsigned long long cpu_prev_idle_jiffies_ = 0;   // idle+iowait system jiffies
  int cpu_online_cores_ = 0;                        // nproc (cached)
  // Fill busy_cores (% of one core, e.g. 180.0 = 1.8 cores) and sys_pct
  // (0..100 system-wide). Returns false if /proc is unreadable. Safe to call
  // even when unsupported (non-Linux): it simply returns false.
  bool sample_cpu(double* busy_cores, double* sys_pct);
  // Opt-in CPU/ReID readout in print_total_stats() (set via --cpu-stats).
  bool cpu_stats_ = false;
  // Opt-in periodic ReID stats block (set via --reid-stats).
  bool reid_stats_ = false;

  // Switch a stream's selector to the black fallback or back to live
  void switch_to_black(int stream_id);

  // Walk the GstObject parent chain to find which stream owns an element.
  // Returns stream index, or -1 if not resolvable.
  int resolve_stream_id(GstObject* obj) const;
  bool is_rtsp_stream(int i) const {
    return stream_config_->get_stream_type(i) == StreamSourceType::RTSP;
  }

  // Application-side rolling retention for RTSP recordings. splitmuxsink's
  // built-in max-files pruning does not fire when segment names are supplied
  // via the "format-location" signal (which we use for timestamped names), so
  // a periodic timer enforces the retention window instead. Each entry keeps
  // at most max_files newest files matching prefix ("cam<N>_") in dir.
  struct RecordCleanup {
    std::string dir;
    std::string prefix;
    int max_files;
  };
  std::vector<RecordCleanup> record_cleanups_;
  guint record_cleanup_timer_id_ = 0;

  // Prune old recording segments for every registered RecordCleanup entry so
  // total disk usage stays bounded. Invoked periodically from the main loop.
  void prune_all_recordings();
};
/**
 * @brief Print usage information
 * @param prog_name Program name
 */
void print_usage(const char* prog_name);

#endif  // MULTISTREAM_YOLO_H
