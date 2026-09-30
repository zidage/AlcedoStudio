//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/local_tone_mapping.hpp"

namespace alcedo {

/// Largest scatter pyramid. 16 levels cover every canvas extent up to 2^16 texels per axis.
inline constexpr std::uint32_t kDiffusionMaxLevels = 16;

/// Long-edge limit of the scatter canvas. The LLF canonical reference uses the same limit.
inline constexpr int kDiffusionCanvasMaxLongEdge = local_tone_mapping::kReferenceMaskMaxLongEdge;

/// Most bilinear render samples per axis that one base-level texel averages.
inline constexpr std::uint32_t kDiffusionMaxReduceSamples = 16;

/**
 * @brief Graph value for the canonical scatter image of the DRT node @p drt_id.
 *
 * The scatter image covers the full reference frame at the base level of the canvas pyramid.
 * Full-edit renders publish it; viewport ROI renders sample it, so the glow does not depend on
 * the zoom or on which part of the frame is visible.
 */
[[nodiscard]] inline auto DiffusionScatterId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"diffusion.scatter.0"}};
}

/// True when @p id is a canonical scatter image of @ref DiffusionScatterId.
[[nodiscard]] inline auto IsDiffusionScatterPort(const GraphValueId& id) -> bool {
  return id.output_port.Value() == "diffusion.scatter.0";
}

/**
 * @brief Scatter canvas extent for a full reference frame.
 *
 * The canvas is the full reference frame, scaled down so its long edge is at most
 * @ref kDiffusionCanvasMaxLongEdge. It does not depend on the render extent, the viewport, or
 * whether the frame is a preview or an export.
 */
[[nodiscard]] auto DiffusionCanvasExtent(Extent2D full_reference_extent) -> ImageExtent;

/**
 * @brief Scatter pyramid on the canvas. Backend-neutral; computed on the CPU per frame.
 *
 * Level `k` has texels of `2^(base_level + k)` canvas texels. Level 0 (the base level) is the
 * highlight-boosted box reduction of the linear scene. Each coarser level is the 13-tap
 * downsample of the previous level. The scatter image is `sum_k weights[k] * level_k` at the
 * base extent, accumulated from the coarsest level with 9-tap tent upsamples.
 *
 * The output pixel is `transmission * ((1 - scatter_fraction) * I + scatter_fraction * B)`,
 * where `B` is the scatter image sampled at the reference position of the render pixel.
 * Weights sum to 1, so the scatter conserves the energy of the boosted input.
 */
struct DiffusionFilterLayout {
  ImageExtent                                  canvas_extent{};
  std::uint32_t                                base_level  = 0;
  std::uint32_t                                level_count = 0;
  std::array<ImageExtent, kDiffusionMaxLevels> extents{};
  std::array<float, kDiffusionMaxLevels>       weights{};
  float                                        scatter_fraction = 0.0f;
  float                                        transmission     = 1.0f;
  float                                        highlight_gain   = 0.0f;
  float                                        highlight_knee   = 0.8f;
};

/**
 * @brief Build the scatter pyramid for @p shape on @p canvas_extent.
 *
 * Every sigma is a fraction of the canvas short side, so the same frame always gets the same
 * pyramid.
 *
 * @throws std::invalid_argument for an empty canvas.
 */
[[nodiscard]] auto MakeDiffusionFilterLayout(ImageExtent                 canvas_extent,
                                             const DiffusionFilterShape& shape)
    -> DiffusionFilterLayout;

/**
 * @brief Render-space mapping of the scatter image for one render.
 *
 * - @ref render_to_base maps a render pixel position to base-level texel coordinates.
 * - @ref base_to_render maps a base-level texel position to render pixel coordinates.
 * - @ref reduce_samples is the bilinear render sample count per axis for one base texel. It
 *   follows the texel footprint in render pixels, so a higher-resolution render averages more
 *   samples into the same texel.
 */
struct DiffusionScatterMapping {
  Matrix3x3     render_to_base{};
  Matrix3x3     base_to_render{};
  std::uint32_t reduce_samples = 1;
};

/**
 * @brief Map the base level of @p layout onto the render of @p geometry.
 *
 * @throws std::invalid_argument when the geometry has an empty reference extent.
 */
[[nodiscard]] auto MakeDiffusionScatterMapping(const ResolvedRenderGeometry& geometry,
                                               const DiffusionFilterLayout&  layout)
    -> DiffusionScatterMapping;

/** @brief Host action chosen before any scatter GPU work. */
enum class DiffusionScatterAction : std::uint8_t {
  SampleCanonical,
  Rebuild,
};

/**
 * @brief Whether to sample the canonical scatter image or rebuild it for one encode.
 *
 * - @ref persist_canonical: the rebuilt image replaces the canonical image. Only a render that
 *   covers the full edit space may publish, because an ROI render cannot see light from
 *   outside the viewport.
 * - @ref required_detail: long edge of the render that the canonical image must be built
 *   from. A full-edit render needs at least its own long edge; an ROI render accepts any
 *   canonical image.
 */
struct DiffusionScatterDecision {
  DiffusionScatterAction action            = DiffusionScatterAction::Rebuild;
  bool                   persist_canonical = false;
  std::uint32_t          required_detail   = 0;
  std::uint32_t          current_long_edge = 0;
};

/**
 * @brief Detail that the canonical scatter image must have for this render.
 *
 * @param full_edit @ref CoversFullEditSpace of the render.
 * @param current_long_edge Long edge of the render in pixels.
 */
[[nodiscard]] inline auto DiffusionScatterRequiredDetail(bool full_edit,
                                                         std::uint32_t current_long_edge)
    -> std::uint32_t {
  return full_edit ? current_long_edge : 0U;
}

/**
 * @brief Choose sample or rebuild for one scatter encode.
 *
 * @param persist False when this render may not read or publish the canonical image
 *        (QualityBase). The render then rebuilds from its own pixels.
 * @param canonical_valid The canonical image matches the current revision, the canvas
 *        extent, and @ref DiffusionScatterRequiredDetail.
 */
[[nodiscard]] inline auto DecideDiffusionScatter(bool persist, bool canonical_valid,
                                                 bool full_edit, std::uint32_t current_long_edge)
    -> DiffusionScatterDecision {
  DiffusionScatterDecision decision;
  decision.current_long_edge = current_long_edge;
  decision.required_detail   = DiffusionScatterRequiredDetail(full_edit, current_long_edge);
  if (persist && canonical_valid) {
    decision.action = DiffusionScatterAction::SampleCanonical;
    return decision;
  }
  decision.action            = DiffusionScatterAction::Rebuild;
  decision.persist_canonical = persist && full_edit;
  return decision;
}

}  // namespace alcedo
