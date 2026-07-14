//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/demosaicnet_preprocess.hpp"

#include <cuda_runtime.h>

#include <cmath>
#include <stdexcept>
#include <string>

#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda_types.hpp>

#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

namespace alcedo {
namespace {

auto GetCudaStream(cv::cuda::Stream* stream) -> cudaStream_t {
  return stream == nullptr ? nullptr : cv::cuda::StreamAccessor::getStream(*stream);
}

__device__ auto PowSignedDevice(const float x, const float g) -> float {
  if (x == 0.0F) {
    return 0.0F;
  }
  return copysignf(powf(fabsf(x), g), x);
}

__global__ void PowSignedKernelGray(cv::cuda::PtrStepSz<float> img, const float gamma) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= img.cols || y >= img.rows) {
    return;
  }
  img(y, x) = PowSignedDevice(img(y, x), gamma);
}

__global__ void PowSignedKernelRgb(cv::cuda::PtrStepSz<float3> img, const float gamma) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= img.cols || y >= img.rows) {
    return;
  }
  float3 v = img(y, x);
  v.x      = PowSignedDevice(v.x, gamma);
  v.y      = PowSignedDevice(v.y, gamma);
  v.z      = PowSignedDevice(v.z, gamma);
  img(y, x) = v;
}

void LaunchPowSigned(cv::cuda::GpuMat& img, const float gamma, cv::cuda::Stream* stream) {
  if (img.empty()) {
    return;
  }
  if (img.type() != CV_32FC1 && img.type() != CV_32FC3) {
    throw std::runtime_error("Neural Engine gamma: only CV_32FC1/CV_32FC3 are supported");
  }

  const dim3 block(16, 16);
  const dim3 grid((img.cols + block.x - 1) / block.x, (img.rows + block.y - 1) / block.y);
  const cudaStream_t cuda_stream = GetCudaStream(stream);

  if (img.type() == CV_32FC1) {
    PowSignedKernelGray<<<grid, block, 0, cuda_stream>>>(img, gamma);
  } else {
    PowSignedKernelRgb<<<grid, block, 0, cuda_stream>>>(img, gamma);
  }
  const cudaError_t err = cudaGetLastError();
  if (err != cudaSuccess) {
    throw std::runtime_error(std::string("Neural Engine gamma kernel: ") + cudaGetErrorString(err));
  }
}

}  // namespace

void GammaEncodeGpuMat(cv::cuda::GpuMat& img, cv::cuda::Stream* stream) {
  LaunchPowSigned(img, kDemosaicNetGammaEncode, stream);
}

void GammaDecodeGpuMat(cv::cuda::GpuMat& img, cv::cuda::Stream* stream) {
  LaunchPowSigned(img, kDemosaicNetGammaDecode, stream);
}

void FinishNeuralEngineRgb(cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream) {
  GammaDecodeGpuMat(rgb, stream);
}

auto PrepareNeuralEngineCfa(const cv::cuda::GpuMat& linear_cfa, const RawCfaPattern& camera_pattern,
                            cv::cuda::GpuMat& aligned_cfa, cv::cuda::Stream* stream)
    -> NeuralEngineCfaPrep {
  NeuralEngineCfaPrep prep;
  try {
    if (linear_cfa.empty() || linear_cfa.type() != CV_32FC1) {
      prep.error = "Neural Engine preprocess requires a non-empty CV_32FC1 CFA image";
      return prep;
    }

    const int min_spatial = camera_pattern.kind == RawCfaKind::XTrans6x6
                                ? XTransDemosaicNet::kMinSpatial
                                : BayerDemosaicNet::kMinSpatial;
    std::string geo_error;
    const auto geo = ComputeNeuralAlignedGeometry(camera_pattern, linear_cfa.cols, linear_cfa.rows,
                                                  min_spatial, &geo_error);
    if (!geo.has_value()) {
      prep.error = geo_error;
      return prep;
    }
    prep.shift = CfaAlignShift{geo->shift_sy, geo->shift_sx};

    // Clone the ROI so gamma encode never mutates the original linear buffer (Legacy fallback).
    const int aligned_w = geo->aligned_width;
    const int aligned_h = geo->aligned_height;
    const cv::Rect roi(prep.shift.sx, prep.shift.sy, aligned_w, aligned_h);
    if (stream == nullptr) {
      linear_cfa(roi).copyTo(aligned_cfa);
    } else {
      linear_cfa(roi).copyTo(aligned_cfa, *stream);
    }

    prep.aligned_pattern = DemosaicNetTrainingPattern(camera_pattern.kind);
    prep.aligned_width   = aligned_w;
    prep.aligned_height  = aligned_h;

    GammaEncodeGpuMat(aligned_cfa, stream);

    prep.succeeded = true;
    prep.error.clear();
    return prep;
  } catch (const std::exception& e) {
    prep.succeeded = false;
    prep.error     = e.what();
    return prep;
  }
}

}  // namespace alcedo
