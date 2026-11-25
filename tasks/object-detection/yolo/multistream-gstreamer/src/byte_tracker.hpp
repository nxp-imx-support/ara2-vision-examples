/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: byte_tracker.hpp
 *
 * Header-only C++ implementation of the ByteTrack multi-object tracker
 * (Zhang et al., "ByteTrack: Multi-Object Tracking by Associating Every
 * Detection Box", ECCV 2022; https://github.com/FoundationVision/ByteTrack).
 *
 * Attribution: the reference ByteTrack implementation is released under the
 * MIT License (Copyright (c) 2021 Yifu Zhang). This file is an independent
 * re-implementation that follows the reference algorithm but shares no source
 * code with it; it is provided here under the BSD-3-Clause license above.
 *
 * The core idea is two-stage data association: high-score detections are
 * matched to existing tracks first, then the remaining low-score detections
 * are matched against still-unmatched tracks. Keeping low-score boxes recovers
 * occluded or blurred objects and reduces identity switches compared to plain
 * SORT.
 *
 * This port follows the reference implementation closely to preserve tracking
 * quality:
 *   - First and unconfirmed association stages fuse IoU similarity with the
 *     detection confidence (fuse_score): cost = 1 - IoU * score.
 *   - The low-score (second) stage uses a fixed 0.5 IoU-distance gate, the
 *     unconfirmed stage uses 0.7, matching the reference.
 *   - Newly created tracks stay "unconfirmed" until they match again, which
 *     suppresses one-frame false-positive tracks.
 *   - Lost tracks are re-activated (with tracklet length reset) rather than a
 *     plain update, and duplicate tracks across the tracked/lost pools are
 *     removed.
 *
 * It implements a self-contained constant-velocity Kalman filter and a
 * self-contained linear assignment so it pulls in no external dependencies. It
 * defines its own TrackingDetection input and TrackResult output structs.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <vector>

// All ByteTrack public types live in the bytetrack namespace to avoid
// polluting the global namespace and to prevent name collisions.
namespace bytetrack {

// Input detection passed to the tracker (top-left width-height plus score and
// class id).
struct TrackingDetection {
  float x;
  float y;
  float w;
  float h;
  float score;
  int classId;
};

// Output track produced by the tracker.
struct TrackResult {
  int trackId;
  int classId;
  float score;
  float x;
  float y;
  float width;
  float height;
  bool confirmed;
};

struct ByteTrackConfig {

  // Detections with score >= trackThresh are used in the first association
  // stage; detections below it (but above a small floor) are used in the
  // second, low-score association stage.
  float trackThresh = 0.3f;

  // IoU-distance gating threshold used by the first (high-score) association
  // stage. A pair is only allowed to match when (1 - IoU * score) < matchThresh.
  float matchThresh = 0.8f;

  // Fixed IoU-distance gate for the second (low-score) association stage.
  // The reference always uses 0.5 here regardless of matchThresh.
  float secondMatchThresh = 0.5f;

  // Fixed IoU-distance gate for the third (unconfirmed) association stage.
  float unconfirmedThresh = 0.7f;

  // Number of frames a lost track is retained before removal. Scaled by
  // frameRate below (buffer = trackBuffer * frameRate / 30). A larger buffer
  // keeps identities stable through short occlusions.
  int trackBuffer = 120;

  // Weight of the raw detection box when blending it with the Kalman estimate
  // to produce the emitted box. 1.0 = raw detection (no smoothing), 0.0 = pure
  // Kalman estimate. Lower values reduce visible jitter.
  float outputDetWeight = 0.5f;

  // Number of consecutive frames a brand-new (unconfirmed) track may go
  // unmatched before it is removed. The reference removes after 1 frame; a
  // small grace period stops blinking detections from churning IDs.
  int unconfirmedGrace = 2;


  // Nominal input frame rate, used to scale the lost-track buffer.
  int frameRate = 30;

  // -1 = track all classes, otherwise restrict to a single COCO class id.
  int trackingClass = -1;
};



namespace byte_detail {

// Minimal fixed-size Kalman filter with an 8 dimensional state
// [cx, cy, aspect, height, vcx, vcy, vaspect, vheight] and a 4 dimensional
// measurement [cx, cy, aspect, height]. This mirrors the filter used by the
// reference ByteTrack implementation.
class KalmanFilter {
 public:
  static constexpr int kStateDim = 8;
  static constexpr int kMeasDim = 4;

  using StateMean = std::array<float, kStateDim>;
  using StateCov = std::array<std::array<float, kStateDim>, kStateDim>;
  using MeasMean = std::array<float, kMeasDim>;

  KalmanFilter() {
    // Lower process-noise weights than the reference (1/20, 1/160) so the
    // filter trusts its constant-velocity model more and reacts less to
    // per-detection noise. This reduces visible box jitter, especially when
    // the detector only runs on a subset of frames.
    stdWeightPosition_ = 1.0f / 40.0f;
    stdWeightVelocity_ = 1.0f / 320.0f;
  }


  // Create a new track state from an initial measurement.
  void initiate(const MeasMean& measurement, StateMean& mean,
                StateCov& cov) const {
    for (int i = 0; i < kMeasDim; ++i) {
      mean[i] = measurement[i];
      mean[i + kMeasDim] = 0.0f;
    }

    const float h = measurement[3];
    std::array<float, kStateDim> std;
    std[0] = 2 * stdWeightPosition_ * h;
    std[1] = 2 * stdWeightPosition_ * h;
    std[2] = 1e-2f;
    std[3] = 2 * stdWeightPosition_ * h;
    std[4] = 10 * stdWeightVelocity_ * h;
    std[5] = 10 * stdWeightVelocity_ * h;
    std[6] = 1e-5f;
    std[7] = 10 * stdWeightVelocity_ * h;

    for (int r = 0; r < kStateDim; ++r) {
      for (int c = 0; c < kStateDim; ++c) {
        cov[r][c] = 0.0f;
      }
      cov[r][r] = std[r] * std[r];
    }
  }

  // Constant-velocity prediction step.
  void predict(StateMean& mean, StateCov& cov) const {
    // x' = F x
    for (int i = 0; i < kMeasDim; ++i) {
      mean[i] += mean[i + kMeasDim];
    }

    const float h = mean[3];
    std::array<float, kStateDim> std;
    std[0] = stdWeightPosition_ * h;
    std[1] = stdWeightPosition_ * h;
    std[2] = 1e-2f;
    std[3] = stdWeightPosition_ * h;
    std[4] = stdWeightVelocity_ * h;
    std[5] = stdWeightVelocity_ * h;
    std[6] = 1e-5f;
    std[7] = stdWeightVelocity_ * h;

    // P' = F P F^T + Q, with F being the constant-velocity transition.
    StateCov fp;
    for (int r = 0; r < kStateDim; ++r) {
      for (int c = 0; c < kStateDim; ++c) {
        float v = cov[r][c];
        if (r < kMeasDim) {
          v += cov[r + kMeasDim][c];
        }
        fp[r][c] = v;
      }
    }
    for (int r = 0; r < kStateDim; ++r) {
      for (int c = 0; c < kStateDim; ++c) {
        float v = fp[r][c];
        if (c < kMeasDim) {
          v += fp[r][c + kMeasDim];
        }
        cov[r][c] = v;
      }
    }
    for (int i = 0; i < kStateDim; ++i) {
      cov[i][i] += std[i] * std[i];
    }
  }

  // Kalman update step given a new measurement. Returns false when the
  // projected measurement covariance is (near-)singular and cannot be safely
  // inverted; in that case the state is left unchanged so the caller can treat
  // the track as diverged rather than propagating a garbage gain.
  bool update(StateMean& mean, StateCov& cov,
              const MeasMean& measurement) const {
    const float h = mean[3];
    std::array<float, kMeasDim> std;
    std[0] = stdWeightPosition_ * h;
    std[1] = stdWeightPosition_ * h;
    std[2] = 1e-1f;
    std[3] = stdWeightPosition_ * h;

    // Projected measurement covariance S = H P H^T + R.
    std::array<std::array<float, kMeasDim>, kMeasDim> s;
    for (int r = 0; r < kMeasDim; ++r) {
      for (int c = 0; c < kMeasDim; ++c) {
        s[r][c] = cov[r][c];
      }
      s[r][r] += std[r] * std[r];
    }

    // Kalman gain K = P H^T S^-1. P H^T is the first kMeasDim columns of P.
    std::array<std::array<float, kMeasDim>, kMeasDim> sInv;
    if (!invert4x4(s, sInv)) {
      // Singular / ill-conditioned innovation covariance: refuse the update
      // rather than substituting garbage. Signal divergence to the caller.
      return false;
    }

    std::array<std::array<float, kMeasDim>, kStateDim> kGain;
    for (int r = 0; r < kStateDim; ++r) {
      for (int c = 0; c < kMeasDim; ++c) {
        float v = 0.0f;
        for (int k = 0; k < kMeasDim; ++k) {
          v += cov[r][k] * sInv[k][c];
        }
        kGain[r][c] = v;
      }
    }

    // Innovation y = z - H x.
    std::array<float, kMeasDim> innov;
    for (int i = 0; i < kMeasDim; ++i) {
      innov[i] = measurement[i] - mean[i];
    }

    // State update x = x + K y.
    for (int r = 0; r < kStateDim; ++r) {
      float v = 0.0f;
      for (int c = 0; c < kMeasDim; ++c) {
        v += kGain[r][c] * innov[c];
      }
      mean[r] += v;
    }

    // Covariance update P = P - K H P = P - K (first kMeasDim rows of P).
    for (int r = 0; r < kStateDim; ++r) {
      for (int c = 0; c < kStateDim; ++c) {
        float v = 0.0f;
        for (int k = 0; k < kMeasDim; ++k) {
          v += kGain[r][k] * cov[k][c];
        }
        cov[r][c] -= v;
      }
    }
    return true;
  }

 private:
  // Invert a 4x4 matrix via Gauss-Jordan elimination. Returns false when the
  // matrix is singular or numerically too close to singular to invert safely
  // (a zero/near-zero pivot is encountered), leaving out unmodified.
  static bool invert4x4(
      const std::array<std::array<float, kMeasDim>, kMeasDim>& in,
      std::array<std::array<float, kMeasDim>, kMeasDim>& out) {
    float a[kMeasDim][2 * kMeasDim];
    for (int r = 0; r < kMeasDim; ++r) {
      for (int c = 0; c < kMeasDim; ++c) {
        a[r][c] = in[r][c];
        a[r][c + kMeasDim] = (r == c) ? 1.0f : 0.0f;
      }
    }

    for (int col = 0; col < kMeasDim; ++col) {
      int pivot = col;
      float best = std::fabs(a[col][col]);
      for (int r = col + 1; r < kMeasDim; ++r) {
        if (std::fabs(a[r][col]) > best) {
          best = std::fabs(a[r][col]);
          pivot = r;
        }
      }
      if (pivot != col) {
        for (int c = 0; c < 2 * kMeasDim; ++c) {
          std::swap(a[col][c], a[pivot][c]);
        }
      }

      float diag = a[col][col];
      if (std::fabs(diag) < 1e-12f) {
        // Near-zero pivot: the matrix is singular / ill-conditioned. Report
        // failure instead of clamping the pivot, which would silently produce
        // a garbage inverse (and hence a garbage Kalman gain).
        return false;
      }
      for (int c = 0; c < 2 * kMeasDim; ++c) {
        a[col][c] /= diag;
      }

      for (int r = 0; r < kMeasDim; ++r) {
        if (r == col) {
          continue;
        }
        float factor = a[r][col];
        for (int c = 0; c < 2 * kMeasDim; ++c) {
          a[r][c] -= factor * a[col][c];
        }
      }
    }

    for (int r = 0; r < kMeasDim; ++r) {
      for (int c = 0; c < kMeasDim; ++c) {
        out[r][c] = a[r][c + kMeasDim];
      }
    }
    return true;
  }

  float stdWeightPosition_;
  float stdWeightVelocity_;
};

// Hungarian (Kuhn-Munkres) solver for a rectangular cost matrix. Returns, for
// every row, the assigned column or -1 when the row is left unassigned because
// its best cost exceeds costLimit.
class HungarianSolver {
 public:
  static std::vector<int> solve(const std::vector<std::vector<float>>& cost,
                                float costLimit) {
    const int rows = static_cast<int>(cost.size());
    if (rows == 0) {
      return {};
    }
    const int cols = static_cast<int>(cost[0].size());
    if (cols == 0) {
      return std::vector<int>(rows, -1);
    }

    const int n = std::max(rows, cols);
    const float kBig = 1e9f;

    std::vector<std::vector<float>> a(n, std::vector<float>(n, kBig));
    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < cols; ++c) {
        a[r][c] = cost[r][c];
      }
    }

    // Jonker-Volgenant style shortest augmenting path over the padded square
    // matrix. u/v are potentials, p[j] is the row matched to column j.
    std::vector<float> u(n + 1, 0.0f);
    std::vector<float> v(n + 1, 0.0f);
    std::vector<int> p(n + 1, 0);
    std::vector<int> way(n + 1, 0);

    for (int i = 1; i <= n; ++i) {
      p[0] = i;
      int j0 = 0;
      std::vector<float> minv(n + 1, std::numeric_limits<float>::max());
      std::vector<char> used(n + 1, false);
      do {
        used[j0] = true;
        int i0 = p[j0];
        float delta = std::numeric_limits<float>::max();
        int j1 = -1;
        for (int j = 1; j <= n; ++j) {
          if (!used[j]) {
            float cur = a[i0 - 1][j - 1] - u[i0] - v[j];
            if (cur < minv[j]) {
              minv[j] = cur;
              way[j] = j0;
            }
            if (minv[j] < delta) {
              delta = minv[j];
              j1 = j;
            }
          }
        }
        for (int j = 0; j <= n; ++j) {
          if (used[j]) {
            u[p[j]] += delta;
            v[j] -= delta;
          } else {
            minv[j] -= delta;
          }
        }
        j0 = j1;
      } while (p[j0] != 0);
      do {
        int j1 = way[j0];
        p[j0] = p[j1];
        j0 = j1;
      } while (j0 != 0);
    }

    std::vector<int> rowToCol(rows, -1);
    for (int j = 1; j <= n; ++j) {
      int i = p[j];
      if (i >= 1 && i <= rows && j <= cols) {
        if (a[i - 1][j - 1] <= costLimit) {
          rowToCol[i - 1] = j - 1;
        }
      }
    }
    return rowToCol;
  }
};

}  // namespace byte_detail

