/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: reid_crop_store.h
 *
 * ReID crop store: crops confirmed person boxes out of an RGB video frame,
 * resizes each to a fixed ReID input size (128x256, OSNet convention) using
 * OpenCV (bilinear), computes a Laplacian-variance sharpness score, optionally
 * runs the OSNet embedder to produce a 512-d appearance embedding, and (for
 * debug) writes the crop to disk as a .png.
 *
 * A per-track cadence gate (minIntervalPerTrackMs) rate-limits how often each
 * track is (re)processed, mirroring ReIdScheduler in the reference repo. This
 * replaces the previous "every frame" behaviour.
 *
 * Boxes are expected to be in the SAME coordinate space as the frame
 * (video_widths_/video_heights_ scale used by the display/cairo overlay), so
 * no scaling is applied here.
 */

#ifndef REID_CROP_STORE_H
#define REID_CROP_STORE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// DetectionBox is defined in multistream_yolo.h. Forward-declare it here to
// avoid a circular include (multistream_yolo.h includes this header).
struct DetectionBox;
class AraReIdEmbedder;

// One track's fresh ReID result for this frame.
struct TrackEmbedding {
  int track_id = -1;
  std::vector<float> embedding;  // L2-normalized; empty => embed failed/disabled
  float quality = 0.0f;          // Laplacian-variance sharpness of the crop
};

class ReidCropStore {
 public:
  explicit ReidCropStore(int stream_id,
                         const std::string& out_dir = "/tmp/reid_crops");

  // Attach an OSNet embedder (optional). If set and ready, process() embeds
  // each due crop and fills TrackEmbedding::embedding. Not owned.
  void setEmbedder(AraReIdEmbedder* embedder) { embedder_ = embedder; }

  // Enable/disable saving crops to disk (debug). Default: disabled.
  // Enable with --save-reid-crops for debugging only; can fill /tmp fast.
  void setSaveCrops(bool on) { save_crops_ = on; }

  // Per-track (re)process cadence in milliseconds (0 = every frame).
  void setMinIntervalMs(int ms) { min_interval_ms_ = ms; }

  // Absolute minimum person-box size (pixels, in this branch's frame
  // resolution). Boxes smaller than w x h are skipped so tiny crops are never
  // upscaled to the OSNet input. Values <=0 leave the corresponding default.
  void setMinCropSize(int w, int h) {
    if (w > 0) min_crop_w_ = w;
    if (h > 0) min_crop_h_ = h;
  }

  // Proximity gate: if another person box overlaps this one with IoU >= this
  // value, skip embedding this track this frame. Close/overlapping boxes are
  // where ByteTrack ID swaps happen and where crops mix two people, so
  // embedding then would corrupt the appearance gallery and swap identities.
  // Range 0..1; set to 1.0 (or >1) to disable the gate. Default 0.30.
  void setProximityIoU(float v) { proximity_iou_ = v; }

  // HEAVY-MERGE threshold (IoMin = intersection / min(area)). When two person
  // boxes overlap this much, it is treated as a genuine MERGE/crossing where
  // ByteTrack is likely to swap track<->person. Both tracks are then DROPPED
  // (identity + appearance re-run from scratch on separation) and shown with a
  // "?" label until a new id is assigned. IoMin (not IoU) is used because tall
  // person boxes make IoU underestimate real crossings. Range 0..1; >=1
  // disables. Default 0.50.
  void setMergeIoMin(float v) { merge_iomin_ = v; }

  // Post-overlap re-verification window (in frames). After two person boxes
  // stop overlapping, the involved tracks stay "recently overlapped" for this
  // many frames. During that window, ReID must RE-VERIFY the identity from the
  // gallery (bypass sticky) so a ByteTrack ID swap during the crossing gets
  // corrected instead of carried forward. 0 disables. Default 30.
  void setReverifyWindow(int frames) { reverify_frames_ = frames; }

  // Track ids that are currently within their post-overlap re-verification
  // window (i.e. were overlapping recently). Valid after the latest process().
  const std::unordered_map<int, char>& recentlyOverlapped() const {
    return recently_overlapped_;
  }

  // Track ids that are in a HEAVY MERGE right now (or in the short settle window
  // just after separation). IdentityManager drops these (re-run ReID), and the
  // GUI draws a "?" label for them until a new id is assigned. Valid after the
  // latest process().
  const std::unordered_map<int, char>& mergedTracks() const {
    return merged_tracks_;
  }

