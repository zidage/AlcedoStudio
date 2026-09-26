//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <optional>
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

}  // namespace alcedo