class ByteTracker {
 public:
  explicit ByteTracker(const ByteTrackConfig& cfg = {}) : mCfg(cfg) {
    mMaxTimeLost = std::max(
        1, static_cast<int>(static_cast<float>(mCfg.frameRate) / 30.0f *
                            static_cast<float>(mCfg.trackBuffer)));
    // The reference spawns new tracks only from detections above
    // track_thresh + 0.1.
    mDetThresh = mCfg.trackThresh + 0.1f;
  }

  std::vector<TrackResult> update(
      const std::vector<TrackingDetection>& detections) {
    mFrameCount++;

    if (mBtDebug) {
      fprintf(stderr,
              "[BT] f%d: dets=%zu tracked=%zu lost=%zu\n",
              mFrameCount, detections.size(), mTracks.size(), mLostTracks.size());
    }

    // Split detections into high-score and low-score sets. Per-class filtering
    // (when a single class is requested) is applied up front.
    std::vector<TrackingDetection> highDets;
    std::vector<TrackingDetection> lowDets;
    for (const auto& d : detections) {
      if (mCfg.trackingClass >= 0 && d.classId != mCfg.trackingClass) {
        continue;
      }
      if (d.score >= mCfg.trackThresh) {
        highDets.push_back(d);
      } else if (d.score > 0.1f) {
        lowDets.push_back(d);
      }
    }

    // Separate existing tracks into confirmed (activated) and unconfirmed
    // (brand new, seen only once) groups, matching the reference.
    std::vector<STrack*> confirmed;
    std::vector<STrack*> unconfirmed;
    for (auto& t : mTracks) {
      if (t.isActivated) {
        confirmed.push_back(&t);
      } else {
        unconfirmed.push_back(&t);
      }
    }

    // Predict the new location of every existing track. Non-tracked tracks
    // have their height-velocity reset (mean[7] = 0) as in the reference.
    for (auto& t : mTracks) {
      if (t.state != TrackState::Tracked) {
        t.mean[7] = 0.0f;
      }
      mKf.predict(t.mean, t.cov);
    }
    for (auto& t : mLostTracks) {
      if (t.state != TrackState::Tracked) {
        t.mean[7] = 0.0f;
      }
      mKf.predict(t.mean, t.cov);
    }

    // Pool of candidate tracks for the first association: confirmed tracked
    // plus lost tracks (ByteTrack re-associates lost tracks in stage one).
    std::vector<STrack*> pool;
    pool.reserve(confirmed.size() + mLostTracks.size());
    for (auto* t : confirmed) {
      pool.push_back(t);
    }
    for (auto& t : mLostTracks) {
      pool.push_back(&t);
    }

    // --- First association: high-score detections against the pool. ---
    // Cost fuses IoU with the detection score, gated by matchThresh.
    std::vector<int> poolMatchDet(pool.size(), -1);
    std::vector<bool> highMatched(highDets.size(), false);
    associate(pool, highDets, poolMatchDet, highMatched, mCfg.matchThresh,
              /*fuseScore=*/true);

    std::vector<STrack*> remainPool;
    for (size_t i = 0; i < pool.size(); ++i) {
      if (poolMatchDet[i] >= 0) {
        if (pool[i]->state == TrackState::Tracked) {
          pool[i]->update(mKf, highDets[poolMatchDet[i]], mFrameCount);
        } else {
          pool[i]->reActivate(mKf, highDets[poolMatchDet[i]], mFrameCount);
        }
      } else {
        remainPool.push_back(pool[i]);
      }
    }

    // --- Second association: low-score detections against leftover tracks. ---
    // Only tracks that were in the Tracked state are eligible here (matching
    // the reference), lost tracks stay lost if unmatched. Plain IoU distance
    // with a fixed 0.5 gate (no score fusion).
    std::vector<STrack*> secondPool;
    for (auto* t : remainPool) {
      if (t->state == TrackState::Tracked) {
        secondPool.push_back(t);
      }
    }

    std::vector<int> secondMatchDet(secondPool.size(), -1);
    std::vector<bool> lowMatched(lowDets.size(), false);
    associate(secondPool, lowDets, secondMatchDet, lowMatched,
              mCfg.secondMatchThresh, /*fuseScore=*/false);

    for (size_t i = 0; i < secondPool.size(); ++i) {
      if (secondMatchDet[i] >= 0) {
        secondPool[i]->update(mKf, lowDets[secondMatchDet[i]], mFrameCount);
      }
    }

    // Collect the tracks that were matched in the second association stage so
    // the lost-marking loop below is an O(1) set lookup per track rather than
    // an O(n^2) linear scan of secondPool.
    std::unordered_set<STrack*> matchedSecondSet;
    matchedSecondSet.reserve(secondPool.size());
    for (size_t i = 0; i < secondPool.size(); ++i) {
      if (secondMatchDet[i] >= 0) {
        matchedSecondSet.insert(secondPool[i]);
      }
    }

    // Any remaining tracked pool member that was not matched in either stage
    // becomes lost.
    for (auto* t : remainPool) {
      const bool matchedSecond = matchedSecondSet.count(t) > 0;
      if (!matchedSecond && t->state != TrackState::Lost) {
        if (mBtDebug) {
          float tx, ty, tw, th;
          t->toTlwh(tx, ty, tw, th);
          float bestIou = 0.0f, bestScore = 0.0f;
          for (const auto& d : highDets) {
            float iou = iouTlwh(tx, ty, tw, th, d.x, d.y, d.w, d.h);
            if (iou > bestIou) { bestIou = iou; bestScore = d.score; }
          }
          for (const auto& d : lowDets) {
            float iou = iouTlwh(tx, ty, tw, th, d.x, d.y, d.w, d.h);
            if (iou > bestIou) { bestIou = iou; bestScore = d.score; }
          }
          const char* reason =
              (bestIou == 0.0f)              ? "NO-DET(yolo-miss)" :
              (bestIou < 0.3f)               ? "LOW-IOU(box-drift)" :
              (bestScore < mCfg.trackThresh) ? "LOW-CONF(score-dip)" :
                                               "IOU-GATE-FAIL";
          fprintf(stderr, "[BT] f%d: LOST ID%d reason=%s bestIoU=%.2f bestScore=%.2f\n",
                  mFrameCount, t->trackId, reason, bestIou, bestScore);
        }
        t->markLost();
      }
    }

    // --- Third association: unconfirmed tracks vs leftover high-score dets. ---
    // Handles brand-new tracks that have only been seen once. Cost fuses IoU
    // with the detection score, gated by unconfirmedThresh. Unmatched
    // unconfirmed tracks are removed after a short grace period
    // (unconfirmedGrace frames) to suppress false positives without churning
    // IDs on blinking detections.

    std::vector<TrackingDetection> remHighDets;
    std::vector<int> remHighIndex;
    for (size_t i = 0; i < highDets.size(); ++i) {
      if (!highMatched[i]) {
        remHighDets.push_back(highDets[i]);
        remHighIndex.push_back(static_cast<int>(i));
      }
    }

    std::vector<int> uncMatchDet(unconfirmed.size(), -1);
    std::vector<bool> remHighMatched(remHighDets.size(), false);
    associate(unconfirmed, remHighDets, uncMatchDet, remHighMatched,
              mCfg.unconfirmedThresh, /*fuseScore=*/true);

    for (size_t i = 0; i < unconfirmed.size(); ++i) {
      if (uncMatchDet[i] >= 0) {
        unconfirmed[i]->update(mKf, remHighDets[uncMatchDet[i]], mFrameCount);
      } else {
        // Give brand-new tracks a short grace period rather than removing them
        // on the first miss; this stops blinking detections from churning IDs.
        unconfirmed[i]->unmatchedCount++;
        if (unconfirmed[i]->unmatchedCount > mCfg.unconfirmedGrace) {
          unconfirmed[i]->markRemoved();
        }
      }
    }

    // Propagate which original high-score detections got consumed here.
    for (size_t i = 0; i < remHighDets.size(); ++i) {
      if (remHighMatched[i]) {
        highMatched[remHighIndex[i]] = true;
      }
    }

    // --- Initialize new tracks from still-unmatched high-score detections. ---
    // Before minting a new ID, check if any just-lost track is close enough
    // (IoU >= mLostReclaimIou). If so, reactivate it instead of creating a
    // new ID -- prevents churn when a confirmed track loses the Hungarian race
    // on the same frame it goes lost.
    //
    // IMPORTANT: do NOT call mTracks.push_back() here. At this point confirmed,
    // unconfirmed, pool, remainPool, secondPool, and reclaimedLost all contain
    // raw pointers into mTracks. A push_back that triggers a reallocation would
    // silently invalidate every one of those pointers, causing undefined behaviour
    // (dangling-pointer reads/writes) that manifests as glibc heap corruption.
    // New tracks are instead staged in pendingNew and merged into newTracked
    // during the housekeeping rebuild below, after all pointer-using work is done.
    std::vector<STrack> pendingNew;
    std::unordered_set<STrack*> reclaimedLost;
    for (size_t i = 0; i < highDets.size(); ++i) {
      if (highMatched[i]) continue;
      if (highDets[i].score < mDetThresh) continue;

      STrack* bestLost = nullptr;
      float bestLostIou = mLostReclaimIou;
      for (auto* t : remainPool) {
        if (t->state != TrackState::Lost) continue;
        if (reclaimedLost.count(t)) continue;
        float tx, ty, tw, th;
        t->toTlwh(tx, ty, tw, th);
        float iou = iouTlwh(tx, ty, tw, th,
                            highDets[i].x, highDets[i].y,
                            highDets[i].w, highDets[i].h);
        if (iou > bestLostIou) { bestLostIou = iou; bestLost = t; }
      }

      if (bestLost) {
        bestLost->reActivate(mKf, highDets[i], mFrameCount);
        reclaimedLost.insert(bestLost);
        if (mBtDebug)
          fprintf(stderr, "[BT] f%d: RECLAIM ID%d IoU=%.2f score=%.2f\n",
                  mFrameCount, bestLost->trackId, bestLostIou, highDets[i].score);
      } else {
        STrack t;
        t.activate(mKf, highDets[i], mNextTrackId++, mFrameCount);
        if (mBtDebug)
          fprintf(stderr, "[BT] f%d: NEW ID%d score=%.2f\n",
                  mFrameCount, t.trackId, highDets[i].score);
        pendingNew.push_back(std::move(t));
      }
    }

    // --- Housekeeping: rebuild tracked/lost lists and drop stale tracks. ---
    // All raw pointers into mTracks (confirmed, unconfirmed, pool, etc.) are
    // no longer used past this point, so it is safe to modify the containers.
    std::vector<STrack> newTracked;
    std::vector<STrack> newLost;

    for (auto& t : mTracks) {
      if (t.state == TrackState::Tracked) {
        newTracked.push_back(t);
      } else if (t.state == TrackState::Lost) {
        newLost.push_back(t);
      }
      // Removed tracks (unmatched unconfirmed) are simply dropped.
    }
    for (auto& t : mLostTracks) {
      // A Tracked entry in mLostTracks is the normal outcome of a successful
      // re-association: reActivate() sets the state to Tracked in stage 1 or
      // the lost-reclaim path, before housekeeping runs. Move it to newTracked.
      if (t.state == TrackState::Tracked) {
        newTracked.push_back(t);
      } else if (t.state == TrackState::Lost) {
        if (mFrameCount - t.frameId <= mMaxTimeLost) {
          newLost.push_back(t);
        }
      }
    }

    // Merge brand-new tracks minted this frame (staged in pendingNew to avoid
    // invalidating the raw pointers held above).
    for (auto& t : pendingNew) {
      newTracked.push_back(std::move(t));
    }

    // Remove duplicate tracks that appear in both the tracked and lost pools
    // (they can overlap after re-association); keep the longer-lived one.
    removeDuplicateStracks(newTracked, newLost);

    mTracks = std::move(newTracked);
    mLostTracks = std::move(newLost);

    // Emit confirmed, currently-tracked outputs.
    std::vector<TrackResult> out;
    out.reserve(mTracks.size());
    for (const auto& t : mTracks) {
      if (t.state != TrackState::Tracked || !t.isActivated) {
        continue;
      }
      float x, y, w, h;
      // Blend the raw detection with the Kalman estimate to reduce jitter.
      t.toOutput(mCfg.outputDetWeight, x, y, w, h);
      out.push_back({t.trackId, t.classId, t.score, x, y, w, h, true});

    }
    return out;
  }

