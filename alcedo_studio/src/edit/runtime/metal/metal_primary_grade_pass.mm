//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_primary_grade_pass.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/grade_executor.hpp"
#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/metal/metal_local_tone_pass.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

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
    throw std::runtime_error("ExecuteMetalPrimaryGrade: command buffer store failed");
  }
  return *stored;
}

auto Pipeline(const char* function, const char* label) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH
  throw std::runtime_error("Metal primary grade metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH, function, label);
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     std::uint32_t width, std::uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1}, MTL::Size{thread_width, thread_height, 1});
}

void EnqueueGradePointwise(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                       MetalBackend::Texture2D& dst, const MetalBackend::Texture2D* mix_source,
                       const MetalBackend::Texture2D* mask, const MetalBackend::Buffer& params,
                       const MetalBackend::Buffer& commands, std::uint32_t command_offset,
                       std::uint32_t command_count, const MetalLutBinding& lut, float mix,
                       std::uint32_t width, std::uint32_t height) {
  auto  pipeline = Pipeline("primary_grade_pointwise", "Metal PrimaryGrade");
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: compute encoder is missing");
  }
  PrimaryGradeDispatchParams dispatch;
  dispatch.command_count   = command_count;
  dispatch.command_offset  = command_offset;
  dispatch.lut_edge        = lut.edge_size;
  dispatch.local_reference = local_tone_mapping::kAcesccMiddleGray;
  dispatch.width           = width;
  const std::uint32_t apply_mix = mix_source == nullptr ? 0u : 1u;
  const std::uint32_t has_mask  = mask == nullptr ? 0u : 1u;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setTexture(static_cast<MTL::Texture*>((mix_source == nullptr ? src : *mix_source).Native()),
                      2);
  encoder->setTexture(static_cast<MTL::Texture*>((mask == nullptr ? src : *mask).Native()), 3);
  encoder->setBuffer(static_cast<MTL::Buffer*>(params.Native()), 0, 0);
  encoder->setBuffer(static_cast<MTL::Buffer*>(commands.Native()), 0, 1);
  encoder->setBytes(&dispatch, sizeof(dispatch), 2);
  encoder->setBuffer(static_cast<MTL::Buffer*>(lut.native), 0, 3);
  encoder->setBytes(&mix, sizeof(mix), 4);
  encoder->setBytes(&apply_mix, sizeof(apply_mix), 5);
  encoder->setBytes(&has_mask, sizeof(has_mask), 6);
  DispatchThreads(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

void EnqueueGradeMix(MetalRenderDevice& device, const MetalBackend::Texture2D& source,
                 const MetalBackend::Texture2D& adjusted, MetalBackend::Texture2D& dst, float mix,
                 const MetalBackend::Texture2D* mask, std::uint32_t width, std::uint32_t height) {
  auto  pipeline = mask == nullptr
                       ? Pipeline("primary_grade_mix", "Metal PrimaryGrade Mix")
                       : Pipeline("primary_grade_mix_masked", "Metal PrimaryGrade Mix Masked");
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: mix encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(source.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(adjusted.Native()), 1);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 2);
  if (mask != nullptr) {
    encoder->setTexture(static_cast<MTL::Texture*>(mask->Native()), 3);
  }
  encoder->setBytes(&mix, sizeof(mix), 0);
  DispatchThreads(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

auto LoadMetalGradeLut(MetalRenderDevice& device, ColorGradeNodeModel& grade) -> MetalLutBinding {
  const auto packed = TryPackGradeLut(grade);
  if (packed == nullptr) {
    return device.Workspace().Device().DummyLut();
  }
  return device.Workspace().Device().AcquireLut(packed->key, packed->rgba, packed->edge,
                                                device.CommandContext());
}


struct MetalGradeOps {
  using Device            = MetalRenderDevice;
  using Backend           = MetalBackend;
  using Texture           = MetalBackend::Texture2D;
  using Scratch           = Texture*;
  using HorizontalScratch = ResourceLease<MetalBackend>;
  using LutBinding        = MetalLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteMetalPrimaryGrade";

  static auto BindingWidth(MetalRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return MetalSceneWidth(device, binding);
  }

  static auto BindingHeight(MetalRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return MetalSceneHeight(device, binding);
  }

  static auto UploadFusedCommands(MetalRenderDevice& device, const NodeId& grade_id,
                                  const std::vector<std::uint32_t>& fused_offsets)
      -> std::uint32_t {
    if (fused_offsets.empty()) {
      return 0;
    }
    auto&              workspace = device.Workspace();
    const GraphValueId command_id{grade_id, PortId{"runtime.order"}};
    const auto         bytes = fused_offsets.size() * sizeof(fused_offsets[0]);
    ContentHash        topology;
    topology.MixText(grade_id.Value());
    topology.MixU32(static_cast<std::uint32_t>(fused_offsets.size()));
    for (const auto offset : fused_offsets) {
      topology.MixU32(offset);
    }
    const auto  topology_hash = topology.Key().hash;
    const auto* existing      = workspace.Values().Find(command_id);
    const bool  needs_upload =
        existing == nullptr || existing->Bytes() < bytes ||
        workspace.Device().GradeCommandTopologyHash() != topology_hash;
    auto& buffer =
        EnsureBuffer(workspace, command_id, std::max<std::size_t>(bytes, sizeof(std::uint32_t)));
    if (needs_upload) {
      workspace.Device().UploadBufferRange(
          buffer, 0,
          std::span<const std::byte>(reinterpret_cast<const std::byte*>(fused_offsets.data()),
                                     bytes),
          device.CommandContext());
      workspace.Device().SetGradeCommandTopologyHash(topology_hash);
      return static_cast<std::uint32_t>(bytes);
    }
    return 0;
  }

  static auto LoadLut(MetalRenderDevice& device, ColorGradeNodeModel& grade) -> MetalLutBinding {
    return LoadMetalGradeLut(device, grade);
  }

  static auto LutResourceId(const MetalLutBinding& lut) -> std::uint64_t { return lut.resource_id; }

  static void DispatchPointwise(MetalRenderDevice& device, const FrameSceneBinding& src,
                                const FrameSceneBinding& dst, const MetalLutBinding& lut,
                                const NodeId& grade_id, std::uint32_t command_start,
                                std::uint32_t command_count, std::uint32_t width,
                                std::uint32_t height) {
    auto& workspace = device.Workspace();
    auto* commands  = workspace.Values().Find(GraphValueId{grade_id, PortId{"runtime.order"}});
    if (commands == nullptr) {
      throw std::runtime_error("ExecuteMetalPrimaryGrade: missing fused command buffer");
    }
    EnqueueGradePointwise(device, MetalSceneTexture(device, src), MetalSceneTexture(device, dst),
                          nullptr, nullptr, workspace.Parameters().DeviceBuffer(), *commands,
                          command_start, command_count, lut, 1.0f, width, height);
  }

  static void DispatchPointwiseWithMix(MetalRenderDevice& device, const FrameSceneBinding& src,
                                       const FrameSceneBinding& dst,
                                       const FrameSceneBinding& original, const MetalLutBinding& lut,
                                       const NodeId& grade_id, std::uint32_t command_start,
                                       std::uint32_t command_count, float mix,
                                       const GraphValueId* mask_id, std::uint32_t width,
                                       std::uint32_t height) {
    auto& workspace = device.Workspace();
    auto* commands  = workspace.Values().Find(GraphValueId{grade_id, PortId{"runtime.order"}});
    if (commands == nullptr) {
      throw std::runtime_error("ExecuteMetalPrimaryGrade: missing fused command buffer");
    }
    const Texture* mask = nullptr;
    if (mask_id != nullptr) {
      auto* lease = device.Workspace().Images().Find(*mask_id);
      if (lease == nullptr) {
        throw std::runtime_error("ExecuteMetalPrimaryGrade: compiled mask output is missing");
      }
      mask = &lease->Texture();
    }
    EnqueueGradePointwise(device, MetalSceneTexture(device, src), MetalSceneTexture(device, dst),
                          &MetalSceneTexture(device, original), mask,
                          workspace.Parameters().DeviceBuffer(), *commands, command_start,
                          command_count, lut, mix, width, height);
  }

  static auto AcquireHorizontalScratch(MetalRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  }

  static void DispatchHorizontal(MetalRenderDevice&, const FrameSceneBinding&, HorizontalScratch&,
                                 const NeighborWork&, std::uint32_t, std::uint32_t) {}

  static void DispatchVerticalApply(MetalRenderDevice& device, const FrameSceneBinding& src,
                                    HorizontalScratch&, const FrameSceneBinding& dst,
                                    const FrameSceneBinding&, const LutBinding& lut,
                                    const NeighborWork& work, float, const GraphValueId*,
                                    std::uint32_t width, std::uint32_t height) {
    DispatchPointwise(device, src, dst, lut, work.owner, work.command_index, 1, width, height);
  }

  static auto ExecuteLocalTone(MetalRenderDevice& device, const FrameSceneBinding& src,
                               const FrameSceneBinding& dest, const FrameSceneBinding& original,
                               const NodeId& grade_id, float shadows_slider,
                               float highlights_slider, const ResolvedRenderGeometry& geometry,
                               float mix, const GraphValueId* mask_id) -> MetalLocalToneResult {
    return ExecuteMetalLocalTone(device, src, dest, original, grade_id, shadows_slider,
                                 highlights_slider, geometry, mix, mask_id);
  }

  static void CheckAfterEncode(MetalRenderDevice&) {}
};

}  // namespace

void AppendMetalPrimaryGradeWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_pointwise", "Metal PrimaryGrade"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_mix", "Metal PrimaryGrade Mix"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_mix_masked",
                                          "Metal PrimaryGrade Mix Masked"});
