//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/cuda.hpp>
#include <string>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo::CUDA {

// Per-decode storage for Neural Engine tile forwards. This is deliberately owned by
// the caller rather than the model cache: weights are immutable and shared, while
// activations, NCHW boundary buffers, and the HWC tile output are stream-local.
// Not thread-safe; use one instance for each concurrent CUDA decode.
class NeuralDemosaicWorkspace {
 public:
  NeuralDemosaicWorkspace()                                          = default;

  NeuralDemosaicWorkspace(const NeuralDemosaicWorkspace&)            = delete;
  NeuralDemosaicWorkspace& operator=(const NeuralDemosaicWorkspace&) = delete;

  [[nodiscard]] auto       activation_workspace() noexcept -> cuda::nn::WorkspacePool& {
    return activation_workspace_;
  }
  [[nodiscard]] auto input_buffer() noexcept -> cuda::nn::DeviceBufferF32& { return input_buffer_; }
  [[nodiscard]] auto output_buffer() noexcept -> cuda::nn::DeviceBufferF32& {
    return output_buffer_;
  }
  [[nodiscard]] auto rgb_buffer() noexcept -> cv::cuda::GpuMat& { return rgb_buffer_; }

 private:
  cuda::nn::WorkspacePool   activation_workspace_;
  cuda::nn::DeviceBufferF32 input_buffer_;
  cuda::nn::DeviceBufferF32 output_buffer_;
  cv::cuda::GpuMat          rgb_buffer_;
};

struct NeuralDemosaicOptions {
  // Null uses the process-wide lazy cache. Injection keeps load-failure and cache-reuse tests
  // independent from product-global state.
  DemosaicNetModelCache*   model_cache  = nullptr;
  DemosaicNetLoadOptions   load_options = {};
  // Optional per-decode storage. Supplying this avoids all per-tile CUDA allocations
  // after the first (largest) tile reserves its buffers.
  NeuralDemosaicWorkspace* workspace    = nullptr;
};

// Fill `destination` with a reflected window of `source`. `source_rect` is in source
// coordinates and may extend beyond its bounds. The function is used to give edge
// tiles the same fixed shape as interior Neural Engine tiles.
void CopyReflectPaddedCfa(const cv::cuda::GpuMat& source, const cv::Rect& source_rect,
                          cv::cuda::GpuMat& destination, cv::cuda::Stream* stream = nullptr);

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
