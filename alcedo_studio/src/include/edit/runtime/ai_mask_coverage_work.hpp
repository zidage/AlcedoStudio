//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
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

/// Largest Feather kernel radius: sigma 50 gives the 401-tap kernel of @ref AiMaskFeatherWeights.
inline constexpr std::int32_t kAiMaskFeatherMaximumRadius = 200;

/**
 * @brief Device form of one Feather and Expand operation (SM8 section 5.1): the passes that a GPU
 *        backend records to make the coverage of @ref AiMaskCoverageWork::output_rect.
 *
 * Coordinates are output texels. The input of the first pass is the original coverage placed at
 * (@p source_x, @p source_y): a texel outside the source rectangle is 0, and a coordinate outside
 * the output is clamped to its nearest edge texel (replicated border). The passes are:
 *
 * 1. @p expand_steps steps of a 3 x 3 dilate (@p dilate) or erode on the R8 values. Step i uses
 *    the cross kernel when i is even and the square kernel when i is odd.
 * 2. With @p feather_weights, a horizontal pass that writes float `sum_k w_k * u / 255` (u: R8
 *    texel, float `u * (1 / 255)`) and a vertical pass over that float result. The vertical pass
 *    writes `clamp(round_half_even(v * 255), 0, 255)`.
 *
 * Each pass reads the previous result with the replicated border. Results equal the host function
 * @ref MakeAiMaskCoverageTexture: morphology exactly, the Feather within one code value (float
 * summation order differs).
 */
struct AiMaskEdgeDispatch {
  Extent2D           output_extent{};
  Extent2D           source_extent{};
  std::int32_t       source_x     = 0;
  std::int32_t       source_y     = 0;
  std::int32_t       expand_steps = 0;
  bool               dilate       = true;
  /// Half kernel of @ref AiMaskFeatherWeights; empty without Feather.
  std::vector<float> feather_weights;

  [[nodiscard]] auto HasFeather() const -> bool { return !feather_weights.empty(); }
  /// Number of R8 output-size textures that the morphology steps need besides the output.
  [[nodiscard]] auto MorphologyWorkTextureCount() const -> std::uint32_t;
};

/**
 * @brief Validate @p work and give its device passes.
 * @throws std::runtime_error with an "AI Mask coverage:" message when @p work has no edge
 *         adjustment.
 */
[[nodiscard]] auto MakeAiMaskEdgeDispatch(const AiMaskCoverageWork& work) -> AiMaskEdgeDispatch;

/**
 * @brief Textures of one @ref AiMaskEdgeDispatch. The caller owns them and keeps them alive until
 *        the GPU work that the backend records completes.
 *
 * @p source holds the original coverage (`source_extent`, R8). @p output receives the result
 * (`output_extent`, R8). The first `MorphologyWorkTextureCount()` entries of @p morphology are R8
 * output-size work textures; @p feather_rows is an R32f output-size work texture when the
 * dispatch has Feather. Unused members are null.
 */
template <class Texture2D>
struct AiMaskEdgeTextures {
  const Texture2D*          source       = nullptr;
  Texture2D*                output       = nullptr;
  std::array<Texture2D*, 2> morphology   = {nullptr, nullptr};
  Texture2D*                feather_rows = nullptr;
};

}  // namespace alcedo
