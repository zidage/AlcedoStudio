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
 * @brief The exposure curve of the model input (integration plan, DN6 addition "input exposure").
 *
 * The model was trained on photos at their normal exposure (lab RawNIND pairs: the noisy input
 * has the brightness of the clean image at each ISO), so it removes less noise from a dark photo.
 * The curve raises a dark photo to the training brightness before the model and the inverse
 * curve after the gain match returns the output to the exposure of the photo:
 *
 * - `y = gain x` up to the knee `y = kAiDenoiseCurveKnee` (also for values below 0);
 * - above it, the shoulder `y = knee + shoulder ln(1 + gain (x - knee / gain) / shoulder)`, with
 *   the shoulder width chosen so that the white level `x = 1` stays at `y = 1`;
 * - above `x = 1`, a line with the slope of the shoulder at `x = 1`.
 *
 * The curve has a continuous slope, rises everywhere, and keeps the highlights at or below the
 * white level, the range of the training inputs. A gain of 1 is the identity.
 */
struct AiDenoiseInputCurve {
  double             gain       = 1.0;
  /// The shoulder width; 0 when the gain is 1.
  double             shoulder   = 0.0;
  /// The slope above the white level.
  double             tail_slope = 1.0;

  [[nodiscard]] auto IsIdentity() const -> bool { return gain == 1.0; }
  [[nodiscard]] auto Apply(double x) const -> double;
  [[nodiscard]] auto Invert(double y) const -> double;
};

/// The output value of the curve up to which the curve is the gain alone.
inline constexpr double kAiDenoiseCurveKnee        = 0.5;
/// The brightness level of the training inputs: the median over the 3404 noisy RawNIND training
/// pairs of the mean of the R, G, and B means of the packed mosaic (lab data/patches, clean level
/// divided by raw_gain; 5% to 95%: 0.0056 to 0.072 of the white level).
inline constexpr double kAiDenoiseTargetInputLevel = 0.028;
/// The largest gain (4 EV).
inline constexpr double kAiDenoiseMaxInputGain     = 16.0;

/**
 * @brief The curve with gain @p gain; a gain of 1 or below gives the identity.
 * @throws std::runtime_error when the gain is not finite.
 */
[[nodiscard]] auto      AiDenoiseInputCurveOfGain(double gain) -> AiDenoiseInputCurve;

/**
 * @brief The curve of a photo with the input means @p input_means (R, G, B of the prepared values).
 *
 * The gain is target level / (mean of the 3 means), limited to [1, kAiDenoiseMaxInputGain]: a
 * photo at or above the training level keeps the identity.
 */
[[nodiscard]] auto      AiDenoiseInputCurveFor(const std::array<double, 3>& input_means)
    -> AiDenoiseInputCurve;

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
  /// The mean of the samples of R, G (both positions), and B in the area, after the curve.
  std::array<double, 3> input_means{};
  /// The curve of the planes (identity after @ref PackBayerForAiDenoise).
  AiDenoiseInputCurve   curve{};

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
 * @brief Apply @p curve to the planes of @p mosaic and set its input means to the means after the
 *        curve (the means of the gain match).
 *
 * @pre The curve of @p mosaic is the identity.
 * @throws std::runtime_error when the mosaic already has a curve.
 */
void ApplyAiDenoiseInputCurve(AiDenoisePackedMosaic& mosaic, const AiDenoiseInputCurve& curve);

/**
 * @brief The column sums w_c of the camera RGB to linear Rec.2020 matrix,
 *        XYZ_to_Rec2020 * inverse(xyz_to_camera): the share of each camera channel in the mean of
 *        a Rec.2020 image.
 *
 * @p xyz_to_camera is the camera matrix of the render at D65 (ResolveDevelopXyzToCameraD65:
 * DNG ColorMatrix sense, camera from XYZ), row-major.
 * @throws std::runtime_error when the matrix is singular or not finite.
 */
[[nodiscard]] auto AiDenoiseGainWeights(const std::array<double, 9>& xyz_to_camera)
    -> std::array<double, 3>;

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
 * Channel c is curve^-1(g * output_c) * (maximum - black_level[c]) / (white_level[c] -
 * black_level[c]), with the curve of @p mosaic: the value domain of
 * `RawInputLoader::FromDenoisedBayer` (0 is the black level, 1 the white level of the channel).
 * @pre @p output is the model output of @p mosaic (3, OutputHeight, OutputWidth).
 * @throws std::runtime_error when the sizes do not match or a white level is not above its black
 *         level.
 */
[[nodiscard]] auto AssembleDenoisedPlane(const nn::RgbPlanesView&     output,
                                         const AiDenoisePackedMosaic& mosaic, double gain,
                                         const RawLinearizationParams& linearization)
    -> HostImagePlane;

}  // namespace alcedo
