//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "cuda/cuda_check.hpp"
#include "cuda_acescc.cuh"
#include "cuda_drt_runtime_state.cuh"
#include "cuda_neighbor_grade.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/GPU_kernels/color_mgmt/disp_enc_funcs.cuh"
#include "edit/operators/GPU_kernels/color_mgmt/odt_funcs.cuh"
#include "edit/operators/GPU_kernels/color_mgmt/open_drt_funcs.cuh"
#include "edit/operators/cst/odt_op.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

auto EnsureDisplayImage(CudaRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                        std::uint32_t height) -> ResourceLease<CudaBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto AcquireScratch(CudaRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
    -> ResourceLease<CudaBackend> {
  return workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
}

auto NeighborVerticalRadius(const GradeNeighborParams& params) -> std::uint32_t {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::Halation) {
    return std::clamp(static_cast<std::uint32_t>(std::ceil(params.sigma_y * 3.0f)), 1U,
                      kGradeNeighborMaxTapCount - 1U);
  }
  return params.radius;
}

void ResolveRuntime(CudaDrtRuntimeState& state, const nlohmann::json& drt_json) {
  ODT_Op descriptor(nlohmann::json{{"odt", drt_json}});
  descriptor.SetGlobalParams(state.cpu_params);
  state.gpu_params = GPUParamsConverter::ConvertFromCPU(state.cpu_params, state.gpu_params);
}

__global__ void DrtKernel(const float4* input, float4* output, std::uint32_t pixel_count,
                          const GPU_TO_OUTPUT_Params* params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  auto         runtime = *params;
  const float4 source  = input[index];
  const float3 scene   = make_float3(cuda_acescc::Decode(source.x), cuda_acescc::Decode(source.y),
                                     cuda_acescc::Decode(source.z));
  float3       display_linear;
  if (runtime.method_ == GPU_ODTMethod::ACES_2_0) {
    auto aces      = runtime.aces_params_;
    display_linear = CUDA::OutputTransform_fwd(scene, aces);
  } else {
    display_linear = CUDA::OpenDRTTransform_fwd(scene, runtime.open_drt_params_);
  }
  const float3 encoded = CUDA::DisplayEncoding(display_linear, runtime.limit_to_display_matx,
                                               runtime.eotf, runtime.display_linear_scale_);
  output[index]        = make_float4(encoded.x, encoded.y, encoded.z, source.w);
}

}  // namespace

