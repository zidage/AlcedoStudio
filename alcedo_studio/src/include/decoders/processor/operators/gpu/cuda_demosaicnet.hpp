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

// Identity for a captured fixed-shape model forward (P3). Pointers are baked into
// the graph executable; any change requires Invalidate + recapture.
struct NeuralForwardGraphKey {
  DemosaicNetVariant variant             = DemosaicNetVariant::Bayer;
  int                device              = -1;
  int                input_h             = 0;
  int                input_w             = 0;
  int                output_h            = 0;
  int                output_w            = 0;
  const float*       input_data          = nullptr;
  float*             output_data         = nullptr;
  void*              activation_base     = nullptr;
  std::size_t        activation_capacity = 0;
  const float*       pack_weight         = nullptr;
  const float*       output_weight       = nullptr;
  // From DemosaicNetModelCache::WeightGeneration — survives CUDA address reuse.
  std::uint64_t      weight_generation   = 0;

  [[nodiscard]] auto Matches(const NeuralForwardGraphKey& other) const noexcept -> bool {
    return variant == other.variant && device == other.device && input_h == other.input_h &&
           input_w == other.input_w && output_h == other.output_h && output_w == other.output_w &&
           input_data == other.input_data && output_data == other.output_data &&
           activation_base == other.activation_base &&
           activation_capacity == other.activation_capacity &&
           pack_weight == other.pack_weight && output_weight == other.output_weight &&
           weight_generation == other.weight_generation;
  }
};

// Caller-owned CUDA Graph executable for one stable student model forward.
// Not process-global; one instance per NeuralDemosaicWorkspace. Capture excludes
// pack/unpack/ROI. Unsupported capture/runtime falls back to ordinary launches.
class NeuralDemosaicForwardGraph {
 public:
  NeuralDemosaicForwardGraph() = default;
  ~NeuralDemosaicForwardGraph();

  NeuralDemosaicForwardGraph(const NeuralDemosaicForwardGraph&)            = delete;
  NeuralDemosaicForwardGraph& operator=(const NeuralDemosaicForwardGraph&) = delete;
  NeuralDemosaicForwardGraph(NeuralDemosaicForwardGraph&&)                 = delete;
  NeuralDemosaicForwardGraph& operator=(NeuralDemosaicForwardGraph&&)      = delete;

  void Invalidate() noexcept;

  // Destroy any executable whose baked weight/activation pointers no longer match.
  void InvalidateIfKeyMismatch(const NeuralForwardGraphKey& key) noexcept;

  [[nodiscard]] auto ready() const noexcept -> bool { return exec_ != nullptr; }
  [[nodiscard]] auto capture_failed() const noexcept -> bool { return capture_failed_; }
  [[nodiscard]] auto capture_count() const noexcept -> std::uint64_t { return capture_count_; }
  [[nodiscard]] auto launch_count() const noexcept -> std::uint64_t { return launch_count_; }
  [[nodiscard]] auto key() const noexcept -> const NeuralForwardGraphKey& { return key_; }

  // When ready and key matches: launch and return true. Otherwise false (caller
  // should CaptureOrForward / ordinary forward).
  [[nodiscard]] auto TryLaunch(const NeuralForwardGraphKey& key, cudaStream_t stream) -> bool;

  // Capture model.Forward via ordinary_forward on `stream`, instantiate, then
  // launch once so the first call produces results (stream capture only records
  // work; it does not execute it). On failure falls back to ordinary launches.
  template <typename ForwardFn>
  void Capture(const NeuralForwardGraphKey& key, cudaStream_t stream, ForwardFn&& ordinary_forward) {
    Invalidate();
    key_ = key;
    if (stream == nullptr) {
      capture_failed_ = true;
      ordinary_forward();
      return;
    }
    const cudaError_t begin_err =
        cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal);
    if (begin_err != cudaSuccess) {
      capture_failed_ = true;
      (void)cudaGetLastError();
      ordinary_forward();
      return;
    }
    try {
      ordinary_forward();
    } catch (...) {
      AbortCapture(stream);
      capture_failed_ = true;
      throw;
    }
    cudaGraph_t graph = nullptr;
    const cudaError_t end_err = cudaStreamEndCapture(stream, &graph);
    if (end_err != cudaSuccess || graph == nullptr) {
      if (graph != nullptr) {
        (void)cudaGraphDestroy(graph);
      }
      capture_failed_ = true;
      (void)cudaGetLastError();
      // Work from the failed capture is discarded; re-run ordinarily.
      ordinary_forward();
      return;
    }
    cudaGraphExec_t exec = nullptr;
    const cudaError_t inst_err =
        cudaGraphInstantiate(&exec, graph, nullptr, nullptr, 0);
    (void)cudaGraphDestroy(graph);
    if (inst_err != cudaSuccess || exec == nullptr) {
      if (exec != nullptr) {
        (void)cudaGraphExecDestroy(exec);
      }
      capture_failed_ = true;
      (void)cudaGetLastError();
      ordinary_forward();
      return;
    }
    exec_ = exec;
    ++capture_count_;
    // Stream capture records but does not execute — launch the first instance now.
    const cudaError_t launch_err = cudaGraphLaunch(exec_, stream);
    if (launch_err != cudaSuccess) {
      Invalidate();
      capture_failed_ = true;
      (void)cudaGetLastError();
      ordinary_forward();
      return;
    }
  }

 private:
  void AbortCapture(cudaStream_t stream) noexcept;

  NeuralForwardGraphKey key_{};
  cudaGraphExec_t       exec_           = nullptr;
  bool                  capture_failed_ = false;
  std::uint64_t         capture_count_  = 0;
  std::uint64_t         launch_count_   = 0;
};

