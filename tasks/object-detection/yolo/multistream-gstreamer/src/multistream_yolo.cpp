
/*
 * Copyright (c) 2025, Kinara, Inc. All rights reserved.
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: multistream_yolo.cpp
 */

#include "multistream_yolo.h"
#include "detection_ws_server.h"
// System headers
#include <gst/video/video.h>
#include <gst/rtp/gstrtpbuffer.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <dirent.h>

// Standard C++ headers
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <future>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <gst/gst.h>

#include <dlfcn.h>
#include <dvapi.h>
#include <glib/gstdio.h>   // g_dir_open, g_remove


//==============================================================================
// Constants
//==============================================================================

static const std::string SAMPLE_VIDEOS_DIR =
    "/usr/share/ara2-vision-examples/sample_videos";

//==============================================================================
// Helper Functions
//==============================================================================

// Per-sink context for the splitmuxsink "format-location" callback. It carries
// the camera index and record directory needed to build each segment file
// name. The context is owned by the sink element (attached with
// g_object_set_data_full) so it lives exactly as long as the sink; the
// callback retrieves it from the element to avoid a second, dangling owner.
struct RecordNaming {
  int cam_id;
  std::string record_dir;
};

// splitmuxsink "format-location" callback. Builds a timestamped file name of
// the form cam<N>_<YYYY_MM_DD_HHMMSS>.mp4 for each new recording segment. The
// naming context is fetched from the splitmuxsink element itself, so there is
// a single owner (the element) and no risk of use-after-free.
static gchar* record_format_location(GstElement* splitmux, guint fragment_id,
                                      gpointer user_data) {
  (void)fragment_id;
  (void)user_data;
  const RecordNaming* naming = static_cast<const RecordNaming*>(
      g_object_get_data(G_OBJECT(splitmux), "record-naming"));
  if (!naming) {
    return nullptr;
  }

  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);

  char ts[32];
  strftime(ts, sizeof(ts), "%Y_%m_%d_%H%M%S", &tm_now);

  std::ostringstream path;
  path << naming->record_dir << "/cam" << naming->cam_id << "_" << ts << ".mp4";

  // splitmuxsink takes ownership of the returned string and frees it with
  // g_free(), so allocate with g_strdup().
  return g_strdup(path.str().c_str());
}

// Delete the oldest recording segments in dir that match "<prefix>*.mp4",
// keeping at most max_files newest ones. Segment file names embed a
// zero-padded YYYY_MM_DD_HHMMSS timestamp, so a lexicographic sort is also a
// chronological sort; the tail of the sorted list is the newest and is kept.
// Only files matching the exact prefix and the .mp4 suffix in this single
// directory are ever considered; the directory is not recursed.
static void prune_recording_dir(const std::string& dir,
                                const std::string& prefix, int max_files) {
  if (max_files < 1) {
    max_files = 1;
  }

  GError* error = nullptr;
  GDir* d = g_dir_open(dir.c_str(), 0, &error);
  if (!d) {
    if (error) {
      g_clear_error(&error);
    }
    return;
  }

  std::vector<std::string> names;
  const gchar* name = nullptr;
  const std::string suffix = ".mp4";
  while ((name = g_dir_read_name(d)) != nullptr) {
    std::string fname = name;
    // Match "<prefix>....mp4" only; ignore anything else in the directory.
    if (fname.size() <= prefix.size() + suffix.size()) {
      continue;
    }
    if (fname.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    if (fname.compare(fname.size() - suffix.size(), suffix.size(), suffix)
        != 0) {
      continue;
    }
    names.push_back(std::move(fname));
  }
  g_dir_close(d);

  if (static_cast<int>(names.size()) <= max_files) {
    return;
  }

  // Oldest first; delete everything except the newest max_files entries.
  std::sort(names.begin(), names.end());
  const int to_delete = static_cast<int>(names.size()) - max_files;
  for (int i = 0; i < to_delete; ++i) {
    std::string full = dir + "/" + names[i];
    if (g_remove(full.c_str()) != 0) {
      g_printerr("Recording cleanup: failed to delete %s\n", full.c_str());
    }
  }
}


/**
 * @brief Check if a file exists
 * @param filepath Path to the file to check
 * @return true if file exists, false otherwise
 */
bool file_exists(const std::string& filepath) {
  struct stat buffer;
  return (stat(filepath.c_str(), &buffer) == 0);
}

// Create a directory and all missing parent directories (mkdir -p semantics).
// Returns true if the full path exists after the call. On a real failure the
// caller can inspect errno. Only absolute paths are supported; a leading '/'
// is preserved and each intermediate component is created in turn, treating
// EEXIST as success.
static bool make_dirs_recursive(const std::string& path, mode_t mode) {
  if (path.empty()) {
    errno = EINVAL;
    return false;
  }

  std::string partial;
  size_t i = 0;
  // Preserve a leading '/' so absolute paths stay absolute.
  if (path[0] == '/') {
    partial = "/";
    i = 1;
  }

  while (i <= path.size()) {
    if (i == path.size() || path[i] == '/') {
      if (!partial.empty() && partial.back() != '/') {
        if (mkdir(partial.c_str(), mode) != 0 && errno != EEXIST) {
          return false;
        }
      }
      if (i < path.size()) {
        partial += '/';
      }
    } else {
      partial += path[i];
    }
    ++i;
  }

  return true;
}


/**
 * @brief Validate that the model file exists
 * @param model_path Path to the model file
 * @return true if model exists, false otherwise
 */
bool validate_model_exists(const std::string& model_path) {
  if (!file_exists(model_path)) {
    std::cerr << "\n========================================\n"
              << "      ERROR: Model Not Found\n"
              << "========================================\n"
              << "The specified model file does not exist:\n"
              << "  " << model_path << "\n\n"
              << "  To download the required models:\n\n"
              << "1. Download all YOLOv8 models:\n"
              << "   ara2-fetch-models --repo-id nxp/Ultralytics-YOLOv8-detection-Ara240\n\n"
              << "2. Download all YOLOX models:\n"
              << "   ara2-fetch-models --repo-id nxp/YOLOX-Detection-Ara240\n\n"
              << "3. Download all YOLO11 models:\n"
              << "   ara2-fetch-models --repo-id nxp/Ultralytics-YOLO11-Detection-Ara240\n\n"
              << "4. Download all YOLO26 models:\n"
              << "   ara2-fetch-models --repo-id nxp/Ultralytics-YOLO26-detection-Ara240\n\n"
              << "5. Verify installation:\n"
              << "   ls -lh /usr/share/cnn/detection/yolov8*/\n"
              << "   ls -lh /usr/share/cnn/detection/yolox*/\n"
              << "   ls -lh /usr/share/cnn/detection/yolo11*/\n"
              << "   ls -lh /usr/share/cnn/detection/yolo26*/\n\n"
              << "  Available YOLOv8 model variants:\n"
              << "   - yolov8n (nano)        - Fastest, lowest accuracy\n"
              << "   - yolov8s (small)       - Balanced speed and accuracy\n"
              << "   - yolov8m (medium)      - Good accuracy, moderate speed\n"
              << "   - yolov8l (large)       - High accuracy, slower\n"
              << "   - yolov8x (extra-large) - Highest accuracy, slowest\n\n"
              << "  Available YOLOX model variants:\n"
              << "   - yoloxt (tiny)         - Fastest speed, lowest accuracy\n"
              << "   - yoloxs (small)        - Balanced speed and accuracy\n"
              << "   - yoloxm (medium)       - Good accuracy, moderate speed\n"
              << "   - yoloxl (large)        - High accuracy, slower\n"
              << "   - yoloxx (extra-large)  - Highest accuracy, slowest\n\n"
              << "  Available YOLO11 model variants:\n"
              << "   - yolo11n (nano)        - Fastest, lowest accuracy\n"
              << "   - yolo11s (small)       - Balanced speed and accuracy\n"
              << "   - yolo11m (medium)      - Good accuracy, moderate speed\n"
              << "   - yolo11l (large)       - High accuracy, slower\n"
              << "   - yolo11x (extra-large) - Highest accuracy, slowest\n\n"
              << "  Available YOLO26 model variants:\n"
              << "   - yolo26n (nano)        - Fastest, lowest accuracy\n"
              << "   - yolo26s (small)       - Balanced speed and accuracy\n"
              << "   - yolo26m (medium)      - Good accuracy, moderate speed\n"
              << "   - yolo26l (large)       - High accuracy, slower\n"
              << "   - yolo26x (extra-large) - Highest accuracy, slowest\n"
              << "========================================\n"
              << std::endl;
    return false;
  }
  return true;
}

/**
 * @brief Discover ts video files in a directory
 * @param directory Path to scan for .ts files
 * @return Sorted vector of full file paths (up to 8 files)
 */
std::vector<std::string> discover_video_files(const std::string& directory) {
  std::vector<std::string> video_files;

  DIR* dir = opendir(directory.c_str());
  if (!dir) {
    std::cerr << "Warning: Cannot open directory: " << directory << "\n";
    return video_files;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string filename(entry->d_name);

    // Skip hidden files
    if (filename.empty() || filename[0] == '.') {
      continue;
    }

    // Check for .ts extension (case-insensitive)
    size_t dot_pos = filename.find_last_of('.');
    if (dot_pos != std::string::npos) {
      std::string ext = filename.substr(dot_pos);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

     if (ext == ".ts") {
        std::string full_path = directory + "/" + filename;

        struct stat st;
        if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            video_files.push_back(full_path);
        }
     }
    }
  }
  closedir(dir);

  // Sort for deterministic, reproducible ordering
  std::sort(video_files.begin(), video_files.end());

  // Cap at 16 files maximum
  if (video_files.size() > 16) {
    video_files.resize(16);
  }

  return video_files;
}

//==============================================================================
// Static Member Initialization
//==============================================================================

bool Detection::display_warning_ = true;
std::mutex Detection::warning_mutex_;

//==============================================================================
// Detection Class Implementation
//==============================================================================

Detection::Detection(int stream_id)
    : stream_id_(stream_id),
      dv_post_sink_(nullptr),
      draw_detections_(nullptr),
      line_width_(5),
      text_size_(25),
      curr_fps_(0.0),
      avg_fps_(0.0),
      curr_ips_(0.0),
      avg_ips_(0.0),
      frame_count_(0),
      inference_count_(0),
      video_caps_(nullptr),
      no_bbox_(false),
      no_osd_stats_(false),
      only_bbox_(false),
      headless_(false),
      console_interval_(2),
      tracking_enabled_(false),
      tracking_class_(-1) {  // -1 = all classes (matches Demo default)
  const auto now = std::chrono::steady_clock::now();
  start_time_ = now;

  last_fps_update_ = now;
  last_frame_time_ = now;
  last_ips_update_ = now;
  last_console_print_ = now;
}

void Detection::print_stats_to_console() {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - last_console_print_).count();

  if (console_interval_ == 0) {
      return;
  }
  if (elapsed >= console_interval_) {
      double current_ips;
    {
      std::lock_guard<std::mutex> lock(ips_mutex_);
      current_ips = avg_ips_;
    }

    if (stats_callback_) {
      stats_callback_();
    }

    last_console_print_ = now;
  }
}

void Detection::set_no_bbox(bool no_bbox) {
  no_bbox_ = no_bbox;
}

void Detection::set_no_osd_stats(bool no_osd_stats) {
  no_osd_stats_ = no_osd_stats;
}

void Detection::set_only_bbox(bool only_bbox) {
  only_bbox_ = only_bbox;
}

void Detection::set_headless(bool headless) {
  headless_ = headless;
}

void Detection::set_tracking(bool enabled, int track_class, int frame_rate) {
  tracking_enabled_ = enabled;
  tracking_class_ = track_class;

  byte_tracker_.reset();

  if (!enabled) {
    return;
  }

  bytetrack::ByteTrackConfig cfg;

  cfg.trackThresh = 0.4f;          // lower threshold catches weaker detections
  cfg.matchThresh = 0.8f;
  cfg.secondMatchThresh = 0.6f;    // stricter second-stage match
  cfg.unconfirmedThresh = 0.7f;
  // Retain lost tracks for ~5 s (150 frames at 30fps) so short occlusions
  // and YOLO miss-detections keep their ID for up to 5 seconds.
  cfg.trackBuffer = 150;
  cfg.frameRate = frame_rate > 0 ? frame_rate : 30;
  cfg.outputDetWeight = 0.5f;
  cfg.unconfirmedGrace = 2;
  cfg.trackingClass = track_class;

  byte_tracker_ = std::make_unique<bytetrack::ByteTracker>(cfg);
  byte_tracker_->setDebug(bt_debug_);
  if (bt_debug_)
    fprintf(stderr, "[BT] config: trackThresh=%.2f secondThresh=%.2f "
            "buffer=%d fps=%d reclaimIou=%.2f\n",
            cfg.trackThresh, cfg.secondMatchThresh,
            cfg.trackBuffer, cfg.frameRate, 0.7f);

}

void Detection::set_stats_callback(std::function<void()> callback) {

  stats_callback_ = callback;
}

void Detection::set_detection_callback(
    std::function<void(int, const std::vector<DetectionBox>&, guint64)> cb) {
  detection_callback_ = std::move(cb);
}

void Detection::set_console_interval(int interval) {
 console_interval_ = interval;
}

void Detection::set_reid(AraReIdEmbedder* embedder, int min_interval_ms) {
  reid_embedder_ = embedder;
  reid_min_interval_ms_ = min_interval_ms;
}

Detection::~Detection() {
  // Print the ReID overlap (IoMin) histogram once at shutdown so the best
  // --reid-merge-iomin threshold can be chosen empirically from a real run.
  if (reid_crop_store_) reid_crop_store_->printMergeStats();
  if (video_caps_) {
    gst_caps_unref(video_caps_);
    video_caps_ = nullptr;
  }
  if (dv_post_sink_) {
    gst_object_unref(dv_post_sink_);
    dv_post_sink_ = nullptr;
  }
  if (draw_detections_) {
    gst_object_unref(draw_detections_);
    draw_detections_ = nullptr;
  }
}

void Detection::start(GstElement* pipeline) {
  if (!pipeline) {
    g_printerr("Pipeline is null\n");
    return;
  }

  // Get dvPost sink (appsink)
  const std::string sink_name = "dv_post_sink_" + std::to_string(stream_id_);
  dv_post_sink_ = gst_bin_get_by_name(GST_BIN(pipeline), sink_name.c_str());
  if (!dv_post_sink_) {
    g_printerr("Failed to get element: %s\n", sink_name.c_str());
    return;
  }

  g_signal_connect(dv_post_sink_, "new-sample",
                   G_CALLBACK(new_detection_callback), this);

  // ReID crop sink: present whether or not the display branch exists. Create
  // the crop store and connect the frame callback if the branch was built.
  const std::string reid_sink_name =
      "reid_frame_sink_" + std::to_string(stream_id_);
  reid_frame_sink_ = gst_bin_get_by_name(GST_BIN(pipeline), reid_sink_name.c_str());
  if (reid_frame_sink_) {
    reid_crop_store_ = std::make_unique<ReidCropStore>(stream_id_);
    reid_crop_store_->setMinIntervalMs(reid_min_interval_ms_);
    reid_crop_store_->setMinCropSize(reid_min_crop_w_, reid_min_crop_h_);
    if (reid_proximity_iou_ >= 0.0f)
      reid_crop_store_->setProximityIoU(reid_proximity_iou_);
    if (reid_merge_iomin_ >= 0.0f)
      reid_crop_store_->setMergeIoMin(reid_merge_iomin_);
    if (reid_reverify_frames_ >= 0)
      reid_crop_store_->setReverifyWindow(reid_reverify_frames_);
    reid_crop_store_->setSaveCrops(reid_save_crops_);  // controlled by --save-reid-crops
    if (reid_embedder_) reid_crop_store_->setEmbedder(reid_embedder_);
    g_signal_connect(reid_frame_sink_, "new-sample",
                     G_CALLBACK(new_reid_frame_callback), this);
  }

  // Headless: no display branch to wire
  if (headless_) {
    return;
  }

  // Get cairo overlay element
  const std::string draw_name = "draw_detections_" + std::to_string(stream_id_);
  draw_detections_ = gst_bin_get_by_name(GST_BIN(pipeline), draw_name.c_str());
  if (!draw_detections_) {
    g_printerr("Failed to get element: %s\n", draw_name.c_str());
    return;
  }

  g_signal_connect(draw_detections_, "draw", G_CALLBACK(draw_overlay_callback),
                   this);
  g_signal_connect(draw_detections_, "caps-changed",
                   G_CALLBACK(prepare_overlay_callback), this);
}

//==============================================================================
// FPS and IPS Calculation
//==============================================================================

void Detection::calculate_fps() {
  std::lock_guard<std::mutex> lock(fps_mutex_);

  const auto now = std::chrono::steady_clock::now();
  ++frame_count_;
  last_frame_time_ = now;  // track when the last real frame arrived

  // Calculate instantaneous FPS
  const std::chrono::duration<double> frame_time = now - last_fps_update_;
  const double frame_seconds = frame_time.count();

  if (frame_seconds > 0.0) {
    curr_fps_ = 1.0 / frame_seconds;
  }

  last_fps_update_ = now;

  // Add current sample to the window
  fps_samples_.push_back({now, frame_count_});

  // Keep only samples from the last second
  while (!fps_samples_.empty()) {
    const std::chrono::duration<double> age = now - fps_samples_.front().first;
    if (age.count() > 1.0) {
      fps_samples_.pop_front();
    } else {
      break;
    }
  }

  // Calculate average FPS over the last second
  if (fps_samples_.size() >= 2) {
    const auto& oldest = fps_samples_.front();
    const auto& newest = fps_samples_.back();

    const std::chrono::duration<double> time_span = newest.first - oldest.first;
    const uint64_t frame_span = newest.second - oldest.second;

    if (time_span.count() > 0.0) {
      avg_fps_ = std::round(frame_span / time_span.count() * 10.0) / 10.0;
    }
  } else {
    // Not enough samples yet, use instantaneous FPS
    avg_fps_ = std::round(curr_fps_ * 10.0) / 10.0;
  }
}

