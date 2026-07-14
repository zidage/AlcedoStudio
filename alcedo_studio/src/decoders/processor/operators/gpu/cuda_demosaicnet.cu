//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>
#include <stdexcept>

#include "cuda/nn/common.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/cuda_tile_jobs.hpp"
#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_profiler.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"

namespace alcedo::CUDA {
namespace {

// Host-side wait counter for Phase 8E tests (Enqueue paths never touch this).
std::uint64_t g_neural_engine_host_sync_count = 0;

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  return stream == nullptr ? nullptr : cv::cuda::StreamAccessor::getStream(*stream);
}

void WaitForNeuralEngineCompletion(cv::cuda::Stream* stream, const char* sync_label) {
  ++g_neural_engine_host_sync_count;
  if (stream == nullptr) {
    cuda::nn::CheckCuda(cudaDeviceSynchronize(), sync_label);
  } else {
    stream->waitForCompletion();
  }
}

__global__ void PackCfaMosaicKernel(const cv::cuda::PtrStepSz<float> cfa, float* mosaic,
                                    const RawCfaPattern pattern) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= cfa.cols || y >= cfa.rows) {
    return;
  }

  const std::int64_t pixel  = static_cast<std::int64_t>(y) * cfa.cols + x;
  const std::int64_t plane  = static_cast<std::int64_t>(cfa.rows) * cfa.cols;
  const int          color  = RgbColorAt(pattern, y, x);
  const float        value  = cfa(y, x);
  mosaic[pixel]             = color == 0 ? value : 0.0F;
  mosaic[plane + pixel]     = color == 1 ? value : 0.0F;
  mosaic[2 * plane + pixel] = color == 2 ? value : 0.0F;
}

// Host/device-identical to alcedo::Reflect101 (demosaicnet_preprocess_common.hpp).
// Kept as a device function so kernels need no host linkage.
__device__ auto Reflect101(const int coordinate, const int limit) -> int {
  if (limit <= 1) {
    return 0;
  }
  int reflected = coordinate;
  while (reflected < 0 || reflected >= limit) {
    reflected = reflected < 0 ? -reflected : 2 * limit - reflected - 2;
  }
  return reflected;
}

__global__ void CopyReflectPaddedCfaKernel(const cv::cuda::PtrStepSz<float> source,
                                           cv::cuda::PtrStepSz<float>       destination,
                                           const int source_x, const int source_y) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= destination.cols || y >= destination.rows) {
    return;
  }
  destination(y, x) =
      source(Reflect101(source_y + y, source.rows), Reflect101(source_x + x, source.cols));
}

void PackCfaMosaic(const cv::cuda::GpuMat& cfa, cuda::nn::DeviceTensor& mosaic,
                   const RawCfaPattern& pattern, const cudaStream_t stream) {
  const dim3 block(16, 16);
  const dim3 grid((cfa.cols + block.x - 1) / block.x, (cfa.rows + block.y - 1) / block.y);
  PackCfaMosaicKernel<<<grid, block, 0, stream>>>(cfa, mosaic.data, pattern);
  cuda::nn::CheckCuda(cudaGetLastError(), "PackCfaMosaicKernel launch");
}

template <typename Model>
void RunModelHwc(const Model& model, const cuda::nn::DeviceTensor& input, cv::cuda::GpuMat& rgb,
                 cuda::nn::WorkspacePool& workspace, const cudaStream_t stream,
                 const bool apply_gamma_decode) {
  workspace.Reserve(Model::EstimateWorkspaceBytes(static_cast<int>(input.shape[2]),
                                                  static_cast<int>(input.shape[3]), 1));
  model.ForwardHwcChannelsLast(input, reinterpret_cast<float*>(rgb.ptr()),
                               static_cast<std::size_t>(rgb.step), workspace, stream,
                               apply_gamma_decode);
}

