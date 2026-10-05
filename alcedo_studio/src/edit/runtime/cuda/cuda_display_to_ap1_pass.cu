//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <algorithm>
#include <stdexcept>
#include <string>

#include "edit/runtime/cuda/cuda_display_to_ap1_pass.hpp"
#include "edit/runtime/display_to_ap1_math.h"

namespace alcedo {
namespace {

void CheckCuda(cudaError_t status, const char* what) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string("CUDA DisplayToAp1: ") + what + ": " +
                             ::cudaGetErrorString(status));
  }
}

__global__ void DisplayToAp1AcesccKernel(const float4* input, float4* output,
                                         std::uint32_t pixel_count, const float* params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) {
    return;
  }
  const float4    source = input[index];
  const D2aFloat3 result = D2aSourceToAcesccAp1(D2aMake3(source.x, source.y, source.z), params);
  output[index]          = make_float4(result.x, result.y, result.z, source.w);
}

__global__ void DisplayToAp0LinearKernel(const float4* input, float4* output,
                                         std::uint32_t pixel_count, const float* params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) {
    return;
  }
  const float4    source = input[index];
  const D2aFloat3 result = D2aDisplayToAp0(D2aMake3(source.x, source.y, source.z), params);
  output[index]          = make_float4(result.x, result.y, result.z, source.w);
}

}  // namespace

CudaDisplayToAp1Parameters::~CudaDisplayToAp1Parameters() {
  if (device_ != nullptr) {
    ::cudaFree(device_);
  }
}

auto CudaDisplayToAp1Parameters::Upload(std::span<const float> packed, cudaStream_t stream) -> const
    float* {
  if (packed.empty()) {
    throw std::runtime_error("CUDA DisplayToAp1: empty parameter block");
  }
  if (device_ != nullptr && uploaded_.size() == packed.size() &&
      std::equal(packed.begin(), packed.end(), uploaded_.begin())) {
    return device_;
  }
  if (packed.size() > capacity_) {
    if (device_ != nullptr) {
      CheckCuda(::cudaFree(device_), "cudaFree");
      device_ = nullptr;
    }
    CheckCuda(::cudaMalloc(reinterpret_cast<void**>(&device_), packed.size_bytes()), "cudaMalloc");
    capacity_ = packed.size();
  }
  CheckCuda(::cudaMemcpyAsync(device_, packed.data(), packed.size_bytes(), cudaMemcpyHostToDevice,
                              stream),
            "cudaMemcpyAsync");
  uploaded_.assign(packed.begin(), packed.end());
  ++upload_count_;
  return device_;
}

void LaunchCudaDisplayToAp1(const float4* input, float4* output, std::uint32_t pixel_count,
                            const float* device_params, DisplayToAp1Output output_kind,
                            cudaStream_t stream) {
  if (pixel_count == 0) {
    return;
  }
  constexpr std::uint32_t kBlock = 256;
  const std::uint32_t     grid   = (pixel_count + kBlock - 1) / kBlock;
  if (output_kind == DisplayToAp1Output::LinearAp0) {
    DisplayToAp0LinearKernel<<<grid, kBlock, 0, stream>>>(input, output, pixel_count,
                                                          device_params);
  } else {
    DisplayToAp1AcesccKernel<<<grid, kBlock, 0, stream>>>(input, output, pixel_count,
                                                          device_params);
  }
  CheckCuda(::cudaGetLastError(), "kernel launch");
}

}  // namespace alcedo
