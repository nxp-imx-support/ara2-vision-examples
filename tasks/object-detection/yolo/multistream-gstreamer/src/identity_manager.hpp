// Copyright 2026 NXP
// SPDX-License-Identifier: BSD-3-Clause
//
// Assigns a persistent personId to each track using cosine similarity against a
// gallery of L2-normalized OSNet embeddings. Two asymmetric thresholds:
//   * matchThreshold  (strict)  -> ACQUIRE / SWITCH to a gallery identity.
//   * stickyThreshold (lenient) -> RETAIN an already-held identity.
// Hard to grab an id, easy to keep one -> stable ids across track fragmentation.
//
// Embedding flow (no per-track mean):
//   raw crop -> quality gate (Laplacian) -> cos(crop, gallery_template) >= directMergeSim
//            -> EMA into gallery (alpha=0.02, only on strong match)
// The per-track mean was removed because it drifted with alpha=0.35 and caused
// the self-consistency gate to reject good crops of the same person after ~100
// frames. The gallery template (alpha=0.02) is stable enough to serve as the
// reference for both the merge gate and the match decision.

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "reid_crop_store.h"  // for TrackEmbedding

// A registered person in the appearance gallery.
struct PersonIdentity {
  int personId = -1;
  std::vector<std::vector<float>> features;  // up to K L2-normalized templates
  int lastCamId = -1;
  int lastTrackId = -1;
  int hits = 0;
  bool active = true;
  // Recency stamp for LRU eviction: the value of IdentityManager::mAccessTick
  // the last time this identity was created / matched (sticky or acquire). The
  // identity with the smallest lastSeenTick is the least-recently-used and is
  // evicted first when the gallery is full.
  uint64_t lastSeenTick = 0;
  // Track that currently "owns" this identity. Prevents a second, concurrently
  // live track from stealing an id already held by another live track (which
  // would show the same personId on two different people). -1 = unowned.
  // DEPRECATED for multi-camera: kept for compatibility; the authoritative
  // ownership is now per-camera in `owners` below.
  int ownerTrackId = -1;
  // Per-camera ownership: camId -> trackId that currently holds this identity
  // on that camera. This allows the SAME person to be legitimately held by one
  // live track on each camera at the same time (correct cross-camera ReID),
  // while still blocking two different tracks ON THE SAME CAMERA from sharing
  // an id. An absent entry means the identity is free to acquire on that cam.
  std::unordered_map<int, int> owners;
};

class IdentityManager {
public:
  // Assign personIds for one frame. `embs` are the fresh embeddings produced
  // this frame (subset of tracks). `presentTrackIds` are all track ids visible
  // this frame (so provisional/sticky tracks without a fresh embed still hold
  // their id). Returns track_id -> person_id for every present track that got
  // an id (>0). Also fills `confidenceOut` (track_id -> 0..1) if non-null.
  std::unordered_map<int, int> assignBatch(
      int camId,
      const std::vector<int>& presentTrackIds,
      const std::vector<TrackEmbedding>& embs,
      std::unordered_map<int, float>* confidenceOut = nullptr,
      const std::unordered_map<int, char>* recentlyOverlapped = nullptr,
      const std::unordered_map<int, char>* mergedTracks = nullptr);

  // Drop a track's per-track accumulation state (call when a track disappears).
  void clearTrack(int camId, int trackId);

  // Threshold configuration (negative leaves the default unchanged).
  void setMatchThreshold(float v)  { if (v >= 0) mMatchThreshold = v; }
  void setStickyThreshold(float v) { if (v >= 0) mStickyThreshold = v; }
  void setMinMatchMargin(float v)  { if (v >= 0) mMinMatchMargin = v; }
  void setStrongThreshold(float v) { if (v >= 0) mStrongMatchThreshold = v; }
  // Minimum cosine similarity between a raw crop and the gallery template to
  // allow direct EMA merge into the gallery (default 0.80). Replaces the old
  // per-track-mean self-consistency gate.
  void setDirectMergeSim(float v)  { if (v >= 0) mDirectMergeSim = v; }
  void setCommitMinCrops(int v)    { if (v >  0) mCommitMinCrops = v; }
  void setGalleryK(int v)          { if (v >  0) mGalleryK = v; }