  // Print an IoMin histogram of all person-pair overlaps seen so far, plus the
  // number of merges triggered at the current threshold. Lets you empirically
  // choose the best --reid-merge-iomin value from a real run.
  void printMergeStats() const;

  // Process this frame's boxes: for each due good person box, crop -> (embed)
  // -> (save). Returns the fresh embeddings produced this frame (one entry per
  // track that was embedded).
  //   frame    : tightly packed interleaved RGB (3 bytes/px), frameW x frameH
  //   nowMs    : monotonic clock in ms (for the cadence gate)
  //   frameSeq : monotonically increasing frame counter (used as filename)
  std::vector<TrackEmbedding> process(const uint8_t* frame, int frameW,
                                      int frameH,
                                      const std::vector<DetectionBox>& boxes,
                                      uint64_t nowMs, uint64_t frameSeq);

  uint64_t cropsSaved() const { return crops_saved_; }
  uint64_t embeddingsMade() const { return embeddings_made_; }

 private:
  // Gate: keep only tracked person boxes of reasonable size/aspect/visibility.
  bool isGoodCrop(const DetectionBox& b, int frameW, int frameH) const;

  int stream_id_;
  std::string out_dir_;
  bool save_crops_ = false;
  bool mDebug = false;       // verbose [MERGE]/[MERGE-STATS] logging

  AraReIdEmbedder* embedder_ = nullptr;  // not owned

  // Per-track cadence (ReIdScheduler port). 0 = process every frame.
  int min_interval_ms_ = 200;
  std::unordered_map<int, uint64_t> last_proc_ms_;  // track_id -> last process ms

  // ReID input size (OSNet convention: 128 wide x 256 tall).
  int out_w_ = 128;
  int out_h_ = 256;

  // Crop gates (ported from reid_config.hpp / reid_cropper.cpp).
  int person_class_id_ = 0;
  float min_height_ratio_ = 0.08f;  // box height >= 8% of frame height
  float min_width_ratio_  = 0.02f;  // box width  >= 2% of frame width
  float min_aspect_ = 0.18f;
  float max_aspect_ = 1.0f;
  float expand_x_ratio_ = 0.10f;    // pad crop by 10% width
  float expand_y_ratio_ = 0.15f;    // pad crop by 15% height

  // Absolute minimum box size (pixels, in the frame's own resolution). Boxes
  // smaller than this are rejected so we never UPSCALE a tiny crop up to the
  // OSNet input (which yields a low-detail, unreliable embedding). Defaults to
  // the OSNet input size (128x256): a box below that would have to be upscaled.
  int min_crop_w_ = 80;
  int min_crop_h_ = 100;

  // Proximity gate threshold (IoU with the nearest other person box). >=1
  // disables. Default 0.30: skip embedding when boxes overlap noticeably.
  float proximity_iou_ = 0.30f;

  // Post-overlap re-verification: how long (frames) a track stays flagged after
  // it stopped overlapping, plus per-track "flagged until this frameSeq" map and
  // the resulting recently-overlapped set (rebuilt each process()).
  int reverify_frames_ = 30;
  std::unordered_map<int, uint64_t> overlap_until_;   // track_id -> frameSeq
  std::unordered_map<int, char> recently_overlapped_;  // set for latest frame

  // Heavy-merge (track-swap) handling. A track whose BEST (max) IoMin against
  // any other person box this frame is >= merge_iomin_ is flagged for a full
  // RESET, and stays flagged for merge_settle_frames_ after separation so the
  // reset lands on clean post-separation frames.
  float merge_iomin_ = 0.50f;
  int merge_settle_frames_ = 8;
  std::unordered_map<int, uint64_t> merged_until_;    // track_id -> frameSeq
  std::unordered_map<int, char> merged_tracks_;        // set for latest frame

  // IoMin histogram over ALL person pairs (10 bins 0.0..1.0), for tuning.
  mutable uint64_t iomin_hist_[10] = {0};
  uint64_t merges_triggered_ = 0;

  uint64_t crops_saved_ = 0;
  uint64_t embeddings_made_ = 0;
};

#endif  // REID_CROP_STORE_H
