//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_drt_pass.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/metal/metal_drt_gpu_params.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

auto Pipeline() -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_DRT_METALLIB_PATH
  throw std::runtime_error("Metal DRT metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(ALCEDO_METAL_DRT_METALLIB_PATH,
                                                                  "drt_display", "Metal DRT");
#endif
}

void DispatchDrt(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                 MetalBackend::Texture2D& dst, const MetalBackend::Buffer& params,
                 std::uint32_t offset) {
  auto  pipeline = Pipeline();
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: compute encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setBuffer(static_cast<MTL::Buffer*>(params.Native()), offset, 0);
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{src.Width(), src.Height(), 1},
                           MTL::Size{thread_width, thread_height, 1});
  device.Workspace().Device().NoteComputeDispatch();
}

}  // namespace

void AppendMetalDrtWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_DRT_METALLIB_PATH
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, "drt_display", "Metal DRT"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalDrt(MetalRenderDevice& device, const ExecutionPlan& plan,
                     PipelineDocument& document) -> MetalDrtResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalDrt: BeginRender has not been called");
  }
  auto* drt = document.Drt();
  if (drt == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: missing DRT endpoint");
  }
  auto* input = workspace.Images().Find(plan.primary_grade_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteMetalDrt: missing primary-grade output");
  }

  auto&                       arena = workspace.Parameters();
  const ParameterSlotKey      key{drt->Id(), AdjustmentInstanceId{"drt.output"}};
  const ParameterFieldBinding field{DirtyFieldMask{kDrtDirtyBits}, 0, 0, sizeof(MetalDrtGpuParams)};
  auto                        pending          = TakePendingParameterPatch(drt->Params());
  const bool                  needs_initialize = !arena.Contains(key);
  if (needs_initialize || pending.has_value()) {
    const auto runtime = ResolveMetalDrtGpuParams(drt->Params().ToJson());
    auto       payload = std::make_shared<TypedOperatorParamPayload<MetalDrtGpuParams>>(
        drt->Params().Type(), 1, runtime);
    if (needs_initialize) {
      arena.BindSlot(key, sizeof(MetalDrtGpuParams), std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{drt->Params().Type(), 1, payload});
    } else {
      arena.ApplyPatch(key, OperatorParamPatchDto{drt->Id(), AdjustmentInstanceId{"drt.output"},
                                                 drt->Params().Type(), DirtyFieldMask{kDrtDirtyBits},
                                                 payload});
    }
  }
  arena.UploadDirty(device.CommandContext());
  if (pending) {
    pending->Commit();
  }

  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  workspace.AcquireImageForWrite(plan.display_output, {width, height, TextureFormat::Rgba32f});
  input        = workspace.Images().Find(plan.primary_grade_output);
  auto* output = workspace.Images().Find(plan.display_output);
  if (input == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: image cache changed during allocation");
  }
  const auto binding = arena.Binding(key);
  DispatchDrt(device, input->Texture(), output->Texture(), arena.DeviceBuffer(), binding.offset);
  return {plan.display_output};
}

}  // namespace alcedo
