/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: reid_crop_store.cpp
 */

#include "reid_crop_store.h"

#include "multistream_yolo.h"    // full definition of DetectionBox
#include "ara_reid_embedder.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

// mkdir -p semantics; ignores EEXIST.
bool makeDirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] == '/' || i + 1 == path.size()) {
      if (cur != "/" && !cur.empty()) {
        ::mkdir(cur.c_str(), 0755);  // ignore errors (e.g. EEXIST)
      }
    }
  }
  return true;
}

}  // namespace

ReidCropStore::ReidCropStore(int stream_id, const std::string& out_dir)
    : stream_id_(stream_id), out_dir_(out_dir) {
  // Directory creation is lazy: only when --save-reid-crops is active.
  // Creating it here would run unconditionally even when the flag is off,
  // producing empty /tmp/reid_crops/streamN dirs on every run.
}

bool ReidCropStore::isGoodCrop(const DetectionBox& b, int frameW,
                               int frameH) const {
  if (b.track_id < 0) return false;  // need a stable track id
  if (b.class_id != person_class_id_) return false;

  const float w = b.x2 - b.x1;
  const float h = b.y2 - b.y1;
  if (w <= 1.0f || h <= 1.0f) return false;

  // Absolute size floor: reject boxes smaller than the OSNet input so a tiny
  // crop is never upscaled (upscaling yields a low-detail, unreliable
  // embedding). This is stricter and more predictable than the ratio gates
  // below, especially on downscaled branches where distant people are small.
  if (w < static_cast<float>(min_crop_w_) ||
      h < static_cast<float>(min_crop_h_)) return false;

  if (h < frameH * min_height_ratio_) return false;
  if (w < frameW * min_width_ratio_) return false;

  const float aspect = w / std::max(1.0f, h);
  if (aspect < min_aspect_ || aspect > max_aspect_) return false;

  // Visibility: at least 70% of the box must lie inside the frame.
  const float visW = std::min(b.x2, static_cast<float>(frameW)) -
                     std::max(b.x1, 0.0f);
  const float visH = std::min(b.y2, static_cast<float>(frameH)) -
                     std::max(b.y1, 0.0f);
  const float visArea = std::max(0.0f, visW) * std::max(0.0f, visH);
  const float boxArea = std::max(1.0f, w * h);
  if (visArea / boxArea < 0.70f) return false;

  return true;
}

