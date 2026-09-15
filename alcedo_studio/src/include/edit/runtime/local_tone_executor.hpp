//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_plan.hpp"
#include "edit/runtime/gpu_work_sample.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {

/**
 * @brief Shared LLF encode outcome used by CUDA, OpenCL, and Metal wrappers.
 */
struct LocalToneExecutionResult {
  std::uint64_t          reference_resource_id       = 0;
  bool                   rebuilt_reference           = false;
  bool                   sampled_canonical_reference = false;
  std::uint32_t          transient_bytes             = 0;
  LocalToneDecisionTrace trace;
};

/**
 * @brief Source/result planes prepared for one Grade encode. Not stored across Grades.
 *
 * @tparam Ops Backend plane type. Higher pyramid levels are released before Apply.
 */
template <class Ops>
struct LocalTonePreparedMaps {
  typename Ops::ScratchPlane source0{};
  typename Ops::ScratchPlane result0{};
  LocalToneDecision          decision{};
  LocalToneExecutionResult   result{};
  bool                       sample_canonical = false;
};

/**
 * @brief Common LLF source/result reuse, pyramid order, remap/collapse, persist, and apply.
 *
 * @tparam Ops Backend plane allocation, dispatch, canonical bind/publish, and copies.
 *         Pyramid levels above source.0/result.0 are scratch on every backend.
 */
template <class Ops>
class LocalToneExecutor {
 public:
  using Device = typename Ops::Device;