void Detection::calculate_ips() {
  std::lock_guard<std::mutex> lock(ips_mutex_);

  const auto now = std::chrono::steady_clock::now();
  ++inference_count_;

  // Calculate instantaneous IPS
  const std::chrono::duration<double> inference_time = now - last_ips_update_;
  const double inference_seconds = inference_time.count();

  if (inference_seconds > 0.0) {
    curr_ips_ = 1.0 / inference_seconds;
  }

  last_ips_update_ = now;

  // Add current sample to the window
  ips_samples_.push_back({now, inference_count_});

  // Keep only samples from the last second
  while (!ips_samples_.empty()) {
    const std::chrono::duration<double> age = now - ips_samples_.front().first;
    if (age.count() > 1.0) {
      ips_samples_.pop_front();
    } else {
      break;
    }
  }

  // Calculate average IPS over the last second
  if (ips_samples_.size() >= 2) {
    const auto& oldest = ips_samples_.front();
    const auto& newest = ips_samples_.back();

    const std::chrono::duration<double> time_span = newest.first - oldest.first;
    const uint64_t inference_span = newest.second - oldest.second;

    if (time_span.count() > 0.0) {
      avg_ips_ = std::round(inference_span / time_span.count() * 10.0) / 10.0;
    }
  } else {
    // Not enough samples yet, use instantaneous IPS
    avg_ips_ = std::round(curr_ips_ * 10.0) / 10.0;
  }
}

//==============================================================================
// Detection Processing
//==============================================================================

GstFlowReturn Detection::new_detection_callback(GstElement* sink,
                                                gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  return self->new_detection(sink);
}

GstFlowReturn Detection::new_detection(GstElement* sink) {
  GstSample* sample = nullptr;
  g_signal_emit_by_name(sink, "pull-sample", &sample);
  if (!sample) {
    return GST_FLOW_OK;
  }

  calculate_ips();

  // Drive FPS calculation and stats printing from the inference path only
  // when the stream is dead (draw_overlay has stopped firing). When alive,
  // draw_overlay() owns calculate_fps() to avoid double-counting.
  // print_stats_to_console() is rate-limited so calling it here is harmless.
  if (stream_dead_ || headless_) {
    calculate_fps();
  }
  print_stats_to_console();

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  // Get buffer timestamp for synchronization
  const guint64 timestamp = GST_BUFFER_PTS(buffer);

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  if (map.size < sizeof(uint32_t)) {
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  uint32_t num_boxes = 0;
  std::memcpy(&num_boxes, map.data, sizeof(uint32_t));

  std::vector<DetectionBox> boxes;
  boxes.reserve(num_boxes);

  constexpr size_t detection_size = sizeof(AraDetection);
  const uint8_t* raw_data = map.data + sizeof(uint32_t);

  const size_t expected_size = sizeof(uint32_t) + num_boxes * detection_size;
  if (map.size < expected_size) {
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  for (uint32_t i = 0; i < num_boxes; ++i) {
    AraDetection det;
    std::memcpy(&det, raw_data + i * detection_size, detection_size);

    DetectionBox box;
    box.x1 = det.xmin;
    box.y1 = det.ymin;
    box.x2 = det.xmax;
    box.y2 = det.ymax;
    box.score = det.confidence;
    box.class_id = det.class_id;
    box.track_id = -1;  // assigned by tracker when tracking is enabled

    // Validate box coordinates
    if (box.x1 >= 0.0f && box.y1 >= 0.0f && box.x2 > box.x1 &&
        box.y2 > box.y1) {
      boxes.push_back(box);
    }
  }

  gst_buffer_unmap(buffer, &map);

  // Object tracking: replace raw detections with stable, tracked boxes.
  if (tracking_enabled_ && byte_tracker_) {
    // Convert detections (x1,y1,x2,y2) to tracker format (x,y,w,h). Class
    // filtering is handled inside ByteTracker::update() via its trackingClass
    // config, so no outer class guard is needed here; adding one would only
    // duplicate the tracker's own filtering.
    std::vector<bytetrack::TrackingDetection> dets;
    dets.reserve(boxes.size());

    for (const auto& box : boxes) {
      bytetrack::TrackingDetection d;
      d.x = box.x1;
      d.y = box.y1;
      d.w = box.x2 - box.x1;
      d.h = box.y2 - box.y1;
      d.score = box.score;
      d.classId = box.class_id;
      dets.push_back(d);
    }

    // Run ByteTrack, producing stable tracked boxes.
    if (bt_debug_)
      fprintf(stderr, "[BT-PTS] pts=%.3fs\n",
              static_cast<double>(timestamp) / 1e9);
    const std::vector<bytetrack::TrackResult> tracks = byte_tracker_->update(dets);


    // Rebuild the render list from the tracker output, converting back to
    // (x1,y1,x2,y2) and attaching each stable track id.
    boxes.clear();
    boxes.reserve(tracks.size());

    for (const auto& t : tracks) {
      DetectionBox box;
      box.x1 = t.x;
      box.y1 = t.y;
      box.x2 = t.x + t.width;
      box.y2 = t.y + t.height;
      box.score = t.score;
      box.class_id = t.classId;
      box.track_id = t.trackId;
      boxes.push_back(box);
    }
  }

  // Stamp the persistent ReID identity onto each box from the shared gallery
  // map. The WS publish path and draw_overlay both read box.person_id. person_id
  // is "best-known-so-far" (it lags detection by ReID's commit latency); tracks
  // without a committed id keep person_id = -1.
  if (identity_manager_) {
    std::lock_guard<std::mutex> lock(person_mutex_);
    for (auto& box : boxes) {
      if (box.track_id < 0) continue;
      auto it = track_to_person_.find(box.track_id);
      if (it != track_to_person_.end()) box.person_id = it->second;
    }
  }

  // Fire the detection callback with the frame's PTS (ns). PTS is the internal
  // join key: the pre-decode RTP pad probe stores (pts -> real wire RTP
  // timestamp) per stream, and the WebSocket publish path looks that PTS up to
  // attach the actual RTP timestamp (which carries MediaMTX's base offset and
  // therefore matches what the browser observes over WebRTC). We deliberately
  // do NOT synthesize the RTP timestamp from PTS here, because that would drop
  // the random RTP base offset and fail to match the browser side.
  if (detection_callback_) {
    detection_callback_(stream_id_, boxes, timestamp);
  }

  // Store detections with timestamp for synchronization

  {
    std::lock_guard<std::mutex> lock(targets_mutex_);
    timestamped_detections_[timestamp] = std::move(boxes);

    // Clean up old timestamps (keep last 2 seconds worth)
    constexpr guint64 MAX_TIMESTAMP_AGE = 2000000000;  // 2 seconds in ns
    auto it = timestamped_detections_.begin();
    while (it != timestamped_detections_.end()) {
      if (timestamp > it->first &&
          (timestamp - it->first) > MAX_TIMESTAMP_AGE) {
        it = timestamped_detections_.erase(it);
      } else {
        ++it;
      }
    }
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

//==============================================================================
// ReID Crop Frame Processing
//==============================================================================

GstFlowReturn Detection::new_reid_frame_callback(GstElement* sink,
                                                 gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  return self->new_reid_frame(sink);
}

GstFlowReturn Detection::new_reid_frame(GstElement* sink) {
  GstSample* sample = nullptr;
  g_signal_emit_by_name(sink, "pull-sample", &sample);
  if (!sample) {
    return GST_FLOW_OK;
  }

  if (!reid_crop_store_) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  // Frame dimensions from the negotiated caps (RGB, video_widths_ scale).
  int frameW = 0;
  int frameH = 0;
  if (GstCaps* caps = gst_sample_get_caps(sample)) {
    if (GstStructure* s = gst_caps_get_structure(caps, 0)) {
      gst_structure_get_int(s, "width", &frameW);
      gst_structure_get_int(s, "height", &frameH);
    }
  }

  const guint64 timestamp = GST_BUFFER_PTS(buffer);

  // Match this frame to the closest detection set by PTS (same logic the
  // overlay uses), so we crop with the boxes that belong to these pixels.
  std::vector<DetectionBox> boxes;
  {
    std::lock_guard<std::mutex> lock(targets_mutex_);
    if (!timestamped_detections_.empty()) {
      guint64 closest_diff = UINT64_MAX;
      auto closest_it = timestamped_detections_.end();
      for (auto iter = timestamped_detections_.begin();
           iter != timestamped_detections_.end(); ++iter) {
        if (iter->first <= timestamp) {
          const guint64 diff = timestamp - iter->first;
          if (diff < closest_diff) {
            closest_diff = diff;
            closest_it = iter;
          }
        }
      }
      if (closest_it == timestamped_detections_.end()) {
        closest_it = timestamped_detections_.begin();
        closest_diff = (closest_it->first > timestamp)
                           ? (closest_it->first - timestamp)
                           : 0;
      }
      if (closest_it != timestamped_detections_.end() &&
          closest_diff < TIMESTAMP_TOLERANCE) {
        boxes = closest_it->second;
      }
    }
  }

  if (frameW > 0 && frameH > 0 && !boxes.empty()) {
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      // Expect tightly packed RGB (3 bytes/px). Guard against short buffers.
      const size_t need = static_cast<size_t>(frameW) * frameH * 3;
      if (map.size >= need) {
        // Monotonic clock in ms for the per-track cadence gate.
        const uint64_t nowMs =
            static_cast<uint64_t>(g_get_monotonic_time() / 1000);
        std::vector<TrackEmbedding> embs = reid_crop_store_->process(
            map.data, frameW, frameH, boxes, nowMs, reid_frame_seq_);

        // Assign persistent person IDs via the shared appearance gallery.
        // Feed ALL present person track ids (so sticky/provisional hold) plus
        // this frame's fresh embeddings; get back trackId -> personId.
        if (identity_manager_) {
          std::vector<int> presentTrackIds;
          presentTrackIds.reserve(boxes.size());
          for (const auto& b : boxes)
            if (b.track_id >= 0 && b.class_id == 0)  // person tracks only
              presentTrackIds.push_back(b.track_id);

          std::unordered_map<int, int> t2p =
              identity_manager_->assignBatch(
                  stream_id_, presentTrackIds, embs, nullptr,
                  &reid_crop_store_->recentlyOverlapped(),
                  &reid_crop_store_->mergedTracks());

          {
            std::lock_guard<std::mutex> lock(person_mutex_);
            track_to_person_ = std::move(t2p);
            // Snapshot which tracks are merged this frame so the overlay can
            // draw "?" for them until they get a fresh id.
            merged_track_set_ = reid_crop_store_->mergedTracks();
          }
        }
      }
      gst_buffer_unmap(buffer, &map);
    }
  }

  ++reid_frame_seq_;
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

//==============================================================================
// Overlay Rendering
//==============================================================================

void Detection::prepare_overlay_callback(GstElement* overlay, GstCaps* caps,
                                         gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  self->prepare_overlay(caps);
}

void Detection::prepare_overlay(GstCaps* caps) {
  if (video_caps_) {
    gst_caps_unref(video_caps_);
  }
  video_caps_ = gst_caps_copy(caps);
}

void Detection::draw_overlay_callback(GstElement* overlay, cairo_t* cr,
                                      guint64 timestamp, guint64 duration,
                                      gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  self->draw_overlay(cr, timestamp);
}

void Detection::draw_overlay(cairo_t* cr, guint64 timestamp) {
  if (!video_caps_) {
    return;
  }

  calculate_fps();
  print_stats_to_console();

  double current_ips;
  {
    std::lock_guard<std::mutex> lock(ips_mutex_);
    current_ips = avg_ips_;
  }

  //----------------------------------------------------------------------------
  // Draw FPS/IPS overlay
  //----------------------------------------------------------------------------
  if (!no_osd_stats_) {
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 24.0);

  char fps_text[32];
  char ips_text[32];
  snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", avg_fps_);
  snprintf(ips_text, sizeof(ips_text), "IPS: %.1f", current_ips);

  cairo_text_extents_t fps_extents, ips_extents;
  cairo_text_extents(cr, fps_text, &fps_extents);
  cairo_text_extents(cr, ips_text, &ips_extents);

  const double max_text_width = std::max(fps_extents.width, ips_extents.width);
  constexpr double padding = 8.0;
  constexpr double line_spacing = 6.0;
  const double total_text_height =
      fps_extents.height + ips_extents.height + line_spacing;

  const double x = padding;
  const double y = padding + fps_extents.height;

  const double bg_width = max_text_width + 2.0 * padding;
  const double bg_height = total_text_height + 2.0 * padding;
  constexpr double corner_radius = 6.0;

  // Draw rounded rectangle background
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + bg_width - corner_radius,
            y - fps_extents.height - padding + corner_radius, corner_radius,
            -M_PI / 2.0, 0.0);
  cairo_arc(
      cr, x + bg_width - corner_radius,
      y + padding + total_text_height - fps_extents.height - corner_radius,
      corner_radius, 0.0, M_PI / 2.0);
  cairo_arc(
      cr, x + corner_radius,
      y + padding + total_text_height - fps_extents.height - corner_radius,
      corner_radius, M_PI / 2.0, M_PI);
  cairo_arc(cr, x + corner_radius,
            y - fps_extents.height - padding + corner_radius, corner_radius,
            M_PI, 3.0 * M_PI / 2.0);
  cairo_close_path(cr);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
  cairo_fill_preserve(cr);

  cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.9);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);

  // Draw FPS text
  cairo_move_to(cr, x + padding, y);
  cairo_set_source_rgb(cr, 0.0, 1.0, 0.8);
  cairo_show_text(cr, fps_text);

  // Draw IPS text
  cairo_move_to(cr, x + padding, y + fps_extents.height + line_spacing);
  cairo_set_source_rgb(cr, 1.0, 0.7, 0.0);
  cairo_show_text(cr, ips_text);

  //----------------------------------------------------------------------------
  // Draw model name label at the top center of the frame
  //----------------------------------------------------------------------------
  if (!model_name_.empty()) {
    // Read the frame dimensions from the negotiated caps.
    int frame_width = 640;
    if (GstStructure* s = gst_caps_get_structure(video_caps_, 0))
      gst_structure_get_int(s, "width", &frame_width);

    constexpr double lbl_font_size = 22.0;
    constexpr double lbl_padding   = 6.0;
    constexpr double lbl_radius    = 5.0;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, lbl_font_size);

    cairo_text_extents_t lbl_ext;
    cairo_text_extents(cr, model_name_.c_str(), &lbl_ext);

    // Center horizontally at the top of the frame.
    const double lbl_bg_w = lbl_ext.width  + 2.0 * lbl_padding;
    const double lbl_bg_h = lbl_ext.height + 2.0 * lbl_padding;
    const double lbl_x    = (static_cast<double>(frame_width) - lbl_bg_w) / 2.0;
    const double lbl_y    = lbl_padding;

    // Draw background pill.
    cairo_new_sub_path(cr);
    cairo_arc(cr, lbl_x + lbl_bg_w - lbl_radius, lbl_y + lbl_radius,
              lbl_radius, -M_PI / 2.0, 0.0);
    cairo_arc(cr, lbl_x + lbl_bg_w - lbl_radius, lbl_y + lbl_bg_h - lbl_radius,
              lbl_radius, 0.0, M_PI / 2.0);
    cairo_arc(cr, lbl_x + lbl_radius, lbl_y + lbl_bg_h - lbl_radius,
              lbl_radius, M_PI / 2.0, M_PI);
    cairo_arc(cr, lbl_x + lbl_radius, lbl_y + lbl_radius,
              lbl_radius, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.9);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // Draw the model name text in white.
    cairo_move_to(cr,
                  lbl_x + lbl_padding - lbl_ext.x_bearing,
                  lbl_y + lbl_padding + lbl_ext.height);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_show_text(cr, model_name_.c_str());
  }
}
  //----------------------------------------------------------------------------
  // Find detections matching this timestamp
  //----------------------------------------------------------------------------
  std::vector<DetectionBox> current_targets;
  {
    std::lock_guard<std::mutex> lock(targets_mutex_);

    if (timestamped_detections_.empty()) {
      return;
    }

    // Find closest timestamp (prefer earlier detections for less latency)
    guint64 closest_diff = UINT64_MAX;
    auto closest_it = timestamped_detections_.end();

    for (auto iter = timestamped_detections_.begin();
         iter != timestamped_detections_.end(); ++iter) {
      // Only consider detections at or before current timestamp
      if (iter->first <= timestamp) {
        const guint64 diff = timestamp - iter->first;

        if (diff < closest_diff) {
          closest_diff = diff;
          closest_it = iter;
        }
      }
    }

    // If no past detection found, use the earliest future one
    if (closest_it == timestamped_detections_.end()) {
      closest_it = timestamped_detections_.begin();
      closest_diff =
          (closest_it->first > timestamp) ? (closest_it->first - timestamp) : 0;
    }

    // Only use detection if within tolerance
    if (closest_it != timestamped_detections_.end() &&
        closest_diff < TIMESTAMP_TOLERANCE) {
      current_targets = closest_it->second;
    }
  }

  if (current_targets.empty()) {
    return;
  }

  //----------------------------------------------------------------------------
  // Draw detections
  //----------------------------------------------------------------------------

  if (!no_bbox_) {
    cairo_set_font_size(cr, 14.0);
    cairo_set_line_width(cr, 2.5);

    // Palette of 50 visually distinct colors. When tracking is enabled the box
    // color is chosen by track id (track_id % 50) so each tracked object keeps
    // a stable, distinct color across frames and can be followed by color alone
    // without reading the id number.
    struct RgbColor {
      double r, g, b;
    };
    static const RgbColor kTrackColors[50] = {
        {0.902, 0.098, 0.294}, {0.235, 0.706, 0.294}, {1.000, 0.882, 0.098},
        {0.263, 0.388, 0.847}, {0.961, 0.510, 0.192}, {0.569, 0.118, 0.706},
        {0.275, 0.941, 0.941}, {0.941, 0.196, 0.902}, {0.737, 0.965, 0.047},
        {0.980, 0.745, 0.831}, {0.000, 0.502, 0.502}, {0.902, 0.745, 1.000},
        {0.667, 0.431, 0.157}, {1.000, 0.980, 0.784}, {0.502, 0.000, 0.000},
        {0.667, 1.000, 0.765}, {0.502, 0.502, 0.000}, {1.000, 0.843, 0.706},
        {0.000, 0.000, 0.502}, {0.502, 0.502, 0.502}, {0.941, 0.502, 0.502},
        {0.196, 0.804, 0.196}, {0.118, 0.565, 1.000}, {1.000, 0.271, 0.000},
        {0.580, 0.000, 0.827}, {0.000, 0.808, 0.820}, {1.000, 0.412, 0.706},
        {0.498, 1.000, 0.000}, {0.855, 0.647, 0.125}, {0.294, 0.000, 0.510},
        {0.000, 1.000, 0.498}, {0.804, 0.361, 0.361}, {0.255, 0.412, 0.882},
        {1.000, 0.549, 0.000}, {0.729, 0.333, 0.827}, {0.251, 0.878, 0.816},
        {1.000, 0.078, 0.576}, {0.604, 0.804, 0.196}, {0.871, 0.722, 0.529},
        {0.416, 0.353, 0.804}, {0.196, 0.804, 0.604}, {0.933, 0.510, 0.933},
        {0.000, 0.749, 1.000}, {1.000, 0.627, 0.478}, {0.541, 0.169, 0.886},
        {0.400, 0.804, 0.667}, {0.980, 0.043, 0.325}, {0.678, 1.000, 0.184},
        {0.282, 0.239, 0.545}, {1.000, 0.714, 0.757}};

    for (const auto& box : current_targets) {
     double r, g, b;

     // ReID: the persistent person id was already stamped onto the box in
     // new_detection() from the shared gallery map. Fall back to the raw track
     // id for color/label when no person id is available.
     int person_id = box.person_id;
     bool in_overlap = false;
     if (identity_manager_ && box.track_id >= 0) {
       std::lock_guard<std::mutex> lock(person_mutex_);
       in_overlap = merged_track_set_.count(box.track_id) > 0;
     }
     const int color_id = (person_id > 0) ? person_id : box.track_id;


     if (tracking_enabled_ && box.track_id >= 0) {
       // Color by stable id (person id when available) so each identity keeps a
       // distinct color even across track fragmentation.
       const RgbColor& c = kTrackColors[color_id % 50];
       r = c.r;
       g = c.g;
       b = c.b;
     } else {
       // Fallback (no tracking): generate color based on class id.
       const double hue = (box.class_id * 137.5) / 360.0;

       const int h_i = static_cast<int>(hue * 6.0);
       const double f = hue * 6.0 - h_i;
       const double q = 1.0 - f;

       switch (h_i % 6) {
         case 0:
           r = 1.0;
           g = f;
           b = 0.0;
           break;
         case 1:
           r = q;
           g = 1.0;
           b = 0.0;
           break;
         case 2:
           r = 0.0;
           g = 1.0;
           b = f;
           break;
         case 3:
           r = 0.0;
           g = q;
           b = 1.0;
           break;
         case 4:
           r = f;
           g = 0.0;
           b = 1.0;
           break;
         case 5:
           r = 1.0;
           g = 0.0;
           b = q;
           break;
         default:
           r = 1.0;
           g = 0.0;
           b = 0.0;
           break;
       }
     }

     // Draw bounding box
     cairo_set_source_rgb(cr, r, g, b);
     cairo_rectangle(cr, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
     cairo_stroke(cr);
     if (!only_bbox_) {
       // Get class name and create label
       const std::string class_name = get_class_name(box.class_id);
       char label_text[80];
       if (in_overlap && person_id <= 0) {
         // Track is in an overlap/merge and has no committed identity yet ->
         // show "?" until ReID assigns a fresh id after the people separate.
         snprintf(label_text, sizeof(label_text), "?");
       } else if (person_id > 0) {
         // ReID assigned a persistent identity -> show the person id.
         snprintf(label_text, sizeof(label_text), "P%d", person_id);
       } else if (tracking_enabled_ && box.track_id >= 0) {
         snprintf(label_text, sizeof(label_text), "ID %d", box.track_id);
       } else {
         snprintf(label_text, sizeof(label_text), "%s", class_name.c_str());
       }

       cairo_text_extents_t label_extents;
       cairo_text_extents(cr, label_text, &label_extents);

       const double label_x = box.x1;
       const double label_y = box.y1 - 4.0;

       // Draw label background
       cairo_rectangle(cr, label_x, label_y - label_extents.height - 4.0,
			    label_extents.width + 8.0, label_extents.height + 6.0);
       cairo_fill(cr);

       // Draw label text
       cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
       cairo_move_to(cr, label_x + 4.0, label_y - 2.0);
       cairo_show_text(cr, label_text);
      }
    }

  }
}