  void reset() {
    mTracks.clear();
    mLostTracks.clear();
    mNextTrackId = 1;
    mFrameCount = 0;
  }

 private:
  enum class TrackState { New, Tracked, Lost, Removed };

  struct STrack {
    byte_detail::KalmanFilter::StateMean mean{};
    byte_detail::KalmanFilter::StateCov cov{};

    int trackId = -1;
    int classId = -1;
    float score = 0.0f;

    TrackState state = TrackState::New;
    bool isActivated = false;
    int frameId = 0;
    int startFrame = 0;

    // Last matched raw detection box (top-left width-height). Used to blend
    // with the Kalman estimate for a less jittery emitted box.
    float lastX = 0.0f;
    float lastY = 0.0f;
    float lastW = 0.0f;
    float lastH = 0.0f;
    bool hasLastDet = false;

    // Consecutive frames this (still unconfirmed) track has gone unmatched.
    int unmatchedCount = 0;


    // Convert a detection (x, y, w, h) to the filter measurement space
    // [cx, cy, aspect, height].
    static byte_detail::KalmanFilter::MeasMean toXyah(
        const TrackingDetection& d) {
      byte_detail::KalmanFilter::MeasMean m;
      m[0] = d.x + d.w * 0.5f;
      m[1] = d.y + d.h * 0.5f;
      m[2] = d.h > 0.0f ? d.w / d.h : 0.0f;
      m[3] = d.h;
      return m;
    }