auto ExecuteCudaDrt(CudaRenderDevice& device, const ExecutionPlan& plan, PipelineDocument& document)
    -> CudaDrtResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaDrt: BeginRender has not been called");
  }
  auto* drt = document.Drt();
  if (drt == nullptr) throw std::runtime_error("ExecuteCudaDrt: missing DRT endpoint");
  auto* input = workspace.Images().Find(plan.primary_grade_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaDrt: missing primary-grade output");
  }

  const auto input_width  = input->Texture().Width();
  const auto input_height = input->Texture().Height();
  auto&      context      = device.CommandContext();

  std::vector<PendingParameterPatch> post_pending;
  std::vector<GradeNeighborParams>   enabled;
  enabled.reserve(plan.drt_post_adjustments.size());
  for (const auto& compiled : plan.drt_post_adjustments) {
    auto* model = drt->FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error("ExecuteCudaDrt: compiled DRT/Post adjustment no longer matches");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value() || !IsNeighborhoodBehavior(*behavior)) {
      throw std::runtime_error("ExecuteCudaDrt: DRT/Post adjustment is not a neighborhood operation");
    }
    if (auto change = TakePendingParameterPatch(*model)) {
      post_pending.push_back(std::move(*change));
    }
    auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
    if (neighbor.enabled != 0U) {
      enabled.push_back(neighbor);
    }
  }
  for (auto& patch : post_pending) {
    patch.Commit();
  }

  auto Resolve = [&](const GraphValueId& id) -> CudaBackend::Texture2D& {
    auto* image = workspace.Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteCudaDrt: scene image is missing");
    }
    return image->Texture();
  };

  GraphValueId scene_id = plan.primary_grade_output;
  if (enabled.empty()) {
    EnsureDisplayImage(workspace, plan.drt_scene_output, input_width, input_height);
    input = workspace.Images().Find(plan.primary_grade_output);
    if (input == nullptr) {
      throw std::runtime_error("ExecuteCudaDrt: primary-grade output lost during scene copy");
    }
    workspace.Device().CopyTexture2D(input->Texture(), Resolve(plan.drt_scene_output), context);
    scene_id = plan.drt_scene_output;
  } else {
    const GraphValueId ping_id{drt->Id(), PortId{"runtime.ping"}};
    const GraphValueId pong_id{drt->Id(), PortId{"runtime.pong"}};
    std::size_t        remaining = enabled.size();
    const dim3         neighbor_block{16, 16};
    const dim3         neighbor_grid{(input_width + neighbor_block.x - 1) / neighbor_block.x,
                                     (input_height + neighbor_block.y - 1) / neighbor_block.y};
    for (const auto& neighbor : enabled) {
      if (remaining == 0) {
        throw std::runtime_error("ExecuteCudaDrt: neighborhood destination underflow");
      }
      --remaining;
      GraphValueId dest_id = plan.drt_scene_output;
      if (remaining != 0) {
        dest_id = scene_id == ping_id ? pong_id : ping_id;
      }
      EnsureDisplayImage(workspace, dest_id, input_width, input_height);
      auto  blur_horizontal = AcquireScratch(workspace, input_width, input_height);
      auto& src             = Resolve(scene_id);
      auto& dest            = Resolve(dest_id);
      cuda_neighbor_grade::BlurHorizontal<<<neighbor_grid, neighbor_block, 0, context.Stream()>>>(
          static_cast<const float4*>(src.DevicePointer()),
          static_cast<float4*>(blur_horizontal.Texture().DevicePointer()),
          static_cast<int>(input_width), static_cast<int>(input_height), neighbor);
      const auto vertical_radius = NeighborVerticalRadius(neighbor);
      const auto shared_bytes    = static_cast<std::size_t>(neighbor_block.x) *
                                (neighbor_block.y + 2U * vertical_radius) * sizeof(float4);
      cuda_neighbor_grade::
          ApplyVertical<<<neighbor_grid, neighbor_block, shared_bytes, context.Stream()>>>(
              static_cast<const float4*>(src.DevicePointer()),
              static_cast<const float4*>(blur_horizontal.Texture().DevicePointer()),
              static_cast<float4*>(dest.DevicePointer()), static_cast<int>(input_width),
              static_cast<int>(input_height), neighbor);
      scene_id = dest_id;
    }
    cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDrt: neighborhood kernel launch");
  }

  auto&                       arena = workspace.Parameters();
  const ParameterSlotKey      key{drt->Id(), AdjustmentInstanceId{"drt.output"}};
  const ParameterFieldBinding field{DirtyFieldMask{kDrtDirtyBits}, 0, 0,
                                    sizeof(GPU_TO_OUTPUT_Params)};
  auto                        pending          = plan.output_color_override.has_value()
                                ? decltype(TakePendingParameterPatch(drt->Params())){}
                                : TakePendingParameterPatch(drt->Params());
  const bool                  needs_initialize = !arena.Contains(key);
  if (needs_initialize || pending.has_value() || plan.output_color_override.has_value()) {
    auto drt_json = drt->Params().ToJson();
    if (plan.output_color_override.has_value()) {
      OverlayExportColorOnDrtJson(drt_json, *plan.output_color_override);
    }
    ResolveRuntime(device.DrtRuntime(), drt_json);
    const auto runtime = device.DrtRuntime().gpu_params.to_output_params_;
    auto       payload = std::make_shared<TypedOperatorParamPayload<GPU_TO_OUTPUT_Params>>(
        drt->Params().Type(), 1, runtime);
    if (needs_initialize) {
      arena.BindSlot(key, sizeof(GPU_TO_OUTPUT_Params), std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{drt->Params().Type(), 1, payload});
    } else {
      arena.ApplyPatch(
          key, OperatorParamPatchDto{drt->Id(), AdjustmentInstanceId{"drt.output"},
                                     drt->Params().Type(), DirtyFieldMask{kDrtDirtyBits}, payload});
    }
  }
  arena.UploadDirty(context);
  if (pending) pending->Commit();

  EnsureDisplayImage(workspace, plan.display_output, input_width, input_height);
  auto* scene  = workspace.Images().Find(scene_id);
  auto* output = workspace.Images().Find(plan.display_output);
  if (scene == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteCudaDrt: image cache changed during allocation");
  }
  const auto& binding = arena.Binding(key);
  const auto* params  = reinterpret_cast<const GPU_TO_OUTPUT_Params*>(
      static_cast<const std::byte*>(arena.DeviceBuffer().DevicePointer()) + binding.offset);
  const std::uint32_t     pixels = input_width * input_height;
  constexpr std::uint32_t block  = 256;
  DrtKernel<<<(pixels + block - 1) / block, block, 0, context.Stream()>>>(
      static_cast<const float4*>(scene->Texture().DevicePointer()),
      static_cast<float4*>(output->Texture().DevicePointer()), pixels, params);
  cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDrt: kernel launch");
  return {plan.display_output, plan.drt_scene_output,
          static_cast<std::uint32_t>(enabled.size())};
}

}  // namespace alcedo