int Detection::get_terminal_columns() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    return w.ws_col;
  }
  return 80;  // Default fallback
}

ModelResolution Demo::get_model_resolution(const std::string& model_path) {
  // Default fallback resolution
  ModelResolution result{640, 360, false};

  // Library is always loaded, so call directly
  dv_model_t* model_info = nullptr;
  int status = dv_model_get_parameters_from_file(model_path.c_str(), &model_info);

  if (status != 0) {  // DV_SUCCESS = 0
    g_printerr("Warning: Could not retrieve model parameters\n");
    return result;
  }

  if (!model_info || model_info->num_inputs == 0) {
    if (model_info) {
      dv_model_free_parameters(model_info);
    }
    return result;
  }

  // Extract width and height from first input layer
  const dv_model_input_param_t* input_param = &model_info->input_param[0];

  if (input_param->width > 0 && input_param->height > 0) {
    result.width = input_param->width;
    result.height = input_param->height;
    result.success = true;

    g_print("Model Resolution (from file): %dx%d\n", result.width, result.height);
    if (input_param->layer_name) {
      g_print("Input Layer: %s\n", input_param->layer_name);
    }
    g_print("Channels: %d, BPP: %d\n", input_param->nch, input_param->bpp);

    // Free model info
    dv_model_free_parameters(model_info);
    return result;
  }

  g_printerr("Warning: Invalid input dimensions in model\n");
  dv_model_free_parameters(model_info);
  return result;
}

//==============================================================================
// Demo Class Implementation
//==============================================================================
 Demo::Demo(int streams, int endpoint, const std::string& group,
           bool enable_perf_stats, bool sync, const std::string& model,
           int console_interval)
    : streams_(streams),
      endpoint_(endpoint),
      group_(group),
      enable_perf_stats_(enable_perf_stats),
      sync_(sync),
      pipeline_(nullptr),
      main_loop_(nullptr),
      model_(model),
      sock_("/var/run/proxy.sock"),
      video_widths_(streams_, 640),
      video_heights_(streams_, 360),
      original_widths_(streams_, 0),
      original_heights_(streams_, 0),
      window_width(1920),
      window_height(1080),
      no_bbox_(false),
      no_osd_stats_(false),
      only_bbox_(false),
      headless_(false),
      tracking_enabled_(false),
      tracking_class_(-1),
      debug_pipeline_(false),
      console_interval_(console_interval),
      stream_config_(std::make_unique<StreamConfigManager>()),
      selector_(streams_, nullptr),
      black_pad_(streams_, nullptr),
      rtsp_pad_(streams_, nullptr),
      stream_alive_(streams_),
      last_total_stats_print_(std::chrono::steady_clock::now()),
      demo_start_time_(std::chrono::steady_clock::now()) {
  // std::atomic<bool> is not copyable, so initialise each element explicitly.
  for (int i = 0; i < streams_; ++i) stream_alive_[i].store(true);
  main_loop_ = g_main_loop_new(nullptr, FALSE);

  const auto [model_width, model_height, success] = get_model_resolution(model);
  model_width_  = model_width;
  model_height_ = model_height;

  if (!success) {
    g_print("Using default resolution: %dx%d\n", model_width, model_height);
  }
}
Demo::~Demo() { cleanup(); }

void Demo::set_stream_config(std::unique_ptr<StreamConfigManager> config) {
  stream_config_ = std::move(config);
}

void Demo::add_stream_source(std::unique_ptr<StreamSourceConfig> config) {
  stream_config_->add_stream(std::move(config));
}

bool Demo::load_stream_config(const std::string& filepath) {
  return stream_config_->load_from_json(filepath);
}

void Demo::list_cameras() {
  std::cout << "\n----------------------------------------\n"
            << "  Available Camera Devices\n"
            << "----------------------------------------\n";

  DIR* dir = opendir("/dev");
  if (!dir) {
    std::cerr << "Error: Cannot open /dev directory\n";
    return;
  }

  struct dirent* entry;
  bool found = false;

  while ((entry = readdir(dir)) != nullptr) {
    if (strncmp(entry->d_name, "video", 5) == 0) {
      std::string device = "/dev/" + std::string(entry->d_name);
      int fd = open(device.c_str(), O_RDONLY);
      if (fd >= 0) {
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
          /* Use per-node device_caps when available (V4L2_CAP_DEVICE_CAPS set).
           * This correctly excludes metadata/control nodes (e.g. /dev/video5)
           * that share V4L2_CAP_VIDEO_CAPTURE in the aggregate capabilities
           * field but do not actually produce video frames. */
          uint32_t node_caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                               ? cap.device_caps
                               : cap.capabilities;
          if ((node_caps & V4L2_CAP_VIDEO_CAPTURE) &&
              !(node_caps & V4L2_CAP_META_CAPTURE)) {
            std::cout << device << " - " << cap.card << "\n";
            found = true;
          }
        }
        close(fd);
      }
    }
  }
  closedir(dir);

  if (!found) {
    std::cout << "No camera devices found\n";
  }
  std::cout << "----------------------------------------\n" << std::endl;
}

void Demo::set_window_size(int width, int height) {
  window_width = width;
  window_height = height;
}

void Demo::set_reid(const std::string& reid_model_path, int min_interval_ms) {
  reid_model_path_ = reid_model_path;
  if (min_interval_ms >= 0) reid_min_interval_ms_ = min_interval_ms;
}

void Demo::set_reid_thresholds(float match, float sticky, int commit_crops) {
  reid_match_thresh_ = match;
  reid_sticky_thresh_ = sticky;
  reid_commit_crops_ = commit_crops;
}

void Demo::set_no_bbox(bool no_bbox) {
  no_bbox_ = no_bbox;
  for (auto& detection : detections_) {
    detection->set_no_bbox(no_bbox);
  }
}

void Demo::set_no_osd_stats(bool no_osd_stats) {
  no_osd_stats_ = no_osd_stats;
  for (auto& detection : detections_) {
    detection->set_no_osd_stats(no_osd_stats);
  }
}

void Demo::set_only_bbox(bool only_bbox) {
  only_bbox_ = only_bbox;
  for (auto& detection : detections_) {
     detection->set_only_bbox(only_bbox);
  }
}

void Demo::set_headless(bool headless) {
  headless_ = headless;
  for (auto& detection : detections_) {
    detection->set_headless(headless);
  }
}

void Demo::set_debug(bool enable) {
    debug_pipeline_ = enable;
}

// Derive the per-stream frame rate used to size the tracker's time-based
// buffers. Camera (V4L2) configs carry an explicit fps; every other source
// (and any out-of-range index) defaults to 30. Shared by set_tracking() and
// start() so the two paths cannot drift.
int Demo::derive_frame_rate(int stream_index) const {
  if (stream_index >= 0 &&
      stream_index < static_cast<int>(stream_config_->get_stream_count())) {
    const StreamSourceConfig* cfg = stream_config_->get_stream(stream_index);
    if (auto cam = dynamic_cast<const CameraV4L2Config*>(cfg)) {
      return cam->get_fps();
    }
  }
  return 30;
}

void Demo::set_tracking(bool enabled, int track_class) {
  tracking_enabled_ = enabled;
  tracking_class_ = track_class;
  // In the normal flow set_tracking() is called from main() before start(),
  // so detections_ is still empty here and the real per-stream frame rate is
  // applied later in start() (where the stream configs are available). If any
  // detections already exist, propagate with the correct per-stream frame
  // rate derived from the stream config rather than a hardcoded value, so the
  // tracker's time-based buffers stay accurate.
  for (size_t i = 0; i < detections_.size(); ++i) {
    int frame_rate = derive_frame_rate(static_cast<int>(i));
    detections_[i]->set_bt_debug(bt_debug_);
    detections_[i]->set_tracking(enabled, track_class, frame_rate);
  }
}




// Sample this process's CPU usage since the previous call. Reads
// /proc/self/stat (process utime+stime) and /proc/stat (total system jiffies)
// and returns the delta ratio. On the first call it just primes the counters
// and returns false (no interval to measure yet). Linux-only; returns false if
// /proc is not readable so callers can silently skip the CPU line.
//   *busy_cores : CPU used by THIS process as % of a single core
//                 (100.0 == one core fully busy; 800.0 == eight cores busy).
//   *sys_pct    : whole-system CPU utilization, 0..100 across all cores.
bool Demo::sample_cpu(double* busy_cores, double* sys_pct) {
  if (cpu_online_cores_ <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_online_cores_ = (n > 0) ? static_cast<int>(n) : 1;
  }

  // --- process jiffies: fields 14 (utime) + 15 (stime) of /proc/self/stat ---
  unsigned long long proc_jiffies = 0;
  {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return false;
    // comm (field 2) may contain spaces/parentheses; skip up to the last ')'.
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    char* rp = strrchr(buf, ')');
    if (!rp) return false;
    // After ')' the fields are: state(3) ppid(4) ... utime(14) stime(15).
    // Relative to the token right after ')', utime is index 11, stime 12.
    unsigned long long vals[16] = {0};
    int idx = 0;
    char* tok = strtok(rp + 1, " ");
    while (tok && idx < 16) {
      vals[idx++] = strtoull(tok, nullptr, 10);
      tok = strtok(nullptr, " ");
    }
    // vals[0]=state? state is non-numeric -> parsed as 0; utime/stime are the
    // 12th/13th whitespace-separated fields after comm, i.e. vals[11]/vals[12].
    proc_jiffies = vals[11] + vals[12];
  }

  // --- total + idle system jiffies from the first line of /proc/stat --------
  // Fields after "cpu": user nice system idle iowait irq softirq steal ...
  // Idle time = idle (field 4) + iowait (field 5). Busy = total - idle.
  unsigned long long total_jiffies = 0;
  unsigned long long idle_jiffies = 0;
  {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
    fclose(f);
    char* tok = strtok(line, " ");  // "cpu"
    int field = 0;                  // 0-based index of the numeric columns
    tok = strtok(nullptr, " ");
    while (tok) {
      unsigned long long v = strtoull(tok, nullptr, 10);
      total_jiffies += v;
      if (field == 3 || field == 4) idle_jiffies += v;  // idle + iowait
      ++field;
      tok = strtok(nullptr, " ");
    }
  }

  const bool primed = (cpu_prev_total_jiffies_ != 0);
  const unsigned long long d_proc =
      proc_jiffies - cpu_prev_proc_jiffies_;
  const unsigned long long d_total =
      total_jiffies - cpu_prev_total_jiffies_;
  const unsigned long long d_idle =
      idle_jiffies - cpu_prev_idle_jiffies_;
  cpu_prev_proc_jiffies_ = proc_jiffies;
  cpu_prev_total_jiffies_ = total_jiffies;
  cpu_prev_idle_jiffies_ = idle_jiffies;

  if (!primed || d_total == 0) return false;  // first sample: just prime.

  // Process share of total system CPU over the interval, scaled to cores.
  const double proc_frac = static_cast<double>(d_proc) / d_total;  // 0..1
  if (busy_cores) *busy_cores = proc_frac * cpu_online_cores_ * 100.0;
  // Whole-system CPU utilization across all cores, 0..100 (busy = 1 - idle).
  const double busy_frac =
      1.0 - static_cast<double>(d_idle) / static_cast<double>(d_total);
  if (sys_pct) *sys_pct = std::max(0.0, std::min(1.0, busy_frac)) * 100.0;
  return true;
}