// Fused reflect + sparse NCHW pack. Phase is taken from the reflected *source*
// coordinate so color-preserving X-Trans space-to-depth packs match the handoff.
__global__ void PackReflectPaddedCfaTileKernel(const cv::cuda::PtrStepSz<float> cfa,
                                               float* mosaic, const int origin_x,
                                               const int origin_y, const int tile_h,
                                               const int tile_w, const RawCfaPattern pattern) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= tile_w || y >= tile_h) {
    return;
  }

  const int   src_x = Reflect101(origin_x + x, cfa.cols);
  const int   src_y = Reflect101(origin_y + y, cfa.rows);
  const int   color = RgbColorAt(pattern, src_y, src_x);
  const float value = cfa(src_y, src_x);

  const std::int64_t pixel = static_cast<std::int64_t>(y) * tile_w + x;
  const std::int64_t plane = static_cast<std::int64_t>(tile_h) * tile_w;
  mosaic[pixel]             = color == 0 ? value : 0.0F;
  mosaic[plane + pixel]     = color == 1 ? value : 0.0F;
  mosaic[2 * plane + pixel] = color == 2 ? value : 0.0F;
}

}  // namespace

void NeuralDemosaicWorkspace::EnsureCapacity(const DemosaicNetVariant variant, const int height,
                                             const int width, const std::size_t input_numel) {
  bool grew = false;

  if (input_buffer_.size() < input_numel) {
    input_buffer_ = cuda::nn::DeviceBufferF32(input_numel);
    grew          = true;
  }

  const std::size_t activation_bytes =
      variant == DemosaicNetVariant::Bayer
          ? BayerDemosaicNet::EstimateWorkspaceBytes(height, width, 1)
          : XTransDemosaicNet::EstimateWorkspaceBytes(height, width, 1);
  const std::size_t prev_activation = activation_workspace_.capacity_bytes();
  activation_workspace_.Reserve(activation_bytes);
  if (activation_workspace_.capacity_bytes() > prev_activation) {
    grew = true;
  }

  const int output_height = variant == DemosaicNetVariant::Bayer
                                ? BayerDemosaicNet::OutputHeight(height, width)
                                : XTransDemosaicNet::OutputHeight(height, width);
  const int output_width = variant == DemosaicNetVariant::Bayer
                               ? BayerDemosaicNet::OutputWidth(width, height)
                               : XTransDemosaicNet::OutputWidth(width, height);
  // Grow-only RGB capacity (ragged edges reuse the max 1K workspace without
  // shrinking/reallocating when a boundary job is smaller than an interior job).
  const int prev_rows = rgb_buffer_.rows;
  const int prev_cols = rgb_buffer_.cols;
  const int prev_type = rgb_buffer_.type();
  if (rgb_buffer_.empty() || prev_type != CV_32FC3 || prev_rows < output_height ||
      prev_cols < output_width) {
    const int new_h = std::max(prev_rows > 0 && prev_type == CV_32FC3 ? prev_rows : 0,
                               output_height);
    const int new_w = std::max(prev_cols > 0 && prev_type == CV_32FC3 ? prev_cols : 0,
                               output_width);
    rgb_buffer_.create(new_h, new_w, CV_32FC3);
    grew = true;
  }

  if (grew) {
    ++allocation_generation_;
  }
}

auto NeuralDemosaicWorkspace::OwnedDeviceBytes() const -> std::size_t {
  std::size_t total = activation_workspace_.capacity_bytes() + input_buffer_.bytes();
  if (!rgb_buffer_.empty()) {
    total += static_cast<std::size_t>(rgb_buffer_.rows) * static_cast<std::size_t>(rgb_buffer_.step);
  }
  return total;
}

void CopyReflectPaddedCfa(const cv::cuda::GpuMat& source, const cv::Rect& source_rect,
                          cv::cuda::GpuMat& destination, cv::cuda::Stream* stream) {
  if (source.empty() || source.type() != CV_32FC1 || destination.empty() ||
      destination.type() != CV_32FC1) {
    throw std::runtime_error("CopyReflectPaddedCfa requires non-empty CV_32FC1 matrices");
  }
  const dim3 block(16, 16);
  const dim3 grid((destination.cols + block.x - 1) / block.x,
                  (destination.rows + block.y - 1) / block.y);
  CopyReflectPaddedCfaKernel<<<grid, block, 0, GetCudaStream(stream)>>>(
      source, destination, source_rect.x, source_rect.y);
  cuda::nn::CheckCuda(cudaGetLastError(), "CopyReflectPaddedCfaKernel launch");
}

