//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"

namespace alcedo {

/**
 * @brief R8 coverage that the render samples for one AI Mask: the stored asset after Feather and
 *        Expand.
 *
 * @p descriptor gives the texel size and the normalized ReferenceSpace rectangle that the texels
 * cover. @p pixels are row-major and tightly packed.
 */
struct AiMaskCoverageTexture {
  MaskAssetDescriptor       descriptor{};
  std::vector<std::uint8_t> pixels;
};

/**
 * @brief Number of analysis pixels by which Feather and Expand can move coverage out of the asset
 *        rectangle.
 *
 * `max(expand_px, 0)` plus, when `feather_px > 0`, `ceil(4 * feather_px) + 1` (the radius of the
 * float Gaussian kernel of OpenCV is at most `4 * sigma + 1`).
 */
[[nodiscard]] auto AiMaskEdgeMargin(const AiMaskEdgeAdjustment& edge) -> std::int32_t;

/**
 * @brief Coefficients of the Feather Gaussian with sigma @p feather_px, for one half of the
 *        symmetric kernel: element k is the weight of the texels at offsets -k and +k.
 *
 * The kernel is the float kernel of OpenCV for sigma > 0 (`cv::getGaussianKernel` with size
 * `round(8 * sigma + 1) | 1`), so the host function and the GPU backends use the same finite
 * coefficients and support. The radius is `size() - 1`, at most 200 for sigma 50.
 *
 * @throws std::runtime_error when @p feather_px is not in (0, 50].
 */
[[nodiscard]] auto AiMaskFeatherWeights(float feather_px) -> std::vector<float>;

/**
 * @brief Apply the Feather and Expand of an AI Mask to its stored asset.
 *
 * The asset lies at @ref AiMaskAssetPixelRect in an analysis image of @p analysis_extent. The
 * texture rectangle is that rectangle widened by @ref AiMaskEdgeMargin and clipped to the image.
 * Inside it, the function computes lab function F8 on coverage / 255 in float: `|expand_px|`
 * steps of a 3 x 3 dilate (`expand_px > 0`) or erode (`expand_px < 0`) with a cross kernel, then
 * a square kernel, in turn; then a Gaussian blur with sigma `feather_px`. Both use replicated
 * borders. Each texel is `round(clamp(v, 0, 1) * 255)`. The texture holds every value above 0 of
 * the same computation on the whole image, and its texels equal that result. With both fields 0,
 * the texture is the asset.
 *
 * @throws std::invalid_argument when the asset pixels are invalid.
 * @throws std::runtime_error when the asset does not fit the analysis image, or when @p edge is
 *         outside `feather_px` in [0, 50] or `expand_px` in [-50, 50].
 */
[[nodiscard]] auto MakeAiMaskCoverageTexture(const MaskAsset& asset, Extent2D analysis_extent,
                                             const AiMaskEdgeAdjustment& edge)
    -> AiMaskCoverageTexture;

/**
 * @brief Host Feather and Expand of @p work on the original coverage @p source_pixels (the texels
 *        of `work.source`), with the computation of the asset overload.
 *
 * The render workspace uses it for Mask candidates and committed AI Masks on a backend that does
 * not record the operation on the device yet (SM8.2 to SM8.4).
 *
 * @throws std::invalid_argument when @p source_pixels does not have the size of `work.source`.
 */
[[nodiscard]] auto MakeAiMaskCoverageTexture(const AiMaskCoverageWork&     work,
                                             std::span<const std::uint8_t> source_pixels)
    -> AiMaskCoverageTexture;

}  // namespace alcedo
