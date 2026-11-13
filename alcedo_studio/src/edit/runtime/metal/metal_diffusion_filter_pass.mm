//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_diffusion_filter_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

/// Mirrors `DiffusionReduceParams` in drt.metal.
struct ReduceParams {
  std::int32_t src_width  = 0;
  std::int32_t src_height = 0;
  std::int32_t dst_width  = 0;
  std::int32_t dst_height = 0;
  std::int32_t samples    = 1;
  float        gain       = 0.0f;
  float        low        = 0.0f;
  float        high       = 0.0f;
  float        base_to_render[12]{};
};
static_assert(sizeof(ReduceParams) == 80);

/// Mirrors `DiffusionMixParams` in drt.metal.
struct MixParams {
  std::int32_t width            = 0;
  std::int32_t height           = 0;
  float        scatter_fraction = 0.0f;
  float        transmission     = 1.0f;
  float        render_to_base[12]{};
};
static_assert(sizeof(MixParams) == 64);

constexpr const char* kDecodeFunction             = "diffusion_filter_decode";
constexpr const char* kReduceBoostFunction        = "diffusion_filter_reduce_boost";
constexpr const char* kDownsampleFunction         = "diffusion_filter_downsample";
constexpr const char* kUpsampleAccumulateFunction = "diffusion_filter_upsample_accumulate";
constexpr const char* kMixFunction                = "diffusion_filter_mix";

constexpr const char* kDecodeLabel                = "Metal DiffusionFilter Decode";
constexpr const char* kReduceBoostLabel           = "Metal DiffusionFilter Reduce Boost";
constexpr const char* kDownsampleLabel            = "Metal DiffusionFilter Downsample";
constexpr const char* kUpsampleAccumulateLabel    = "Metal DiffusionFilter Upsample Accumulate";
constexpr const char* kMixLabel                   = "Metal DiffusionFilter Mix";

void                  CopyMatrix(float* destination, const Matrix3x3& matrix) {
  for (int index = 0; index < 9; ++index) {
    destination[index] = matrix.m[index];
  }
}

auto Pipeline(const char* function, const char* label) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_DRT_METALLIB_PATH
  (void)function;
  (void)label;
  throw std::runtime_error("Metal DRT metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(ALCEDO_METAL_DRT_METALLIB_PATH,
                                                                  function, label);
#endif
}

/**
 * @brief Bind @p function on the frame's compute encoder.
 *
 * The encoder dispatches serially and the textures use tracked hazards, so each dispatch sees
 * the writes of the previous one.
 */
auto BeginDispatch(MetalRenderDevice& device, const char* function, const char* label)
    -> std::pair<MTL::ComputeCommandEncoder*, NS::SharedPtr<MTL::ComputePipelineState>> {
  auto  pipeline = Pipeline(function, label);
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalDiffusionFilter: compute encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  return {encoder, std::move(pipeline)};
}

void Dispatch(MetalRenderDevice& device, MTL::ComputeCommandEncoder* encoder,
              MTL::ComputePipelineState* pipeline, std::uint32_t width, std::uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1}, MTL::Size{thread_width, thread_height, 1});
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

auto Native(const MetalBackend::Texture2D& texture) -> MTL::Texture* {
  return static_cast<MTL::Texture*>(texture.Native());
}

void EncodeDecode(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                  const MetalBackend::Texture2D& dst) {
  auto [encoder, pipeline] = BeginDispatch(device, kDecodeFunction, kDecodeLabel);
  encoder->setTexture(Native(src), 0);
  encoder->setTexture(Native(dst), 1);
  Dispatch(device, encoder, pipeline.get(), src.Width(), src.Height());
}

/**
 * @brief Build the scatter image of @p layout from the render @p scene into @p target.
 *
 * @p target has the base-level extent. Pyramid levels are pooled scratch leases, returned at
 * scope exit; later users encode into the same serial compute encoder, so these dispatches
 * finish before any reuse.
 */