    // Current estimate as top-left width-height.
    void toTlwh(float& x, float& y, float& w, float& h) const {
      float cx = mean[0];
      float cy = mean[1];
      float aspect = mean[2];
      h = mean[3];
      w = aspect * h;
      x = cx - w * 0.5f;
      y = cy - h * 0.5f;
    }

    // Emitted box: a blend of the last matched raw detection and the Kalman
    // estimate. detWeight in [0, 1] selects how much of the raw detection is
    // used (0 = pure Kalman, 1 = raw detection). Falls back to the pure Kalman
    // estimate when no detection has been matched yet.
    void toOutput(float detWeight, float& x, float& y, float& w,
                  float& h) const {
      float kx, ky, kw, kh;
      toTlwh(kx, ky, kw, kh);
      if (!hasLastDet) {
        x = kx;
        y = ky;
        w = kw;
        h = kh;
        return;
      }
      float dw = std::min(1.0f, std::max(0.0f, detWeight));
      x = dw * lastX + (1.0f - dw) * kx;
      y = dw * lastY + (1.0f - dw) * ky;
      w = dw * lastW + (1.0f - dw) * kw;
      h = dw * lastH + (1.0f - dw) * kh;
    }

    // Cache the raw detection box for output blending.
    void cacheDet(const TrackingDetection& d) {
      lastX = d.x;
      lastY = d.y;
      lastW = d.w;
      lastH = d.h;
      hasLastDet = true;
    }


