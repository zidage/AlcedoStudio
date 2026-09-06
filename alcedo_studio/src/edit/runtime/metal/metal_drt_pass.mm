//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_drt_pass.hpp"

#include <algorithm>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/metal/metal_drt_gpu_params.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

struct PrimaryGradeDispatchParams {
  std::uint32_t command_count   = 0;
  std::uint32_t command_offset  = 0;
  std::uint32_t lut_edge        = 0;
  float         local_reference = 0.0f;
  std::uint32_t width           = 0;
  std::uint32_t pad[3]          = {};
};

auto AcquireRgba(MetalRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<MetalBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto EnsureBuffer(MetalRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> MetalBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: command buffer store failed");
  }
  return *stored;
}

auto GradePipeline() -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH
  throw std::runtime_error("Metal primary grade metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH, "primary_grade_pointwise", "Metal DRT Post");
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     std::uint32_t width, std::uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1}, MTL::Size{thread_width, thread_height, 1});
}

void DispatchPointwise(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                       MetalBackend::Texture2D& dst, const MetalBackend::Buffer& params,
                       const MetalBackend::Buffer& commands, std::uint32_t command_offset,
                       const MetalLutBinding& lut, std::uint32_t width, std::uint32_t height) {
  auto  pipeline = GradePipeline();
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: compute encoder is missing");
  }
  PrimaryGradeDispatchParams dispatch;
  dispatch.command_count   = 1;
  dispatch.command_offset  = command_offset;
  dispatch.lut_edge        = lut.edge_size;
  dispatch.local_reference = local_tone_mapping::kAcesccMiddleGray;
  dispatch.width           = width;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setBuffer(static_cast<MTL::Buffer*>(params.Native()), 0, 0);
  encoder->setBuffer(static_cast<MTL::Buffer*>(commands.Native()), 0, 1);
  encoder->setBytes(&dispatch, sizeof(dispatch), 2);
  encoder->setBuffer(static_cast<MTL::Buffer*>(lut.native), 0, 3);
  DispatchThreads(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

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
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

}  // namespace

void AppendMetalDrtWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_DRT_METALLIB_PATH
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, "drt_display", "Metal DRT"});
#endif
#ifdef ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_pointwise", "Metal DRT Post"});
#endif
#if !defined(ALCEDO_METAL_DRT_METALLIB_PATH) && !defined(ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH)
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
  auto* input = workspace.Images().Find(plan.SceneInputForDrt());
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteMetalDrt: missing DRT scene input");
  }

  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  auto&      context = device.CommandContext();
  auto&      arena   = workspace.Parameters();
  std::vector<PendingParameterPatch> post_pending;
  std::vector<std::uint32_t>         command_offsets;
  command_offsets.reserve(plan.drt.post_adjustments.size());
  for (const auto& compiled : plan.drt.post_adjustments) {
    auto* model = drt->FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error("ExecuteMetalDrt: compiled DRT/Post adjustment no longer matches");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value() || !IsNeighborhoodBehavior(*behavior)) {
      throw std::runtime_error(
          "ExecuteMetalDrt: DRT/Post adjustment is not a neighborhood operation");
    }
    const ParameterSlotKey key{drt->Id(), compiled.instance_id};
    if (auto change = BindOrRefreshGradeRuntimeSlot(arena, key, *model, *behavior)) {
      post_pending.push_back(std::move(*change));
    }
    if (PackedGradeControlValue(arena, key) != 0.0f) {
      command_offsets.push_back(arena.Binding(key).offset);
    }
  }
  arena.UploadDirty(context);
  for (auto& patch : post_pending) {
    patch.Commit();
  }

  auto Resolve = [&](const GraphValueId& id) -> MetalBackend::Texture2D& {
    auto* image = workspace.Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteMetalDrt: scene image is missing");
    }
    return image->Texture();
  };

  GraphValueId scene_id = plan.SceneInputForDrt();
  if (command_offsets.empty()) {
    AcquireRgba(workspace, plan.drt.scene_output, width, height);
    input = workspace.Images().Find(plan.SceneInputForDrt());
    if (input == nullptr) {
      throw std::runtime_error("ExecuteMetalDrt: DRT scene input lost during scene copy");
    }
    workspace.Device().CopyTexture2D(input->Texture(), Resolve(plan.drt.scene_output), context);
    scene_id = plan.drt.scene_output;
  } else {
    const GraphValueId command_id{drt->Id(), PortId{"runtime.order"}};
    const auto         bytes = command_offsets.size() * sizeof(command_offsets[0]);
    auto&              command_buffer =
        EnsureBuffer(workspace, command_id, std::max<std::size_t>(bytes, sizeof(std::uint32_t)));
    workspace.Device().UploadBufferRange(
        command_buffer, 0,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(command_offsets.data()),
                                   bytes),
        context);
    const auto         lut     = workspace.Device().DummyLut();
    const GraphValueId ping_id{drt->Id(), PortId{"runtime.ping"}};
    const GraphValueId pong_id{drt->Id(), PortId{"runtime.pong"}};
    std::size_t        remaining = command_offsets.size();
    for (std::size_t index = 0; index < command_offsets.size(); ++index) {
      if (remaining == 0) {
        throw std::runtime_error("ExecuteMetalDrt: neighborhood destination underflow");
      }
      --remaining;
      GraphValueId dest_id = plan.drt.scene_output;
      if (remaining != 0) {
        dest_id = scene_id == ping_id ? pong_id : ping_id;
      }
      AcquireRgba(workspace, dest_id, width, height);
      auto& src  = Resolve(scene_id);
      auto& dest = Resolve(dest_id);
      DispatchPointwise(device, src, dest, arena.DeviceBuffer(), command_buffer,
                        static_cast<std::uint32_t>(index), lut, width, height);
      scene_id = dest_id;
    }
  }

  const ParameterSlotKey key{drt->Id(), AdjustmentInstanceId{"drt.output"}};
  auto                   pending = plan.output_color_override.has_value()
                                       ? decltype(TakePendingDirtyFields(drt->Params())){}
                                       : TakePendingDirtyFields(drt->Params());
  const bool             needs_initialize = !arena.Contains(key);
  if (needs_initialize || pending.has_value() || plan.output_color_override.has_value()) {
    auto drt_json = drt->Params().ToJson();
    if (plan.output_color_override.has_value()) {
      OverlayExportColorOnDrtJson(drt_json, *plan.output_color_override);
    }
    const auto runtime = ResolveMetalDrtGpuParams(drt_json);
    arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, runtime);
  }
  arena.UploadDirty(context);
  if (pending) {
    pending->Commit();
  }

  AcquireRgba(workspace, plan.display_output, width, height);
  auto* scene  = workspace.Images().Find(scene_id);
  auto* output = workspace.Images().Find(plan.display_output);
  if (scene == nullptr || output == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: image cache changed during allocation");
  }
  const auto binding = arena.Binding(key);
  DispatchDrt(device, scene->Texture(), output->Texture(), arena.DeviceBuffer(), binding.offset);
  return {plan.display_output, plan.drt.scene_output,
          static_cast<std::uint32_t>(command_offsets.size())};
}

}  // namespace alcedo
