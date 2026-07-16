//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include <opencv2/core/cuda.hpp>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {

// CUDA product-path sandwich for Neural Engine (not inside hard-coded Forward).
// Backend-neutral CFA alignment, gamma constants, and Reflect101 live in
// demosaicnet_preprocess_common.hpp; this header adds GpuMat execution.

struct NeuralEngineCfaPrep {
  bool          succeeded       = false;
  CfaAlignShift shift           = {};
  RawCfaPattern aligned_pattern = {};
  int           aligned_width   = 0;
  int           aligned_height  = 0;
  std::string   error;
};

// Product-path sandwich for Neural Engine:
//   1) CFA phase-align crop to training origin + trim to period
//   2) clone ROI so the original linear CFA stays intact for Legacy fallback
//   3) gamma encode x^(1/2.2) without saturating to [0,1]
// White balance is already applied by ToLinearRef; do not re-WB here.
//
// On success, `aligned_cfa` is a private CV_32FC1 buffer ready for pack/forward,
// and `aligned_pattern` is the fixed training pattern (origin GRBG / X-Trans target).
auto PrepareNeuralEngineCfa(const cv::cuda::GpuMat& linear_cfa, const RawCfaPattern& camera_pattern,
                            cv::cuda::GpuMat& aligned_cfa, cv::cuda::Stream* stream = nullptr)
    -> NeuralEngineCfaPrep;

// Gamma decode y^2.2 on CV_32FC3 RGB after the network (sign-preserving, no hard clip).
void FinishNeuralEngineRgb(cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr);

// Elementwise signed power on GpuMat (CV_32FC1 or CV_32FC3). Does not clamp to [0,1].
void GammaEncodeGpuMat(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);
void GammaDecodeGpuMat(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);

}  // namespace alcedo