    void activate(const byte_detail::KalmanFilter& kf,
                  const TrackingDetection& d, int id, int frame) {
      kf.initiate(toXyah(d), mean, cov);
      trackId = id;
      classId = d.classId;
      score = d.score;
      state = TrackState::Tracked;
      // A track is only confirmed immediately on the very first frame; all
      // others must be matched again next frame to become activated.
      isActivated = (frame == 1);
      frameId = frame;
      startFrame = frame;
      unmatchedCount = 0;
      cacheDet(d);
    }

    void update(const byte_detail::KalmanFilter& kf,
                const TrackingDetection& d, int frame) {
      // If the innovation covariance is singular the filter refuses the
      // update and keeps the predicted state. Treat the track as diverged:
      // mark it Lost immediately so it enters mLostTracks and ages out via
      // the normal mMaxTimeLost expiry path. Leaving it Tracked with a stale
      // frameId would make it immortal because the age-out logic only applies
      // to tracks that are already in mLostTracks.
      if (!kf.update(mean, cov, toXyah(d))) {
        markLost();
        return;
      }
      classId = d.classId;
      score = d.score;
      state = TrackState::Tracked;
      isActivated = true;
      frameId = frame;
      unmatchedCount = 0;
      cacheDet(d);
    }



    // Re-activate a lost (or non-tracked) track from a new detection. Resets
    // the state to Tracked and confirms it, keeping the same track id.
    void reActivate(const byte_detail::KalmanFilter& kf,
                    const TrackingDetection& d, int frame) {
      // As in update(): if the innovation covariance is singular the filter
      // refuses the update and keeps the predicted state. Do NOT re-activate
      // the track on a measurement that could not be applied; leave it in its
      // current (lost/non-tracked) state so it continues to age out.
      if (!kf.update(mean, cov, toXyah(d))) {
        return;
      }
      classId = d.classId;
      score = d.score;
      state = TrackState::Tracked;
      isActivated = true;
      frameId = frame;
      unmatchedCount = 0;
      cacheDet(d);
    }