  // Maximum number of identities kept in the appearance gallery. When full, the
  // least-recently-used identity (smallest lastSeenTick, and not currently held
  // by a live track) is evicted to make room for a new one. Default 1000.
  void setMaxGallery(int v)        { if (v >  0) mMaxGallery = v; }

  // Verbose ReID debug logging (default off). When on, assignBatch prints a
  // per-frame summary and a per-track decision line (sticky/acquire/mint/
  // provisional/quality-reject) so cross-camera behaviour can be inspected live.
  void setDebug(bool on)           { mDebug = on; }

  int gallerySize() const;

  // ReID statistics snapshot (thread-safe). Filled by getStats().
  struct Stats {
    int   gallerySize      = 0;
    int   totalMinted      = 0;
    int   totalAcquired    = 0;
    int   totalSticky      = 0;
    int   totalMergeResets = 0;
    int   totalSwapFixes   = 0;
    int   totalXcam        = 0;
    int   totalDirectMerge = 0;
    int   totalSkippedSim  = 0;
    int   totalSkippedQual = 0;
  };
  void getStats(Stats& s) const;


private:
  static uint64_t makeTrackKey(int camId, int trackId);
  static float dot(const std::vector<float>& a, const std::vector<float>& b);
  static float maxSim(const PersonIdentity& id, const std::vector<float>& emb);
  static void l2Normalize(std::vector<float>& v);

  int createIdentity(int camId, int trackId, const std::vector<float>& emb);
  void emaUpdate(PersonIdentity& id, const std::vector<float>& emb, float sim);

  // Evict the least-recently-used identity when the gallery is at capacity, so a
  // new identity can be created. Never evicts an identity currently held by a
  // live track (owners non-empty). Returns true if space was made (or already
  // available). Caller must hold mMutex.
  bool evictLruIfFull();

  // Per-track accumulation state.
  // NOTE: no running mean is kept here. Raw crops go directly to the gallery
  // (gated by quality + cos >= directMergeSim). goodCrops counts accepted crops
  // for the commit-new-identity decision only.
  struct TrackState {
    int goodCrops = 0;
    int personId = -1;
    float confidence = 0.0f;
    bool reverifying = false;    // currently in a post-overlap re-verify window
    // Last accepted raw embedding: used as the query for gallery matching and
    // as the seed when minting a new identity. Updated every accepted crop.
    std::vector<float> lastEmb;
  };

  mutable std::mutex mMutex;

  int mNextPersonId = 1;
  uint64_t mAccessTick = 0;  // monotonic counter bumped each assignBatch; recency clock
  int mMaxGallery = 1000;    // cap on gallery size (LRU eviction when full)
  std::unordered_map<uint64_t, TrackState> mTrackState;
  std::unordered_map<int, PersonIdentity> mGallery;  // personId -> identity

  // Tunables. Defaults = osnet INT8 operating points from the reference repo.
  float mMatchThreshold = 0.823f;       // acquire/switch identity (FAR<=1%)
  float mStickyThreshold = 0.55f;       // retain a held identity
  float mMinMatchMargin = 0.05f;        // best must beat 2nd-best to acquire
  float mStrongMatchThreshold = 0.85f;  // only EMA-update the gallery at/above this
  int   mGalleryK = 2;                  // templates stored per person (multi-view)
  float mAddTemplateThreshold = 0.75f;  // store a NEW template if best <= this
  int   mCommitMinCrops = 20;           // good crops before committing an id
  float mMinCropQuality = 20.0f;        // Laplacian-variance floor for a good crop
  // Direct-to-gallery merge gate: a raw crop is merged into the gallery template
  // only if cos(crop, gallery_template) >= this. Replaces the old per-track-mean
  // self-consistency gate (which drifted and rejected good crops after ~100 frames).
  float mDirectMergeSim = 0.80f;        // min cos to merge raw crop into gallery
  float mEmaAlpha = 0.20f;              // gallery template EMA adapt rate (80% old + 20% new)
  bool  mDebug = false;                 // verbose per-frame/per-track logging

  // Statistics counters (protected by mMutex).
  int mTotalMinted      = 0;
  int mTotalAcquired    = 0;
  int mTotalSticky      = 0;
  int mTotalMergeResets = 0;
  int mTotalSwapFixes   = 0;
  int mTotalXcam        = 0;
  int mTotalDirectMerge = 0;
  int mTotalSkippedSim  = 0;
  int mTotalSkippedQual = 0;
};