void Demo::print_total_stats() {
  // Rate-limit: only print once per console_interval_ seconds.
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (std::chrono::duration_cast<std::chrono::seconds>(
            now - last_total_stats_print_).count() < console_interval_) return;
    last_total_stats_print_ = now;
  }

  double total_fps = 0.0;
  double total_ips = 0.0;

  for (int i = 0; i < streams_; ++i) {
    if (!stream_alive_[i].load()) {
      std::cout << "[Stream " << i << "] FPS:  0.0 | IPS:  0.0 [BLACK]\n";
      continue;
    }
    double fps = detections_[i]->get_avg_fps();
    double ips = detections_[i]->get_avg_ips();
    // If no frame has arrived for >3s while marked alive, the source has
    // silently stalled (camera unplugged without ERROR/EOS). Show [STALLED]
    // and flip to black so the compositor tile goes dark.
    const char* label;
    if (!headless_ && is_rtsp_stream(i) &&
        detections_[i]->get_last_frame_age_s() > 3.0) {
      label = "[STALLED]";
      switch_to_black(i);
    } else {
      label = "[LIVE]";
    }
    std::cout << "[Stream " << i << "] "
              << "FPS: " << std::fixed << std::setprecision(1) << fps
              << " | IPS: " << std::fixed << std::setprecision(1) << ips
              << " " << label << "\n";
    total_fps += fps;
    total_ips += ips;
  }

  // Elapsed time since pipeline start
  const auto uptime_secs =
      std::chrono::duration_cast<std::chrono::seconds>(
          now - demo_start_time_).count();
  char uptime_buf[32];
  snprintf(uptime_buf, sizeof(uptime_buf), "[%02ld:%02ld:%02ld]",
           uptime_secs / 3600, (uptime_secs % 3600) / 60, uptime_secs % 60);

  std::cout << "----------------------------------------\n"
            << "TOTAL: FPS: " << std::fixed << std::setprecision(1) << total_fps
            << " | IPS: " << std::fixed << std::setprecision(1) << total_ips
            << "  " << uptime_buf << "\n";

  // Whole-machine CPU: consumed vs idle (across all cores), 0..100 each.
  // Opt-in via --cpu-stats to keep the default TOTAL line compact.
  if (cpu_stats_) {
    double busy_cores = 0.0, sys_pct = 0.0;
    (void)busy_cores;  // per-process detail not shown in the simplified view
    if (sample_cpu(&busy_cores, &sys_pct)) {
      const double idle_pct = 100.0 - sys_pct;
      std::cout << "CPU:   consumed " << std::fixed << std::setprecision(1)
                << sys_pct << "% | idle " << idle_pct << "%\n";
    }
  }

  std::cout << "----------------------------------------\n";
  // ---- ReID stats block (only when --reid-stats and ReID is active) ----------
  if (reid_stats_ && identity_manager_) {
    IdentityManager::Stats gs;
    identity_manager_->getStats(gs);

    std::cout << "========================================\n";
    std::cout << "  ReID Statistics  " << uptime_buf << "\n";
    std::cout << "========================================\n";

    // --- Per-camera block ---
    for (int i = 0; i < streams_; ++i) {
      if (!detections_[i]) continue;
      auto rs = detections_[i]->getReidStats();
      std::cout << "  Cam " << i << " │ "
                << "Tracks: "     << std::setw(3) << rs.activeTracks  << "  "
                << "Identified: " << std::setw(3) << rs.identified    << "  "
                << "Pending: "    << std::setw(3) << rs.unidentified  << "  "
                << "Overlap(?): " << std::setw(2) << rs.inOverlap     << "\n";
      std::cout << "         │ "
                << "ReID runs: "  << std::setw(6) << rs.reidRuns      << "  "
                << "Skipped: "    << std::setw(6) << rs.cropsSkipped  << "\n";
    }

    std::cout << "----------------------------------------\n";

    // --- Global gallery block ---
    std::cout << "  Gallery  │ "
              << "Size: "        << std::setw(4) << gs.gallerySize    << "  "
              << "Minted: "      << std::setw(4) << gs.totalMinted    << "  "
              << "X-cam: "       << std::setw(3) << gs.totalXcam      << "\n";
    std::cout << "           │ "
              << "Acquired: "    << std::setw(5) << gs.totalAcquired  << "  "
              << "Sticky: "      << std::setw(5) << gs.totalSticky    << "  "
              << "SwapFix: "     << std::setw(3) << gs.totalSwapFixes << "\n";
    std::cout << "           │ "
              << "Merged: "      << std::setw(4) << gs.totalMergeResets << "  "
              << "DirectMerge: " << std::setw(5) << gs.totalDirectMerge << "  "
              << "SimSkip: "     << std::setw(5) << gs.totalSkippedSim  << "  "
              << "QualSkip: "    << std::setw(5) << gs.totalSkippedQual << "\n";
    std::cout << "========================================\n";
  }
	 std::cout << "----------------------------------------\n"
            << std::endl;
}

// Prune old recording segments for every registered RecordCleanup entry so
// total disk usage stays bounded. Called once at startup and periodically
// from the main loop via a g_timeout_add_seconds() timer.
void Demo::prune_all_recordings() {
  for (const auto& entry : record_cleanups_) {
    prune_recording_dir(entry.dir, entry.prefix, entry.max_files);
  }
}

// ---------------------------------------------------------------------------
// RTP timestamp pad probe
// ---------------------------------------------------------------------------
// Pre-decode buffer probe on each RTSP stream's watchdog src pad. Buffers here
// are still RTP-packetized (application/x-rtp), so the 32-bit RTP timestamp is
// available. We store (PTS -> rtp_ts) in the per-stream RtpTimestampMap so the
// surviving PTS at the inference appsink can be mapped back to the RTP
// timestamp the browser also observes over WebRTC.
GstPadProbeReturn Demo::rtp_probe_cb(GstPad* /*pad*/, GstPadProbeInfo* info,
                                     gpointer user_data) {
  auto* map = static_cast<RtpTimestampMap*>(user_data);
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer || !map) {
    return GST_PAD_PROBE_OK;
  }

  const guint64 pts = GST_BUFFER_PTS(buffer);
  if (pts == GST_CLOCK_TIME_NONE) {
    return GST_PAD_PROBE_OK;
  }

  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
    const guint32 rtp_ts = gst_rtp_buffer_get_timestamp(&rtp);
    gst_rtp_buffer_unmap(&rtp);
    map->insert(pts, rtp_ts);
  }

  return GST_PAD_PROBE_OK;
}

// Attach the pre-decode RTP-timestamp probe to every RTSP stream's watchdog
// src pad. Called from start() after the pipeline is created. The watchdog
// element is static in the pipeline string (named watchdog_N), so its src pad
// exists immediately and does not require waiting on rtspsrc's dynamic pads.
void Demo::attach_rtp_probes() {
  rtp_maps_.resize(streams_);
  for (int i = 0; i < streams_; ++i) {
    if (!is_rtsp_stream(i)) {
      continue;  // leave entry null for non-RTSP sources
    }
    const std::string wd_name = "watchdog_" + std::to_string(i);
    GstElement* wd = gst_bin_get_by_name(GST_BIN(pipeline_), wd_name.c_str());
    if (!wd) {
      g_printerr("[rtp-probe] stream %d: %s not found; RTP mapping disabled\n",
                 i, wd_name.c_str());
      continue;
    }
    GstPad* srcpad = gst_element_get_static_pad(wd, "src");
    if (!srcpad) {
      g_printerr("[rtp-probe] stream %d: %s has no src pad\n", i,
                 wd_name.c_str());
      gst_object_unref(wd);
      continue;
    }

    rtp_maps_[i] = std::make_unique<RtpTimestampMap>();
    gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, rtp_probe_cb,
                      rtp_maps_[i].get(), nullptr);
    g_print("[rtp-probe] stream %d: probe attached to %s src pad\n", i,
            wd_name.c_str());

    gst_object_unref(srcpad);
    gst_object_unref(wd);
  }
}

bool Demo::start() {

  const std::string pipeline_str = build_compositor(streams_);

  std::string p = pipeline_str;
  size_t pos = 0;
  while ((pos = p.find("! ", pos)) != std::string::npos) {
      p.insert(pos + 2, "\n");
      pos += 3;
  }

  std::cout << "\n";
  std::cout << "┌──────────┬────────────────────┬───────────────────┬──────────┐\n";
  std::cout << "│ Stream   │ Initial Resolution │ Final Resolution  │ Codec    │\n";
  std::cout << "├──────────┼────────────────────┼───────────────────┼──────────┤\n";

  for (int i = 0; i < get_streams_count(); ++i) {
    std::string codec = "unknown";
    if (stream_config_) {
      const StreamSourceConfig* cfg = stream_config_->get_stream(i);
      if (cfg) codec = cfg->get_codec();
    }
    // A zero original size means the source was never successfully probed
    // (unreachable camera, unsupported source type). Report that honestly
    // instead of echoing the internal caps fallback as if it were measured.
    const std::string initial =
        (get_original_width(i) > 0 && get_original_height(i) > 0)
            ? std::to_string(get_original_width(i)) + "x" +
                  std::to_string(get_original_height(i))
            : "unknown";
    std::cout << "│ " << std::setw(8) << std::left << i << " │ "
              << std::setw(18) << std::left << initial << " │ "
              << std::setw(17) << std::left
              << (std::to_string(video_widths_[i]) + "x" + std::to_string(video_heights_[i])) << " │ "
              << std::setw(8) << std::left << codec << " │\n";
  }
  std::cout << "└──────────┴────────────────────┴───────────────────┴──────────┘\n";
  std::cout << "\n";

  std::cout << "\n----------------------------------------" << std::endl;
  std::cout << "LAUNCHING GSTREAMER PIPELINE:" << std::endl;
  std::cout << "----------------------------------------" << std::endl;

  if (debug_pipeline_) {
        std::cout << "========================================\n" << std::endl;
	std::cout << p << std::endl;
        std::cout << "========================================\n" << std::endl;
  }
  GError* error = nullptr;
  pipeline_ = gst_parse_launch(pipeline_str.c_str(), &error);

  if (error) {
    g_printerr("Failed to parse pipeline: %s\n", error->message);
    g_error_free(error);
    return false;
  }

  if (!pipeline_) {
    g_printerr("Failed to create pipeline\n");
    return false;
  }

  // ---- ReID: init the shared Ara-2 runtime + OSNet embedder (once). Uses the
  // same NPU proxy socket the dvInf detector uses. Empty model path or any
  // failure => ReID embedding stays disabled (cropping still runs). ----
  if (!reid_model_path_.empty()) {
    reid_runtime_ = std::make_unique<AraRuntime>();
    if (reid_runtime_->init(sock_)) {
      reid_embedder_ = std::make_unique<AraReIdEmbedder>();
      if (!reid_embedder_->init(reid_runtime_.get(), reid_model_path_)) {
        g_printerr("[REID] OSNet embedder init failed (%s); "
                   "continuing with cropping only\n",
                   reid_model_path_.c_str());
        reid_embedder_.reset();
      } else {
        g_print("[REID] OSNet embedder ready: model=%s interval=%dms\n",
                reid_model_path_.c_str(), reid_min_interval_ms_);
        // Shared appearance gallery: enables persistent person IDs across
        // track fragmentation (and across streams).
        identity_manager_ = std::make_unique<IdentityManager>();
        identity_manager_->setMatchThreshold(reid_match_thresh_);
        identity_manager_->setStickyThreshold(reid_sticky_thresh_);
        if (reid_max_gallery_ > 0)
          identity_manager_->setMaxGallery(reid_max_gallery_);
        if (reid_commit_crops_ > 0)
          identity_manager_->setCommitMinCrops(reid_commit_crops_);
        if (reid_gallery_k_ > 0)
          identity_manager_->setGalleryK(reid_gallery_k_);
        identity_manager_->setDebug(reid_debug_);
        g_print("[REID] gallery thresholds: match=%.3f sticky=%.3f "
                "commitCrops=%d galleryK=%d debug=%d (any -1 = default)\n",
                reid_match_thresh_, reid_sticky_thresh_, reid_commit_crops_,
                reid_gallery_k_, reid_debug_ ? 1 : 0);
      }
    } else {
      g_printerr("[REID] Ara runtime init failed on %s (%s); "
                 "continuing with cropping only\n",
                 sock_.c_str(), reid_runtime_->lastError());
      reid_runtime_.reset();
    }
  }

  // Create Detection instances
  detections_.reserve(streams_);
  for (int i = 0; i < streams_; ++i) {
    detections_.push_back(std::make_unique<Detection>(i));
    detections_[i]->set_no_bbox(no_bbox_);
    detections_[i]->set_no_osd_stats(no_osd_stats_);
    detections_[i]->set_only_bbox(only_bbox_);
    detections_[i]->set_headless(headless_);
    detections_[i]->set_console_interval(console_interval_);
    detections_[i]->set_reid(reid_embedder_.get(), reid_min_interval_ms_);
    detections_[i]->set_reid_min_crop(reid_min_crop_w_, reid_min_crop_h_);
    detections_[i]->set_reid_proximity_iou(reid_proximity_iou_);
    detections_[i]->set_reid_merge_iomin(reid_merge_iomin_);
    detections_[i]->set_reid_reverify_window(reid_reverify_frames_);
    detections_[i]->set_reid_save_crops(reid_save_crops_);
    detections_[i]->set_identity_manager(identity_manager_.get());

    // Set model name for the on-screen label: extract the directory basename
    // from the model path (e.g. "yolov8n" from ".../yolov8n/model.dvm").
    {
      std::string mn = model_;
      // Strip trailing filename (everything after last '/')
      size_t slash = mn.rfind('/');
      if (slash != std::string::npos) mn = mn.substr(0, slash);
      // Now strip everything before the last '/' to get the directory name
      slash = mn.rfind('/');
      if (slash != std::string::npos) mn = mn.substr(slash + 1);
      detections_[i]->set_model_name(mn);
    }

    // Enable tracking, deriving the frame rate from the stream config so the
    // tracker's time-based buffers are correct. Camera configs carry an fps;
    // for other sources default to 30.
    if (tracking_enabled_) {
      int frame_rate = derive_frame_rate(i);
      detections_[i]->set_bt_debug(bt_debug_);
      detections_[i]->set_tracking(true, tracking_class_, frame_rate);
    }


    detections_[i]->start(pipeline_);
  }


  if (!detections_.empty()) {
    for (int i = 0; i < streams_; ++i) {
      detections_[i]->set_stats_callback([this]() {
        this->print_total_stats();
      });
    }
  }

  // Start WS server and inject publish callback into each Detection.
  if (ws_detections_enabled_) {
    ws_server_ = std::make_unique<DetectionWebSocketServer>(ws_port_);
    ws_server_->start();
    for (int i = 0; i < streams_; ++i) {
      detections_[i]->set_detection_callback(
          [this](int sid, const std::vector<DetectionBox>& b, guint64 pts) {
            // Recover the real wire RTP timestamp for this frame by looking up
            // its surviving PTS in the per-stream map populated by the
            // pre-decode probe. When there is no RTSP source / no match (e.g.
            // file streams), rtp_valid stays false and the JSON
            // omits rtp_ts.
            guint32 rtp_ts = 0;
            bool rtp_valid = false;
            if (sid >= 0 && sid < static_cast<int>(rtp_maps_.size()) &&
                rtp_maps_[sid]) {
              // Allow ~1 frame (33 ms) of slack for decodebin3 reordering.
              rtp_valid = rtp_maps_[sid]->lookup_nearest(
                  pts, 33000000ULL, &rtp_ts);
            }
            ws_server_->publish(sid, b, rtp_ts, rtp_valid);
          });
    }
  }

  // Attach the pre-decode RTP-timestamp probes (RTSP streams only). Safe to
  // call whether or not WS publishing is enabled; the maps are only consulted
  // from the publish lambda above.
  attach_rtp_probes();

  // Resolve input-selector and pad handles.
  // sink_0 -> black videotestsrc (fallback), sink_1 -> live branch.
  // Start with live pad active; switch to black on failure.
  for (int i = 0; i < streams_; ++i) {
    if (!is_rtsp_stream(i)) continue;
    const std::string sel_name = "selector_" + std::to_string(i);
    selector_[i] = gst_bin_get_by_name(GST_BIN(pipeline_), sel_name.c_str());
    if (selector_[i]) {
      black_pad_[i] = gst_element_get_static_pad(selector_[i], "sink_0");
      rtsp_pad_[i]  = gst_element_get_static_pad(selector_[i], "sink_1");
      if (rtsp_pad_[i]) {
        g_object_set(selector_[i], "active-pad", rtsp_pad_[i], nullptr);
      }
    } else {
      g_print("[Stream %d] No input-selector found (non-RTSP source)\n", i);
    }
  }

  // Wire up timestamped file naming for any RTSP recording sinks. Each
  // splitmuxsink emits "format-location" when it needs a new segment file
  // name; record_format_location() builds cam<N>_<timestamp>.mp4. The
  // RecordNaming context is owned by the sink element (freed via
  // g_object_set_data_full) so it stays alive for the sink's lifetime.
  for (int i = 0; i < streams_; ++i) {
    const StreamSourceConfig* config = stream_config_->get_stream(i);
    auto rtsp_cfg = dynamic_cast<const RTSPConfig*>(config);
    if (!rtsp_cfg || !rtsp_cfg->is_recording_enabled()) {
      continue;
    }

    const std::string recsink_name = "recsink_" + std::to_string(i);
    GstElement* recsink =
        gst_bin_get_by_name(GST_BIN(pipeline_), recsink_name.c_str());
    if (!recsink) {
      continue;
    }

    RecordNaming* naming = new RecordNaming{i, rtsp_cfg->get_record_dir()};
    g_object_set_data_full(G_OBJECT(recsink), "record-naming", naming,
                           [](gpointer data) {
                             delete static_cast<RecordNaming*>(data);
                           });
    // Pass nullptr as user_data: the callback deliberately reads the naming
    // context from the element (single owner) rather than from user_data.
    g_signal_connect(recsink, "format-location",
                     G_CALLBACK(record_format_location), nullptr);
    gst_object_unref(recsink);

    // Register an application-side retention entry for this sink. Compute
    // max_files via the shared compute_retention_max_files() helper so it can
    // never diverge from the value configured on splitmuxsink; the janitor
    // enforces it here because splitmuxsink's built-in pruning does not run
    // when format-location supplies segment names.
    int max_files = compute_retention_max_files(
        rtsp_cfg->get_retention_minutes(), rtsp_cfg->get_segment_seconds());
    record_cleanups_.push_back(

        {rtsp_cfg->get_record_dir(), "cam" + std::to_string(i) + "_",
         max_files});
  }

  // If any RTSP recording sink is active, prune once immediately (so a
  // restart reclaims disk left over from a prior run) then run the janitor
  // periodically from the main loop. A fixed 30 s interval keeps disk usage
  // bounded without depending on segment length; the lambda trampoline returns
  // G_SOURCE_CONTINUE to stay armed for the lifetime of the loop.
  if (!record_cleanups_.empty()) {
    prune_all_recordings();
    record_cleanup_timer_id_ = g_timeout_add_seconds(
        30,
        +[](gpointer data) -> gboolean {
          static_cast<Demo*>(data)->prune_all_recordings();
          return G_SOURCE_CONTINUE;
        },
        this);
  }



  // Setup bus
  GstBus* bus = gst_element_get_bus(pipeline_);
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message", G_CALLBACK(on_bus_message_callback), this);
  gst_object_unref(bus);

  // Connect overrun signals on leaky display queues so dropped overlay frames
  // are observable via GST_DEBUG output rather than silently discarded.
  if (!headless_) {
    for (int i = 0; i < streams_; ++i) {
      for (const std::string& qname :
           {"q_disp" + std::to_string(i), "q_af_c" + std::to_string(i)}) {
        GstElement* q = gst_bin_get_by_name(GST_BIN(pipeline_), qname.c_str());
        if (q) {
          g_signal_connect(q, "overrun", G_CALLBACK(+[](GstElement* queue, gpointer) {
            GST_DEBUG_OBJECT(queue, "leaky queue overrun: frame dropped");
          }), nullptr);
          gst_object_unref(q);
        }
      }
    }
  }

  // Start playing
  const GstStateChangeReturn ret =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set pipeline to playing state\n");
    return false;
  }

  g_print("Pipeline started with %d stream(s)\n", streams_);
  if (enable_perf_stats_) {
    g_print("Preprocessing performance statistics: ENABLED\n");
  }
  g_print("Press Ctrl+C to stop\n");

  // Run main loop
  g_main_loop_run(main_loop_);

  return true;
}