    void markLost() { state = TrackState::Lost; }

    void markRemoved() { state = TrackState::Removed; }
  };

  // IoU between two axis-aligned boxes given as top-left width-height.
  static float iouTlwh(float ax, float ay, float aw, float ah, float bx,
                       float by, float bw, float bh) {
    float x1 = std::max(ax, bx);
    float y1 = std::max(ay, by);
    float x2 = std::min(ax + aw, bx + bw);
    float y2 = std::min(ay + ah, by + bh);
    float w = std::max(0.0f, x2 - x1);
    float h = std::max(0.0f, y2 - y1);
    float inter = w * h;
    float denom = aw * ah + bw * bh - inter;
    return denom > 0.0f ? inter / denom : 0.0f;
  }

  // Run one linear assignment between a set of tracks and detections. The cost
  // is either plain IoU distance (1 - IoU) or the fused score distance
  // (1 - IoU * detScore) when fuseScore is true, matching the reference
  // fuse_score step. Fills matchDet[track] with the matched detection index
  // (or -1) and marks detMatched accordingly. costLimit is the IoU-distance
  // gate for this stage.
  void associate(const std::vector<STrack*>& tracks,
                 const std::vector<TrackingDetection>& dets,
                 std::vector<int>& matchDet, std::vector<bool>& detMatched,
                 float costLimit, bool fuseScore) {
    if (tracks.empty() || dets.empty()) {
      return;
    }

    std::vector<std::vector<float>> cost(
        tracks.size(), std::vector<float>(dets.size(), 1.0f));

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
      float tx, ty, tw, th;
      tracks[ti]->toTlwh(tx, ty, tw, th);
      for (size_t di = 0; di < dets.size(); ++di) {
        float iou = iouTlwh(tx, ty, tw, th, dets[di].x, dets[di].y,
                            dets[di].w, dets[di].h);
        if (fuseScore) {
          // fuse_score: cost = 1 - IoU_sim * det_score.
          cost[ti][di] = 1.0f - iou * dets[di].score;
        } else {
          cost[ti][di] = 1.0f - iou;
        }
      }
    }

