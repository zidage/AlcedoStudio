//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>
#include <string>

#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo::CUDA {

struct NeuralDemosaicOptions {
  // Null uses the process-wide lazy cache. Injection keeps load-failure and cache-reuse tests
  // independent from product-global state.
  DemosaicNetModelCache* model_cache  = nullptr;
  DemosaicNetLoadOptions load_options = {};
};

struct NeuralDemosaicResult {
  bool        succeeded     = false;
  int         source_border = 0;
  std::string error;
};

// Convert a linear single-channel CFA GpuMat into the sparse three-channel mosaic convention
// used to train DemosaicNet, lazy-load the matching Bayer/X-Trans module, run its fixed forward,
// and return interleaved CV_32FC3 RGB. The input is never modified. Bayer inputs with odd
// dimensions drop the trailing row/column so the 2x2 pack remains phase-aligned.
//
// Failure is soft: `rgb` is left untouched and the caller can run Legacy demosaic instead.
auto DemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                              cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr,
                              const NeuralDemosaicOptions& options = {}) -> NeuralDemosaicResult;

}  // namespace alcedo::CUDA