std::string Demo::build_compositor(int number_of_streams) {
  int rows, cols;
  // Optimize grid layout for different stream counts
    switch (number_of_streams) {
    case 1:
      rows = 1;
      cols = 1;
      break;
    case 2:
      rows = 1;
      cols = 2;
      break;
    case 3:
    case 4:
      rows = 2;
      cols = 2;
      break;
    case 5:
    case 6:
      rows = 2;
      cols = 3;
      break;
    case 7:
    case 8:
      rows = 2;
      cols = 4;
      break;
    case 9:
    case 10:
    case 11:
    case 12:
      rows = 3;
      cols = 4;
      break;
    case 13:
    case 14:
    case 15:
    case 16:
      rows = 4;
      cols = 4;
      break;
    default:
      rows = 4;
      cols = static_cast<int>(
          std::ceil(static_cast<double>(number_of_streams) / rows));
  }

  const int stream_width = window_width / cols;
  const int stream_height = window_height / rows;

  std::string detection_pipeline;
  std::string compositor_pipeline;

  int group_id = 0;
  if (group_ == "pcie") {
    group_id = 1;
  } else if (group_ == "usb") {
    group_id = 2;
  }

  // Probe all RTSP sources' real frame size up front, in parallel, so the
  // per-stream pipelines below can use it without N sequential waits.
  probe_rtsp_resolutions(number_of_streams);

  // Build source pipelines using new configuration system
  for (int num = 0; num < number_of_streams; ++num) {
    detection_pipeline += build_source_pipeline(num);
  }

  // Build processing branches (SAME FOR ALL SOURCE TYPES)
  // Helper lambda: builds the recording+RTSP branches for a libcamera stream.
  // Returns empty string if neither recording nor rtsp_out is enabled.
  //
  // enc_tee_N is only created when BOTH recording and RTSP are active.
  // When only one is active, the encoder output goes directly to that sink —
  // no tee, no fakesink, no wasted buffer copies.
  //
  //   record=true,  rtsp=true:  encoder -> enc_tee_N -> splitmuxsink + rtspclientsink
  //   record=true,  rtsp=false: encoder -> h264parse -> splitmuxsink
  //   record=false, rtsp=true:  encoder -> rtspclientsink (no parse needed)
  //
  // Codec note: this path deliberately uses H.264 (v4l2h264enc), unlike the
  // RTSP-source recorder which passes H.265 through. The encoded stream is
  // republished over WebRTC by MediaMTX, and browsers do not reliably support
  // H.265 in WebRTC, so H.264 is required for the stream to be viewable.
  auto build_rec_branch = [&](int num) -> std::string {
    if (num >= static_cast<int>(stream_config_->get_stream_count())) return "";
    const StreamSourceConfig* cfg = stream_config_->get_stream(num);
    auto cam_cfg = dynamic_cast<const CameraLibcameraConfig*>(cfg);
    if (!cam_cfg) return "";
    if (!cam_cfg->is_recording_enabled() && !cam_cfg->is_rtsp_out_enabled()) return "";

    bool do_record     = cam_cfg->is_recording_enabled();
    const bool do_rtsp = cam_cfg->is_rtsp_out_enabled();

    // Validate the recording directory BEFORE any element is appended, so the
    // branch we return is either fully valid or empty. Creating the directory
    // later would leave a dangling encoder with no sink and make
    // gst_parse_launch() fail with a cryptic error.
    if (do_record && !make_dirs_recursive(cam_cfg->get_record_dir(), 0755)) {
      g_printerr("Stream %d: cannot create recording dir %s; "
                 "recording disabled\n",
                 num, cam_cfg->get_record_dir().c_str());
      do_record = false;
    }

    // Nothing left to build once recording has been disabled above.
    if (!do_record && !do_rtsp) return "";

    const bool need_tee = do_record && do_rtsp;

    // Common encoder path
    std::string branch =
        "tee_" + std::to_string(num) + ". "
        "! queue name=q_rec" + std::to_string(num) +
        " leaky=downstream max-size-buffers=4 max-size-bytes=0 max-size-time=0 "
        "! imxvideoconvert_g2d "
        "! video/x-raw,format=NV12 "
        "! v4l2h264enc extra-controls=\"controls,repeat_sequence_header=1\" ";

    if (need_tee) {
      // Both active — split after encoder
      branch += "! tee name=enc_tee_" + std::to_string(num) + " ";
    }

    // Branch A: recording
    if (do_record) {
      const std::string& rec_dir = cam_cfg->get_record_dir();
      guint64 seg_ns = static_cast<guint64>(cam_cfg->get_segment_seconds())
                       * 1000000000ULL;
      int max_f = cam_cfg->get_max_files();
      g_print("Stream %d: libcamera recording enabled -> %s\n",
              num, rec_dir.c_str());
      const std::string tap = need_tee
          ? "enc_tee_" + std::to_string(num) + ". ! "
            "queue max-size-buffers=4 leaky=downstream "
            "max-size-bytes=0 max-size-time=0 ! "
          : "! ";
      branch +=
          tap +
          "h264parse "
          "! splitmuxsink name=recsink_" + std::to_string(num) + " "
          "location=" + rec_dir + "/cam" + std::to_string(num) + "_%05d.mp4 "
          "max-size-time=" + std::to_string(seg_ns) + " "
          "max-files=" + std::to_string(max_f) + " "
          "muxer-factory=mp4mux "
          "send-keyframe-requests=true "
          "async-finalize=true "
          "use-robust-muxing=true ";
    }

    // Branch B: RTSP out
    if (do_rtsp) {
      g_print("Stream %d: RTSP out enabled -> rtsp://127.0.0.1:%d/cam%d\n",
              num, cam_cfg->get_rtsp_port(), num);
      const std::string tap = need_tee
          ? "enc_tee_" + std::to_string(num) + ". ! "
            "queue max-size-buffers=2 leaky=downstream "
            "max-size-bytes=0 max-size-time=0 ! "
          : "! ";
      // h264parse only needed when tee is present (recording path needs it for MP4)
      const std::string parse = need_tee ? "h264parse config-interval=-1 ! " : "";
      branch +=
          tap + parse +
          "rtspclientsink "
          "location=rtsp://127.0.0.1:" + std::to_string(cam_cfg->get_rtsp_port()) +
          "/cam" + std::to_string(num) + " protocols=udp ";
    }

    return branch;
  };

  for (int num = 0; num < number_of_streams; ++num) {
    const int xpos = (num % cols) * stream_width;
    const int ypos = (num / cols) * stream_height;

    //--------------------------------------------------------------------------
    // Inference branch
    //--------------------------------------------------------------------------
    detection_pipeline += "tee_" + std::to_string(num) + ". ";
    detection_pipeline +=
        "! queue name=q_inf" + std::to_string(num) + " leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";

    // Build dvInf element
    detection_pipeline += "! dvInf model=" + model_ + " sock=" + sock_ +
                          " endpoint=" + std::to_string(endpoint_) +
                          " group=" + std::to_string(group_id) +
                          " stream=" + std::to_string(num) +
                          " orig-width=" + std::to_string(video_widths_[num]) +
                          " orig-height=" + std::to_string(video_heights_[num]) +
                          " iou=" + std::to_string(0.45) +
			                    " nms=" + std::to_string(0.25) +
                          " use-shm=true shm-path=/dev/shm/ara_inf_" +
                          std::to_string(getpid()) + "_" + std::to_string(num) +
                          " shm-size=24 skip=1 offset=0 silent=true ";

    // Only enable perf stats for the first stream to avoid cluttered output
    if (enable_perf_stats_ && num == 0) {
      detection_pipeline += " enable-perf-stats=true stats-interval=5";
    }

    detection_pipeline += " ";

    detection_pipeline +=
        "! queue name=q_det" + std::to_string(num) + " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
    detection_pipeline += "! appsink name=dv_post_sink_" + std::to_string(num) +
                          " emit-signals=true sync=false drop=false "
                          "max-buffers=1 ";

    //--------------------------------------------------------------------------
    // ReID crop branch: a dedicated tap off tee_N that delivers raw RGB frames
    // (at the same video_widths_/video_heights_ scale the boxes use) to an
    // appsink, so ReID can crop person boxes out of real pixels. Independent of
    // the display branch, so it also works in headless mode.
    //--------------------------------------------------------------------------
    detection_pipeline += "tee_" + std::to_string(num) + ". ";
    detection_pipeline +=
        "! queue name=q_reid" + std::to_string(num) +
        " leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ";
    detection_pipeline +=
        "! imxvideoconvert_g2d ! video/x-raw,format=RGB,width=" +
        std::to_string(video_widths_[num]) +
        ",height=" + std::to_string(video_heights_[num]) + " ";
    detection_pipeline +=
        "! appsink name=reid_frame_sink_" + std::to_string(num) +
        " emit-signals=true sync=false drop=true max-buffers=1 ";

    //--------------------------------------------------------------------------
    // Display branch (skip if headless)
    // Architecture:
    //   videotestsrc(black) -> q_black_N -> selector_N.          (sink_0)
    //   tN -> q_dispN -> cairooverlay -> q_af_cN -> imxvideoconvert -> selector_N.  (sink_1)
    //   selector_N -> q_compN (leaky) -> comp.sink_N
    //--------------------------------------------------------------------------
    if (!headless_) {
      if (is_rtsp_stream(num)) {
      // Black fallback source -- links first, gets sink_0
      detection_pipeline +=
          "videotestsrc name=black_src_" + std::to_string(num) +
          " pattern=black is-live=true do-timestamp=true "
          "! video/x-raw,format=RGBA"
          ",width=" + std::to_string(video_widths_[num]) +
          ",height=" + std::to_string(video_heights_[num]) + " "
          "! queue name=q_black_" + std::to_string(num) +
          " max-size-buffers=2 max-size-bytes=0 max-size-time=0 "
          "! selector_" + std::to_string(num) + ". ";

      // Live branch -- links second, gets sink_1
      detection_pipeline += "tee_" + std::to_string(num) + ". ";
      detection_pipeline +=
          "! queue name=q_disp" + std::to_string(num) +
          " leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
      detection_pipeline +=
          "! cairooverlay name=draw_detections_" + std::to_string(num) + " ";
      detection_pipeline +=
          "! queue name=q_af_c" + std::to_string(num) +
          " leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
      detection_pipeline +=
          "! imxvideoconvert_g2d ! video/x-raw,format=RGBA,"
          "width=" + std::to_string(video_widths_[num]) +
          ",height=" + std::to_string(video_heights_[num]) + " ";
      detection_pipeline += "! selector_" + std::to_string(num) + ". ";

      // input-selector declaration
      detection_pipeline +=
          "input-selector name=selector_" + std::to_string(num) +
          " sync-streams=false sync-mode=0 ";

      // leaky queue before compositor prevents back-pressure from dead streams
      detection_pipeline +=
          "! queue name=q_comp" + std::to_string(num) +
          " leaky=downstream max-size-buffers=2"
          " max-size-bytes=0 max-size-time=0 ";
      detection_pipeline += "! comp.sink_" + std::to_string(num) + " ";
      } else {
        // Recording/RTSP taps tee_N directly (see build_rec_branch), which is
        // upstream of cairooverlay — so recordings carry no bounding boxes.
        // Emitted as its own chain; returns "" when neither is enabled.
        detection_pipeline += build_rec_branch(num);

        detection_pipeline += "tee_" + std::to_string(num) + ". ";
        detection_pipeline +=
            "! queue name=q_disp" + std::to_string(num) +
            " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
        detection_pipeline +=
            "! cairooverlay name=draw_detections_" + std::to_string(num) + " ";
        detection_pipeline +=
            "! queue name=q_af_c" + std::to_string(num) +
            " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
        detection_pipeline += "! imxvideoconvert_g2d ! video/x-raw,format=RGBA ";
        detection_pipeline += "! comp.sink_" + std::to_string(num) + " ";
      }

      //------------------------------------------------------------------------
      // Compositor configuration with keep-ratio
      //------------------------------------------------------------------------
      compositor_pipeline +=
          "sink_" + std::to_string(num) + "::xpos=" + std::to_string(xpos) + " ";
      compositor_pipeline +=
          "sink_" + std::to_string(num) + "::ypos=" + std::to_string(ypos) + " ";
      compositor_pipeline += "sink_" + std::to_string(num) +
                             "::width=" + std::to_string(stream_width) + " ";
      compositor_pipeline += "sink_" + std::to_string(num) +
                             "::height=" + std::to_string(stream_height) + " ";
      compositor_pipeline += "sink_" + std::to_string(num) + "::keep-ratio=true ";
    }
  }

  // Headless: build_rec_branch() handles both recording and RTSP out via
  // enc_tee_N after the shared encoder. One encoder per stream serves both.
  if (headless_) {
    for (int num = 0; num < number_of_streams; num++) {
      detection_pipeline += build_rec_branch(num);
    }
    return detection_pipeline;
  }

  // Build waylandsink with configurable window size
  std::string waylandsink_config =
	std::string(sync_ ? "" : "! queue leaky=1 ") +
       "! waylandsink name=wlsink sync=" + std::string(sync_ ? "true" : "false");
  waylandsink_config += " window-width=" + std::to_string(window_width) +
                          " window-height=" + std::to_string(window_height);
  const std::string entire_pipeline =
      "imxcompositor_g2d name=comp background=0xFF000000 " +
      compositor_pipeline + waylandsink_config + " " + detection_pipeline;

  return entire_pipeline;
}

//==============================================================================
// Message Handling
//==============================================================================

gboolean Demo::on_bus_message_callback(GstBus* bus, GstMessage* message,
                                       gpointer user_data) {
  auto* self = static_cast<Demo*>(user_data);
  self->on_bus_message(message);
  return TRUE;
}

void Demo::set_fullscreen_when_ready() {
  GstElement* waylandsink = gst_bin_get_by_name(GST_BIN(pipeline_), "wlsink");
  if (waylandsink) {
    // Set fullscreen property to FALSE to allow window drag
    g_object_set(waylandsink, "fullscreen", FALSE, NULL);
    gst_object_unref(waylandsink);
  }
}

// ---------------------------------------------------------------------------
// resolve_stream_id: walk the GstObject parent chain looking for a named
// per-stream element (src_N, decodebin_N, tN, q_infN, ...).
// Returns stream index or -1.
// ---------------------------------------------------------------------------
int Demo::resolve_stream_id(GstObject* obj) const {
  static const std::vector<std::string> PREFIXES = {
      "src_", "decodebin_", "tee_", "q_inf", "q_disp",
      "q_det", "q_af_c", "draw_detections_", "dv_post_sink_",
      "selector_", "black_src_", "watchdog_"
  };
  GstObject* cur = obj;
  while (cur) {
    const gchar* name = GST_OBJECT_NAME(cur);
    if (name) {
      std::string n(name);
      for (int i = 0; i < streams_; ++i) {
        const std::string idx = std::to_string(i);
        for (const auto& pfx : PREFIXES) {
          if (n == pfx + idx) return i;
        }
      }
      // rtspsrc is auto-named rtspsrcN by GStreamer (not src_N)
      if (n.size() > 7 && n.substr(0, 7) == "rtspsrc") {
        try {
          int idx = std::stoi(n.substr(7));
          if (idx >= 0 && idx < streams_) return idx;
        } catch (...) {}
      }
    }
    cur = GST_OBJECT_PARENT(cur);
  }
  return -1;
}

void Demo::switch_to_black(int stream_id) {
  if (stream_id < 0 || stream_id >= streams_) return;

  // Headless has no display branch or input-selector, so there is nothing to
  // blank. Skip the demotion entirely; otherwise it would only mark the stream
  // permanently dead and zero its FPS/IPS on transient RTSP drops, with no
  // recovery path. This guards all callers (bus ERROR/WARNING/EOS handlers and
  // the silent-stall check).
  if (headless_) return;

  stream_alive_[stream_id].store(false);
  if (stream_id < static_cast<int>(detections_.size())) {
    detections_[stream_id]->set_stream_dead(true);
    detections_[stream_id]->reset_fps();
  }
  if (selector_[stream_id] && black_pad_[stream_id]) {
    g_object_set(selector_[stream_id], "active-pad", black_pad_[stream_id], nullptr);
    g_print("[Stream %d] -> black fallback\n", stream_id);
  } else {
    g_print("[Stream %d] -> marked dead (no selector for this source type)\n", stream_id);
  }
}

