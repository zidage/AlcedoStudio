//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_drt_pass.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/metal/metal_drt_gpu_params.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

auto NeighborPipeline(const char* function, const char* label)
    -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_DRT_METALLIB_PATH
  throw std::runtime_error("Metal DRT metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(ALCEDO_METAL_DRT_METALLIB_PATH,
                                                                  function, label);
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     std::uint32_t width, std::uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1}, MTL::Size{thread_width, thread_height, 1});
}

/** @brief Start the display-referred horizontal neighborhood kernel. */
void DispatchNeighborHorizontal(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                                MetalBackend::Texture2D& dst, const GradeNeighborParams& params,
                                std::uint32_t width, std::uint32_t height) {
  auto pipeline = NeighborPipeline("drt_neighbor_blur_horizontal", "Metal DRT Post Horizontal");
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: compute encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setBytes(&params, sizeof(params), 0);
  DispatchThreads(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

/** @brief Start the display-referred vertical apply kernel. */
void DispatchNeighborVertical(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                              const MetalBackend::Texture2D& horizontal,
                              MetalBackend::Texture2D& dst, const GradeNeighborParams& params,
                              std::uint32_t width, std::uint32_t height) {
  auto pipeline = NeighborPipeline("drt_neighbor_apply_vertical", "Metal DRT Post Vertical");
  auto* encoder = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalDrt: compute encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(horizontal.Native()), 1);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 2);
  encoder->setBytes(&params, sizeof(params), 0);
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

struct MetalDrtOps {
  using Device            = MetalRenderDevice;
  using Texture           = MetalBackend::Texture2D;
  using HorizontalScratch = ResourceLease<MetalBackend>;
  using LutBinding        = MetalLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteMetalDrt";

  static auto RefreshNeighborhoodAdjustment(MetalRenderDevice& device, IOperatorModel& model,
                                            const ParameterSlotKey& key, AdjustmentBehavior behavior)
      -> std::optional<PendingParameterPatch> {
    return BindOrRefreshGradeRuntimeSlot(device.Workspace().Parameters(), key, model, behavior);
  }

  static void PrepareNeighborCommands(MetalRenderDevice&, const NodeId&,
                                      std::span<const std::uint32_t>) {}

  static auto NeighborLut(MetalRenderDevice& device) -> LutBinding {
    return device.Workspace().Device().DummyLut();
  }

  static auto AcquireHorizontalScratch(MetalRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
  }

  static auto BindingWidth(MetalRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return MetalSceneWidth(device, binding);
  }

  static auto BindingHeight(MetalRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return MetalSceneHeight(device, binding);
  }

  static void DispatchHorizontal(MetalRenderDevice& device, const FrameSceneBinding& src,
                                 HorizontalScratch& scratch, const NeighborWork& work,
                                 std::uint32_t width, std::uint32_t height) {
    DispatchNeighborHorizontal(device, MetalSceneTexture(device, src), scratch.Texture(),
                               work.params, width, height);
  }

  static void DispatchVerticalApply(MetalRenderDevice& device, const FrameSceneBinding& src,
                                    HorizontalScratch& scratch, const FrameSceneBinding& dst,
                                    const FrameSceneBinding&, const LutBinding& lut,
                                    const NeighborWork& work, float, const GraphValueId*,
                                    std::uint32_t width, std::uint32_t height) {
    (void)lut;
    DispatchNeighborVertical(device, MetalSceneTexture(device, src), scratch.Texture(),
                             MetalSceneTexture(device, dst), work.params, width, height);
  }

  static void AcquireDisplayOutput(MetalRenderDevice& device, const GraphValueId& id,
                                   std::uint32_t width, std::uint32_t height) {
    (void)device.Workspace().AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
  }

  static void BindDisplayParams(MetalRenderDevice& device, const ExecutionPlan& plan,
                                DrtNodeModel& drt, std::vector<PendingParameterPatch>& pending) {
    auto&                  arena = device.Workspace().Parameters();
    const ParameterSlotKey key{drt.Id(), AdjustmentInstanceId{"drt.output"}};
    auto                   display_pending = plan.output_color_override.has_value()
                                                 ? decltype(TakePendingDirtyFields(drt.Params())){}
                                                 : TakePendingDirtyFields(drt.Params());
    const bool             needs_initialize = !arena.Contains(key);
    if (needs_initialize || display_pending.has_value() || plan.output_color_override.has_value()) {
      const auto runtime = PackMetalDrtGpuParams(
          DrtOutputResolver::ResolveNode(drt, plan.output_color_override, kErrorPrefix));
      arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, runtime);
    }
    if (display_pending) {
      pending.push_back(std::move(*display_pending));
    }
  }

  static void DispatchDisplayTransform(MetalRenderDevice& device, const FrameSceneBinding& scene,
                                       const FrameSceneBinding& display, const NodeId& drt_id,
                                       std::uint32_t, std::uint32_t) {
    auto&                  arena   = device.Workspace().Parameters();
    const ParameterSlotKey key{drt_id, AdjustmentInstanceId{"drt.output"}};
    const auto             binding = arena.Binding(key);
    DispatchDrt(device, MetalSceneTexture(device, scene), MetalSceneTexture(device, display),
                arena.DeviceBuffer(), binding.offset);
  }

  static void CheckAfterEncode(MetalRenderDevice&) {}
};

}  // namespace

void AppendMetalDrtWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_DRT_METALLIB_PATH
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, "drt_display", "Metal DRT"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH,
                                          "drt_neighbor_blur_horizontal",
                                          "Metal DRT Post Horizontal"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH,
                                          "drt_neighbor_apply_vertical",
                                          "Metal DRT Post Vertical"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalDrt(MetalRenderDevice& device, const ExecutionPlan& plan,
                     PipelineDocument& document, const FrameSceneBinding& scene) -> MetalDrtResult {
  const auto executed = DrtPostExecutor<MetalDrtOps>::Execute(device, plan, document, scene);
  return {executed.output, executed.display_post, executed.post_neighborhood_count};
}

}  // namespace alcedo