#else
  (void)pipelines;
#endif
  AppendMetalLocalToneWarmup(pipelines);
}

auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                              const PreparedRawInput& prepared, PipelineDocument& document,
                              const CompiledGradeNode& compiled_grade_node,
                              const FrameSceneBinding& scene) -> MetalPrimaryGradeResult {
  const auto executed = GradeExecutor<MetalGradeOps>::Execute(device, plan, prepared, document,
                                                             compiled_grade_node, scene);
  MetalPrimaryGradeResult result;
  result.output                                 = executed.output;
  result.output_binding                         = executed.output_binding;
  result.pointwise_dispatch_count               = executed.pointwise_dispatch_count;
  result.detail_pass_count                      = executed.detail_pass_count;
  result.command_upload_bytes                   = executed.command_upload_bytes;
  result.lut_resource_id                        = executed.lut_resource_id;
  result.local_tone_reference_resource_id       = executed.local_tone_reference_resource_id;
  result.local_tone_rebuilt_reference           = executed.local_tone_rebuilt_reference;
  result.local_tone_sampled_canonical_reference = executed.local_tone_sampled_canonical_reference;
  result.local_tone_transient_bytes             = executed.local_tone_transient_bytes;
  return result;
}

auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                              const PreparedRawInput& prepared, PipelineDocument& document)
    -> MetalPrimaryGradeResult {
  if (plan.grade_nodes.empty()) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: plan has no Color Grade");
  }
  device.Workspace().PrepareResultValidity(plan, document, prepared);
  const ImageExtent extent{plan.geometry.render_extent.width, plan.geometry.render_extent.height};
  device.Workspace().EnsureSceneWorkImages(extent);
  FrameSceneBinding scene = FrameSceneBinding::CachedImage(plan.develop_output);
  MetalPrimaryGradeResult last{};
  for (const auto& compiled_grade : plan.grade_nodes) {
    last  = ExecuteMetalPrimaryGrade(device, plan, prepared, document, compiled_grade, scene);
    scene = last.output_binding;
  }
  return last;
}

}  // namespace alcedo