std::vector<TrackEmbedding> ReidCropStore::process(
    const uint8_t* frame, int frameW, int frameH,
    const std::vector<DetectionBox>& boxes, uint64_t nowMs,
    uint64_t frameSeq) {
  std::vector<TrackEmbedding> results;
  if (!frame || frameW <= 0 || frameH <= 0) return results;

  // Wrap the incoming RGB frame as a cv::Mat (no copy). The frame is RGB (from
  // imxvideoconvert_g2d format=RGB); OpenCV's imwrite expects BGR, so we
  // convert once per crop just before writing.
  const cv::Mat frameRgb(frameH, frameW, CV_8UC3,
                         const_cast<uint8_t*>(frame));

  // ---- Proximity gate + post-overlap re-verification bookkeeping ----
  // "crowded" = person tracks whose box overlaps ANOTHER person box with IoU >=
  // proximity_iou_ THIS frame. We (a) skip embedding crowded tracks this frame
  // (crop would mix two people), and (b) extend each crowded track's
  // "recently overlapped" window by reverify_frames_ frames. Tracks inside that
  // window (but no longer overlapping) ARE embedded, and reported via
  // recentlyOverlapped() so IdentityManager re-verifies their id from the
  // gallery (correcting any ByteTrack ID swap that happened during the crossing).
  std::unordered_map<int, char> crowded;
  if (proximity_iou_ < 1.0f || merge_iomin_ < 1.0f) {
    // IoMin = intersection / min(areaA, areaB). Unlike IoU this does not inflate
    // the denominator with the (large) union of two tall person boxes, so it
    // fires reliably on real crossings where one person partly covers another.
    auto iomin = [](const DetectionBox& a, const DetectionBox& b) -> float {
      const float ix1 = std::max(a.x1, b.x1);
      const float iy1 = std::max(a.y1, b.y1);
      const float ix2 = std::min(a.x2, b.x2);
      const float iy2 = std::min(a.y2, b.y2);
      const float iw = std::max(0.0f, ix2 - ix1);
      const float ih = std::max(0.0f, iy2 - iy1);
      const float inter = iw * ih;
      if (inter <= 0.0f) return 0.0f;
      const float aa = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
      const float ab = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
      const float mn = std::min(aa, ab);
      return mn > 0.0f ? inter / mn : 0.0f;
    };
    for (size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].class_id != person_class_id_ || boxes[i].track_id < 0) continue;
      for (size_t j = i + 1; j < boxes.size(); ++j) {
        if (boxes[j].class_id != person_class_id_ || boxes[j].track_id < 0)
          continue;
        const float ov = iomin(boxes[i], boxes[j]);
        // Stats: bucket every overlapping person pair (>0) for threshold tuning.
        if (ov > 0.0f) {
          int bin = static_cast<int>(ov * 10.0f);
          if (bin > 9) bin = 9;
          iomin_hist_[bin]++;
        }
        // Light overlap -> pause embedding (crop would mix two people).
        if (proximity_iou_ < 1.0f && ov >= proximity_iou_) {
          crowded[boxes[i].track_id] = 1;
          crowded[boxes[j].track_id] = 1;
        }
        // HEAVY merge -> DROP both tracks (new track + re-run ReID). Using the
        // pairwise IoMin naturally captures each track's worst-case overlap.
        if (merge_iomin_ < 1.0f && ov >= merge_iomin_) {
          merged_until_[boxes[i].track_id] =
              frameSeq + static_cast<uint64_t>(merge_settle_frames_);
          merged_until_[boxes[j].track_id] =
              frameSeq + static_cast<uint64_t>(merge_settle_frames_);
          merges_triggered_++;
          if (mDebug) g_print("[MERGE] stream%d T%d <-> T%d IoMin=%.3f (>= %.2f) -> DROP "
                  "tracks + re-run ReID (settle %d frames)\n",
                  stream_id_, boxes[i].track_id, boxes[j].track_id, ov,
                  merge_iomin_, merge_settle_frames_);
        }
      }
    }
    // Extend the re-verification window for tracks overlapping this frame.
    for (const auto& kv : crowded)
      overlap_until_[kv.first] = frameSeq + static_cast<uint64_t>(reverify_frames_);
  }
  // Rebuild the "recently overlapped" set: tracks still within their window.
  recently_overlapped_.clear();
  for (auto it = overlap_until_.begin(); it != overlap_until_.end();) {
    if (frameSeq <= it->second) {
      recently_overlapped_[it->first] = 1;
      ++it;
    } else {
      it = overlap_until_.erase(it);  // window expired
    }
  }
  // Rebuild the "merged" set: tracks in/just-out-of a heavy merge (to be RESET
  // and shown with a "?" label until re-identified).
  merged_tracks_.clear();
  for (auto it = merged_until_.begin(); it != merged_until_.end();) {
    if (frameSeq <= it->second) {
      merged_tracks_[it->first] = 1;
      ++it;
    } else {
      it = merged_until_.erase(it);  // settle window expired
    }
  }

  for (const auto& b : boxes) {
    if (!isGoodCrop(b, frameW, frameH)) continue;
    if (crowded.count(b.track_id)) continue;  // overlapping NOW -> skip embed
    if (merged_tracks_.count(b.track_id)) continue;  // merged -> dropped, skip

    // ---- Per-track cadence gate (ReIdScheduler port) ----
    if (min_interval_ms_ > 0) {
      auto it = last_proc_ms_.find(b.track_id);
      if (it != last_proc_ms_.end() &&
          nowMs - it->second < static_cast<uint64_t>(min_interval_ms_)) {
        continue;  // too soon since this track was last processed
      }
    }

    const float w = b.x2 - b.x1;
    const float h = b.y2 - b.y1;
    const float ex = expand_x_ratio_ * w;
    const float ey = expand_y_ratio_ * h;

    int x0 = std::max(0, static_cast<int>(std::floor(b.x1 - ex)));
    int y0 = std::max(0, static_cast<int>(std::floor(b.y1 - ey)));
    int x1 = std::min(frameW, static_cast<int>(std::ceil(b.x2 + ex)));
    int y1 = std::min(frameH, static_cast<int>(std::ceil(b.y2 + ey)));
    const int cropW = x1 - x0;
    const int cropH = y1 - y0;
    if (cropW < 8 || cropH < 16) continue;

    // Cut the ROI (still RGB) and resize to the fixed OSNet input (bilinear).
    const cv::Rect roi(x0, y0, cropW, cropH);
    cv::Mat cropRgb = frameRgb(roi);

    cv::Mat resizedRgb;
    cv::resize(cropRgb, resizedRgb, cv::Size(out_w_, out_h_), 0, 0,
               cv::INTER_LINEAR);
    if (!resizedRgb.isContinuous()) resizedRgb = resizedRgb.clone();

    // Passed the gate -> record the process time for this track.
    last_proc_ms_[b.track_id] = nowMs;

    // Sharpness = variance of the Laplacian (low => motion blur / soft crop).
    float quality = 0.0f;
    {
      cv::Mat gray, lap;
      cv::cvtColor(resizedRgb, gray, cv::COLOR_RGB2GRAY);
      cv::Laplacian(gray, lap, CV_32F);
      cv::Scalar mean, stddev;
      cv::meanStdDev(lap, mean, stddev);
      quality = static_cast<float>(stddev[0] * stddev[0]);
    }

    // ---- OSNet embedding (if an embedder is attached and ready) ----
    TrackEmbedding te;
    te.track_id = b.track_id;
    te.quality = quality;
    if (embedder_ && embedder_->ready()) {
      const size_t rgbSize =
          static_cast<size_t>(out_w_) * out_h_ * 3;
      if (embedder_->run(resizedRgb.data, rgbSize, te.embedding) &&
          !te.embedding.empty()) {
        embeddings_made_++;
      }
    }
    results.push_back(std::move(te));

    // ---- Debug: save the crop as PNG (--save-reid-crops only) ----
    // Rotation: keep at most kMaxCropsPerTrack files per track directory,
    // deleting the oldest by name (frameSeq is monotonic, so the
    // shortest-then-lexicographically-smallest name (== numeric order) is the oldest).
    if (save_crops_) {
      cv::Mat resizedBgr;
      cv::cvtColor(resizedRgb, resizedBgr, cv::COLOR_RGB2BGR);

      const std::string dir = out_dir_ + "/stream" +
                              std::to_string(stream_id_) + "/track" +
                              std::to_string(b.track_id);
      makeDirs(dir);  // mkdir -p streamN/trackN, only when saving is enabled

      // Enforce a per-track cap before writing the new file (limits files per
      // track; directories of ended tracks are not removed).
      static constexpr int kMaxCropsPerTrack = 10;
      {
        // Collect existing PNG files in this track directory.
        DIR* dp = ::opendir(dir.c_str());
        if (dp) {
          std::vector<std::string> files;
          struct dirent* ent;
          while ((ent = ::readdir(dp)) != nullptr) {
            const std::string name(ent->d_name);
            if (name.size() > 4 &&
                name.compare(name.size() - 4, 4, ".png") == 0) {
              files.push_back(name);
            }
          }
          ::closedir(dp);
          // Delete oldest files (smallest frameSeq) until we are below cap.
          if (static_cast<int>(files.size()) >= kMaxCropsPerTrack) {
            std::sort(files.begin(), files.end(),
                      [](const std::string& a, const std::string& b) {
                        return a.size() != b.size() ? a.size() < b.size() : a < b;
                      });
            const int to_delete =
                static_cast<int>(files.size()) - kMaxCropsPerTrack + 1;
            for (int k = 0; k < to_delete; ++k) {
              const std::string old_path = dir + "/" + files[k];
              ::unlink(old_path.c_str());
            }
          }
        }
      }

      const std::string path = dir + "/" + std::to_string(frameSeq) + ".png";
      if (cv::imwrite(path, resizedBgr)) crops_saved_++;
    }
  }
  return results;
}

void ReidCropStore::printMergeStats() const {
  uint64_t total = 0;
  for (int i = 0; i < 10; ++i) total += iomin_hist_[i];
  if (mDebug) g_print("[MERGE-STATS] stream%d IoMin histogram over %llu overlapping person "
          "pairs (merge threshold=%.2f, merges triggered=%llu):\n",
          stream_id_, static_cast<unsigned long long>(total), merge_iomin_,
          static_cast<unsigned long long>(merges_triggered_));
  // cumulative-from-top helps pick a threshold: "how many pairs >= X".
  uint64_t cum = 0;
  for (int i = 9; i >= 0; --i) {
    cum += iomin_hist_[i];
    if (mDebug) g_print("[MERGE-STATS]   IoMin %.1f-%.1f: %6llu   (>= %.1f: %llu)\n",
            i / 10.0, (i + 1) / 10.0,
            static_cast<unsigned long long>(iomin_hist_[i]), i / 10.0,
            static_cast<unsigned long long>(cum));
  }
}
