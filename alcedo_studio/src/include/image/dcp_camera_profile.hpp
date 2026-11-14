// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <json.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "image/dng_color_profile.hpp"
#include "image/dng_tone_curve.hpp"

namespace alcedo {

/**
 * @brief Parsed content of one DCP file (DNG Camera Profile).
 *
 * The matrices and the illuminant temperatures replace the camera matrices of an image when the
 * user selects the profile. @ref tables holds the hue and saturation maps, the look table, and
 * `BaselineExposureOffset`, with identity calibration, so the embedded DNG profile path evaluates
 * it unchanged.
 *
 * @ref profile_tone_curve holds `ProfileToneCurve`, or null when the file lacks the tag. It stays
 * outside @ref DcpCameraProfileToJson, so the curve leaves @ref fingerprint unchanged and every
 * stored reference keeps its fingerprint (dcp_profile_tone_curve_study.md, section 6.3). The curve
 * carries its own fingerprint.
 *
 * The value is immutable after @ref MakeDcpCameraProfile. It is session data: project data stores
 * only the identity and the fingerprint, never the matrices or the tables.
 */
struct DcpCameraProfile {
  std::string           unique_camera_model;                  ///< Tag 50708, UniqueCameraModel.
  std::string           profile_name;                         ///< Tag 50936, ProfileName.
  std::array<double, 9> color_matrix_1 = kDngIdentityMatrix;  ///< XYZ to camera, illuminant 1.
  std::array<double, 9> color_matrix_2 = kDngIdentityMatrix;  ///< XYZ to camera, illuminant 2.
  bool                  forward_matrices_valid = false;
  std::array<double, 9> forward_matrix_1       = kDngIdentityMatrix;  ///< Camera to XYZ D50.
  std::array<double, 9> forward_matrix_2       = kDngIdentityMatrix;
  double                color_matrix_1_cct     = 0.0;   ///< Kelvin of CalibrationIlluminant1.
  double                color_matrix_2_cct     = 0.0;   ///< Kelvin of CalibrationIlluminant2.
  std::string           profile_calibration_signature;  ///< Tag 50932; empty when absent.
  DngColorProfilePtr    tables;           ///< Maps, look table, and exposure offset. Never null.
  DngToneCurvePtr       profile_tone_curve;  ///< Tag 50940; null when absent. Not fingerprinted.
  std::uint64_t         fingerprint = 0;  ///< FNV-1a of the content; set by MakeDcpCameraProfile.
};
using DcpCameraProfilePtr = std::shared_ptr<const DcpCameraProfile>;

/// Complete profile content as JSON. It is the fingerprint input; project data never stores it.
/// The tables enter through their own fingerprint, which covers their complete content.
inline auto DcpCameraProfileToJson(const DcpCameraProfile& profile) -> nlohmann::json {
  return {{"version", 1},
          {"unique_camera_model", profile.unique_camera_model},
          {"profile_name", profile.profile_name},
          {"color_matrix_1", profile.color_matrix_1},
          {"color_matrix_2", profile.color_matrix_2},
          {"forward_matrices_valid", profile.forward_matrices_valid},
          {"forward_matrix_1", profile.forward_matrix_1},
          {"forward_matrix_2", profile.forward_matrix_2},
          {"color_matrix_1_cct", profile.color_matrix_1_cct},
          {"color_matrix_2_cct", profile.color_matrix_2_cct},
          {"profile_calibration_signature", profile.profile_calibration_signature},
          {"tables", profile.tables ? DngColorProfileFingerprintToText(profile.tables->fingerprint)
                                    : std::string{}}};
}

/**
 * @brief Validate a parsed DCP profile and publish it as an immutable value.
 *
 * Preconditions checked here: non-empty identity strings, finite matrices, positive finite CCT
 * values, and non-null @p profile.tables (already validated by MakeDngColorProfile). The function
 * computes @ref DcpCameraProfile::fingerprint from @ref DcpCameraProfileToJson.
 *
 * @throws std::runtime_error when a precondition fails.
 */
inline auto MakeDcpCameraProfile(DcpCameraProfile profile) -> DcpCameraProfilePtr {
  if (profile.unique_camera_model.empty())
    throw std::runtime_error("DCP profile: UniqueCameraModel is empty");
  if (profile.profile_name.empty()) throw std::runtime_error("DCP profile: ProfileName is empty");
  for (const auto* matrix : {&profile.color_matrix_1, &profile.color_matrix_2,
                             &profile.forward_matrix_1, &profile.forward_matrix_2}) {
    for (double value : *matrix) {
      if (!std::isfinite(value)) throw std::runtime_error("DCP profile: non-finite matrix entry");
    }
  }
  for (double cct : {profile.color_matrix_1_cct, profile.color_matrix_2_cct}) {
    if (!std::isfinite(cct) || cct <= 0.0)
      throw std::runtime_error("DCP profile: invalid illuminant temperature");
  }
  if (!profile.tables) throw std::runtime_error("DCP profile: tables are missing");
  auto          result = std::make_shared<DcpCameraProfile>(std::move(profile));
  const auto    text   = DcpCameraProfileToJson(*result).dump();
  std::uint64_t hash   = 14695981039346656037ULL;
  for (unsigned char byte : text) hash = (hash ^ byte) * 1099511628211ULL;
  result->fingerprint = hash;
  return result;
}

}  // namespace alcedo
