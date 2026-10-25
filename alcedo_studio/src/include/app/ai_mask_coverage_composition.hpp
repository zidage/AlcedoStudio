//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_asset.hpp"

/**
 * @file
 * @brief Host composition of the AI Mask coverage (SM5 decision M6, M9).
 *
 * The coverage is R8 at the session image size: 255 is fully inside, 0 is outside. Each model
 * output converts to one component coverage; the components fold in recipe order. All functions
 * are pure and thread-safe; they change only their output arguments.
 */
namespace alcedo {

/// Decoder logits of one selected mask level: @p size x @p size values, row-major.
struct AiMaskSamLogits {
  std::uint32_t      size = 0;
  std::vector<float> logits;
};

/**
 * @brief An alpha grid in [0, 1] that covers @p box of the session image.
 *
 * Subject, Background, and People cover the full image; a Person grid covers its crop rectangle.
 */
struct AiMaskAlphaGrid {
  std::uint32_t      width  = 0;
  std::uint32_t      height = 0;
  std::vector<float> values;
  AiMaskBox          box{0.0f, 0.0f, 1.0f, 1.0f};
};

/// An R8 mask at the session image size (Sky).
struct AiMaskBinaryR8 {
  std::uint32_t             width  = 0;
  std::uint32_t             height = 0;
  std::vector<std::uint8_t> values;
};

/// One model output in its native grid.
using AiMaskModelOutput = std::variant<AiMaskSamLogits, AiMaskAlphaGrid, AiMaskBinaryR8>;

/**
 * @brief The analysis image that guides the upscale of an alpha grid (#522, master plan D23).
 *
 * RGB8 at the session image size, row-major, smoothed by a Gaussian of sigma 1 pixel against its
 * noise. Make it with @ref MakeAiMaskCoverageGuide; it does not change after that.
 */
struct AiMaskCoverageGuide {
  Extent2D                  extent;
  std::vector<std::uint8_t> rgb8;
};

/**
 * @brief Make the guide of @p rgb8, the session image (width x height x 3 bytes, row-major).
 * @throws std::invalid_argument when @p extent is empty or the byte count differs.
 */
[[nodiscard]] auto MakeAiMaskCoverageGuide(Extent2D extent, std::span<const std::uint8_t> rgb8)
    -> AiMaskCoverageGuide;

/**
 * @brief Check that @p output has a consistent grid: a non-empty size and a value count equal to
 * the grid size; an alpha box with ordered corners in [0, 1]; an R8 mask of the size @p extent.
 *
 * @throws std::invalid_argument naming the first rule that fails.
 */
void               ValidateAiMaskModelOutput(const AiMaskModelOutput& output, Extent2D extent);

/**
 * @brief Convert one model output to a component coverage of @p extent.
 *
 * Logits: `cv::resize` (bilinear, half-pixel centers) to @p extent; 255 where the value is > 0,
 * else 0. Alpha grid (master plan D23): the session pixels whose centers lie inside the box map
 * to grid positions with half-pixel centers. A pixel whose 5 x 5 cell window around its nearest
 * cell has one alpha value takes that value. Another pixel takes the joint bilateral mean of its
 * 4 x 4 nearest cells: spatial sigma 1 cell; with @p guide, a range weight on the RGB difference
 * between the pixel and the area mean of the cell, with a range sigma of 0.7 times the color
 * standard deviation of the window (at least 0.01). The value is then
 * `round(clamp((alpha - 0.5) * gain + 0.5) * 255)` with ties to even, where gain is half the
 * larger upscale factor of the two axes and at least 1. The other pixels are 0. R8 mask: the
 * values as given.
 *
 * @param guide The session image for the alpha grid edges; null uses the spatial weight only.
 * @return @p extent.width * @p extent.height values, row-major.
 * @throws std::invalid_argument when @ref ValidateAiMaskModelOutput fails, @p extent is empty, or
 *         @p guide does not have the size @p extent.
 */
[[nodiscard]] auto ConvertModelOutputToCoverage(const AiMaskModelOutput& output, Extent2D extent,
                                                const AiMaskCoverageGuide* guide = nullptr)
    -> std::vector<std::uint8_t>;

/**
 * @brief Fold one component into @p coverage: Add takes the maximum, Subtract the minimum with
 * (255 - component).
 *
 * @throws std::invalid_argument when the sizes differ; @p coverage is then unchanged.
 */
void               FoldAiMaskCoverage(std::vector<std::uint8_t>&    coverage,
                                      std::span<const std::uint8_t> component,
                                      AiMaskComponentOperation      operation);

/**
 * @brief Crop @p coverage to the bounding rectangle of its values > 0, widened by 1 pixel on each
 * side and clipped to the image (M9), and make the content-addressed asset.
 *
 * The descriptor bounds are the rectangle divided by @p extent.
 *
 * @return The asset, or empty when no value is > 0.
 * @throws std::invalid_argument when the size of @p coverage differs from @p extent.
 */
[[nodiscard]] auto CropAiMaskCoverage(std::span<const std::uint8_t> coverage, Extent2D extent)
    -> std::optional<MaskAsset>;

/**
 * @brief Place a stored asset at its reference bounds in a zero coverage of @p extent: the inverse
 * of @ref CropAiMaskCoverage.
 *
 * The left and top pixel are the bounds origin times @p extent, rounded; the bounds size times
 * @p extent must be the asset extent within half a pixel.
 *
 * @throws std::invalid_argument when the asset is invalid or does not fit @p extent.
 */
[[nodiscard]] auto PlaceAiMaskAssetInCoverage(const MaskAsset& asset, Extent2D extent)
    -> std::vector<std::uint8_t>;

}  // namespace alcedo