void Demo::on_bus_message(GstMessage* message) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ASYNC_DONE:
      // Pipeline is fully prerolled, now safe to set fullscreen
      set_fullscreen_when_ready();
      break;

    case GST_MESSAGE_EOS: {
      int stream_id = resolve_stream_id(GST_MESSAGE_SRC(message));
      if (stream_id >= 0) {
        g_print("\n[Stream %d] EOS -- switching to black\n", stream_id);
        switch_to_black(stream_id);
      } else {
        g_print("\nPipeline EOS -- shutting down\n");
        g_main_loop_quit(main_loop_);
      }
      break;
    }

    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_error(message, &err, &debug_info);
      int stream_id = resolve_stream_id(GST_MESSAGE_SRC(message));
      if (stream_id >= 0) {
        g_printerr("[Stream %d] Error: %s -- switching to black\n",
                   stream_id, err ? err->message : "?");
        switch_to_black(stream_id);
      } else {
        const gchar* src_name = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
        std::string sname(src_name ? src_name : "");
        bool is_fatal = (sname.find("comp") != std::string::npos ||
                         sname.find("waylandsink") != std::string::npos ||
                         sname.find("pipeline") != std::string::npos);
        if (is_fatal) {
          g_printerr("Fatal error from [%s]: %s\n", src_name,
                     err ? err->message : "?");
          // free before quitting the loop
          g_clear_error(&err);
          g_free(debug_info);
          g_main_loop_quit(main_loop_);
          return;
        }
      }
      // Always free after both branches to avoid future double-free risk.
      g_clear_error(&err);
      g_free(debug_info);
      break;
    }

    case GST_MESSAGE_WARNING: {
      GError* err = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_warning(message, &err, &debug_info);
      int stream_id = resolve_stream_id(GST_MESSAGE_SRC(message));

      if (stream_id >= 0 && is_rtsp_stream(stream_id)) {
        g_printerr("[Stream %d] Warning: %s -- switching to black\n",
                   stream_id, err ? err->message : "?");
        switch_to_black(stream_id);
      } else if (stream_id >= 0) {
        g_print("[Stream %d] Warning (non-RTSP, ignored): %s\n",
                stream_id, err ? err->message : "?");
      }
      // Always free after both branches to avoid future double-free risk.
      g_clear_error(&err);
      g_free(debug_info);
      break;
    }

    case GST_MESSAGE_STREAM_START:
    case GST_MESSAGE_QOS:
      // Silently ignore these messages
      break;

    default:
      break;
  }
}

void Demo::cleanup() {
  // Release input-selector pad/element references
  for (int i = 0; i < streams_; ++i) {
    if (black_pad_[i]) {
      gst_object_unref(black_pad_[i]);
      black_pad_[i] = nullptr;
    }
    if (rtsp_pad_[i]) {
      gst_object_unref(rtsp_pad_[i]);
      rtsp_pad_[i] = nullptr;
    }
    if (selector_[i]) {
      gst_object_unref(selector_[i]);
      selector_[i] = nullptr;
    }
  }

  // Stop the recording-retention janitor timer before tearing down the loop
  // so it cannot fire against freed state.
  if (record_cleanup_timer_id_) {
    g_source_remove(record_cleanup_timer_id_);
    record_cleanup_timer_id_ = 0;
  }
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }

  if (main_loop_) {
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
  }

  detections_.clear();
}

struct VideoInfoData {
    int width = 0;
    int height = 0;
    bool found = false;
};

static void on_pad_added(GstElement* decodebin, GstPad* pad, gpointer user_data)
{
    VideoInfoData* data = (VideoInfoData*)user_data;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps)
        caps = gst_pad_query_caps(pad, nullptr);

    if (!caps)
        return;

    GstStructure* str = gst_caps_get_structure(caps, 0);

    if (gst_structure_has_field(str, "width") &&
        gst_structure_has_field(str, "height")) {

        gst_structure_get_int(str, "width", &data->width);
        gst_structure_get_int(str, "height", &data->height);

        data->found = true;
    }

    gst_caps_unref(caps);
}

bool get_video_resolution_caps(const std::string& filepath,
                               int& width, int& height)
{
    gst_init(nullptr, nullptr);

    VideoInfoData data;

    GstElement* pipeline = gst_pipeline_new("pipeline");
    GstElement* src = gst_element_factory_make("filesrc", "src");
    GstElement* decodebin = gst_element_factory_make("decodebin", "decodebin");

    if (!pipeline || !src || !decodebin)
        return false;

    g_object_set(src, "location", filepath.c_str(), NULL);

    gst_bin_add_many(GST_BIN(pipeline), src, decodebin, NULL);
    gst_element_link(src, decodebin);

    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_pad_added), &data);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);

    GstStateChangeReturn ret =
        gst_element_get_state(pipeline, NULL, NULL, 3 * GST_SECOND);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (data.found) {
        width = data.width;
        height = data.height;
    }

    return data.found;
}

//==============================================================================
// RTSP resolution probe
//==============================================================================
// Discover the real frame size of a live RTSP source before the main pipeline
// is built, so the console summary can report the stream's true "Initial
// Resolution" and so the downstream caps can be fitted to the model aspect
// ratio exactly the way video-file sources already are.
//
// A file can be prerolled in PAUSED; an RTSP source is live with dynamic pads
// and has no caps until it is PLAYING, hence a dedicated helper.
//
// For H.264/H.265 (codec taken from the SDP, already detected for the summary
// table) the probe runs rtspsrc ! depay ! parse ! appsink and reads width and
// height from the parser's output caps. Nothing is decoded, so it does not
// wait for a keyframe when the SDP carries the parameter sets, and it does not
// instantiate a hardware decoder that would compete with the real pipelines.
// Any other codec falls back to rtspsrc ! decodebin ! appsink.
namespace {

struct RtspProbeData {
  GstElement* head = nullptr;  // first element after rtspsrc (depay/decodebin)
  GstElement* sink = nullptr;  // appsink (decodebin fallback links to it)
};

// Effective probe timeout: RTSP_PROBE_TIMEOUT_SEC if set to a sane value,
// otherwise kRtspProbeTimeoutSec. Clamped so a typo cannot disable the bound.
int rtsp_probe_timeout_sec() {
  const char* env = std::getenv("RTSP_PROBE_TIMEOUT_SEC");
  if (env && env[0] != '\0') {
    char* end = nullptr;
    const long v = std::strtol(env, &end, 10);
    if (end != env && *end == '\0') {
      return static_cast<int>(std::min(60L, std::max(1L, v)));
    }
  }
  return kRtspProbeTimeoutSec;
}

// rtspsrc exposes one dynamic pad per SDP media. Link the video one into the
// head element; audio/metadata pads are left unlinked.
//
// A pad is rejected ONLY when its current caps positively say it is not video
// (media field present and != "video"). If the caps are not set yet, or carry
// no media field, the pad is linked anyway: the depay/decodebin behind it
// accepts only RTP video, so linking a non-video pad just fails harmlessly,
// whereas wrongly rejecting the one real video pad would make the probe time
// out and look exactly like an unreachable camera. (The pad TEMPLATE caps are
// generic application/x-rtp with no media field, so querying them cannot
// identify video either.)
void rtsp_probe_src_pad_added(GstElement* /*src*/, GstPad* pad,
                              gpointer user_data) {
  auto* data = static_cast<RtspProbeData*>(user_data);
  if (!data || !data->head) return;

  if (GstCaps* caps = gst_pad_get_current_caps(pad)) {
    bool not_video = false;
    if (GstStructure* s = gst_caps_get_structure(caps, 0)) {
      const gchar* media = gst_structure_get_string(s, "media");
      not_video = (media && g_strcmp0(media, "video") != 0);
    }
    gst_caps_unref(caps);
    if (not_video) return;
  }

  GstPad* sinkpad = gst_element_get_static_pad(data->head, "sink");
  if (!sinkpad) return;
  // Already linked (a second video media in the SDP): ignore the extra pad.
  if (!gst_pad_is_linked(sinkpad)) {
    gst_pad_link(pad, sinkpad);
  }
  gst_object_unref(sinkpad);
}

// decodebin fallback only: its raw-video pad -> appsink. decodebin can also
// expose audio pads, and linking one into the sink would yield caps with no
// width/height, so accept video/x-raw only. A decoded pad's queried caps are
// its real caps, so the query fallback is meaningful here.
void rtsp_probe_decode_pad_added(GstElement* /*decodebin*/, GstPad* pad,
                                 gpointer user_data) {
  auto* data = static_cast<RtspProbeData*>(user_data);
  if (!data || !data->sink) return;

  bool is_raw_video = false;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  if (caps) {
    if (GstStructure* s = gst_caps_get_structure(caps, 0)) {
      const gchar* name = gst_structure_get_name(s);
      is_raw_video = (name && g_str_has_prefix(name, "video/x-raw"));
    }
    gst_caps_unref(caps);
  }
  if (!is_raw_video) return;

  GstPad* sinkpad = gst_element_get_static_pad(data->sink, "sink");
  if (!sinkpad) return;
  if (!gst_pad_is_linked(sinkpad)) {
    gst_pad_link(pad, sinkpad);
  }
  gst_object_unref(sinkpad);
}

}  // namespace

bool get_rtsp_resolution_caps(const std::string& url, const std::string& codec,
                              int latency_ms, bool use_tcp,
                              guint64 transport_timeout_us, int timeout_sec,
                              int& width, int& height, std::string* reason) {
  auto fail = [&reason](const std::string& why) {
    if (reason) *reason = why;
    return false;
  };

  gst_init(nullptr, nullptr);

  GstElement* pipeline = gst_pipeline_new("rtsp_probe");
  GstElement* src = gst_element_factory_make("rtspsrc", "probe_src");
  GstElement* sink = gst_element_factory_make("appsink", "probe_sink");
  GstElement* head = nullptr;   // depay (no-decode path) or decodebin
  GstElement* parse = nullptr;  // parser (no-decode path only)

  // No-decode path for the codecs the SDP detection recognises.
  const char* depay_name = nullptr;
  const char* parse_name = nullptr;
  if (codec == "h264") {
    depay_name = "rtph264depay";
    parse_name = "h264parse";
  } else if (codec == "h265") {
    depay_name = "rtph265depay";
    parse_name = "h265parse";
  }
  if (depay_name) {
    head = gst_element_factory_make(depay_name, "probe_depay");
    parse = gst_element_factory_make(parse_name, "probe_parse");
    if (!head || !parse) {  // plugin missing: fall back to decodebin
      if (head) gst_object_unref(head);
      if (parse) gst_object_unref(parse);
      head = nullptr;
      parse = nullptr;
    }
  }
  const bool no_decode = (head != nullptr);
  if (!no_decode) {
    head = gst_element_factory_make("decodebin", "probe_dec");
  }

  if (!pipeline || !src || !head || !sink) {
    // Nothing has been added to the bin yet, so each element still owns its
    // floating reference and must be released individually.
    if (src) gst_object_unref(src);
    if (head) gst_object_unref(head);
    if (parse) gst_object_unref(parse);
    if (sink) gst_object_unref(sink);
    if (pipeline) gst_object_unref(pipeline);
    return fail("could not create probe elements");
  }

  // Properties are set through the GObject API, so the URL is never
  // interpolated into a pipeline description and cannot alter the graph.
  g_object_set(src,
               "location", url.c_str(),
               "latency", static_cast<guint>(latency_ms),
               "tcp-timeout", transport_timeout_us,
               "timeout", transport_timeout_us,
               "do-retransmission", FALSE,
               "retry", static_cast<guint>(0),
               NULL);
  // Same nick strings the real pipeline description uses (protocols=tcp|udp),
  // so the probe's transport cannot drift from RTSPConfig::build_gstreamer_source
  // by a hand-copied numeric flag (UDP=0x1, UDP_MCAST=0x2, TCP=0x4).
  gst_util_set_object_arg(G_OBJECT(src), "protocols", use_tcp ? "tcp" : "udp");
  // Caps are read from the sample, so no buffers need to be retained and the
  // probe must never block the source.
  g_object_set(sink, "sync", FALSE, "drop", TRUE,
               "max-buffers", static_cast<guint>(1), NULL);

  RtspProbeData data{head, sink};

  if (no_decode) {
    gst_bin_add_many(GST_BIN(pipeline), src, head, parse, sink, NULL);
    if (!gst_element_link_many(head, parse, sink, NULL)) {
      gst_object_unref(pipeline);
      return fail("could not link probe elements");
    }
  } else {
    gst_bin_add_many(GST_BIN(pipeline), src, head, sink, NULL);
    g_signal_connect(head, "pad-added",
                     G_CALLBACK(rtsp_probe_decode_pad_added), &data);
  }
  g_signal_connect(src, "pad-added",
                   G_CALLBACK(rtsp_probe_src_pad_added), &data);

  bool found = false;
  std::string why = "timeout after " + std::to_string(timeout_sec) + "s";
  GstBus* bus = gst_element_get_bus(pipeline);

  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    why = "pipeline refused to start";
  } else {
    // Poll in short slices so a connect/auth/404 failure posted on the bus
    // ends the probe immediately instead of burning the whole timeout.
    // try-pull-sample is an appsink action signal, so no link against
    // gstreamer-app-1.0 is needed.
    const gint64 deadline =
        g_get_monotonic_time() +
        static_cast<gint64>(timeout_sec) * G_USEC_PER_SEC;
    constexpr guint64 kSliceNs = 100 * 1000 * 1000;  // 100 ms

    while (!found && g_get_monotonic_time() < deadline) {
      GstSample* sample = nullptr;
      g_signal_emit_by_name(sink, "try-pull-sample", kSliceNs, &sample);
      if (sample) {
        // A parser can emit a first sample before the SPS has been seen, so
        // caps without width/height are not an answer yet; keep pulling.
        if (GstCaps* caps = gst_sample_get_caps(sample)) {
          if (GstStructure* s = gst_caps_get_structure(caps, 0)) {
            int w = 0;
            int h = 0;
            if (gst_structure_get_int(s, "width", &w) &&
                gst_structure_get_int(s, "height", &h) && w > 0 && h > 0) {
              width = w;
              height = h;
              found = true;
            }
          }
        }
        gst_sample_unref(sample);
        continue;
      }

      if (GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        GError* err = nullptr;
        gst_message_parse_error(msg, &err, nullptr);
        why = err ? err->message : "unknown pipeline error";
        if (err) g_error_free(err);
        gst_message_unref(msg);
        break;
      }

      gboolean eos = FALSE;
      g_object_get(sink, "eos", &eos, NULL);
      if (eos) {
        why = "stream ended before a frame size was known";
        break;
      }
    }
  }

  if (bus) gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);

  if (!found) return fail(why);
  return true;
}

//==============================================================================
// Model-aspect fitting
//==============================================================================
// Scale a source frame size to fit inside the model input while preserving
// aspect ratio (letterbox-free: the model sees the whole frame, the decoder's
// orig-width/orig-height match these dimensions exactly).
//
// The scale is NOT clamped to 1.0: a source smaller than the model is scaled
// UP to fill it, exactly as the pre-existing video-file path always did, so
// file, RTSP, V4L2 and discovered-video sources all behave the same.
//
// Integer arithmetic on purpose. The previous float gain was off by 2 on the
// limiting dimension for roughly 8% of source sizes (e.g. 160x266 into a
// 512x320 model gave 192x318 instead of 192x320, because 266*(320/266.0f)
// lands just below 320 and truncates). Exact floor division cannot do that.
//
// Results are rounded DOWN to even: the g2d converter and the subsampled YUV
// formats downstream need even width/height, and an odd value shows up as a
// negotiation failure rather than a clean error.
void fit_dimensions_to_model(int src_w, int src_h, int model_w, int model_h,
                             int& out_w, int& out_h) {
  if (src_w <= 0 || src_h <= 0 || model_w <= 0 || model_h <= 0) {
    out_w = src_w;
    out_h = src_h;
    return;
  }

  // Compare model_w/src_w with model_h/src_h without dividing.
  const long long width_limited = static_cast<long long>(model_w) * src_h;
  const long long height_limited = static_cast<long long>(model_h) * src_w;
  long long w;
  long long h;
  if (width_limited <= height_limited) {
    w = model_w;
    h = static_cast<long long>(src_h) * model_w / src_w;
  } else {
    h = model_h;
    w = static_cast<long long>(src_w) * model_h / src_h;
  }

  out_w = static_cast<int>(w) & ~1;
  out_h = static_cast<int>(h) & ~1;

  // Never collapse a stream to nothing if the model input is tiny.
  if (out_w < 2) out_w = 2;
  if (out_h < 2) out_h = 2;
}

void Demo::set_stream_resolution(int stream_id, int src_w, int src_h) {
  original_widths_[stream_id]  = src_w;
  original_heights_[stream_id] = src_h;

  int scaled_w = 0;
  int scaled_h = 0;
  fit_dimensions_to_model(src_w, src_h, model_width_, model_height_,
                          scaled_w, scaled_h);
  video_widths_[stream_id]  = scaled_w;
  video_heights_[stream_id] = scaled_h;
}

void Demo::probe_rtsp_resolutions(int number_of_streams) {
  rtsp_probe_results_.clear();
  if (!stream_config_) return;

  // Settings are copied out before the threads start so the workers touch no
  // Demo state. The codec comes from RTSPConfig::get_codec(), a blocking SDP
  // DESCRIBE cached with std::call_once (safe from several threads). It is
  // called INSIDE the worker so those waits overlap too, and it is the same
  // cached value the summary table prints, so no extra session is opened.
  struct Job {
    int stream_id;
    const RTSPConfig* cfg;
    std::string url;
    int latency_ms;
    bool use_tcp;
    guint64 transport_timeout_us;
  };
  std::vector<Job> jobs;
  const int configured = static_cast<int>(stream_config_->get_stream_count());
  for (int i = 0; i < number_of_streams && i < configured; ++i) {
    auto rtsp = dynamic_cast<const RTSPConfig*>(stream_config_->get_stream(i));
    if (!rtsp) continue;
    jobs.push_back({i, rtsp, rtsp->get_url(), rtsp->get_latency_ms(),
                    rtsp->uses_tcp(), rtsp->get_transport_timeout_us()});
  }
  if (jobs.empty()) return;

  const int timeout_sec = rtsp_probe_timeout_sec();
  std::vector<std::future<RtspProbeResult>> futures;
  futures.reserve(jobs.size());
  for (const Job& job : jobs) {
    futures.push_back(std::async(std::launch::async, [job, timeout_sec]() {
      RtspProbeResult r;
      r.ok = get_rtsp_resolution_caps(job.url, job.cfg->get_codec(),
                                      job.latency_ms, job.use_tcp,
                                      job.transport_timeout_us, timeout_sec,
                                      r.width, r.height, &r.reason);
      return r;
    }));
  }
  for (size_t k = 0; k < jobs.size(); ++k) {
    rtsp_probe_results_[jobs[k].stream_id] = futures[k].get();
  }
}

