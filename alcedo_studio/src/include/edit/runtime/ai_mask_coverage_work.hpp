//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_asset.hpp"

namespace alcedo {

/**
 * @brief Shared read of the original R8 coverage of an AI Mask candidate (SM8 section 3).
 *
 * The AI Mask edit session owns the allocation (`AiMaskEditSession::AcquireCoveragePixels`) and
 * never writes to it after it shares it. The coverage owner of the render workspace holds the
 * read until the GPU upload of the bytes is complete.
 */
using AiMaskCoveragePixels = std::shared_ptr<const std::vector<std::uint8_t>>;

/**
 * @brief Texel rectangles of one Feather and Expand operation on the original R8 coverage of an AI
 *        Mask (SM8 section 5.1). Backend-neutral; it holds no pixels.
 *
 * @p source is the descriptor of the original coverage; @p source_rect is its texel rectangle in
 * an analysis image of @p analysis_extent. @p output_rect is @p source_rect widened by
 * @ref AiMaskEdgeMargin and clipped to the image: every texel that the operation can make above 0.
 * With no edge adjustment, the two are equal and the output is the original coverage.
 */
struct AiMaskCoverageWork {
  MaskAssetDescriptor  source{};
  Extent2D             analysis_extent{};
  RectI                source_rect{};
  RectI                output_rect{};
  AiMaskEdgeAdjustment edge{};

  /// True when Feather or Expand changes the coverage.
  [[nodiscard]] auto   HasEdgeAdjustment() const -> bool {
    return edge.feather_px != 0.0f || edge.expand_px != 0;
  }
  /// Descriptor of the coverage after Feather and Expand; @p source without an edge adjustment.
  [[nodiscard]] auto OutputDescriptor() const -> MaskAssetDescriptor;
};

/**
 * @brief Validate the inputs of one coverage operation and compute its texel rectangles.
 *
 * @p source is the descriptor of the original coverage: the stored asset of a committed AI Mask,
 * or the whole analysis image for a candidate.
 *
 * @throws std::runtime_error with an "AI Mask coverage:" message when @p analysis_extent is empty
 *         or an axis exceeds @ref kMaximumRasterMaskAxis, when @p source does not lie in the
 *         analysis image (@ref AiMaskAssetPixelRect), or when @p edge is outside `feather_px` in
 *         [0, 50] or `expand_px` in [-50, 50].
 */
[[nodiscard]] auto MakeAiMaskCoverageWork(const MaskAssetDescriptor&  source,
                                          Extent2D                    analysis_extent,
                                          const AiMaskEdgeAdjustment& edge) -> AiMaskCoverageWork;

/// Descriptor of a candidate coverage: the whole analysis image of @p analysis_extent.
[[nodiscard]] auto AiMaskCandidateDescriptor(Extent2D analysis_extent) -> MaskAssetDescriptor;

}  // namespace alcedo
