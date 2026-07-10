//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>
#include <stdexcept>

#include "cuda/nn/common.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/layout.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"

namespace alcedo::CUDA {
namespace {

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  return stream == nullptr ? nullptr : cv::cuda::StreamAccessor::getStream(*stream);
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
void RunModel(const Model& model, const cuda::nn::DeviceTensor& input,
              cuda::nn::DeviceTensor& output, cuda::nn::WorkspacePool& workspace,
              const cudaStream_t stream) {
  workspace.Reserve(Model::EstimateWorkspaceBytes(static_cast<int>(input.shape[2]),
                                                  static_cast<int>(input.shape[3]), 1));
  model.Forward(input, output, workspace, stream);
}

void ReserveWorkspace(NeuralDemosaicWorkspace& workspace, const DemosaicNetVariant variant,
                      const int height, const int width, const std::size_t input_numel,
                      const std::size_t output_numel) {
  if (workspace.input_buffer().size() < input_numel) {
    workspace.input_buffer() = cuda::nn::DeviceBufferF32(input_numel);
  }
  if (workspace.output_buffer().size() < output_numel) {
    workspace.output_buffer() = cuda::nn::DeviceBufferF32(output_numel);
  }

  const std::size_t activation_bytes =
      variant == DemosaicNetVariant::Bayer
          ? BayerDemosaicNet::EstimateWorkspaceBytes(height, width, 1)
          : XTransDemosaicNet::EstimateWorkspaceBytes(height, width, 1);
  workspace.activation_workspace().Reserve(activation_bytes);

  const int output_height =
      height - (variant == DemosaicNetVariant::Bayer ? BayerDemosaicNet::kSpatialLoss
                                                     : XTransDemosaicNet::kSpatialLoss);
  const int output_width =
      width - (variant == DemosaicNetVariant::Bayer ? BayerDemosaicNet::kSpatialLoss
                                                    : XTransDemosaicNet::kSpatialLoss);
  workspace.rgb_buffer().create(output_height, output_width, CV_32FC3);
}

}  // namespace

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

auto DemosaicWithNeuralEngine(const cv::cuda::GpuMat& cfa, const RawCfaPattern& pattern,
                              cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream,
                              const NeuralDemosaicOptions& options) -> NeuralDemosaicResult {
  NeuralDemosaicResult result;
  try {
    if (cfa.empty() || cfa.type() != CV_32FC1) {
      throw std::runtime_error("Neural Engine requires a non-empty CV_32FC1 CFA image");
    }

    cv::cuda::GpuMat model_input = cfa;
    if (pattern.kind == RawCfaKind::Bayer2x2) {
      // The Bayer model starts with a phase-sensitive 2x2/stride-2 pack. Preserve the top-left
      // CFA phase and discard only an unmatched trailing row or column.
      const int even_width  = cfa.cols & ~1;
      const int even_height = cfa.rows & ~1;
      if (even_width < BayerDemosaicNet::kMinSpatial ||
          even_height < BayerDemosaicNet::kMinSpatial) {
        throw std::runtime_error("Neural Engine Bayer input is smaller than 64x64");
      }
      model_input          = cfa(cv::Rect(0, 0, even_width, even_height));
      result.source_border = BayerDemosaicNet::kSpatialLoss / 2;
    } else {
      if (cfa.cols < XTransDemosaicNet::kMinSpatial || cfa.rows < XTransDemosaicNet::kMinSpatial) {
        throw std::runtime_error("Neural Engine X-Trans input is smaller than 26x26");
      }
      result.source_border = XTransDemosaicNet::kSpatialLoss / 2;
    }

    const int          height        = model_input.rows;
    const int          width         = model_input.cols;
    const std::size_t  input_numel   = static_cast<std::size_t>(3) * height * width;
    const int          output_height = height - 2 * result.source_border;
    const int          output_width  = width - 2 * result.source_border;
    const std::size_t  output_numel  = static_cast<std::size_t>(3) * output_height * output_width;

    const cudaStream_t cuda_stream   = GetCudaStream(stream);

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

    NeuralDemosaicWorkspace  local_workspace;
    NeuralDemosaicWorkspace& workspace =
        options.workspace == nullptr ? local_workspace : *options.workspace;
    ReserveWorkspace(workspace, variant, height, width, input_numel, output_numel);
    auto input_tensor =
        cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {1, 3, height, width});
    auto output_tensor = cuda::nn::DeviceTensor::Contiguous(workspace.output_buffer().data(),
                                                            {1, 3, output_height, output_width});
    PackCfaMosaic(model_input, input_tensor, pattern, cuda_stream);

    if (variant == DemosaicNetVariant::Bayer) {
      RunModel(cache.Bayer(), input_tensor, output_tensor, workspace.activation_workspace(),
               cuda_stream);
    } else {
      RunModel(cache.XTrans(), input_tensor, output_tensor, workspace.activation_workspace(),
               cuda_stream);
    }

    cv::cuda::GpuMat& neural_rgb = workspace.rgb_buffer();
    cuda::nn::UnpackNchwToHwc(output_tensor, neural_rgb, cuda_stream);
    if (stream == nullptr) {
      cuda::nn::CheckCuda(cudaDeviceSynchronize(), "DemosaicWithNeuralEngine sync");
    } else {
      stream->waitForCompletion();
    }
    rgb              = options.workspace == nullptr ? std::move(neural_rgb) : neural_rgb;
    result.succeeded = true;
    result.error.clear();
    return result;
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }
}

}  // namespace alcedo::CUDA