//==============================================================================
// Source Pipeline Building
//==============================================================================
// NOTE: Each source pipeline terminates with "! tee name=tee_N" which is then
// referenced by build_compositor() as "tee_N. ! queue ...".  Earlier versions
// of this file also emitted a dangling "tee_N. ! queue" line here inside
// build_source_pipeline(); that was removed because the tee fan-out branches
// (inference + display) are built entirely in build_compositor(), and a second
// unreferenced branch would have caused a GStreamer pipeline parse error.
//==============================================================================

std::string Demo::build_source_pipeline(int stream_id) {
  std::string pipeline;
  video_widths_[stream_id]  = 640;
  video_heights_[stream_id] = 360;
  int src_w = 0;
  int src_h = 0;
  const char* dewarp_env = std::getenv("DEWARP");
  bool has_dewarp = dewarp_env && dewarp_env[0] != '\0';

  // Check if we have a configured source for this stream
  if (stream_id < static_cast<int>(stream_config_->get_stream_count())) {
    const StreamSourceConfig* config = stream_config_->get_stream(stream_id);
    if (config) {
  	bool is_libcamera =
                 dynamic_cast<const CameraLibcameraConfig*>(config) != nullptr;
	if (auto video_cfg =
                    dynamic_cast<const VideoFileConfig*>(config)) {

                if (!get_video_resolution_caps(
                        video_cfg->get_filepath(), src_w, src_h)) {
			std::cerr << "Warning: Failed to detect video resolution, using fallback 1920x1080\n";
			src_w = 1920;
			src_h = 1080;
                }
                set_stream_resolution(stream_id, src_w, src_h);
                const int scaled_w = video_widths_[stream_id];
                const int scaled_h = video_heights_[stream_id];

                pipeline += video_cfg->build_gstreamer_source(stream_id);

                pipeline += "! imxvideoconvert_g2d "
                            "! video/x-raw,format=BGRx,width=" +
                            std::to_string(scaled_w) +
                            ",height=" + std::to_string(scaled_h) + " ";

	        pipeline += "! tee name=tee_" + std::to_string(stream_id) + " ";
                return pipeline;
            }
            
            pipeline += config->build_gstreamer_source(stream_id);

            // RTSP: the real frame size was probed in parallel up front
            // (probe_rtsp_resolutions); fit it to the model like a video
            // file. A failed probe leaves the 640x360 caps fallback in place
            // and warns; the summary then shows "unknown" rather than a
            // fabricated number, and startup is never worse than before.
            if (dynamic_cast<const RTSPConfig*>(config)) {
              auto it = rtsp_probe_results_.find(stream_id);
              if (it != rtsp_probe_results_.end() && it->second.ok) {
                set_stream_resolution(stream_id, it->second.width,
                                      it->second.height);
              } else {
                std::cerr << "Warning: stream " << stream_id
                          << ": could not probe RTSP resolution ("
                          << (it != rtsp_probe_results_.end()
                                  ? it->second.reason : std::string("not probed"))
                          << "); using fallback caps "
                          << video_widths_[stream_id] << "x"
                          << video_heights_[stream_id]
                          << " and reporting the initial resolution as"
                             " unknown\n";
              }
            } else if (auto v4l2_cfg =
                           dynamic_cast<const CameraV4L2Config*>(config)) {
              // A V4L2 camera needs no probe: build_gstreamer_source() pins
              // these dimensions in the source caps, so they ARE the initial
              // resolution.
              set_stream_resolution(stream_id, v4l2_cfg->get_width(),
                                    v4l2_cfg->get_height());
            }

            if (is_libcamera && has_dewarp) {
              pipeline += "! video/x-raw,format=BGRx,width=" +
                          std::to_string(video_widths_[stream_id]) +
                          ",height=" +
                          std::to_string(video_heights_[stream_id]) + " ";
            } else {
              pipeline += "! imxvideoconvert_g2d "
                          "! video/x-raw,format=BGRx,width=" +
                          std::to_string(video_widths_[stream_id]) +
                          ",height=" +
                          std::to_string(video_heights_[stream_id]) + " ";
            }

	    pipeline += "! tee name=tee_" + std::to_string(stream_id) + " ";

            // RTSP-only clip recording: add a standalone recorder branch that
            // pulls the dedicated high-resolution record URL with its own
            // rtspsrc and writes bounded MP4 segments (H.265 passthrough).
            if (auto rtsp_cfg = dynamic_cast<const RTSPConfig*>(config)) {
              if (rtsp_cfg->is_recording_enabled()) {
                const std::string& rec_dir = rtsp_cfg->get_record_dir();
                // Ensure the recording directory exists before the pipeline
                // starts writing segments; disable recording if it cannot be
                // created. The default record_dir is nested, so create parent
                // directories as needed (mkdir -p semantics).
                if (!make_dirs_recursive(rec_dir, 0755)) {
                  g_printerr("Stream %d: cannot create recording dir %s "
                             "(%s); recording disabled\n",
                             stream_id, rec_dir.c_str(), strerror(errno));
                } else if (access(rec_dir.c_str(), W_OK) != 0) {
                  // The directory exists (or was just created) but is not
                  // writable by this process. Surface a clean early warning
                  // instead of letting splitmuxsink fail later at runtime.
                  g_printerr("Stream %d: recording dir %s is not writable "
                             "(%s); recording disabled\n",
                             stream_id, rec_dir.c_str(), strerror(errno));
                } else {
                  std::string rec_branch =
                      rtsp_cfg->build_recording_branch(stream_id);
                  if (!rec_branch.empty()) {
                    pipeline += rec_branch;
                    g_print("Stream %d: RTSP recording enabled -> %s\n",
                            stream_id, rec_dir.c_str());
                  }
                }
              }
            }

            return pipeline;
        }
    }

  // Fallback: dynamically discover videos in sample directory
  auto video_files = discover_video_files(SAMPLE_VIDEOS_DIR);

  std::string video_path;
  if (!video_files.empty()) {
    // Cycle through available videos if stream_id exceeds count
    int video_idx = stream_id % static_cast<int>(video_files.size());
    video_path = video_files[video_idx];
    g_print("Stream %d fallback: using discovered video %s\n",
            stream_id, video_path.c_str());
  } else {
    // Last resort: try legacy naming convention
    video_path = SAMPLE_VIDEOS_DIR + "/video_" +
                 std::to_string(stream_id) + ".ts";
    g_printerr("Warning: No videos discovered, trying legacy path: %s\n",
               video_path.c_str());
  }

  if (!get_video_resolution_caps(video_path, src_w, src_h)) {
           std::cerr << "Warning: Failed to detect input video resolution \n";
  }
  // Same fit as the configured video-file path, so an auto-discovered video
  // gets the same caps as that file passed with -v. A failed probe leaves the
  // 640x360 caps fallback and an "unknown" initial resolution.
  if (src_w > 0 && src_h > 0) {
    set_stream_resolution(stream_id, src_w, src_h);
  }
  pipeline += "multifilesrc location=" + video_path +
              " loop=true name=src_" + std::to_string(stream_id) + " ";
  pipeline += "! tsdemux name=demux_" + std::to_string(stream_id) + " ";
  pipeline += "demux_" + std::to_string(stream_id) + ". ! queue ! h264parse ! v4l2h264dec ";
  pipeline += "! imxvideoconvert_g2d ! video/x-raw,format=BGRx,width=" +
                 std::to_string(video_widths_[stream_id]) +
                 ",height=" + std::to_string(video_heights_[stream_id]) + " ";
  pipeline += "! tee name=tee_" + std::to_string(stream_id) + " ";
  return pipeline;
}
//==============================================================================
// Main Function
//==============================================================================
void print_usage(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
            << "Options:\n"
            << "\nGeneral Options:\n"
            << "  -s, --stream <1-16>         Number of streams to process\n"
            << "                              (default: auto-detected from available videos)\n"
            << "                              Note: Ignored when using -f/--config\n"
            << "  -e, --endpoint <0-10>       ARA2 Endpoint to use (default: 0)\n"
            << "  -g, --group <pcie|all>      Device group (default: all)\n"
            << "  -y, --sync <true|false>     Enable/disable Waylandsink synchronization\n"
            << "                              (default: false)\n"
            << "  -m, --model <model-name>    Select model. Currently supported models:\n"
            << "                              YOLOv8: yolov8n, yolov8s, yolov8m, yolov8l, yolov8x\n"
            << "                              YOLOx : yoloxt, yoloxs, yoloxs_512, yoloxm, yoloxl, yoloxx\n"
            << "                              YOLO11: yolo11n, yolo11s, yolo11m, yolo11l, yolo11x\n"
            << "                              YOLO26: yolo26n, yolo26s, yolo26m, yolo26l, yolo26x\n"
            << "  -t                          Console stats will be refreshed each t seconds, To disable set t to 0\n"
            << "  -p, --perf-stats            Enable detailed performance stats every 5 seconds\n"
            << "  --cpu-stats                 Show process CPU% (core units, per-stream) and ReID\n"
            << "                              gallery size in the console TOTAL block (implied by -p)\n"
            << "  -d, --debug                 To debug the pipeline \n"
            << "  -h, --help                  Show this help message\n"
            << "\nWindow Configuration:\n"
            << "  --width <pixels>            Window width (default: 1920)\n"
            << "  --height <pixels>           Window height (default: 1080)\n"
            << "\nStream Source Options:\n"
            << "  -c, --camera <device>       Camera device (e.g., /dev/video0)\n"
            << "  -r, --rtsp <url>            RTSP stream URL\n"
            << "  -v, --video <file>          Video file path\n"
            << "  -f, --config <file>         Load stream configuration from JSON file\n"
            << "                              (automatically sets stream count from file)\n"
            << "  --list-cameras              List available V4L2 cameras and exit\n"
            << "\nDisplay Options:\n"
            << "  --no-bbox                   Draw no bounding boxes and labels\n"
            << "  --no-osd-stats              No on screen stats display\n"
            << "  --only-bbox                 Draw only bounding boxes, no labels\n"
            << "  --headless                  Run without a display; track metrics only\n"
            << "\nObject Tracking Options:\n"
            << "  --track                     Enable object tracking (default: disabled)\n"
            << "  --track-class <name|id|all> Class to track (COCO class name or id, or 'all';\n"
            << "                              default: all). Implies --track if not set.\n"
            << "\nRe-Identification (ReID) Options:\n"
            << "  --reid-model <path>         OSNet .dvm model path. Enables ReID embedding\n"
            << "                              (implies --track --track-class person).\n"
            << "  --reid-interval-ms <ms>     Per-track (re)embed cadence (default: 200; 0 = every frame).\n"
            << "  --reid-min-crop <WxH>       Skip person boxes smaller than WxH px so tiny crops are\n"
            << "                              not upscaled (default: 128x256, the OSNet input size).\n"
            << "  --reid-proximity-iou <0..1> Skip ReID embedding when a person box overlaps another\n"
            << "                              by >= this IoU (avoids ID swaps during crossings;\n"
            << "                              default 0.30; set 1.0 to disable).\n"
            << "  --reid-merge-iomin <0..1>   On heavy overlap (IoMin >= this) DROP both tracks +\n"
            << "                              re-run ReID after they separate; box shows '?' until\n"
            << "                              re-identified. Prints an IoMin histogram at exit to\n"
            << "                              help tune this value (default 0.50; 1.0 disables).\n"
            << "  --reid-reverify-window <n>  Frames after an overlap during which identity is\n"
            << "                              re-verified from the gallery (corrects ID swaps on\n"
            << "                              track crossings; default 30; 0 disables).\n"
            << "  --reid-match-thresh <0..1>  Cosine to ACQUIRE a gallery identity (default: 0.823).\n"
            << "                              Lower it (e.g. 0.72) if re-entries get new IDs.\n"
            << "  --reid-sticky-thresh <0..1> Cosine to RETAIN a held identity (default: 0.55).\n"
            << "  --reid-commit-crops <n>     Good crops before committing an ID (default: 20).\n"
            << "  --reid-max-gallery <n>      Max ReID identities kept; evict least-recently-used\n"
            << "                              when full (default: 1000).\n"
            << "  --reid-gallery-k <n>        Appearance templates kept per person (default: 2).\n"
            << "                              Raise to 4-5 for better cross-camera matching.\n"
            << "  --save-reid-crops           Save person crop PNGs to /tmp/reid_crops/ for debug.\n"
            << "                              Disabled by default. WARNING: can fill /tmp quickly;\n"
            << "                              at most 10 crops are kept per track (rolling).\n"
            << "  --reid-debug                Verbose ReID decision logs (per-frame + per-track:\n"
            << "                              STICKY/ACQUIRE/ID-NEW/ID-XCAM/PROVISIONAL/quality).\n"
            << "\nWebSocket Detection Server:\n"
            << "  --ws-detections             Publish detections over WebSocket\n"
            << "  --ws-port <port>            WebSocket server port (default: 8765)\n"
            << "\nDefault Behavior:\n"
            << "  When no source flags (-c, -v, -r, -f) are used,\n"
            << "  the application auto-discovers .ts files in:\n"
            << "    " << SAMPLE_VIDEOS_DIR << "\n"
            << "  and sets the stream count to the number of videos found (max 8).\n";
}

// ---------------------------------------------------------------------------
// Wayland display probe
//
// Returns true if a live Wayland compositor is reachable.
//   1. Fast path: WAYLAND_DISPLAY env var must be set (Weston sets it).
//   2. Deep path: actually connect via wl_display_connect() loaded at runtime
//      through dlopen so we don't add a link-time dependency on libwayland-client.
// ---------------------------------------------------------------------------
static bool wayland_display_available() {
  const char* wd = getenv("WAYLAND_DISPLAY");
  if (!wd || wd[0] == '\0') {
    return false;  // env var not set → no compositor running
  }

  // Try to open libwayland-client and call wl_display_connect(NULL).
  void* lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!lib) {
    // libwayland-client not present on this build.
    return true;
  }

  using wl_display_connect_fn = void* (*)(const char*);
  using wl_display_disconnect_fn = void (*)(void*);

  auto wl_connect =
      reinterpret_cast<wl_display_connect_fn>(dlsym(lib, "wl_display_connect"));
  auto wl_disconnect =
      reinterpret_cast<wl_display_disconnect_fn>(dlsym(lib, "wl_display_disconnect"));

  bool ok = false;
  if (wl_connect  && wl_disconnect) {
    void* dpy = wl_connect(nullptr);
    if (dpy) {
      ok = true;
      wl_disconnect(dpy);
    }
  }
  dlclose(lib); // always reached — no leak
  return ok;
}