void PackReflectPaddedCfaTile(const cv::cuda::GpuMat& aligned_cfa, const cv::Point input_origin,
                              const RawCfaPattern& training_pattern,
                              cuda::nn::DeviceTensor& input_tensor, const int tile_h,
                              const int tile_w, cv::cuda::Stream* stream) {
  if (aligned_cfa.empty() || aligned_cfa.type() != CV_32FC1) {
    throw std::runtime_error("PackReflectPaddedCfaTile requires non-empty CV_32FC1 CFA");
  }
  if (tile_h <= 0 || tile_w <= 0) {
    throw std::runtime_error("PackReflectPaddedCfaTile: invalid tile size");
  }
  if (input_tensor.rank != 4 || input_tensor.data == nullptr || !input_tensor.IsContiguous()) {
    throw std::runtime_error("PackReflectPaddedCfaTile: input_tensor must be contiguous NCHW");
  }
  if (input_tensor.shape[0] != 1 || input_tensor.shape[1] != 3 ||
      input_tensor.shape[2] != tile_h || input_tensor.shape[3] != tile_w) {
    throw std::runtime_error("PackReflectPaddedCfaTile: input_tensor shape must be [1,3,H,W]");
  }

  const dim3 block(16, 16);
  const dim3 grid((tile_w + block.x - 1) / block.x, (tile_h + block.y - 1) / block.y);
  PackReflectPaddedCfaTileKernel<<<grid, block, 0, GetCudaStream(stream)>>>(
      aligned_cfa, input_tensor.data, input_origin.x, input_origin.y, tile_h, tile_w,
      training_pattern);
  cuda::nn::CheckCuda(cudaGetLastError(), "PackReflectPaddedCfaTileKernel launch");
}

void ResetNeuralEngineHostSyncCountForTest() { g_neural_engine_host_sync_count = 0; }

auto NeuralEngineHostSyncCountForTest() noexcept -> std::uint64_t {
  return g_neural_engine_host_sync_count;
}

auto EnqueueDemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                                     cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream,
                                     const NeuralDemosaicOptions& options) -> NeuralDemosaicResult {
  NeuralDemosaicResult result;
  try {
    if (options.workspace == nullptr) {
      result.error = "EnqueueDemosaicWithNeuralEngine requires options.workspace "
                     "(async buffers must outlive enqueued work)";
      return result;
    }
    if (cfa.empty() || cfa.type() != CV_32FC1) {
      throw std::runtime_error("Neural Engine requires a non-empty CV_32FC1 CFA image");
    }

    cv::cuda::GpuMat model_input = cfa;
    if (pattern.kind == RawCfaKind::Bayer2x2) {
      // The Bayer student starts with a phase-sensitive 2x2/stride-2 pack. Preserve the
      // top-left CFA phase and discard only an unmatched trailing row or column.
      const int even_width  = cfa.cols & ~1;
      const int even_height = cfa.rows & ~1;
      if (even_width < BayerDemosaicNet::kMinSpatial ||
          even_height < BayerDemosaicNet::kMinSpatial) {
        throw std::runtime_error("Neural Engine Bayer input is smaller than student minimum");
      }
      model_input = cfa(cv::Rect(0, 0, even_width, even_height));
    } else {
      const int even_width  = cfa.cols & ~1;
      const int even_height = cfa.rows & ~1;
      if (even_width < XTransDemosaicNet::kMinSpatial ||
          even_height < XTransDemosaicNet::kMinSpatial) {
        throw std::runtime_error("Neural Engine X-Trans input is smaller than student minimum");
      }
      // Space-to-depth pack requires pack_factor-aligned spatial size.
      model_input = cfa(cv::Rect(0, 0, even_width, even_height));
    }

    const int         height      = model_input.rows;
    const int         width       = model_input.cols;
    const std::size_t input_numel = static_cast<std::size_t>(3) * height * width;
    const int         output_height =
        pattern.kind == RawCfaKind::Bayer2x2 ? BayerDemosaicNet::OutputHeight(height, width)
                                             : XTransDemosaicNet::OutputHeight(height, width);
    const int output_width =
        pattern.kind == RawCfaKind::Bayer2x2 ? BayerDemosaicNet::OutputWidth(width, height)
                                             : XTransDemosaicNet::OutputWidth(width, height);
    if (output_height <= 0 || output_width <= 0) {
      throw std::runtime_error("Neural Engine input produces empty RGB output");
    }
    // Symmetric border used by crop mappers (exact for even natural/export shrink).
    result.source_border = (height - output_height) / 2;

    const cudaStream_t cuda_stream = GetCudaStream(stream);

    DemosaicNetModelCache& cache =
        options.model_cache == nullptr ? DemosaicNetModelCache::Instance() : *options.model_cache;
    DemosaicNetLoadOptions load_options = options.load_options;
    load_options.stream                 = cuda_stream;
    const DemosaicNetVariant variant    = pattern.kind == RawCfaKind::XTrans6x6
                                              ? DemosaicNetVariant::XTrans
                                              : DemosaicNetVariant::Bayer;
    if (!cache.EnsureLoaded(variant, load_options)) {
      result.error = cache.LastError();
      return result;
    }

    NeuralDemosaicWorkspace& workspace = *options.workspace;
    workspace.EnsureCapacity(variant, height, width, input_numel);
    auto input_tensor =
        cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {1, 3, height, width});
    PackCfaMosaic(model_input, input_tensor, pattern, cuda_stream);

    cv::cuda::GpuMat& neural_rgb = workspace.rgb_buffer();
    // Free-size path keeps gamma outside (FinishNeuralEngineRgb) so callers that
    // only demosaic a patch without the RAW finish still see network-space RGB
    // when they skip Finish.
    if (variant == DemosaicNetVariant::Bayer) {
      RunModelHwc(cache.Bayer(), input_tensor, neural_rgb, workspace.activation_workspace(),
                  cuda_stream, /*apply_gamma_decode=*/false);
    } else {
      RunModelHwc(cache.XTrans(), input_tensor, neural_rgb, workspace.activation_workspace(),
                  cuda_stream, /*apply_gamma_decode=*/false);
    }
    // No host wait: caller owns workspace for the lifetime of enqueued work.
    rgb              = neural_rgb;
    result.succeeded = true;
    result.error.clear();
    return result;
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }
}

