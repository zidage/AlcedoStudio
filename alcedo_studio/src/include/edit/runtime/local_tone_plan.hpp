//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/geometry/types.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/content_key.hpp"

namespace alcedo {

/** @brief Host action chosen before any LLF GPU work. */
enum class LocalToneAction : std::uint8_t {
  SampleCanonical,
  RebuildResult,
  RebuildSourceAndResult,
};

/**
 * @brief Canonical LLF lookup used by @ref DecideLocalTone.
 *
 * @p source_valid / @p result_valid are already persistence- and revision-qualified.
 */
struct LocalToneCanonicalLookup {
  bool        source_valid       = false;
  bool        result_valid       = false;
  int         source_long_edge   = 0;
  ImageExtent extent{};
};

/** @brief Width/height of each LLF pyramid level for one mask plane. */
struct LocalTonePyramidLayout {
  int                                             count = 0;
  std::array<int, local_tone_mapping::kMaxLevels> widths{};
  std::array<int, local_tone_mapping::kMaxLevels> heights{};
};

/**
 * @brief Shared LLF source/result/scratch decision for one Grade encode.
 *
 * Pyramid levels above source.0 / result.0 are scratch. Canonical persist writes those
 * two planes only. Owner: LocalToneExecutor. Not persisted to the document.
 */
struct LocalToneDecision {
  LocalToneAction action                   = LocalToneAction::RebuildSourceAndResult;
  bool            persist_canonical        = false;
  bool            write_canonical_reference = false;
  bool            reuse_source             = false;
  ImageExtent     mask_extent{};
  int             current_long_edge        = 0;
  int             pyramid_level_count      = 0;
  std::array<int, local_tone_mapping::kMaxLevels> widths{};
  std::array<int, local_tone_mapping::kMaxLevels> heights{};
  Matrix3x3       apply_uv{};
};

/**
 * @brief Comparable LLF decisions for equivalent geometry and cache state.
 *
 * Backends may bind images or buffers; these fields must match for the same inputs.
 */
struct LocalToneDecisionTrace {
  LocalToneAction action                    = LocalToneAction::RebuildSourceAndResult;
  bool            persist_canonical         = false;
  bool            write_canonical_reference = false;
  bool            reuse_source              = false;
  ImageExtent     mask_extent{};
  int             pyramid_level_count       = 0;
};

[[nodiscard]] inline auto MakeLocalToneIdentityUv(std::uint32_t width, std::uint32_t height)
    -> Matrix3x3 {
  Matrix3x3 matrix;
  matrix.m[0] = 1.0f / static_cast<float>(width);
  matrix.m[4] = 1.0f / static_cast<float>(height);
  return matrix;
}

[[nodiscard]] inline auto ScaledLocalToneShadow(float slider) -> float {
  return slider * local_tone_mapping::kHighlightStrengthScale / 80.0f;
}

[[nodiscard]] inline auto ScaledLocalToneHighlight(float slider) -> float {
  return -slider * local_tone_mapping::kHighlightStrengthScale / 100.0f;
}

[[nodiscard]] inline auto ClampedLocalToneShadow(float slider) -> float {
  return std::clamp(ScaledLocalToneShadow(slider), -local_tone_mapping::kBackendAmountLimit,
                    local_tone_mapping::kBackendAmountLimit);
}

[[nodiscard]] inline auto ClampedLocalToneHighlight(float slider) -> float {
  return std::clamp(ScaledLocalToneHighlight(slider), -local_tone_mapping::kBackendAmountLimit,
                    local_tone_mapping::kBackendAmountLimit);
}

/**
 * @brief Pyramid widths/heights for an LLF mask plane.
 *
 * Level 0 is @p width by @p height. Higher levels halve, floored at 1.
 */
[[nodiscard]] inline auto MakeLocalTonePyramidLayout(int width, int height)
    -> LocalTonePyramidLayout {
  LocalTonePyramidLayout layout;
  layout.count =
      local_tone_mapping::ComputeLevelCount(width, height, local_tone_mapping::kPyramidRadius);
  layout.widths[0]  = width;
  layout.heights[0] = height;
  for (int level = 1; level < layout.count; ++level) {
    layout.widths[level]  = std::max(1, (layout.widths[level - 1] + 1) / 2);
    layout.heights[level] = std::max(1, (layout.heights[level - 1] + 1) / 2);
  }
  return layout;
}

/**
 * @brief Choose sample / reuse-source / full rebuild for one LLF encode.
 *
 * @param persist_llf False for QualityBase downstream bypass. Isolated work still runs.
 * @param lookup Persistence-qualified canonical hit. Ignored when @p persist_llf is false.
 * @param input_width Render-space width of the Grade stage being adjusted.
 */
[[nodiscard]] inline auto DecideLocalTone(bool persist_llf, const LocalToneCanonicalLookup& lookup,
                                          bool full_edit, int current_long_edge,
                                          std::uint32_t input_width, std::uint32_t input_height,
                                          const ResolvedRenderGeometry& geometry)
    -> LocalToneDecision {
  const auto canonical = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(geometry.full_reference_extent.width),
      static_cast<int>(geometry.full_reference_extent.height),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const bool source_valid =
      persist_llf && lookup.source_valid && lookup.source_long_edge > 0;
  const bool result_valid = source_valid && lookup.result_valid;
  const bool upgrade_detail =
      full_edit && current_long_edge > lookup.source_long_edge;
  const bool sample_canonical = result_valid && !upgrade_detail;
  const bool reuse_source     = source_valid && !upgrade_detail;

  LocalToneDecision decision;
  decision.current_long_edge         = current_long_edge;
  decision.write_canonical_reference = full_edit;
  decision.reuse_source              = reuse_source;
  decision.persist_canonical         = persist_llf && (full_edit || reuse_source);
  if (sample_canonical) {
    decision.action       = LocalToneAction::SampleCanonical;
    decision.reuse_source = false;
    decision.mask_extent  = lookup.extent.width != 0
                                ? lookup.extent
                                : ImageExtent{static_cast<std::uint32_t>(canonical.width),
                                              static_cast<std::uint32_t>(canonical.height)};
    const auto sampling  = MakeLlfSamplingPlan(
        geometry, Extent2D{decision.mask_extent.width, decision.mask_extent.height});
    decision.apply_uv = sampling.render_to_texture_uv;
    return decision;
  }

  const auto mask_dims = full_edit || reuse_source
                             ? canonical
                             : local_tone_mapping::ComputeMaskDimensions(
                                   static_cast<int>(input_width), static_cast<int>(input_height),
                                   local_tone_mapping::kReferenceMaskMaxLongEdge);
  const auto layout            = MakeLocalTonePyramidLayout(mask_dims.width, mask_dims.height);
  decision.action              = reuse_source ? LocalToneAction::RebuildResult
                                              : LocalToneAction::RebuildSourceAndResult;
  decision.mask_extent         = {static_cast<std::uint32_t>(mask_dims.width),
                                  static_cast<std::uint32_t>(mask_dims.height)};
  decision.pyramid_level_count = layout.count;
  decision.widths              = layout.widths;
  decision.heights             = layout.heights;
  decision.apply_uv =
      full_edit || reuse_source
          ? MakeLlfSamplingPlan(geometry, Extent2D{decision.mask_extent.width,
                                                   decision.mask_extent.height})
                .render_to_texture_uv
          : MakeLocalToneIdentityUv(input_width, input_height);
  return decision;
}

[[nodiscard]] inline auto MakeLocalToneDecisionTrace(const LocalToneDecision& decision)
    -> LocalToneDecisionTrace {
  LocalToneDecisionTrace trace;
  trace.action                    = decision.action;
  trace.persist_canonical         = decision.persist_canonical;
  trace.write_canonical_reference = decision.write_canonical_reference;
  trace.reuse_source              = decision.reuse_source;
  trace.mask_extent               = decision.mask_extent;
  trace.pyramid_level_count       = decision.pyramid_level_count;
  return trace;
}

}  // namespace alcedo
