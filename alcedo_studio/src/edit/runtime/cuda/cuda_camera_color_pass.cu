//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include "cuda_acescc.cuh"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/camera_color_gpu_params.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/dng_profile_gpu_math.h"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

auto MakeGpuParams(const DevelopColorTransform& transform) -> CameraColorGpuParams {
  CameraColorGpuParams params;
  for (int i = 0; i < 9; ++i) {
    params.camera_to_ap1[i] = transform.camera_to_ap1[static_cast<std::size_t>(i)];
  }
  return params;
}

__global__ void CameraColorKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                                  const CameraColorGpuParams* camera, const float* dng_profile) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) {
    return;
  }
  const float4 source = input[index];
  const float* m      = camera->camera_to_ap1;
  float3       c;
  c.x           = m[0] * source.x + m[1] * source.y + m[2] * source.z;
  c.y           = m[3] * source.x + m[4] * source.y + m[5] * source.z;
  c.z           = m[6] * source.x + m[7] * source.y + m[8] * source.z;
  const auto corrected = DngApplyColorProfile(DngMakeRgb(c.x, c.y, c.z), dng_profile);
  output[index] = make_float4(cuda_acescc::Encode(corrected.r), cuda_acescc::Encode(corrected.g),
                              cuda_acescc::Encode(corrected.b), source.w);
}

}  // namespace

void ExecuteCudaCameraColor(CudaRenderDevice& device, const ExecutionPlan& plan,
                            PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaCameraColor: BeginRender has not been called");
  }
  auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteCudaCameraColor: missing develop node");
  }
  auto pending = TakePendingParameterPatch(develop->Params());
  const auto develop_params = develop->Params().Params();
  const auto resolved       = ResolveDevelopColorTransform(develop_params);
  if (!resolved.ok) {
    throw std::runtime_error(std::string("ExecuteCudaCameraColor: ") +
                             std::string(ColorTransformErrorMessage(resolved.error)));
  }

  auto* input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaCameraColor: missing geometry.scene_source");
  }
  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  auto&      output =
      workspace.AcquireImageForWrite(plan.develop_output, {width, height, TextureFormat::Rgba32f});
  input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteCudaCameraColor: geometry texture lost during acquire");
  }

  const auto                  gpu_params = MakeGpuParams(resolved.transform);
  auto&                       arena      = workspace.Parameters();
  const ParameterSlotKey      key{develop->Id(), kDevelopCameraColorSlot};
  const ParameterFieldBinding field{DirtyFieldMask{DevelopDirty::WhiteBalance}, 0, 0,
                                    sizeof(CameraColorGpuParams)};
  auto payload = std::make_shared<TypedOperatorParamPayload<CameraColorGpuParams>>(
      type_ids::DevelopNode(), 1, gpu_params);
  if (!arena.Contains(key)) {
    arena.BindSlot(key, sizeof(CameraColorGpuParams), std::span{&field, 1});
    arena.InitializeFromFullDto(key, OperatorParamDto{type_ids::DevelopNode(), 1, payload});
  } else {
    OperatorParamPatchDto patch{develop->Id(), kDevelopCameraColorSlot, type_ids::DevelopNode(),
                                DirtyFieldMask{DevelopDirty::WhiteBalance}, payload};
    arena.ApplyPatch(key, patch);
  }
  auto& context = device.CommandContext();
  arena.UploadDirty(context);

  const auto table_data = PackDngProfileGpuData(develop_params.camera_profile, resolved.transform);
  auto&      tables     = UploadDngProfileGpuData(workspace, develop->Id(), table_data, context);
  const auto              binding = arena.Binding(key);
  const std::uint32_t     pixels  = width * height;
  constexpr std::uint32_t block   = 256;
  const auto*             params  = reinterpret_cast<const CameraColorGpuParams*>(
      static_cast<const std::byte*>(arena.DeviceBuffer().DevicePointer()) + binding.offset);
  CameraColorKernel<<<(pixels + block - 1) / block, block, 0, context.Stream()>>>(
      static_cast<const float4*>(input->Texture().DevicePointer()),
      static_cast<float4*>(output.Texture().DevicePointer()), pixels, params,
      static_cast<const float*>(tables.DevicePointer()));
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaCameraColor: CUDA kernel launch failed");
  }
  if (pending.has_value()) {
    pending->Commit();
  }
}

}  // namespace alcedo
