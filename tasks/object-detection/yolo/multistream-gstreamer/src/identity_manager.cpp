// Copyright 2026 NXP
// SPDX-License-Identifier: BSD-3-Clause

#include "identity_manager.hpp"

#include <algorithm>
#include <cmath>

#include <glib.h>

uint64_t IdentityManager::makeTrackKey(int camId, int trackId) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(camId)) << 32) |
         static_cast<uint32_t>(trackId);
}

float IdentityManager::dot(const std::vector<float>& a,
                           const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return -1.0f;
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) s += static_cast<double>(a[i]) * b[i];
  return static_cast<float>(s);
}

void IdentityManager::l2Normalize(std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * x;
  double n = std::sqrt(s);
  if (n <= 1e-12) return;
  float inv = static_cast<float>(1.0 / n);
  for (float& x : v) x *= inv;
}

float IdentityManager::maxSim(const PersonIdentity& id,
                              const std::vector<float>& emb) {
  float best = -2.0f;
  for (const auto& f : id.features) {
    float s = dot(f, emb);
    if (s > best) best = s;
  }
  return best;
}

void IdentityManager::clearTrack(int camId, int trackId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mTrackState.erase(makeTrackKey(camId, trackId));
}

int IdentityManager::gallerySize() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return static_cast<int>(mGallery.size());
}

void IdentityManager::getStats(Stats& s) const {
  std::lock_guard<std::mutex> lock(mMutex);
  s.gallerySize      = static_cast<int>(mGallery.size());
  s.totalMinted      = mTotalMinted;
  s.totalAcquired    = mTotalAcquired;
  s.totalSticky      = mTotalSticky;
  s.totalMergeResets = mTotalMergeResets;
  s.totalSwapFixes   = mTotalSwapFixes;
  s.totalXcam        = mTotalXcam;
  s.totalDirectMerge = mTotalDirectMerge;
  s.totalSkippedSim  = mTotalSkippedSim;
  s.totalSkippedQual = mTotalSkippedQual;
}

int IdentityManager::createIdentity(int camId, int trackId,
                                    const std::vector<float>& emb) {
  evictLruIfFull();  // make room if the gallery is at capacity (LRU)
  int pid = mNextPersonId++;
  PersonIdentity id;
  id.personId = pid;
  id.features.push_back(emb);
  id.lastCamId = camId;
  id.lastTrackId = trackId;
  id.ownerTrackId = trackId;  // legacy field (kept for compatibility)
  id.owners[camId] = trackId;  // creator track owns the id ON THIS CAMERA
  id.hits = 1;
  id.active = true;
  id.lastSeenTick = mAccessTick;  // recency stamp for LRU
  mGallery[pid] = std::move(id);
  return pid;
}

// Evict the least-recently-used identity if the gallery is at/over capacity.
// Skips identities that are currently held by a live track (owners non-empty)
// so we never purge someone actively on screen. Removes at most one entry per
// call (createIdentity is about to add exactly one).
bool IdentityManager::evictLruIfFull() {
  if (static_cast<int>(mGallery.size()) < mMaxGallery) return true;

  int victim = -1;
  uint64_t oldest = UINT64_MAX;
  for (const auto& kv : mGallery) {
    if (!kv.second.owners.empty()) continue;  // in use by a live track: keep
    if (kv.second.lastSeenTick < oldest) {
      oldest = kv.second.lastSeenTick;
      victim = kv.first;
    }
  }
  if (victim < 0) {
    // Every identity is currently held (all owned). Fall back to evicting the
    // globally oldest regardless of ownership so we can still admit a new one.
    for (const auto& kv : mGallery) {
      if (kv.second.lastSeenTick < oldest) {
        oldest = kv.second.lastSeenTick;
        victim = kv.first;
      }
    }
  }
  if (victim >= 0) {
    g_print("[ID-EVICT] gallery full (%d) -> purge LRU P%d (lastSeenTick=%llu)\n",
            static_cast<int>(mGallery.size()), victim,
            static_cast<unsigned long long>(oldest));
    mGallery.erase(victim);
    return true;
  }
  return false;
}

