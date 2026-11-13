//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {

/**
 * @brief Camera→XYZ/AP1 matrices produced from stored Develop camera-profile params.
 *
 * Row-major 3×3, column vectors. CCT/tint interpolation uses ColorMatrix/ForwardMatrix
 * fields bound at import. The interpolated camera→AP1 matrix is written into the GPU
 * parameter body; this struct is not serialized.
 */
struct DevelopColorTransform {
  std::array<float, 9> camera_to_xyz{};
  std::array<float, 9> camera_to_xyz_d50{};
  std::array<float, 9> xyz_d50_to_ap1{};
  std::array<float, 9> camera_to_ap1{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float                resolved_cct  = 6500.0f;
  float                resolved_tint = 0.0f;
};

enum class ColorTransformError {
  Ok = 0,
  MissingCameraMatrices,
  SingularCameraMatrix,
  NonFiniteMatrix,
  InvalidAsShotNeutral,
  InvalidWhitePoint,
  /// The Develop data references a DNG profile that no owner bound from the source file.
  UnboundDngProfile,
  /// The Develop data references a DCP file that no owner bound from the DCP library, or whose
  /// bound tables do not belong to the DNG profile of the source.
  UnboundCameraProfileFile,
};

/**
 * @brief Result of @ref ResolveDevelopColorTransform.
 *
 * @c ok is true only when every matrix is finite and invertible. Failure never
 * yields an identity camera→AP1 stand-in.
 */
struct ColorTransformResult {
  bool                  ok     = false;
  ColorTransformError   error  = ColorTransformError::MissingCameraMatrices;
  DevelopColorTransform transform{};
};

/**
 * @brief The camera profile that the render uses for @p payload.
 *
 * This is the only reader of matrices and tables for the render:
 * - no `camera_profile_file`: the import values of `camera_profile`;
 * - bound reference: `ColorMatrix1/2`, `ForwardMatrix1/2`, and the two illuminant CCT values of the
 *   DCP profile, and `AsShotNeutral` and `cam_mul` of `camera_profile`. The DNG profile is the
 *   source tables of the reference for a source with an embedded DNG profile (the DCP tables with
 *   the AnalogBalance, BaselineExposure, and CameraCalibration of the source; plan phase C6), and
 *   the DCP tables with identity calibration otherwise.
 *
 * The result is a computed value; `camera_profile` keeps the import values.
 * @throws std::runtime_error for an unbound reference, or for source tables that do not belong
 *         to the DNG profile of `camera_profile` (DcpSourceTablesMatchSource), with the message
 *         of @ref ColorTransformFailureMessage.
 */
[[nodiscard]] auto EffectiveCameraProfile(const DevelopPayload& payload) -> DevelopCameraProfile;

/**
 * @brief Solve `as_shot_cct` and `as_shot_tint` from the effective matrices and the as-shot
 * neutral.
 *
 * Runs @ref ResolveDevelopColorTransform in As Shot mode. On failure (no usable matrices or
 * neutral, or an unbound reference) the two values keep their numbers. Other fields never change.
 */
void               SolveDevelopAsShotWhiteBalance(DevelopPayload& payload);

/**
 * @brief Copy import-time RAW camera matrices into Develop payload fields.
 *
 * Also solves as-shot CCT/tint from AsShotNeutral (or cam_mul fallback).
 *
 * @pre @p imported was populated at import by MetadataExtractor, or its DNG profile reference
 *      was bound by the pipeline service after a project read.
 * Side effects: overwrites camera-profile fields and, on success, as-shot CCT/tint.
 * @throws std::runtime_error when @p imported references a DNG profile that is not bound.
 */
void BindDevelopCameraProfile(DevelopPayload& payload, const RawRuntimeColorContext& imported);

/**
 * @brief Bind import-time RAW camera metadata to the document's Develop node.
 *
 * Applies @ref BindDevelopCameraProfile to a copy of the Develop payload and publishes it with one
 * `ReplaceParams` call. User edits outside the camera-profile and as-shot fields stay unchanged.
 * An equal result does not mark the document dirty. A document without a Develop node is left
 * unchanged.
 *
 * @pre The caller owns @p document under its render lock or has exclusive access before
 *      publication (import, open, replay, checkout). Never call from a render task.
 * @throws Whatever `DevelopParamsModel::ReplaceParams` throws; no write happens in that case.
 */
void BindImportedCameraProfile(PipelineDocument& document, const RawRuntimeColorContext& imported);

/**
 * @brief Bind Rec.709 / sRGB XYZ→camera matrices for files that have no RAW camera profile.
 *
 * Used when image-root initialization has no @c RawRuntimeColorContext (JPEG,
 * TIFF, PNG, and other mock RGB files).
 * IEC 61966-2-1 XYZ→Rec.709 is stored as ColorMatrix1 and ColorMatrix2 with a
 * single D65 illuminant. Missing RAW calibration is not substituted: RAW import
 * must still bind extracted camera matrices and still fails when those
 * matrices are absent.
 *
 * @pre Called only on the null-RAW import / root-init branch. Does not change
 *      @c CreateDefaultPipelineDocument().
 * Side effects: overwrites camera-profile fields and, on success, as-shot
 *               CCT/tint via @ref BindDevelopCameraProfile.
 * Thread: CPU; the caller holds any required document lock.
 */
void BindRgbWorkingSpaceCameraProfile(DevelopPayload& payload);

/**
 * @brief Interpolate stored ColorMatrix/ForwardMatrix fields for the current CCT/tint.
 *
 * @pre @p develop.camera_profile.color_matrices_valid is true and both colour
 *      matrices are finite and invertible. Does not read LibRaw, rgb_cam, cam_xyz,
 *      or the CameraMatrices database.
 * @return Success with camera→AP1, or a typed error. Missing or singular matrices
 *         are errors, never identity.
 * Thread: CPU, no shared mutable state. Call when the user changes CCT/tint or
 *         when first filling the GPU parameter body.
 */
[[nodiscard]] auto ResolveDevelopColorTransform(const DevelopPayload& develop)
    -> ColorTransformResult;

[[nodiscard]] auto ColorTransformErrorMessage(ColorTransformError error) -> std::string_view;

/**
 * @brief Message for a failed @ref ResolveDevelopColorTransform of @p develop.
 *
 * For `UnboundCameraProfileFile` it names the profile and the camera model of the reference; for
 * the other errors it equals @ref ColorTransformErrorMessage.
 */
[[nodiscard]] auto      ColorTransformFailureMessage(const DevelopPayload& develop,
                                                     ColorTransformError   error) -> std::string;

/**
 * @brief The colour temperature of the RAW white balance of @p develop: the light of scene light
 *        As Shot of the film print.
 *
 * Mode Custom gives its Kelvin; the other modes give the solved as-shot temperature. None for a
 * raster input, and when @ref ResolveDevelopColorTransform fails for @p develop: then the as-shot
 * temperature was not solved (for example, no usable camera matrices or as-shot neutral), and the
 * stored value is not the light of the shot (issue 672).
 */
[[nodiscard]] auto RawWhiteBalanceCct(const DevelopPayload& develop) -> std::optional<double>;

/// White-balance CCT/tint ranges shared by RAW Custom WB and Color Grade CAT02 WB.
inline constexpr double kWhiteBalanceCctMin  = 2000.0;
inline constexpr double kWhiteBalanceCctMax  = 15000.0;
inline constexpr double kWhiteBalanceTintMin = -150.0;
inline constexpr double kWhiteBalanceTintMax = 150.0;

/// CIE 1931 xy of the ACES AP1 white point (ACES "D60").
inline constexpr std::array<double, 2> kAcesWhiteXy{0.32168, 0.33767};

struct WhiteBalanceTemperatureTint {
  double cct  = 0.0;
  double tint = 0.0;
};

/**
 * @brief The colour temperature and the tint of the RAW white balance of @p develop: the light of
 *        scene light As Shot of the film print, with its distance from the Planckian locus.
 *
 * Mode Custom gives its Kelvin and tint; the other modes give the solved as-shot values. None in
 * the cases of @ref RawWhiteBalanceCct.
 */
[[nodiscard]] auto RawWhiteBalanceWhite(const DevelopPayload& develop)
    -> std::optional<WhiteBalanceTemperatureTint>;

/**
 * @brief Convert CCT/tint to CIE 1931 xy with the RAW Custom WB mapping.
 *
 * Uses the same CIE 1931 Planckian locus table and tint scale as Develop Custom WB. Inputs are
 * clamped to the white-balance ranges above.
 */
[[nodiscard]] auto WhiteBalanceTemperatureTintToXy(double cct, double tint)
    -> std::array<double, 2>;

/**
 * @brief Inverse of @ref WhiteBalanceTemperatureTintToXy (Ohno CCT solve + locus-normal tint).
 * @return nullopt when @p xy is not finite or the solve fails.
 */
[[nodiscard]] auto WhiteBalanceXyToTemperatureTint(const std::array<double, 2>& xy)
    -> std::optional<WhiteBalanceTemperatureTint>;

/**
 * @brief CCT/tint of the ACES AP1 white point under the RAW Custom WB mapping.
 *
 * This is the identity setting of Color Grade CAT02 white balance. Computed once.
 */
[[nodiscard]] auto AcesWhiteTemperatureTint() -> const WhiteBalanceTemperatureTint&;

/**
 * @brief Linear-AP1 CAT02 adaptation for Color Grade white balance.
 *
 * The grade input is assumed to be white-balanced to the AP1 white point. @p cct / @p tint name
 * the illuminant to neutralize: its chromaticity is adapted to the AP1 white with the CAT02
 * (von Kries in CAT02 LMS) transform. Raising @p cct above the AP1 white warms the image, as
 * with RAW Custom WB.
 *
 * @return Row-major 3x3 matrix applied to linear AP1 column vectors. Identity at
 *         @ref AcesWhiteTemperatureTint.
 */
[[nodiscard]] auto BuildAp1Cat02WhiteBalanceMatrix(double cct, double tint)
    -> std::array<float, 9>;

/**
 * @brief XYZ D50 to linear AP1 (Bradford D50 to the AP1 white), row-major, column vectors.
 *
 * The value equals DevelopColorTransform::xyz_d50_to_ap1 of every resolved transform: the matrix
 * does not depend on the camera. Computed once; thread-safe.
 */
[[nodiscard]] auto XyzD50ToAp1Matrix() -> const std::array<float, 9>&;

}  // namespace alcedo