void BuildScatter(MetalRenderDevice& device, const MetalBackend::Texture2D& scene,
                  const DiffusionFilterLayout& layout, const DiffusionScatterMapping& mapping,
                  MetalBackend::Texture2D& target) {
  auto&                                    workspace = device.Workspace();
  const auto                               count     = layout.level_count;
  std::vector<ResourceLease<MetalBackend>> leases;
  leases.reserve(2U * count);
  auto acquire = [&](ImageExtent extent) -> MetalBackend::Texture2D* {
    leases.push_back(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    return &leases.back().Texture();
  };
  // With one level the reduction is the scatter image. Otherwise the level-0 accumulation is.
  std::vector<MetalBackend::Texture2D*> levels(count, nullptr);
  std::vector<MetalBackend::Texture2D*> accumulated(count, nullptr);
  for (std::uint32_t index = 0; index < count; ++index) {
    levels[index] = count == 1 ? &target : acquire(layout.extents[index]);
    if (index + 1 < count) {
      accumulated[index] = index == 0 ? &target : acquire(layout.extents[index]);
    }
  }

  const auto base = layout.extents[0];
  {
    ReduceParams params;
    params.src_width  = static_cast<std::int32_t>(scene.Width());
    params.src_height = static_cast<std::int32_t>(scene.Height());
    params.dst_width  = static_cast<std::int32_t>(base.width);
    params.dst_height = static_cast<std::int32_t>(base.height);
    params.samples    = static_cast<std::int32_t>(mapping.reduce_samples);
    params.gain       = layout.highlight_gain;
    params.low        = layout.highlight_low;
    params.high       = layout.highlight_high;
    CopyMatrix(params.base_to_render, mapping.base_to_render);
    auto [encoder, pipeline] = BeginDispatch(device, kReduceBoostFunction, kReduceBoostLabel);
    encoder->setTexture(Native(scene), 0);
    encoder->setTexture(Native(*levels[0]), 1);
    encoder->setBytes(&params, sizeof(params), 0);
    Dispatch(device, encoder, pipeline.get(), base.width, base.height);
  }
  for (std::uint32_t index = 1; index < count; ++index) {
    const auto to            = layout.extents[index];
    auto [encoder, pipeline] = BeginDispatch(device, kDownsampleFunction, kDownsampleLabel);
    encoder->setTexture(Native(*levels[index - 1]), 0);
    encoder->setTexture(Native(*levels[index]), 1);
    Dispatch(device, encoder, pipeline.get(), to.width, to.height);
  }

  // Accumulate from the coarsest level. The coarsest level enters with its own weight.
  const MetalBackend::Texture2D* coarse        = levels[count - 1];
  float                          coarse_weight = layout.weights[count - 1];
  for (std::uint32_t index = count - 1; index-- > 0;) {
    const auto  extent       = layout.extents[index];
    const float level_weight = layout.weights[index];
    auto [encoder, pipeline] =
        BeginDispatch(device, kUpsampleAccumulateFunction, kUpsampleAccumulateLabel);
    encoder->setTexture(Native(*coarse), 0);
    encoder->setTexture(Native(*levels[index]), 1);
    encoder->setTexture(Native(*accumulated[index]), 2);
    encoder->setBytes(&coarse_weight, sizeof(coarse_weight), 0);
    encoder->setBytes(&level_weight, sizeof(level_weight), 1);
    Dispatch(device, encoder, pipeline.get(), extent.width, extent.height);
    coarse        = accumulated[index];
    coarse_weight = 1.0f;
  }
}

void EncodeMix(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
               const MetalBackend::Texture2D& dst, const MetalBackend::Texture2D& scatter,
               const DiffusionFilterLayout& layout, const DiffusionScatterMapping& mapping) {
  MixParams params;
  params.width            = static_cast<std::int32_t>(src.Width());
  params.height           = static_cast<std::int32_t>(src.Height());
  params.scatter_fraction = layout.scatter_fraction;
  params.transmission     = layout.transmission;
  CopyMatrix(params.render_to_base, mapping.render_to_base);
  auto [encoder, pipeline] = BeginDispatch(device, kMixFunction, kMixLabel);
  encoder->setTexture(Native(src), 0);
  encoder->setTexture(Native(dst), 1);
  encoder->setTexture(Native(scatter), 2);
  encoder->setBytes(&params, sizeof(params), 0);
  Dispatch(device, encoder, pipeline.get(), src.Width(), src.Height());
}

}  // namespace

void AppendMetalDiffusionFilterWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_DRT_METALLIB_PATH
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, kDecodeFunction, kDecodeLabel});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, kReduceBoostFunction, kReduceBoostLabel});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, kDownsampleFunction, kDownsampleLabel});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH,
                                          kUpsampleAccumulateFunction, kUpsampleAccumulateLabel});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_DRT_METALLIB_PATH, kMixFunction, kMixLabel});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalDiffusionFilter(MetalRenderDevice& device, const ExecutionPlan& plan,
                                 const PipelineDocument& document, const FrameSceneBinding& scene)
    -> FrameSceneBinding {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalDiffusionFilter: BeginRender has not been called");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    throw std::runtime_error("ExecuteMetalDiffusionFilter: missing DRT endpoint");
  }
  const auto output = FrameSceneBinding::WorkImage(DestinationWorkMember(scene));
  auto&      src    = MetalSceneTexture(device, scene);
  auto&      dst    = MetalSceneTexture(device, output);
  const auto width  = src.Width();
  const auto height = src.Height();
  if (dst.Width() != width || dst.Height() != height) {
    throw std::runtime_error("ExecuteMetalDiffusionFilter: scene-work extent does not match scene");
  }

  const float strength = drt->Params().DiffusionStrength();
  if (!IsDiffusionFilterActive(strength)) {
    EncodeDecode(device, src, dst);
    return output;
  }

  const auto& geometry = plan.geometry;
  const auto  layout   = MakeDiffusionFilterLayout(
      DiffusionCanvasExtent(geometry.full_reference_extent), ResolveDiffusionFilterShape(strength));
  const auto                   mapping      = MakeDiffusionScatterMapping(geometry, layout);
  const auto                   base         = layout.extents[0];

  // The scatter image covers the full frame. A full-edit render builds and publishes it; an
  // ROI render samples the published image, so light outside the viewport still scatters in.
  auto&                        invalidation = workspace.ResultInvalidation();
  const auto                   scatter_id   = DiffusionScatterId(plan.drt.node_id);
  const bool                   persist      = workspace.PersistsResult(scatter_id);
  const bool                   full_edit    = CoversFullEditSpace(geometry);
  const auto                   long_edge    = (std::max)(width, height);
  ResourceLease<MetalBackend>* canonical    = nullptr;
  if (persist) {
    const auto needed =
        invalidation.MakeImageRepresentation(scatter_id, base, TextureFormat::Rgba32f,
                                             DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical =
        workspace.Images().BindValidResult(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           needed, workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);

  const MetalBackend::Texture2D*             scatter = nullptr;
  std::optional<ResourceLease<MetalBackend>> transient_scatter;
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    scatter = &canonical->Texture();
    ++device.PassStats().diffusion_scatter_sample;
  } else {
    MetalBackend::Texture2D* target = nullptr;
    if (decision.persist_canonical) {
      target =
          &workspace
               .AcquireImageForWrite(scatter_id, {base.width, base.height, TextureFormat::Rgba32f})
               .Texture();
    } else {
      transient_scatter.emplace(
          workspace.Textures().Acquire({base.width, base.height, TextureFormat::Rgba32f}));
      target = &transient_scatter->Texture();
    }
    BuildScatter(device, src, layout, mapping, *target);
    if (decision.persist_canonical) {
      const auto published = invalidation.MakeImageRepresentation(
          scatter_id, base, TextureFormat::Rgba32f, decision.current_long_edge);
      workspace.Images().RecordUnpublished(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           published, device.CommandContext().SubmissionId(),
                                           decision.current_long_edge);
    }
    scatter = target;
    ++device.PassStats().diffusion_scatter_rebuild;
  }

  EncodeMix(device, src, dst, *scatter, layout, mapping);
  return output;
}

}  // namespace alcedo