int main(int argc, char* argv[]) {
  int streams = 0;  // 0 means "auto-detect"
  int endpoint = 0;
  std::string group = "all";
  bool enable_perf_stats = false;
  bool cpu_stats = false;
  bool reid_stats = false;
  bool sync = false;
  std::string model = "/usr/share/cnn/detection/yolov8n/model.dvm";
  bool no_bbox = false;
  bool no_osd_stats = false;
  int console_interval = 2;
  bool only_bbox = false;
  bool debug_pipeline = false;
  bool headless = false;
  bool tracking_enabled = false;
  int tracking_class = -1;  // -1 = all classes
  bool bt_debug = false;     // verbose ByteTrack logging (--bt-debug)
  bool track_class_explicitly_set = false;

  std::string reid_model;
  int reid_interval_ms = 200;
  int reid_min_crop_w = -1;
  int reid_min_crop_h = -1;
  float reid_proximity_iou = -1.0f;
  float reid_merge_iomin = -1.0f;
  int reid_reverify_frames = -1;
  bool reid_save_crops = false;
  float reid_match_thresh = -1.0f;
  float reid_sticky_thresh = -1.0f;
  int reid_commit_crops = -1;
  int reid_max_gallery = -1;
  int reid_gallery_k = -1;
  bool reid_debug = false;

  bool ws_detections = false;
  uint16_t ws_port = 8765;

  int window_width = 1920;
  int window_height = 1080;

  std::string config_file;
  auto stream_config = std::make_unique<StreamConfigManager>();
  bool config_file_loaded = false;
  bool streams_explicitly_set = false;
  bool source_explicitly_set = false;  // Track if any source flag was used
  bool model_explicitly_set = false;    // Track if -m/--model was provided by the user

  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--width") {
      if (i + 1 < argc) {
        window_width = std::atoi(argv[++i]);
        if (window_width < 320 || window_width > 7680) {
          std::cerr << "Error: width must be between 320 and 7680\n";
          return 1;
        }
      }
    } else if (arg == "--height") {
      if (i + 1 < argc) {
        window_height = std::atoi(argv[++i]);
        if (window_height < 240 || window_height > 4320) {
          std::cerr << "Error: height must be between 240 and 4320\n";
          return 1;
        }
       }
      } else if (arg == "--list-cameras") {
      gst_init(&argc, &argv);
      Demo::list_cameras();
      gst_deinit();
      return 0;
    } else if (arg == "-f" || arg == "--config") {
      if (i + 1 < argc) {
        config_file = argv[++i];
        source_explicitly_set = true;
      }
    } else if (arg == "-d" || arg == "--debug") {
           debug_pipeline = true;
    } else if (arg == "-c" || arg == "--camera") {
      if (i + 1 < argc) {
        stream_config->add_stream(
          std::make_unique<CameraV4L2Config>(argv[++i], 640, 360, 30));
        source_explicitly_set = true;
      }
    } else if (arg == "-r" || arg == "--rtsp") {
      if (i + 1 < argc) {
        bool use_tcp = true;
        stream_config->add_stream(
          std::make_unique<RTSPConfig>(argv[++i], 100, use_tcp));
        source_explicitly_set = true;
      }
    } else if (arg == "-v" || arg == "--video") {
      if (i + 1 < argc) {
        stream_config->add_stream(
          std::make_unique<VideoFileConfig>(argv[++i]));
        source_explicitly_set = true;
      }
    } else if (arg == "--no-bbox") {
      no_bbox = true;
    } else if (arg == "--no-osd-stats") {
      no_osd_stats = true;
    } else if (arg == "--only-bbox") {
      only_bbox = true;
    } else if (arg == "--headless") {
      headless = true;
    } else if (arg == "--track") {
      tracking_enabled = true;
    } else if (arg == "--bt-debug") {
      bt_debug = true;
    } else if (arg == "--reid-stats") {
      reid_stats = true;
    } else if (arg == "--reid-model") {
      if (i + 1 < argc) reid_model = argv[++i];
    } else if (arg == "--reid-interval-ms") {
      if (i + 1 < argc) reid_interval_ms = std::atoi(argv[++i]);
    } else if (arg == "--reid-min-crop") {
      if (i + 1 < argc) {
        std::string v = argv[++i];
        size_t xpos = v.find_first_of("xX");
        if (xpos != std::string::npos) {
          reid_min_crop_w = std::atoi(v.substr(0, xpos).c_str());
          reid_min_crop_h = std::atoi(v.substr(xpos + 1).c_str());
        } else {
          reid_min_crop_h = std::atoi(v.c_str());
          reid_min_crop_w = reid_min_crop_h / 2;
        }
      }
    } else if (arg == "--reid-proximity-iou") {
      if (i + 1 < argc) reid_proximity_iou = std::atof(argv[++i]);
    } else if (arg == "--reid-merge-iomin") {
      if (i + 1 < argc) reid_merge_iomin = std::atof(argv[++i]);
    } else if (arg == "--reid-reverify-window") {
      if (i + 1 < argc) reid_reverify_frames = std::atoi(argv[++i]);
    } else if (arg == "--reid-match-thresh") {
      if (i + 1 < argc) reid_match_thresh = std::atof(argv[++i]);
    } else if (arg == "--reid-sticky-thresh") {
      if (i + 1 < argc) reid_sticky_thresh = std::atof(argv[++i]);
    } else if (arg == "--reid-commit-crops") {
      if (i + 1 < argc) reid_commit_crops = std::atoi(argv[++i]);
    } else if (arg == "--reid-max-gallery") {
      if (i + 1 < argc) reid_max_gallery = std::atoi(argv[++i]);
    } else if (arg == "--reid-gallery-k") {
      if (i + 1 < argc) reid_gallery_k = std::atoi(argv[++i]);
    } else if (arg == "--reid-debug") {
      reid_debug = true;
    } else if (arg == "--save-reid-crops") {
      reid_save_crops = true;
    } else if (arg == "--track-class") {
      if (i + 1 < argc) {
        std::string val = argv[++i];
        track_class_explicitly_set = true;
        if (val == "all") {
          tracking_class = -1;  // track every class
        } else {
          // Accept a numeric COCO class id or a class name.
          bool is_numeric =
              !val.empty() &&
              val.find_first_not_of("0123456789") == std::string::npos;
          if (is_numeric) {
            int id = std::atoi(val.c_str());
            if (id < 0 || id >= static_cast<int>(COCO_CLASSES.size())) {
              std::cerr << "Error: --track-class id must be between 0 and "
                        << (COCO_CLASSES.size() - 1) << "\n";
              return 1;
            }
            tracking_class = id;
          } else {
            auto it = std::find(COCO_CLASSES.begin(), COCO_CLASSES.end(), val);
            if (it == COCO_CLASSES.end()) {
              std::cerr << "Error: unknown --track-class '" << val
                        << "'\n";
              return 1;
            }
            tracking_class =
                static_cast<int>(std::distance(COCO_CLASSES.begin(), it));
          }
        }
      } else {
        std::cerr << "Error: --track-class requires a class name, id, or 'all'\n";
        return 1;
      }
    } else if (arg == "-s" || arg == "--stream") {
      if (i + 1 < argc) {
        streams = std::atoi(argv[++i]);
        streams_explicitly_set = true;
        if (streams < 1 || streams > 16) {
          std::cerr << "Error: stream count must be between 1 and 16\n";
          return 1;
        }
      }
    } else if (arg == "-e" || arg == "--endpoint") {
      if (i + 1 < argc) {
        endpoint = std::atoi(argv[++i]);
        if (endpoint < 0 || endpoint > 10) {
          std::cerr << "Error: endpoint must be between 0 and 10\n";
          return 1;
        }
      }
    } else if (arg == "-g" || arg == "--group") {
      if (i + 1 < argc) {
        std::string group_val = argv[++i];
        if (group_val == "pcie" || group_val == "all") {
          group = group_val;
        } else {
          std::cerr << "Error: group must be 'pcie' or 'all'\n";
          return 1;
        }
      }
    } else if (arg == "-y" || arg == "--sync") {
      if (i + 1 < argc) {
        std::string sync_val = argv[++i];
        if (sync_val == "true" || sync_val == "1") {
          sync = true;
        } else if (sync_val == "false" || sync_val == "0") {
          sync = false;
        } else {
          std::cerr << "Error: sync must be 'true' or 'false'\n";
          return 1;
        }
      }
    } else if (arg == "-m" || arg == "--model") {
      if (i + 1 < argc) {
        model_explicitly_set = true;
        std::string val = argv[++i];
        if (val == "yolov8n")
          model = "/usr/share/cnn/detection/yolov8n/model.dvm";
        else if (val == "yolov8s")
          model = "/usr/share/cnn/detection/yolov8s/model.dvm";
        else if (val == "yolov8m")
          model = "/usr/share/cnn/detection/yolov8m/model.dvm";
        else if (val == "yolov8l")
          model = "/usr/share/cnn/detection/yolov8l/model.dvm";
        else if (val == "yolov8x")
          model = "/usr/share/cnn/detection/yolov8x/model.dvm";
        else if (val == "yoloxt")
          model = "/usr/share/cnn/detection/yoloxt/model.dvm";
        else if (val == "yoloxs")
          model = "/usr/share/cnn/detection/yoloxs/model.dvm";
        else if (val == "yoloxs_512")
          model = "/usr/share/cnn/detection/yoloxs_512/model.dvm";
        else if (val == "yoloxm")
          model = "/usr/share/cnn/detection/yoloxm/model.dvm";
        else if (val == "yoloxl")
          model = "/usr/share/cnn/detection/yoloxl/model.dvm";
        else if (val == "yoloxx")
          model = "/usr/share/cnn/detection/yoloxx/model.dvm";
        else if (val == "yolo11n")
          model = "/usr/share/cnn/detection/yolo11n/model.dvm";
        else if (val == "yolo11s")
          model = "/usr/share/cnn/detection/yolo11s/model.dvm";
        else if (val == "yolo11m")
          model = "/usr/share/cnn/detection/yolo11m/model.dvm";
        else if (val == "yolo11l")
          model = "/usr/share/cnn/detection/yolo11l/model.dvm";
        else if (val == "yolo11x")
          model = "/usr/share/cnn/detection/yolo11x/model.dvm";
        // YOLO26 variants (similar architecture to YOLOv8: n,s,m,l,x)
        else if (val == "yolo26n")
          model = "/usr/share/cnn/detection/yolo26n/model.dvm";
        else if (val == "yolo26s")
          model = "/usr/share/cnn/detection/yolo26s/model.dvm";
        else if (val == "yolo26m")
          model = "/usr/share/cnn/detection/yolo26m/model.dvm";
        else if (val == "yolo26l")
          model = "/usr/share/cnn/detection/yolo26l/model.dvm";
        else if (val == "yolo26x")
          model = "/usr/share/cnn/detection/yolo26x/model.dvm";
        // Wildcard / passthrough: if the value looks like an absolute path
        // or a directory name under /usr/share/cnn/detection/ that is not
        // in the known list, treat it as a direct model path so future
        // model families do not require a code change.
        else if (!val.empty() && val[0] == '/') {
          // Absolute path supplied directly
          model = val;
        } else if (val.find('/') != std::string::npos || val.find("..") != std::string::npos) {
          std::cerr << "Error: model name must not contain '/' or '..': '" << val << "'\n";
          return 1;
        } else {
          // Try the standard installation directory as a wildcard fallback
          std::string candidate = "/usr/share/cnn/detection/" + val + "/model.dvm";
          if (file_exists(candidate)) {
            model = candidate;
            std::cout << "Resolved model: " << model << "\n";
          } else {
            std::cerr << "Error: unknown model '" << val
                      << "'. Pass an absolute path or a directory name under\n"
                      << "  /usr/share/cnn/detection/<name>/model.dvm\n";
            return 1;
          }
        }
      }
    } else if (arg == "-t") {
      if (i + 1 < argc) {
        console_interval = std::atoi(argv[++i]);
      } else {
        std::cerr << "Error: -t requires a time value in seconds\n";
        return 1;
      }
    } else if (arg == "-p" || arg == "--perf-stats") {
      enable_perf_stats = true;
      cpu_stats = true;  // perf mode also shows the CPU/ReID readout
    } else if (arg == "--cpu-stats") {
      cpu_stats = true;
    } else if (arg == "--ws-detections") {
      ws_detections = true;
    } else if (arg == "--ws-port") {
      if (i + 1 < argc) {
        int p = std::atoi(argv[++i]);
        if (p < 1 || p > 65535) {
          std::cerr << "Error: --ws-port must be between 1 and 65535\n";
          return 1;
        }
        ws_port = static_cast<uint16_t>(p);
      } else {
        std::cerr << "Error: --ws-port requires a port number\n";
        return 1;
      }
    }
  }

  // Load config file if specified
  if (!config_file.empty()) {
    if (!stream_config->load_from_json(config_file)) {
      std::cerr << "Failed to load configuration file: " << config_file << std::endl;
      return 1;
    }
    config_file_loaded = true;
    streams = static_cast<int>(stream_config->get_stream_count());

    if (streams < 1 || streams > 16) {
      std::cerr << "Error: JSON file must define between 1 and 16 streams (found "
                << streams << ")\n";
      return 1;
    }
    if (streams_explicitly_set) {
      std::cout << "Note: -s parameter ignored when using -f (config file takes priority)\n";
    }

    std::cout << "Loaded " << streams << " stream(s) from configuration file: "
              << config_file << std::endl;
  }
  // Auto-discover video files when no source is explicitly configured
  else if (!source_explicitly_set) {
    auto video_files = discover_video_files(SAMPLE_VIDEOS_DIR);

    if (!video_files.empty()) {
      int available_videos = static_cast<int>(video_files.size());

      if (!streams_explicitly_set) {
        // Auto-set stream count to match number of available videos
        streams = available_videos;
      }
      // else: -s was set, use that value; cycle through videos if needed

      // Add a VideoFileConfig for each stream
      for (int i = 0; i < streams; ++i) {
        int video_idx = i % available_videos;
        stream_config->add_stream(
            std::make_unique<VideoFileConfig>(video_files[video_idx]));
      }

      std::cout << "\n========================================\n"
                << "  Auto-discovered Video Files\n"
                << "========================================\n";
      for (size_t i = 0; i < video_files.size(); ++i) {
        std::cout << "  [" << i << "] " << video_files[i] << "\n";
      }
      std::cout << "Using " << streams << " stream(s) from "
                << available_videos << " available video(s)\n";
      if (streams > available_videos) {
        std::cout << "Note: Videos will be reused (cycled) across streams\n";
      }
      std::cout << "========================================\n" << std::endl;
    } else {
      std::cerr << "\n========================================\n"
                << "  ERROR: No Video Files Found\n"
                << "========================================\n"
                << "No .ts files found in:\n"
                << "  " << SAMPLE_VIDEOS_DIR << "\n\n"
                << "Please either:\n"
                << "  1. Place .ts files in the directory above\n"
                << "  2. Specify a video file:  -v /path/to/video.ts\n"
                << "  3. Use a camera:          -c /dev/video0\n"
                << "  4. Use a config file:     -f config.json\n"
                << "========================================\n" << std::endl;
      return 1;
    }
  }
  // Source explicitly set via CLI flags (-c, -v, -r)
  // but no config file: use stream_config count or -s value
  else {
    if (!streams_explicitly_set) {
      int configured = static_cast<int>(stream_config->get_stream_count());
      streams = (configured > 0) ? configured : 1;
    }
    // If -s is set higher than configured sources, remaining streams
    // will use the fallback in build_source_pipeline()
  }

  // Passing --track-class implies --track so users can just select a class
  // without also passing --track.
  if (track_class_explicitly_set && !tracking_enabled) {
    tracking_enabled = true;
  }

  // ReID needs stable track IDs to key crops/embeddings, so enabling ReID
  // implies tracking persons. Default the tracking class to person (0) if the
  // user did not pick one explicitly.
  if (!reid_model.empty()) {
    tracking_enabled = true;
    if (!track_class_explicitly_set) tracking_class = 0;  // person
  }

  // Final validation of stream count
  if (streams < 1) {
    streams = 1;
  } else if (streams > 16) {
    streams = 16;
  }


  // AUTO-SELECT MODEL when the user did not specify -m/--model.
  // Mirror the logic used by launcher_multistream_yolo (GUI): scan
  // /usr/share/cnn/detection/ for any installed model and pick the first
  // available one in priority order (yolov8 → yolox → yolo11).
  // This fixes MICRSE-6286: the CLI was always defaulting to yolov8n even
  // when only yolox or yolo11 models were present, causing a "Model Not
  // Found" error.  When -m is given explicitly the user-supplied path is
  // used unchanged (existing behaviour preserved).
  if (!model_explicitly_set) {
    static const char* AUTO_MODEL_CANDIDATES[] = {
        "yolov8n", "yolov8s", "yolov8m", "yolov8l", "yolov8x",
        "yoloxt",  "yoloxs",  "yoloxm",  "yoloxl",  "yoloxx",
        "yolo11n", "yolo11s", "yolo11m", "yolo11l", "yolo11x",
        "yolo26n", "yolo26s", "yolo26m", "yolo26l", "yolo26x",
        nullptr
    };
    std::string found_model;
    for (int mi = 0; AUTO_MODEL_CANDIDATES[mi] != nullptr; ++mi) {
      std::string candidate = std::string("/usr/share/cnn/detection/") +
                              AUTO_MODEL_CANDIDATES[mi] + "/model.dvm";
      if (file_exists(candidate)) {
        found_model = candidate;
        break;
      }
    }
    if (found_model.empty()) {
      // No model found at all — print the same helpful error as before.
      validate_model_exists(model);  // model still holds the yolov8n default path
      return 1;
    }
    model = found_model;
    std::cout << "Auto-selected model: " << model << "\n";
  }

  // VALIDATE MODEL EXISTS BEFORE PROCEEDING
  if (!validate_model_exists(model)) {
    return 1;
  }

  // Auto-headless: if the user did not explicitly pass --headless but no
  // Wayland compositor is reachable, fall back to headless mode automatically.
  const bool wayland_ok = wayland_display_available();
  if (!headless && !wayland_ok) {
    std::cerr << "[WARN] No Wayland display detected (WAYLAND_DISPLAY not set "
                 "or compositor unreachable). Switching to --headless mode "
                 "automatically.\n";
    headless = true;
  }

  // Initialize GStreamer
  gst_init(&argc, &argv);

  // Create and run demo
  {
            Demo demo(streams, endpoint, group, enable_perf_stats, sync, model, console_interval);
            demo.set_no_bbox(no_bbox);
            demo.set_no_osd_stats(no_osd_stats);
            demo.set_only_bbox(only_bbox);
            demo.set_headless(headless);
            demo.set_ws_detections(ws_detections);
            demo.set_ws_port(ws_port);
            demo.set_window_size(window_width, window_height);
            demo.set_debug(debug_pipeline);
            demo.set_bt_debug(bt_debug);
            demo.set_reid_stats(reid_stats);
            demo.set_tracking(tracking_enabled, tracking_class);
            demo.set_reid(reid_model, reid_interval_ms);
            demo.set_reid_min_crop(reid_min_crop_w, reid_min_crop_h);
            demo.set_reid_proximity_iou(reid_proximity_iou);
            demo.set_reid_merge_iomin(reid_merge_iomin);
            demo.set_reid_reverify_window(reid_reverify_frames);
            demo.set_reid_thresholds(reid_match_thresh, reid_sticky_thresh,
                                     reid_commit_crops);
            demo.set_reid_max_gallery(reid_max_gallery);
            demo.set_reid_gallery_k(reid_gallery_k);
            demo.set_reid_debug(reid_debug);
            demo.set_reid_save_crops(reid_save_crops);
            demo.set_cpu_stats(cpu_stats);
            std::cout << "\n========================================\n"
                      << "  Ara YOLO Multi-Stream Demo\n"
                      << "========================================\n"
                      << "Streams:    " << streams << "\n"
                      << "Endpoint:   " << endpoint << "\n"
                      << "Group:      " << group << "\n"
                      << "Sync:       " << (sync ? "true" : "false") << "\n"
                      << "Model Used: " << model << "\n"
                      << "Window:     " << window_width << "x" << window_height << "\n"
                      << "Headless:   " << (headless ? "true (no display)" : "false") << "\n"
                      << "Tracking:   " << (tracking_enabled ? "enabled" : "disabled");
            if (tracking_enabled) {
              std::cout << " (class: "
                        << (tracking_class < 0 ? "all"
                                               : get_class_name(tracking_class))
                        << ")";
            }
            std::cout << "\n";
            std::cout << "WS Detections: " << (ws_detections ? "enabled (port " + std::to_string(ws_port) + ")" : "disabled") << "\n";

            if (!reid_model.empty()) {
              std::cout << "ReID:       enabled (OSNet " << reid_model
                        << ", interval " << reid_interval_ms << "ms)\n";
            }

            if (enable_perf_stats) {
              std::cout << "Perf Stats: ENABLED (stream 0 only)\n";
            }

            if (cpu_stats) {
              std::cout << "CPU Stats:  ENABLED (process CPU% + ReID gallery in "
                           "TOTAL block)\n";
            }

            // Print stream configuration BEFORE moving ownership to demo
            if (config_file_loaded) {
              stream_config->print_summary();
            }

            // Move stream_config into demo AFTER we're done using it
            demo.set_stream_config(std::move(stream_config));

            demo.start();
  }

  // Cleanup
  gst_deinit();

  std::cout << "\nDemo completed successfully\n" << std::endl;

  return 0;
}