void IdentityManager::emaUpdate(PersonIdentity& id,
                                const std::vector<float>& emb, float sim) {
  id.hits++;
  if (id.features.empty()) { id.features.push_back(emb); return; }
  int j = 0; float bestS = -2.0f;
  for (int i = 0; i < static_cast<int>(id.features.size()); ++i) {
    float s = dot(id.features[i], emb);
    if (s > bestS) { bestS = s; j = i; }
  }
  // Multi-view: if room and this is a DISTINCT view, store as a new template.
  if (mGalleryK > 1 && static_cast<int>(id.features.size()) < mGalleryK &&
      bestS <= mAddTemplateThreshold) {
    id.features.push_back(emb);
    return;
  }
  if (sim < mStrongMatchThreshold) return;  // weak match: don't corrupt template
  if (id.features[j].size() == emb.size()) {
    for (size_t i = 0; i < emb.size(); ++i)
      id.features[j][i] =
          (1.0f - mEmaAlpha) * id.features[j][i] + mEmaAlpha * emb[i];
    l2Normalize(id.features[j]);
  }
}

std::unordered_map<int, int> IdentityManager::assignBatch(
    int camId, const std::vector<int>& presentTrackIds,
    const std::vector<TrackEmbedding>& embs,
    std::unordered_map<int, float>* confidenceOut,
    const std::unordered_map<int, char>* recentlyOverlapped,
    const std::unordered_map<int, char>* mergedTracks) {
  std::lock_guard<std::mutex> lock(mMutex);

  // Monotonic recency clock: one tick per assignBatch call. Identities touched
  // this call get stamped with this value; LRU eviction removes the smallest.
  ++mAccessTick;

  // Index fresh embeddings by track id.
  std::unordered_map<int, const TrackEmbedding*> embOf;
  for (const auto& e : embs)
    if (!e.embedding.empty()) embOf[e.track_id] = &e;

  if (mDebug) {
    g_print("[REID] cam%d frame: present=%zu freshEmb=%zu gallery=%zu\n",
            camId, presentTrackIds.size(), embOf.size(), mGallery.size());
  }

  // ---- Step 1: process fresh crops directly into the gallery (no local mean).
  // For each track with a fresh embedding this frame:
  //   (a) quality gate: reject blurry crops (Laplacian < mMinCropQuality)
  //   (b) if track has a committed personId:
  //         cos(raw_crop, gallery_template) >= mDirectMergeSim (0.80)
  //         -> EMA merge into gallery + count as good crop
  //         else: skip merge but still count toward goodCrops for commit
  //   (c) if track has no personId yet: just count goodCrops (for commit gate)
  //       and store lastEmb for use as the seed when minting a new identity.
  for (int tid : presentTrackIds) {
    uint64_t key = makeTrackKey(camId, tid);
    TrackState& st = mTrackState[key];

    // HEAVY-MERGE RESET: drop identity + appearance + gallery ownership.
    const bool merged = mergedTracks && mergedTracks->count(tid) > 0;
    if (merged) {
      if (st.personId > 0) {
        auto git = mGallery.find(st.personId);
        if (git != mGallery.end()) {
          auto oit = git->second.owners.find(camId);
          if (oit != git->second.owners.end() && oit->second == tid)
            git->second.owners.erase(oit);
        }
        g_print("[MERGE-RESET] cam%d T%d: dropping identity P%d + appearance "
                "(overlap) -> re-run ReID after separation\n",
                camId, tid, st.personId);
        mTotalMergeResets++;
      }
      st.personId = -1;
      st.goodCrops = 0;
      st.lastEmb.clear();
      st.reverifying = false;
      continue;
    }

    // Post-overlap re-verification: reset crop accumulation on first reverify
    // frame so the track re-earns its identity from current appearance.
    const bool nowReverify =
        recentlyOverlapped && recentlyOverlapped->count(tid) > 0;
    if (nowReverify && !st.reverifying) {
      st.goodCrops = 0;
      st.lastEmb.clear();
    }
    st.reverifying = nowReverify;

    auto eit = embOf.find(tid);
    if (eit == embOf.end()) continue;
    const TrackEmbedding& e = *eit->second;

    // (a) Quality gate.
    if (e.quality < mMinCropQuality) {
      if (mDebug)
        g_print("[REID]   cam%d T%d: crop REJECTED quality=%.1f < %.1f\n",
                camId, tid, e.quality, mMinCropQuality);
      mTotalSkippedQual++;
      continue;
    }

    // Always store the latest accepted raw embedding (used as seed for new id).
    // Guard: skip if embedding is empty (embedder failed this crop).
    if (e.embedding.empty()) continue;
    st.lastEmb = e.embedding;

    // (b) If track has a committed personId AND is the active owner on this
    // camera, merge directly into the gallery template (sim >= mDirectMergeSim).
    // Ownership check prevents a re-entering track (not yet re-confirmed) from
    // writing into the gallery before it has re-earned the identity.
    if (st.personId > 0) {
      auto git = mGallery.find(st.personId);
      if (git != mGallery.end()) {
        // Only merge if this track is the current owner on this camera.
        auto oit = git->second.owners.find(camId);
        const bool isOwner = (oit != git->second.owners.end() &&
                              oit->second == tid);
        if (isOwner) {
          float sim = maxSim(git->second, e.embedding);
          if (sim >= mDirectMergeSim) {
            // Direct EMA merge into gallery template (80% old + 20% new).
            emaUpdate(git->second, e.embedding, sim);
            git->second.lastSeenTick = mAccessTick;
            st.goodCrops++;
            mTotalDirectMerge++;
            if (mDebug)
              g_print("[REID]   cam%d T%d: crop MERGED into P%d sim=%.3f "
                      "(direct-to-gallery, owner)\n", camId, tid, st.personId, sim);
          } else {
            mTotalSkippedSim++;
            if (mDebug)
              g_print("[REID]   cam%d T%d: crop SKIPPED sim=%.3f < %.2f "
                      "(direct-merge gate, would corrupt gallery)\n",
                      camId, tid, sim, mDirectMergeSim);
          }
        } else {
          if (mDebug)
            g_print("[REID]   cam%d T%d: crop SKIPPED not owner of P%d "
                    "(no gallery write until re-confirmed)\n",
                    camId, tid, st.personId);
        }
      }
    } else {
      // (c) No personId yet: count crop toward commit gate.
      st.goodCrops++;
      if (mDebug)
        g_print("[REID]   cam%d T%d: crop COUNTED goodCrops=%d (no id yet)\n",
                camId, tid, st.goodCrops);
    }
  }

  // ---- Step 2: assign. Confident tracks (more good crops) claim ids first;
  // used-set keeps one live track per person id this frame. ----
  std::vector<int> order = presentTrackIds;
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return mTrackState[makeTrackKey(camId, a)].goodCrops >
           mTrackState[makeTrackKey(camId, b)].goodCrops;
  });

  std::unordered_map<int, int> usedBy;  // personId -> track id
  auto usedByOther = [&](int pid, int tid) {
    auto it = usedBy.find(pid);
    return it != usedBy.end() && it->second != tid;
  };

  // Set of tracks present (live) this frame ON THIS CAMERA. Used to prevent a
  // track from stealing an identity that another *still-live* track on the
  // SAME camera currently owns. Cross-camera ownership does NOT block: the same
  // person can be held by one live track per camera simultaneously (this is
  // what makes cross-camera re-identification work).
  std::unordered_map<int, char> liveSet;
  for (int t : presentTrackIds) liveSet[t] = 1;
  // A track is "swapping" if it is itself in the post-overlap re-verify window.
  auto isReverifying = [&](int t) {
    return recentlyOverlapped && recentlyOverlapped->count(t) > 0;
  };
  auto heldByOtherLive = [&](int pid, int tid) {
    auto git = mGallery.find(pid);
    if (git == mGallery.end()) return false;
    auto oit = git->second.owners.find(camId);   // ownership ON THIS CAMERA
    if (oit == git->second.owners.end()) return false;
    int owner = oit->second;
    if (owner <= 0 || owner == tid || !liveSet.count(owner)) return false;
    // Normally an id held by another live track is NOT acquirable. But during a
    // crossing two tracks may need to SWAP identities: if BOTH this track and
    // the current owner are re-verifying, allow acquisition so the swap can be
    // corrected (each track re-earns the id that matches its appearance).
    if (isReverifying(tid) && isReverifying(owner)) return false;
    return true;
  };

  std::unordered_map<int, int> result;  // trackId -> personId

  for (int tid : order) {
    uint64_t key = makeTrackKey(camId, tid);
    TrackState& st = mTrackState[key];
    float outConf = 0.0f;

    // Merged tracks were fully reset in Step 1 -> leave UNASSIGNED this frame
    // (GUI shows "?") until they separate and re-run ReID from clean crops.
    if (mergedTracks && mergedTracks->count(tid) > 0) {
      if (confidenceOut) (*confidenceOut)[tid] = 0.0f;
      continue;
    }

    // Post-overlap re-verification: if this track recently overlapped another
    // person (a crossing where ByteTrack may have SWAPPED track<->box), we must
    // NOT trust the sticky/held id. Instead re-earn the identity from the
    // gallery by appearance, and do not EMA-write the gallery until it
    // re-confirms, so a swap can be corrected and no template gets polluted.
    const bool reverify =
        recentlyOverlapped && recentlyOverlapped->count(tid) > 0;

    // No usable embedding yet -> provisional: hold a prior id (if free) for display.
    if (st.lastEmb.empty()) {
      if (st.personId > 0 && !usedByOther(st.personId, tid)) {
        result[tid] = st.personId; usedBy[st.personId] = tid;
      }
      if (confidenceOut) (*confidenceOut)[tid] = outConf;
      continue;
    }

    // Best / second-best gallery match against the track's latest raw embedding.
    float best = -1.0f, second = -1.0f; int bestPid = -1;
    for (auto& kv : mGallery) {
      if (!kv.second.active) continue;
      if (heldByOtherLive(kv.first, tid)) continue;
      float s = maxSim(kv.second, st.lastEmb);
      if (s > best) { second = best; best = s; bestPid = kv.first; }
      else if (s > second) second = s;
    }
    float margin = best - second;
    float evidence =
        std::min(1.0f, st.goodCrops / static_cast<float>(mCommitMinCrops));
    st.confidence =
        evidence * std::min(1.0f, std::max(0.0f, margin / mMinMatchMargin));
    outConf = st.confidence;
    bool enough = st.goodCrops >= mCommitMinCrops;

    if (mDebug) {
      g_print("[REID]   cam%d T%d: heldP%d goodCrops=%d best=P%d %.3f 2nd=%.3f "
              "margin=%.3f (match=%.2f sticky=%.2f enough=%d)\n",
              camId, tid, st.personId, st.goodCrops, bestPid, best, second,
              margin, mMatchThreshold, mStickyThreshold, enough ? 1 : 0);
    }

    // (RE-VERIFY) Post-overlap identity re-search. When this track recently
    // overlapped another person, ByteTrack may have SWAPPED track<->person, so
    // we do NOT trust the sticky/held id. We simply take the best cosine match
    // over the whole gallery (bestPid/best, computed above) and assign that
    // EXISTING identity. No new id is minted during the window: a crossing
    // person is already known, so we only re-attach. A change from the carried
    // id is a corrected swap ([ID-SWAPFIX]).
    if (reverify) {
      if (bestPid > 0 && best >= mMatchThreshold && !usedByOther(bestPid, tid)) {
        const int heldBefore = st.personId;
        st.personId = bestPid; result[tid] = bestPid; usedBy[bestPid] = tid;
        auto& id = mGallery[bestPid];
        id.lastCamId = camId; id.lastTrackId = tid;
        id.ownerTrackId = tid; id.owners[camId] = tid;
        id.lastSeenTick = mAccessTick;
        if (confidenceOut) (*confidenceOut)[tid] = outConf;
        if (mDebug) {
          if (heldBefore > 0 && heldBefore != bestPid) {
            g_print("[REID-RERUN] cam%d T%d: re-ran gallery -> SWAP FIXED %d -> P%d "
                    "(sim=%.3f)\n", camId, tid, heldBefore, bestPid, best);
            mTotalSwapFixes++;
          } else {
            g_print("[REID-RERUN] cam%d T%d: re-ran gallery -> CONFIRMED P%d "
                    "(sim=%.3f)\n", camId, tid, bestPid, best);
          }
        } else if (heldBefore > 0 && heldBefore != bestPid) {
          mTotalSwapFixes++;
        }
      } else {
        if (st.personId > 0 && !usedByOther(st.personId, tid)) {
          result[tid] = st.personId; usedBy[st.personId] = tid;
        }
        if (mDebug)
          g_print("[REID-RERUN] cam%d T%d: re-ran gallery -> NO MATCH (best=P%d "
                  "%.3f < %.2f), hold P%d (no mint)\n",
                  camId, tid, bestPid, best, mMatchThreshold, st.personId);
      }
      if (confidenceOut) (*confidenceOut)[tid] = outConf;
      continue;
    }

    // (a) STICKY: keep a committed id while the latest crop still plausibly matches.
    if (st.personId > 0 && !usedByOther(st.personId, tid)) {
      auto gid = mGallery.find(st.personId);
      if (gid != mGallery.end()) {
        float selfSim = maxSim(gid->second, st.lastEmb);
        if (selfSim >= mStickyThreshold) {
          result[tid] = st.personId; usedBy[st.personId] = tid;
          // Gallery EMA update already done in Step 1 (direct-to-gallery).
          gid->second.lastCamId = camId; gid->second.lastTrackId = tid;
          gid->second.ownerTrackId = tid;
          gid->second.owners[camId] = tid;
          gid->second.lastSeenTick = mAccessTick;
          mTotalSticky++;
          if (confidenceOut) (*confidenceOut)[tid] = std::max(outConf, 0.5f);
          if (mDebug)
            g_print("[REID]   cam%d T%d: STICKY -> P%d selfSim=%.3f\n",
                    camId, tid, st.personId, selfSim);
          continue;
        }
      }
    }

    // (b) ACQUIRE an existing id: strong match + margin.
    if (bestPid > 0 && best >= mMatchThreshold && margin >= mMinMatchMargin &&
        !usedByOther(bestPid, tid) && !heldByOtherLive(bestPid, tid)) {
      st.personId = bestPid; result[tid] = bestPid; usedBy[bestPid] = tid;
      auto& id = mGallery[bestPid];
      const int prevCam = id.lastCamId;
      // Gallery EMA update already done in Step 1 (direct-to-gallery).
      id.lastCamId = camId; id.lastTrackId = tid;
      id.ownerTrackId = tid;
      id.owners[camId] = tid;
      id.lastSeenTick = mAccessTick;
      if (confidenceOut) (*confidenceOut)[tid] = outConf;
      mTotalAcquired++;
      if (prevCam >= 0 && prevCam != camId) {
        mTotalXcam++;
        g_print("[ID-XCAM] P%d re-acquired on cam%d T%d (prev cam%d) "
                "sim=%.3f margin=%.3f templates=%zu\n",
                bestPid, camId, tid, prevCam, best, margin, id.features.size());
      } else if (mDebug) {
        g_print("[REID]   cam%d T%d: ACQUIRE -> P%d sim=%.3f margin=%.3f\n",
                camId, tid, bestPid, best, margin);
      }
      continue;
    }

    // (c) COMMIT a NEW identity only with enough evidence AND when it clearly is
    // not an existing person. Use the latest raw embedding as the seed template.
    if (enough && (bestPid <= 0 || best < mMatchThreshold) && !st.lastEmb.empty()) {
      int pid = createIdentity(camId, tid, st.lastEmb);
      st.personId = pid; result[tid] = pid; usedBy[pid] = tid;
      mTotalMinted++;
      if (confidenceOut) (*confidenceOut)[tid] = outConf;
      g_print("[ID-NEW] cam%d T%d -> P%d goodCrops=%d bestExisting=P%d %.3f "
              "(< match %.2f) 2nd=%.3f margin=%.3f gallery=%zu\n",
              camId, tid, pid, st.goodCrops, bestPid, best, mMatchThreshold,
              second, margin, mGallery.size());
      continue;
    }

    // (d) Provisional: keep prior id for display continuity if free.
    if (st.personId > 0 && !usedByOther(st.personId, tid)) {
      result[tid] = st.personId; usedBy[st.personId] = tid;
      if (mDebug)
        g_print("[REID]   cam%d T%d: PROVISIONAL hold P%d (best %.3f < match, "
                "not enough evidence)\n", camId, tid, st.personId, best);
    } else if (mDebug) {
      g_print("[REID]   cam%d T%d: UNASSIGNED (best=P%d %.3f goodCrops=%d)\n",
              camId, tid, bestPid, best, st.goodCrops);
    }
    if (confidenceOut) (*confidenceOut)[tid] = outConf;
  }

  // ---- Prune per-track state for tracks that vanished on this camera. ----
  std::unordered_map<int, char> alive;
  for (int tid : presentTrackIds) alive[tid] = 1;
  for (auto it = mTrackState.begin(); it != mTrackState.end();) {
    int kc = static_cast<int>(it->first >> 32);
    int kt = static_cast<int>(it->first & 0xffffffffu);
    if (kc == camId && !alive.count(kt)) it = mTrackState.erase(it);
    else ++it;
  }

  // ---- Release identity ownership for owner tracks that are no longer live,
  // so a returning person (new track id) can re-acquire the identity. Ownership
  // is per-camera: only touch THIS camera's owner entry, so releasing on cam A
  // never disturbs a live hold on cam B (cross-camera identities stay intact).
  for (auto& kv : mGallery) {
    auto oit = kv.second.owners.find(camId);
    if (oit != kv.second.owners.end()) {
      int owner = oit->second;
      if (owner > 0 && !alive.count(owner)) {
        kv.second.owners.erase(oit);
        // Keep the legacy scalar roughly in sync for any external reader.
        if (kv.second.ownerTrackId == owner) kv.second.ownerTrackId = -1;
      }
    }
  }

  return result;
}