  /**
   * @brief Build or bind canonical LLF planes from the Grade's post-tone/color pixels.
   *
   * Does not write scene RGBA. ApplyMix consumes the returned planes in this Grade
   * encode only. Failures throw before Ops::PersistCanonical.
   *
   * @throws std::runtime_error when BeginRender was not called, extents are empty,
   *         or a backend dispatch fails.
   */
  static auto Prepare(Device& device, const FrameSceneBinding& input, const NodeId& grade_id,
                      float shadows_slider, float highlights_slider,
                      const ResolvedRenderGeometry& geometry) -> LocalTonePreparedMaps<Ops> {
    auto& workspace = device.Workspace();
    if (!workspace.IsRendering()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": BeginRender has not been called");
    }
    if (geometry.full_reference_extent.Empty() || geometry.render_extent.Empty()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                               ": geometry extents must be positive");
    }
    const auto width  = Ops::BindingWidth(device, input);
    const auto height = Ops::BindingHeight(device, input);

    const auto source_id   = LocalToneSourceId(grade_id);
    const auto result_id   = LocalToneResultId(grade_id);
    const bool persist_llf = workspace.PersistsResult(source_id);
    if (!persist_llf) {
      device.PassStats().result_policy_bypass += 2;
    }
    const bool full_edit =
        CoversFullEditSpace(geometry);
    const int current_long_edge =
        std::max(static_cast<int>(width), static_cast<int>(height));
    const auto lookup =
        persist_llf ? Ops::LookupCanonical(device, source_id, result_id, current_long_edge, geometry)
                    : LocalToneCanonicalLookup{};
    const auto decision =
        DecideLocalTone(persist_llf, lookup, full_edit, current_long_edge, width, height, geometry);

    LocalTonePreparedMaps<Ops> maps;
    maps.decision     = decision;
    maps.result.trace = MakeLocalToneDecisionTrace(decision);
    const auto transient_mark = Ops::TransientBytes(device);
    if (decision.action == LocalToneAction::SampleCanonical) {
      maps.sample_canonical                    = true;
      maps.result.sampled_canonical_reference  = true;
      maps.result.reference_resource_id        = Ops::CanonicalResourceId(device, source_id);
      maps.result.transient_bytes =
          static_cast<std::uint32_t>(Ops::TransientBytes(device) - transient_mark);
      return maps;
    }

    using Plane = typename Ops::ScratchPlane;
    std::array<Plane, local_tone_mapping::kMaxLevels> source{};
    std::array<Plane, local_tone_mapping::kMaxLevels> remap_a{};
    std::array<Plane, local_tone_mapping::kMaxLevels> remap_b{};
    std::array<Plane, local_tone_mapping::kMaxLevels> result{};
    for (int level = 0; level < decision.pyramid_level_count; ++level) {
      const auto bytes = static_cast<std::size_t>(decision.widths[level]) *
                         static_cast<std::size_t>(decision.heights[level]) * sizeof(float);
      if (level == 0 && decision.reuse_source) {
        source[level] = Ops::BindCanonicalSourcePlane(device, source_id, bytes);
      } else {
        source[level] = Ops::AllocateScratchPlane(device, bytes);
      }
      remap_a[level] = Ops::AllocateScratchPlane(device, bytes);
      remap_b[level] = Ops::AllocateScratchPlane(device, bytes);
      result[level]  = Ops::AllocateScratchPlane(device, bytes);
    }
    maps.result.transient_bytes =
        static_cast<std::uint32_t>(Ops::TransientBytes(device) - transient_mark);

    if (!decision.reuse_source) {
      diag::PreviewSubStageInterval extract(diag::PreviewSubStageKind::LlfExtract);
      GpuWorkSample<Device> gpu(device);
      if (decision.write_canonical_reference) {
        Ops::ExtractReference(device, input, source[0], width, height, decision, geometry);
      } else {
        Ops::Extract(device, input, source[0], width, height, decision);
      }
    }
    {
      diag::PreviewSubStageInterval pyramid(diag::PreviewSubStageKind::LlfPyramid);
      GpuWorkSample<Device> gpu(device);
      for (int level = 1; level < decision.pyramid_level_count; ++level) {
        Ops::PyramidDown(device, source[level - 1], source[level], decision, level);
      }
    }

    const float shadow_amount    = ClampedLocalToneShadow(shadows_slider);
    const float highlight_amount = ClampedLocalToneHighlight(highlights_slider);
    const float sigma            = local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
    const auto  samples          = local_tone_mapping::BuildSamples(shadow_amount, highlight_amount);
    for (int level = 0; level < decision.pyramid_level_count; ++level) {
      Ops::FillZero(device, result[level]);
    }
    auto BuildRemap = [&](const local_tone_mapping::LlfSample& sample,
                          std::array<Plane, local_tone_mapping::kMaxLevels>& levels) {
      Ops::Remap(device, source[0], levels[0], decision, sample, sigma);
      for (int level = 1; level < decision.pyramid_level_count; ++level) {
        Ops::PyramidDown(device, levels[level - 1], levels[level], decision, level);
      }
    };
    {
      diag::PreviewSubStageInterval remap(diag::PreviewSubStageKind::LlfRemap);
      GpuWorkSample<Device> gpu(device);
      BuildRemap(samples[0], remap_a);
      BuildRemap(samples[1], remap_b);
    }
    {
      diag::PreviewSubStageInterval select(diag::PreviewSubStageKind::LlfSelect);
      GpuWorkSample<Device> gpu(device);
      for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
        for (int level = 0; level < decision.pyramid_level_count; ++level) {
          const bool top = level + 1 == decision.pyramid_level_count;
          Ops::Select(device, source[level], remap_a[level],
                      top ? remap_a[level] : remap_a[level + 1], remap_b[level],
                      top ? remap_b[level] : remap_b[level + 1], result[level], decision, level,
                      samples[pair], samples[pair + 1], pair == 0, pair + 2 == samples.size(),
                      top);
        }
        if (pair + 2 < samples.size()) {
          std::swap(remap_a, remap_b);
          BuildRemap(samples[pair + 2], remap_b);
        }
      }
    }
    {
      diag::PreviewSubStageInterval collapse(diag::PreviewSubStageKind::LlfCollapse);
      GpuWorkSample<Device> gpu(device);
      for (int level = decision.pyramid_level_count - 2; level >= 0; --level) {
        Ops::Collapse(device, result[level], result[level + 1], remap_a[level], decision, level);
        std::swap(result[level], remap_a[level]);
      }
    }

    if (decision.persist_canonical) {
      if (!decision.reuse_source) {
        Ops::PersistCanonicalSource(device, source[0], source_id, decision, current_long_edge);
      }
      Ops::PersistCanonicalResult(device, result[0], result_id, decision, current_long_edge);
      maps.result.reference_resource_id = Ops::CanonicalResourceId(device, source_id);
    }
    maps.source0                 = source[0];
    maps.result0                 = result[0];
    maps.result.rebuilt_reference = true;
    return maps;
  }

  /**
   * @brief Apply prepared LLF planes to working pixels and mix with the Grade input.
   *
   * Reads original[p] and working[p] before writing working[p]. Neighborhood samples
   * come only from the independent LLF planes. working may alias the adjusted input
   * when that input already lives in the destination member.
   */
  static void ApplyMix(Device& device, const LocalTonePreparedMaps<Ops>& maps,
                       const NodeId& grade_id, const FrameSceneBinding& original,
                       const FrameSceneBinding& working, const FrameSceneBinding& adjusted,
                       float mix, const GraphValueId* mask_id, std::uint32_t width,
                       std::uint32_t height) {
    diag::PreviewSubStageInterval apply(diag::PreviewSubStageKind::LlfApply);
    GpuWorkSample<Device> gpu(device);
    if (maps.sample_canonical) {
      Ops::ApplyCanonicalSampleAndMix(device, original, working, adjusted, grade_id, mix, mask_id,
                                      maps.decision, width, height);
      return;
    }
    Ops::ApplyAdjustedAndMix(device, original, working, adjusted, maps.source0, maps.result0, mix,
                             mask_id, width, height, maps.decision.widths[0],
                             maps.decision.heights[0], maps.decision.apply_uv);
  }

  /**
   * @brief Prepare then apply with mix. Used by backend wrappers.
   */
  static auto Execute(Device& device, const FrameSceneBinding& adjusted,
                      const FrameSceneBinding& working, const FrameSceneBinding& original,
                      const NodeId& grade_id, float shadows_slider, float highlights_slider,
                      const ResolvedRenderGeometry& geometry, float mix,
                      const GraphValueId* mask_id) -> LocalToneExecutionResult {
    auto maps = Prepare(device, adjusted, grade_id, shadows_slider, highlights_slider, geometry);
    const auto width  = Ops::BindingWidth(device, adjusted);
    const auto height = Ops::BindingHeight(device, adjusted);
    ApplyMix(device, maps, grade_id, original, working, adjusted, mix, mask_id, width, height);
    return maps.result;
  }
};

}  // namespace alcedo