auto DemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                              cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream,
                              const NeuralDemosaicOptions& options) -> NeuralDemosaicResult {
  NeuralDemosaicWorkspace  local_workspace;
  NeuralDemosaicOptions    opts = options;
  if (opts.workspace == nullptr) {
    opts.workspace = &local_workspace;
  }
  const auto result = EnqueueDemosaicWithNeuralEngine(cfa, pattern, rgb, stream, opts);
  if (!result.succeeded) {
    return result;
  }
  try {
    WaitForNeuralEngineCompletion(stream, "DemosaicWithNeuralEngine sync");
  } catch (const std::exception& e) {
    NeuralDemosaicResult failed;
    failed.error = e.what();
    return failed;
  }
  // When the caller did not supply a workspace, transfer RGB ownership out of the local one
  // (GpuMat is refcounted; move keeps a unique owner after local_workspace dies).
  if (options.workspace == nullptr) {
    rgb = std::move(local_workspace.rgb_buffer());
  }
  return result;
}

auto EnqueueDemosaicStudentTileWithNeuralEngine(const cv::cuda::GpuMat& aligned_cfa,
                                                const cv::Point input_origin,
                                                const RawCfaPattern& training_pattern,
                                                cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream,
                                                const NeuralDemosaicOptions& options)
    -> NeuralDemosaicResult {
  NeuralDemosaicResult result;
  try {
    if (options.workspace == nullptr) {
      result.error = "EnqueueDemosaicStudentTileWithNeuralEngine requires options.workspace "
                     "(async buffers must outlive enqueued work)";
      return result;
    }
    if (aligned_cfa.empty() || aligned_cfa.type() != CV_32FC1) {
      throw std::runtime_error("Student tile Neural Engine requires non-empty CV_32FC1 CFA");
    }

    const bool is_xtrans = training_pattern.kind == RawCfaKind::XTrans6x6;
    const detail::CudaTilePolicy policy =
        is_xtrans ? detail::MakeXTransStudentTilePolicy() : detail::MakeBayerStudentTilePolicy();
    const int tile_h = policy.input_tile.height;
    const int tile_w = policy.input_tile.width;
    const int out_h  = policy.output_tile.height;
    const int out_w  = policy.output_tile.width;
    result.source_border = policy.output_border.x;

    const int period = policy.cfa_period;
    if ((input_origin.x % period) != 0 || (input_origin.y % period) != 0) {
      throw std::runtime_error(
          "Student tile input origin is not CFA-period aligned (phase-unsafe)");
    }

    const std::size_t input_numel  = static_cast<std::size_t>(3) * tile_h * tile_w;
    const cudaStream_t cuda_stream = GetCudaStream(stream);

    DemosaicNetModelCache& cache =
        options.model_cache == nullptr ? DemosaicNetModelCache::Instance() : *options.model_cache;
    DemosaicNetLoadOptions load_options = options.load_options;
    load_options.stream                 = cuda_stream;
    const DemosaicNetVariant variant =
        is_xtrans ? DemosaicNetVariant::XTrans : DemosaicNetVariant::Bayer;
    if (!cache.EnsureLoaded(variant, load_options)) {
      result.error = cache.LastError();
      return result;
    }

    NeuralDemosaicWorkspace& workspace = *options.workspace;
    workspace.EnsureCapacity(variant, tile_h, tile_w, input_numel);

    auto input_tensor =
        cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {1, 3, tile_h, tile_w});

    DemosaicNetProfiler* profiler = ActiveDemosaicNetProfiler();
    if (profiler != nullptr) {
      profiler->NoteTile();
      profiler->BeginRange(DemosaicNetProfileRange::ReflectPadPack, cuda_stream);
    }
    PackReflectPaddedCfaTile(aligned_cfa, input_origin, training_pattern, input_tensor, tile_h,
                             tile_w, stream);
    if (profiler != nullptr) {
      profiler->EndRange(DemosaicNetProfileRange::ReflectPadPack, cuda_stream);
    }

    cv::cuda::GpuMat& neural_rgb = workspace.rgb_buffer();
    // Capacity may be larger than this job's owned export (ragged edges).
    // Forward writes only the active out_h×out_w region using the full row pitch.
    if (neural_rgb.rows < out_h || neural_rgb.cols < out_w) {
      throw std::runtime_error("Student tile RGB buffer smaller than owned export");
    }
    cv::cuda::GpuMat tile_rgb_view = neural_rgb(cv::Rect(0, 0, out_w, out_h));
    if (variant == DemosaicNetVariant::Bayer) {
      RunModelHwc(cache.Bayer(), input_tensor, tile_rgb_view, workspace.activation_workspace(),
                  cuda_stream, /*apply_gamma_decode=*/true);
    } else {
      RunModelHwc(cache.XTrans(), input_tensor, tile_rgb_view, workspace.activation_workspace(),
                  cuda_stream, /*apply_gamma_decode=*/true);
    }
    // Product tiled path reuses workspace RGB without a per-tile stream wait; stream
    // ordering serializes pack → forward → ROI copy.
    // Return the job-sized view so model_output_roi is relative to owned_w×owned_h.
    rgb              = tile_rgb_view;
    result.succeeded = true;
    result.error.clear();
    return result;
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }
}

auto DemosaicStudentTileWithNeuralEngine(const cv::cuda::GpuMat& aligned_cfa,
                                         const cv::Point input_origin,
                                         const RawCfaPattern& training_pattern,
                                         cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream,
                                         const NeuralDemosaicOptions& options)
    -> NeuralDemosaicResult {
  NeuralDemosaicWorkspace  local_workspace;
  NeuralDemosaicOptions    opts = options;
  if (opts.workspace == nullptr) {
    opts.workspace = &local_workspace;
  }
  const auto result =
      EnqueueDemosaicStudentTileWithNeuralEngine(aligned_cfa, input_origin, training_pattern, rgb,
                                                 stream, opts);
  if (!result.succeeded) {
    return result;
  }
  try {
    WaitForNeuralEngineCompletion(stream, "DemosaicStudentTileWithNeuralEngine sync");
  } catch (const std::exception& e) {
    NeuralDemosaicResult failed;
    failed.error = e.what();
    return failed;
  }
  if (options.workspace == nullptr) {
    rgb = std::move(local_workspace.rgb_buffer());
  }
  return result;
}

}  // namespace alcedo::CUDA
