//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <opencv2/core/cuda.hpp>
#include <string>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo::CUDA {

// Per-decode storage for Neural Engine tile forwards. This is deliberately owned by
// the caller rather than the model cache: weights are immutable and shared, while
// activations, NCHW input boundary buffers, and the HWC tile output are stream-local.
// Not thread-safe; use one instance for each concurrent CUDA decode.
//
// `allocation_generation()` increments whenever an owned device allocation grows.
// After warm-up, timed hot-path iterations must keep the generation constant.
class NeuralDemosaicWorkspace {
 public:
  NeuralDemosaicWorkspace()                                          = default;

  NeuralDemosaicWorkspace(const NeuralDemosaicWorkspace&)            = delete;
  NeuralDemosaicWorkspace& operator=(const NeuralDemosaicWorkspace&) = delete;

  [[nodiscard]] auto       activation_workspace() noexcept -> cuda::nn::WorkspacePool& {
    return activation_workspace_;
  }
  [[nodiscard]] auto input_buffer() noexcept -> cuda::nn::DeviceBufferF32& { return input_buffer_; }
  [[nodiscard]] auto rgb_buffer() noexcept -> cv::cuda::GpuMat& { return rgb_buffer_; }

  // Grow-only reservation for a single forward of the given CFA spatial size.
  // Increments `allocation_generation()` when any owned capacity actually grows.
  void EnsureCapacity(DemosaicNetVariant variant, int height, int width, std::size_t input_numel);

  [[nodiscard]] auto allocation_generation() const noexcept -> std::uint64_t {
    return allocation_generation_;
  }

  // Best-effort sum of owned device bytes (activation pool + NCHW input + RGB tile).
  // Treat as residual observability; do not use as a substitute for generation checks.
  [[nodiscard]] auto OwnedDeviceBytes() const -> std::size_t;

 private:
  cuda::nn::WorkspacePool   activation_workspace_;
  cuda::nn::DeviceBufferF32 input_buffer_;
  cv::cuda::GpuMat          rgb_buffer_;
  std::uint64_t             allocation_generation_ = 0;
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

// Fused student tile pack: for each pixel of a fixed input tile with signed origin
// in the aligned CFA lattice, reflect-pad (OpenCV/NumPy reflect101), read the scalar
// CFA sample, place it in the RGB plane of the *reflected source* coordinate's CFA
// phase, and write contiguous NCHW [1,3,H,W]. Matches “pack full sparse mosaic,
// reflect-pad, then slice” without materializing either intermediate.
//
// `aligned_cfa` is the phase-aligned, period-trimmed linear CFA (CV_32FC1).
// `training_pattern` is the fixed training CFA pattern at the aligned origin (0,0).
// `input_origin` may be negative (virtual pad). `input_tensor` must be contiguous
// NCHW f32 with shape [1,3,tile_h,tile_w].
void PackReflectPaddedCfaTile(const cv::cuda::GpuMat& aligned_cfa, cv::Point input_origin,
                              const RawCfaPattern& training_pattern,
                              cuda::nn::DeviceTensor& input_tensor, int tile_h, int tile_w,
                              cv::cuda::Stream* stream = nullptr);

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
// Synchronous wrapper: enqueues then waits so `rgb` is complete when this returns.
auto DemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                              cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr,
                              const NeuralDemosaicOptions& options = {}) -> NeuralDemosaicResult;

// Asynchronous full-frame / free-size Neural Engine forward (Phase 8E).
// Requires `options.workspace` (caller owns input/activation/RGB buffers for the lifetime of
// enqueued work). Lazy-loads weights on first use. Enqueues pack → HWC channels-last forward
// and returns without host/device synchronization. Soft-fail leaves `rgb` untouched.
auto EnqueueDemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                                     cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr,
                                     const NeuralDemosaicOptions& options = {})
    -> NeuralDemosaicResult;

// Product student tile: fused virtual-pad pack + fixed 1024 owned export
// (Bayer 1086→1024 / X-Trans 1048→1024) + persistent NHWC fused HWC tail.
// `training_pattern` must be the phase-aligned training origin (from PrepareNeuralEngineCfa).
// `input_origin` is signed in the aligned CFA lattice (may be negative under virtual pad).
// On success, `rgb` references the workspace HWC buffer (owned²) when `options.workspace` is set;
// otherwise `rgb` owns a copy. Soft-fail leaves `rgb` untouched.
// Synchronous wrapper: enqueues then waits so `rgb` is complete when this returns.
auto DemosaicStudentTileWithNeuralEngine(const cv::cuda::GpuMat& aligned_cfa,
                                         cv::Point input_origin,
                                         const RawCfaPattern& training_pattern,
                                         cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr,
                                         const NeuralDemosaicOptions& options = {})
    -> NeuralDemosaicResult;

// Asynchronous student tile forward (Phase 8E). Same contract as
// DemosaicStudentTileWithNeuralEngine but never synchronizes. Requires
// `options.workspace`. Product tiled decode enqueues pack → forward → ROI copy
// per job on one stream, then waits once at the product completion boundary.
auto EnqueueDemosaicStudentTileWithNeuralEngine(const cv::cuda::GpuMat& aligned_cfa,
                                                cv::Point input_origin,
                                                const RawCfaPattern& training_pattern,
                                                cv::cuda::GpuMat& rgb,
                                                cv::cuda::Stream* stream = nullptr,
                                                const NeuralDemosaicOptions& options = {})
    -> NeuralDemosaicResult;

// Test/observability: host-side waits performed by the synchronous Neural Engine wrappers
// (not by Enqueue*). Reset to 0 before a timed/async assertion pass.
void ResetNeuralEngineHostSyncCountForTest();
[[nodiscard]] auto NeuralEngineHostSyncCountForTest() noexcept -> std::uint64_t;

}  // namespace alcedo::CUDA