// Per-decode storage for Neural Engine tile forwards. This is deliberately owned by
// the caller rather than the model cache: weights are immutable and shared, while
// activations, NCHW boundary buffers, and the HWC tile output are stream-local.
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
  [[nodiscard]] auto output_buffer() noexcept -> cuda::nn::DeviceBufferF32& {
    return output_buffer_;
  }
  [[nodiscard]] auto rgb_buffer() noexcept -> cv::cuda::GpuMat& { return rgb_buffer_; }

  [[nodiscard]] auto forward_graph() noexcept -> NeuralDemosaicForwardGraph& {
    return forward_graph_;
  }
  [[nodiscard]] auto forward_graph() const noexcept -> const NeuralDemosaicForwardGraph& {
    return forward_graph_;
  }

  // Grow-only reservation for a single forward of the given CFA spatial size.
  // Increments `allocation_generation()` when any owned capacity actually grows.
  // Invalidates a captured forward graph when any owned pointer may change.
  void EnsureCapacity(DemosaicNetVariant variant, int height, int width, std::size_t input_numel,
                      std::size_t output_numel);

  [[nodiscard]] auto allocation_generation() const noexcept -> std::uint64_t {
    return allocation_generation_;
  }

  // Best-effort sum of owned device bytes (activation pool + NCHW buffers + RGB tile).
  // Treat as residual observability; do not use as a substitute for generation checks.
  [[nodiscard]] auto OwnedDeviceBytes() const -> std::size_t;

 private:
  cuda::nn::WorkspacePool     activation_workspace_;
  cuda::nn::DeviceBufferF32   input_buffer_;
  cuda::nn::DeviceBufferF32   output_buffer_;
  cv::cuda::GpuMat            rgb_buffer_;
  NeuralDemosaicForwardGraph  forward_graph_;
  std::uint64_t               allocation_generation_ = 0;
};

struct NeuralDemosaicOptions {
  // Null uses the process-wide lazy cache. Injection keeps load-failure and cache-reuse tests
  // independent from product-global state.
  DemosaicNetModelCache*   model_cache  = nullptr;
  DemosaicNetLoadOptions   load_options = {};
  // Optional per-decode storage. Supplying this avoids all per-tile CUDA allocations
  // after the first (largest) tile reserves its buffers.
  NeuralDemosaicWorkspace* workspace    = nullptr;
  // Product student owned-output edge (export tile). 0 selects the 1K control
  // policy (Bayer 1086→1024 / X-Trans 1048→1024). P2 candidates: 1024/1536/2048/3072.
  int student_owned_tile_edge = 0;
  // P3: capture/replay fixed-shape model Forward when workspace + non-null stream
  // are available. Default off — full-frame p50 did not clear the +5% retention
  // gate on the WDDM laptop path (P0 wall≈batch; launch amortization not material).
  // Set true for re-measure / future non-WDDM hosts. Env
  // ALCEDO_DEMOASICNET_DISABLE_CUDA_GRAPH also forces ordinary launches.
  bool enable_cuda_graph = false;
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
// enqueued work). Lazy-loads weights on first use. Enqueues pack → forward → unpack and
// returns without host/device synchronization. Soft-fail leaves `rgb` untouched.
auto EnqueueDemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                                     cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr,
                                     const NeuralDemosaicOptions& options = {})
    -> NeuralDemosaicResult;

// Product student tile: fused virtual-pad pack + fixed-shape student forward + HWC unpack.
// Tile geometry comes from `options.student_owned_tile_edge` (0 → 1024 control policy).
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
// `options.workspace`. Product tiled decode enqueues pack → forward → unpack → ROI copy
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
