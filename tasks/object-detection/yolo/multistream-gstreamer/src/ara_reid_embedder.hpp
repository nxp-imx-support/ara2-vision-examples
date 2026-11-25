// Copyright 2026 NXP
// SPDX-License-Identifier: BSD-3-Clause
//
// OSNet contract (from the original repo's docs/ara-reid-dvm-contracts.md):
//   input : int8 CHW 3x256x128 (H=256,W=128), inputQn=32.0
//           runtime int8 = round(imagenet_normed_fp32 * 32), clamp [-128,127]
//           preprocess = RGB crop -> /255 -> ImageNet norm -> CHW
//   output: uint8 512-d, outputQn=16.0  => fp = raw / 16.0, then L2-normalize.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ara_runtime.hpp"

class AraReIdEmbedder {
public:
  // `rt` must outlive this embedder; modelPath = OSNet .dvm.
  bool init(AraRuntime *rt, const std::string &modelPath);

  // Embed one crop. `rgb` is tightly packed interleaved RGB of size
  // kInW*kInH*3 (128*256*3). Writes the L2-normalized embedding into `outEmb`.
  // Returns false on any error.
  bool run(const uint8_t *rgb, size_t rgbSize, std::vector<float> &outEmb);

  bool ready() const { return mModel != nullptr; }

  int inputWidth() const { return kInW; }
  int inputHeight() const { return kInH; }

private:
  static void l2Normalize(std::vector<float> &v);

  // OSNet input geometry / quant (from contract).
  static constexpr int kInW = 128;
  static constexpr int kInH = 256;
  static constexpr float kInputQn = 32.0f;
  static constexpr float kOutputQn = 16.0f;
  static constexpr int kOutDim = 512;

  AraRuntime *mRt = nullptr;
  std::unique_ptr<AraModel> mModel;

  std::vector<int8_t> mInput;    // 3*256*128 int8 CHW
  std::vector<uint8_t> mOutput;  // 512 uint8
};
