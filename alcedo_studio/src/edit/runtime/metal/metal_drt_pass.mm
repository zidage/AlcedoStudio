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
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/metal/metal_drt_gpu_params.hpp"
#include "edit/runtime/metal/metal_film_grain_pass.hpp"
#include "edit/runtime/metal/metal_film_print_pass.hpp"
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

/// Film Grain has its own pass (RecordMetalFilmGrainPost), so these kernels do not take it.
void RequireNeighborhoodKernelBehavior(const GradeNeighborParams& params, const char* caller) {
  if (static_cast<AdjustmentBehavior>(params.behavior) == AdjustmentBehavior::FilmGrain) {
    throw std::invalid_argument(std::string(caller) +
                                ": Film Grain runs RecordMetalFilmGrainPost, not this kernel");
  }
}

/** @brief Start the display-referred horizontal neighborhood kernel. */
void DispatchNeighborHorizontal(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                                MetalBackend::Texture2D& dst, const GradeNeighborParams& params,
                                std::uint32_t width, std::uint32_t height) {
  RequireNeighborhoodKernelBehavior(params, "DispatchNeighborHorizontal");
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
  RequireNeighborhoodKernelBehavior(params, "DispatchNeighborVertical");
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

/// @p profile_tone is the buffer of the profile tone stage, or null without the stage. The kernel
/// then reads no stage buffer, and the parameter buffer fills its argument.
void DispatchDrt(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                 MetalBackend::Texture2D& dst, const MetalBackend::Buffer& params,
                 std::uint32_t offset, const MetalBackend::Buffer* profile_tone) {
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
  const std::uint32_t profile_tone_enabled = profile_tone != nullptr ? 1U : 0U;
  encoder->setBuffer(
      static_cast<MTL::Buffer*>(profile_tone != nullptr ? profile_tone->Native() : params.Native()),
      0, 1);
  encoder->setBytes(&profile_tone_enabled, sizeof(profile_tone_enabled), 2);
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
  static constexpr bool        kImplementsFilmPrint = true;

  static void DispatchFilmGrain(MetalRenderDevice& device, const FrameSceneBinding& src,
                                const FrameSceneBinding& dst, const FilmGrainPlan& grain,
                                FilmGrainDisplayEncoding      encoding,
                                const ResolvedRenderGeometry& geometry, std::uint32_t width,
                                std::uint32_t height) {
    RecordMetalFilmGrainPost(device, grain, geometry, encoding, MetalSceneTexture(device, src),
                             MetalSceneTexture(device, dst), width, height);
  }

  static void RefreshNeighborhoodAdjustment(MetalRenderDevice& device, const IOperatorModel& model,
                                            const ParameterSlotKey& key,
                                            AdjustmentBehavior      behavior) {
    BindOrRefreshGradeRuntimeSlot(device.Workspace().Parameters(), key, model, behavior);
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

  /**
   * @brief Pack the display transform when the slot is missing, the DRT revision changed,
   *        or this frame overrides the output color.
   *
   * An override is recorded without a revision, so the next plain frame packs again.
   */
  static void BindDisplayParams(MetalRenderDevice& device, const ExecutionPlan& plan,
                                const DrtNodeModel& drt) {
    auto&                  arena = device.Workspace().Parameters();
    const ParameterSlotKey key{drt.Id(), AdjustmentInstanceId{"drt.output"}};
    const bool             overridden = plan.output_color_override.has_value();
    const auto             revision   = drt.Params().Revision();
    if (!overridden && arena.Contains(key) && arena.AppliedRevision(key) == revision) {
      return;
    }
    const auto runtime = PackMetalDrtGpuParams(
        DrtOutputResolver::ResolveNode(drt, plan.output_color_override, kErrorPrefix));
    arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, runtime,
                                overridden ? kNoParameterRevision : revision);
  }

  static void DispatchDisplayTransform(MetalRenderDevice& device, const FrameSceneBinding& scene,
                                       const FrameSceneBinding& display, const NodeId& drt_id,
                                       const MetalBackend::Buffer* profile_tone, std::uint32_t,
                                       std::uint32_t) {
    auto&                  arena   = device.Workspace().Parameters();
    const ParameterSlotKey key{drt_id, AdjustmentInstanceId{"drt.output"}};
    const auto             binding = arena.Binding(key);
    DispatchDrt(device, MetalSceneTexture(device, scene), MetalSceneTexture(device, display),
                arena.DeviceBuffer(), binding.offset, profile_tone);
  }

  static void DispatchFilmPrint(MetalRenderDevice& device, const ExecutionPlan& plan,
                                const DrtNodeModel& drt, std::optional<FilmRawWhite> raw_white,
                                const FrameSceneBinding& scene, const FrameSceneBinding& display,
                                std::uint32_t width, std::uint32_t height, float grain_strength) {
    RecordMetalFilmPrint(device, plan, drt, raw_white, scene, display, width, height,
                         grain_strength);
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
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, "film_grain_post",
                                          "Metal DRT Post Film Grain"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalDrt(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document, const FrameSceneBinding& scene)
    -> MetalDrtResult {
  const auto executed = DrtPostExecutor<MetalDrtOps>::Execute(device, plan, document, scene);
  return {executed.output, executed.display_post, executed.post_neighborhood_count};
}

}  // namespace alcedo
