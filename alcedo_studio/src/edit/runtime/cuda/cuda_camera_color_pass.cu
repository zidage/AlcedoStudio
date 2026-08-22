//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cmath>
#include <stdexcept>

#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

struct CameraToAp1Params {
  float matrix[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

auto MakeCameraToAp1(const RawRuntimeColorContext& context) -> CameraToAp1Params {
  CameraToAp1Params result;
  if (!context.valid_) {
    return result;
  }
  constexpr float srgb_to_ap1[9] = {0.613097f, 0.339523f, 0.047380f, 0.070194f, 0.916354f,
                                    0.013452f, 0.020616f, 0.109570f, 0.869815f};
  float           absolute_sum   = 0.0f;
  for (float value : context.rgb_cam_) {
    absolute_sum += std::abs(value);
  }
  if (absolute_sum <= 1.0e-6f) {
    return result;
  }
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      result.matrix[row * 3 + column] = srgb_to_ap1[row * 3] * context.rgb_cam_[column] +
                                        srgb_to_ap1[row * 3 + 1] * context.rgb_cam_[3 + column] +
                                        srgb_to_ap1[row * 3 + 2] * context.rgb_cam_[6 + column];
    }
  }
  return result;
}

__global__ void CameraColorKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                                  CameraToAp1Params camera) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) {
    return;
  }
  const float4 source = input[index];
  float3       c;
  c.x = camera.matrix[0] * source.x + camera.matrix[1] * source.y + camera.matrix[2] * source.z;
  c.y = camera.matrix[3] * source.x + camera.matrix[4] * source.y + camera.matrix[5] * source.z;
  c.z = camera.matrix[6] * source.x + camera.matrix[7] * source.y + camera.matrix[8] * source.z;
  output[index] = make_float4(c.x, c.y, c.z, source.w);
}

}  // namespace

void ExecuteCudaCameraColor(CudaRenderDevice& device, const ExecutionPlan& plan,
                            const RawRuntimeColorContext& color_context,
                            const PipelineDocument& /*document*/) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaCameraColor: BeginRender has not been called");
  }
  auto* input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaCameraColor: missing geometry.scene_source");
  }
  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  auto& output = workspace.AcquireImageForWrite(plan.develop_output,
                                                {width, height, TextureFormat::Rgba32f});
  input        = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteCudaCameraColor: geometry texture lost during acquire");
  }
  const auto camera = MakeCameraToAp1(color_context);
  const std::uint32_t     pixels = width * height;
  constexpr std::uint32_t block  = 256;
  CameraColorKernel<<<(pixels + block - 1) / block, block, 0, device.CommandContext().Stream()>>>(
      static_cast<const float4*>(input->Texture().DevicePointer()),
      static_cast<float4*>(output.Texture().DevicePointer()), pixels, camera);
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaCameraColor: CUDA kernel launch failed");
  }
}

}  // namespace alcedo