    std::vector<int> assignment =
        byte_detail::HungarianSolver::solve(cost, costLimit);

    for (size_t ti = 0; ti < tracks.size(); ++ti) {
      int di = assignment[ti];
      if (di >= 0 && !detMatched[di]) {
        matchDet[ti] = di;
        detMatched[di] = true;
      }
    }
  }



  // Remove near-duplicate tracks that appear in both the tracked and lost
  // pools. For each overlapping pair (IoU distance < 0.15) the shorter-lived
  // track is discarded, matching the reference remove_duplicate_stracks.
  void removeDuplicateStracks(std::vector<STrack>& tracked,
                              std::vector<STrack>& lost) {
    if (tracked.empty() || lost.empty()) {
      return;
    }

    std::vector<bool> dropTracked(tracked.size(), false);
    std::vector<bool> dropLost(lost.size(), false);

    for (size_t a = 0; a < tracked.size(); ++a) {
      float ax, ay, aw, ah;
      tracked[a].toTlwh(ax, ay, aw, ah);
      for (size_t b = 0; b < lost.size(); ++b) {
        float bx, by, bw, bh;
        lost[b].toTlwh(bx, by, bw, bh);
        float iou = iouTlwh(ax, ay, aw, ah, bx, by, bw, bh);
        if (1.0f - iou < 0.15f) {
          int timeA = tracked[a].frameId - tracked[a].startFrame;
          int timeB = lost[b].frameId - lost[b].startFrame;
          if (timeA > timeB) {
            dropLost[b] = true;
          } else {
            dropTracked[a] = true;
          }
        }
      }
    }

    std::vector<STrack> keptTracked;
    keptTracked.reserve(tracked.size());
    for (size_t i = 0; i < tracked.size(); ++i) {
      if (!dropTracked[i]) {
        keptTracked.push_back(tracked[i]);
      }
    }
    std::vector<STrack> keptLost;
    keptLost.reserve(lost.size());
    for (size_t i = 0; i < lost.size(); ++i) {
      if (!dropLost[i]) {
        keptLost.push_back(lost[i]);
      }
    }

    tracked = std::move(keptTracked);
    lost = std::move(keptLost);
  }

  ByteTrackConfig mCfg;
  byte_detail::KalmanFilter mKf;
  std::vector<STrack> mTracks;
  std::vector<STrack> mLostTracks;
  int mNextTrackId = 1;
  int mFrameCount = 0;
  int mMaxTimeLost = 30;
  float mDetThresh = 0.6f;
  float mLostReclaimIou = 0.5f;  // min IoU to reclaim a just-lost track
  bool mBtDebug = false;         // verbose logging, set via --bt-debug

 public:
  void setDebug(bool on) { mBtDebug = on; }

 private:
};

}  // namespace bytetrack

