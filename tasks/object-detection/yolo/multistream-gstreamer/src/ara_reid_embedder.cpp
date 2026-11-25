// Copyright 2026 NXP
// SPDX-License-Identifier: BSD-3-Clause
//

#include "ara_reid_embedder.hpp"

#include <algorithm>
#include <cmath>

#include <glib.h>

namespace {
// ImageNet normalization constants (torchreid default, matches OSNet training).
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};
}  // namespace

bool AraReIdEmbedder::init(AraRuntime *rt, const std::string &modelPath) {
  if (!rt || !rt->initialized()) {
    g_printerr("[ARA-REID] runtime not initialized\n");
    return false;
  }
  mRt = rt;
  mModel = rt->loadModel(modelPath, "osnet");
  if (!mModel) {
    g_printerr("[ARA-REID] failed to load OSNet model: %s\n", modelPath.c_str());
    return false;
  }

  int outDim = kOutDim;
  const dv_model_output_param_t *op = mModel->outputParam(0);
  if (op && op->size > 0) outDim = op->size;  // OSNet output_param is reliable

  mInput.assign(static_cast<size_t>(3) * kInH * kInW, 0);
  mOutput.assign(static_cast<size_t>(outDim), 0);
  g_print("[ARA-REID] OSNet embedder ready (input %dx%d int8, out %d, "
          "inQn=%.0f outQn=%.0f)\n",
          kInW, kInH, outDim, kInputQn, kOutputQn);
  return true;
}

bool AraReIdEmbedder::run(const uint8_t *rgb, size_t rgbSize,
                          std::vector<float> &outEmb) {
  outEmb.clear();
  if (!mModel || !rgb) return false;

  const int W = kInW;
  const int H = kInH;
  const size_t need = static_cast<size_t>(W) * H * 3;
  if (rgbSize < need) {
    g_printerr("[ARA-REID] inputRgb too small: %zu < %zu\n", rgbSize, need);
    return false;
  }

  // Preprocess: interleaved RGB HxW -> int8 CHW via /255, ImageNet norm, *inputQn.
  const int plane = H * W;
  for (int c = 0; c < 3; ++c) {
    int8_t *dst = mInput.data() + static_cast<size_t>(c) * plane;
    const float inv = 1.0f / (255.0f * kStd[c]);
    const float sub = kMean[c] / kStd[c];  // normed = px/255/std - mean/std
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const uint8_t v = rgb[(static_cast<size_t>(y) * W + x) * 3 + c];
        float normed = static_cast<float>(v) * inv - sub;
        int q = static_cast<int>(lrintf(normed * kInputQn));
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        dst[static_cast<size_t>(y) * W + x] = static_cast<int8_t>(q);
      }
    }
  }

  std::vector<AraBlob> ins = {{mInput.data(), mInput.size()}};
  std::vector<AraBlob> outs = {{mOutput.data(), mOutput.size()}};
  if (!mModel->infer(ins, outs)) {
    g_printerr("[ARA-REID] infer failed\n");
    return false;
  }

  outEmb.resize(mOutput.size());
  for (size_t i = 0; i < mOutput.size(); ++i)
    outEmb[i] = static_cast<float>(mOutput[i]) / kOutputQn;
  l2Normalize(outEmb);
  return true;
}

void AraReIdEmbedder::l2Normalize(std::vector<float> &v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * x;
  double n = std::sqrt(s);
  if (n <= 1e-12) return;
  float inv = static_cast<float>(1.0 / n);
  for (float &x : v) x *= inv;
}
