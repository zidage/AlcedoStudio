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

}  // namespace

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

    const int         height        = model_input.rows;
    const int         width         = model_input.cols;
    const std::size_t input_numel   = static_cast<std::size_t>(3) * height * width;
    const int         output_height = height - 2 * result.source_border;
    const int         output_width  = width - 2 * result.source_border;
    const std::size_t output_numel  = static_cast<std::size_t>(3) * output_height * output_width;

    cuda::nn::DeviceBufferF32 input_buffer(input_numel);
    cuda::nn::DeviceBufferF32 output_buffer(output_numel);
    auto                      input_tensor = input_buffer.AsTensor({1, 3, height, width});
    auto               output_tensor = output_buffer.AsTensor({1, 3, output_height, output_width});
    const cudaStream_t cuda_stream   = GetCudaStream(stream);
    PackCfaMosaic(model_input, input_tensor, pattern, cuda_stream);

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

    cuda::nn::WorkspacePool workspace;
    if (variant == DemosaicNetVariant::Bayer) {
      RunModel(cache.Bayer(), input_tensor, output_tensor, workspace, cuda_stream);
    } else {
      RunModel(cache.XTrans(), input_tensor, output_tensor, workspace, cuda_stream);
    }

    cv::cuda::GpuMat neural_rgb;
    cuda::nn::UnpackNchwToHwc(output_tensor, neural_rgb, cuda_stream);
    if (stream == nullptr) {
      cuda::nn::CheckCuda(cudaDeviceSynchronize(), "DemosaicWithNeuralEngine sync");
    } else {
      stream->waitForCompletion();
    }
    rgb              = std::move(neural_rgb);
    result.succeeded = true;
    result.error.clear();
    return result;
  } catch (const std::exception& e) {
    result.error = e.what();
    return result;
  }
}

}  // namespace alcedo::CUDA
