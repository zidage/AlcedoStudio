//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "image/dng_color_profile.hpp"

#include <json.hpp>
#include <string>

namespace alcedo {

/// Lightweight struct carrying raw-file colour and lens metadata extracted
/// during decode or import.  Kept in its own header so that headers without
/// heavyweight dependencies (Image, the Develop model, …) can use it without
/// pulling in libraw / decoder_scheduler.
struct RawRuntimeColorContext {
  /// Bound at import. Project data keeps only the fingerprint; a context read from project data
  /// holds an unbound reference until the pipeline service loads the profile from the source file.
  DngColorProfileRef dng_profile_;
  bool              valid_                   = false;
  bool              output_in_camera_space_  = false;
  float             cam_mul_[3]              = {1.0f, 1.0f, 1.0f};
  float             pre_mul_[3]              = {1.0f, 1.0f, 1.0f};
  float             cam_xyz_[9]              = {};
  float             rgb_cam_[9]              = {};
  std::string       camera_make_             = {};
  std::string       camera_model_            = {};
  bool              lens_metadata_valid_     = false;
  std::string       lens_make_               = {};
  std::string       lens_model_              = {};
  float             focal_length_mm_         = 0.0f;
  float             aperture_f_number_       = 0.0f;
  float             focus_distance_m_        = 0.0f;
  float             focal_35mm_mm_           = 0.0f;
  float             crop_factor_hint_        = 0.0f;
  bool              dng_warp_rectilinear_present_ = false;
  bool              dng_warp_rectilinear_applied_ = false;

  // Adobe DNG colour matrices resolved at import time from the camera
  // matrix database.  Avoids repeated database lookups per frame.
  bool              color_matrices_valid_    = false;
  double            color_matrix_1_[9]       = {};
  double            color_matrix_2_[9]       = {};
  bool              forward_matrices_valid_  = false;
  double            forward_matrix_1_[9]     = {};
  double            forward_matrix_2_[9]     = {};
  bool              as_shot_neutral_valid_   = false;
  double            as_shot_neutral_[3]      = {};
  bool              calibration_illuminants_valid_ = false;
  double            color_matrix_1_cct_      = 2856.0;
  double            color_matrix_2_cct_      = 6504.0;
};

/// Serialize the import-resolved RAW color and lens state. The DNG profile is written as its
/// fingerprint only; the profile tables are never written.
auto RawColorContextToJson(const RawRuntimeColorContext& context) -> nlohmann::json;

/// Decode a previously serialized RAW color and lens state. A DNG profile is read as an unbound
/// reference. Returns false when the JSON does not contain a usable RAW context.
auto RawColorContextFromJson(const nlohmann::json& value, RawRuntimeColorContext& context) -> bool;

}  // namespace alcedo
