//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "decoders/processor/nn/overlap_tile_jobs.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"
#include "decoders/processor/raw_linearization_params.hpp"
#include "edit/geometry/types.hpp"
#include "edit/input/prepared_raw_input.hpp"

// The input preparation and the gain match of AI Denoise (integration plan, DN6; lab
// docs/alcedo_integration.md, "Input preparation (version 1)" and "Output and gain match").

namespace alcedo {

/**
 * @brief The packed RGGB mosaic of the decode area of a full-size Bayer input.
 *
 * The extended mosaic is the decode area with `added_top` rows and `added_left` columns in front
 * (0 or 1, so that it starts with R) and one row or column at the bottom or the right when the
 * size is then odd, all by reflection without the edge pixel. Its output (the model output at 2
 * x the packed size) holds the area at (`added_left`, `added_top`).
 */
struct AiDenoisePackedMosaic {
  /// (4, height, width) float32 planes: R, G1, G2, B. The value of a sample is
  /// (raw - black) / (maximum - black_level[c]); black includes the pattern black; no clip.
  std::vector<float>    planes;
  int                   width       = 0;
  int                   height      = 0;
  int                   added_top   = 0;
  int                   added_left  = 0;
  int                   area_width  = 0;
  int                   area_height = 0;
  /// The mean of the prepared samples of R, G (both positions), and B in the area.
  std::array<double, 3> input_means{};

  [[nodiscard]] auto    View() const -> nn::PackedMosaicView { return {planes, width, height}; }
  /// The output size of the extended mosaic in px (2 x the packed size).
  [[nodiscard]] auto    OutputWidth() const -> int { return 2 * width; }
  [[nodiscard]] auto    OutputHeight() const -> int { return 2 * height; }
};

/**
 * @brief Prepare the model input of @p cfa (input preparation version 1).
 *
 * @pre @p cfa is a full-size Bayer CFA input of LibRaw: U16 CFA pixels, a classic 2 x 2 pattern,
 *      `linearization.libraw_maximum` above the black level of each color.
 * @throws std::runtime_error when the precondition does not hold or the decode area is smaller
 *         than 2 x 2 px.
 */
[[nodiscard]] auto PackBayerForAiDenoise(const PreparedRawInput& cfa) -> AiDenoisePackedMosaic;

/**
 * @brief The column sums w_c of the camera RGB to linear Rec.2020 matrix,
 *        XYZ_to_Rec2020 * inverse(cam_xyz): the share of each camera channel in the mean of a
 *        Rec.2020 image.
 *
 * @p cam_xyz is the 3 x 3 LibRaw `color.cam_xyz` (camera from XYZ), row-major.
 * @throws std::runtime_error when the matrix is singular or not finite.
 */
[[nodiscard]] auto AiDenoiseGainWeights(const float (&cam_xyz)[9]) -> std::array<double, 3>;

/**
 * @brief The sums of the model output in the decode area, added tile by tile.
 *
 * The tile callback of the tile loop calls @ref AddKept after each tile; @ref Means then holds the
 * mean of each output channel over the area (the rows and columns of the reflection and of the
 * alignment are not part of it).
 */
class AiDenoiseOutputSums {
 public:
  explicit AiDenoiseOutputSums(const AiDenoisePackedMosaic& mosaic);

  /// Adds the output px of the packed rectangle @p kept that are inside the area.
  void               AddKept(const nn::RgbPlanesView& output, const nn::PackedTileRect& kept);

  /// The mean of each output channel over the area.
  [[nodiscard]] auto Means() const -> std::array<double, 3>;

 private:
  RectI                 area_{};
  std::array<double, 3> sums_{};
  double                pixels_ = 0.0;
};

/**
 * @brief The gain match factor g = sum_c w_c mean_in(c) / sum_c w_c mean_out(c).
 * @throws std::runtime_error when the factor is not finite or not above 0.
 */
[[nodiscard]] auto AiDenoiseGainFactor(const std::array<double, 3>& weights,
                                       const std::array<double, 3>& input_means,
                                       const std::array<double, 3>& output_means) -> double;

/**
 * @brief The denoised plane of the decode area: F32 RGBA, tightly packed, alpha 1.
 *
 * Channel c is g * output_c * (maximum - black_level[c]) / (white_level[c] - black_level[c]):
 * the value domain of `RawInputLoader::FromDenoisedBayer` (0 is the black level, 1 the white
 * level of the channel).
 * @pre @p output is the model output of @p mosaic (3, OutputHeight, OutputWidth).
 * @throws std::runtime_error when the sizes do not match or a white level is not above its black
 *         level.
 */
[[nodiscard]] auto AssembleDenoisedPlane(const nn::RgbPlanesView&     output,
                                         const AiDenoisePackedMosaic& mosaic, double gain,
                                         const RawLinearizationParams& linearization)
    -> HostImagePlane;

}  // namespace alcedo
