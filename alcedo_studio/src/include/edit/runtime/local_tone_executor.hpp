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
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_plan.hpp"

namespace alcedo {

/**
 * @brief Shared LLF encode outcome used by CUDA, OpenCL, and Metal wrappers.
 */
struct LocalToneExecutionResult {
  std::uint64_t           reference_resource_id       = 0;
  bool                    rebuilt_reference           = false;
  bool                    sampled_canonical_reference = false;
  std::uint32_t           transient_bytes             = 0;
  LocalToneDecisionTrace  trace;
};

/**
 * @brief Common LLF source/result reuse, pyramid order, remap/collapse, and persist.
 *
 * @tparam Ops Backend plane allocation, dispatch, canonical bind/publish, and copies.
 *         Pyramid levels above source.0/result.0 are scratch on every backend.
 */
template <class Ops>
class LocalToneExecutor {
 public:
  using Device = typename Ops::Device;

  /**
   * @brief Apply Shadows/Highlights LLF to matching RGBA32F textures.
   *
   * Canonical sample returns without rebuilding pyramids. Rebuild failures throw
   * before Ops::PersistCanonical, so a failed write cannot publish reusable metadata.
   *
   * @throws std::runtime_error when BeginRender was not called, extents are empty,
   *         or a backend dispatch fails.
   */
  static auto Execute(Device& device, const typename Ops::Texture& input,
                      typename Ops::Texture& output, const NodeId& grade_id, float shadows_slider,
                      float highlights_slider, const ResolvedRenderGeometry& geometry)
      -> LocalToneExecutionResult {
    auto& workspace = device.Workspace();
    if (!workspace.IsRendering()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": BeginRender has not been called");
    }
    if (geometry.full_reference_extent.Empty() || geometry.render_extent.Empty()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                               ": geometry extents must be positive");
    }
    const auto width  = Ops::TextureWidth(input);
    const auto height = Ops::TextureHeight(input);
    if (Ops::TextureWidth(output) != width || Ops::TextureHeight(output) != height) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                               ": input and output must be matching RGBA32F");
    }

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

    LocalToneExecutionResult tone;
    tone.trace = MakeLocalToneDecisionTrace(decision);
    const auto transient_mark = Ops::TransientBytes(device);
    if (decision.action == LocalToneAction::SampleCanonical) {
      Ops::ApplyCanonicalSample(device, input, output, source_id, result_id, decision, width,
                                height);
      tone.sampled_canonical_reference = true;
      tone.reference_resource_id       = Ops::CanonicalResourceId(device, source_id);
      tone.transient_bytes =
          static_cast<std::uint32_t>(Ops::TransientBytes(device) - transient_mark);
      return tone;
    }

    using Plane                              = typename Ops::ScratchPlane;
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
    tone.transient_bytes =
        static_cast<std::uint32_t>(Ops::TransientBytes(device) - transient_mark);

    if (!decision.reuse_source) {
      if (decision.write_canonical_reference) {
        Ops::ExtractReference(device, input, source[0], width, height, decision, geometry);
      } else {
        Ops::Extract(device, input, source[0], width, height, decision);
      }
    }
    for (int level = 1; level < decision.pyramid_level_count; ++level) {
      Ops::PyramidDown(device, source[level - 1], source[level], decision, level);
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
    BuildRemap(samples[0], remap_a);
    BuildRemap(samples[1], remap_b);
    for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
      for (int level = 0; level < decision.pyramid_level_count; ++level) {
        const bool top = level + 1 == decision.pyramid_level_count;
        Ops::Select(device, source[level], remap_a[level],
                    top ? remap_a[level] : remap_a[level + 1], remap_b[level],
                    top ? remap_b[level] : remap_b[level + 1], result[level], decision, level,
                    samples[pair], samples[pair + 1], pair == 0, pair + 2 == samples.size(), top);
      }
      if (pair + 2 < samples.size()) {
        std::swap(remap_a, remap_b);
        BuildRemap(samples[pair + 2], remap_b);
      }
    }
    for (int level = decision.pyramid_level_count - 2; level >= 0; --level) {
      Ops::Collapse(device, result[level], result[level + 1], remap_a[level], decision, level);
      std::swap(result[level], remap_a[level]);
    }

    Ops::ApplyAdjusted(device, input, output, source[0], result[0], width, height, decision);
    if (decision.persist_canonical) {
      if (!decision.reuse_source) {
        Ops::PersistCanonicalSource(device, source[0], source_id, decision, current_long_edge);
      }
      Ops::PersistCanonicalResult(device, result[0], result_id, decision, current_long_edge);
      tone.reference_resource_id = Ops::CanonicalResourceId(device, source_id);
    }
    tone.rebuilt_reference = true;
    return tone;
  }
};

}  // namespace alcedo
