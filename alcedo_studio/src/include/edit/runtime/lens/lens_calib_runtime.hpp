//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

enum class LensCalibDistortionModel : std::int32_t {
  NONE   = 0,
  POLY3  = 1,
  POLY5  = 2,
  PTLENS = 3,
};

enum class LensCalibTCAModel : std::int32_t {
  NONE   = 0,
  LINEAR = 1,
  POLY3  = 2,
};

enum class LensCalibVignettingModel : std::int32_t {
  NONE = 0,
  PA   = 1,
};

enum class LensCalibCropMode : std::int32_t {
  NONE      = 0,
  RECTANGLE = 1,
  CIRCLE    = 2,
};

enum class LensCalibProjectionType : std::int32_t {
  UNKNOWN               = 0,
  RECTILINEAR           = 1,
  FISHEYE               = 2,
  PANORAMIC             = 3,
  EQUIRECTANGULAR       = 4,
  FISHEYE_ORTHOGRAPHIC  = 5,
  FISHEYE_STEREOGRAPHIC = 6,
  FISHEYE_EQUISOLID     = 7,
  FISHEYE_THOBY         = 8,
};

enum class LensCalibInterpolation : std::int32_t {
  BILINEAR = 0,
  BICUBIC  = 1,
  LANCZOS  = 2,
};

struct LensCalibGpuParams {
  std::int32_t version = 1;

  std::int32_t src_width  = 0;
  std::int32_t src_height = 0;
  std::int32_t dst_width  = 0;
  std::int32_t dst_height = 0;

  // Lensfun-style normalized coordinate conversion:
  // normalized = pixel * norm_scale - center
  // pixel      = (normalized + center) * norm_unscale
  float norm_scale   = 0.0f;
  float norm_unscale = 0.0f;
  float center_x     = 0.0f;
  float center_y     = 0.0f;

  float camera_crop_factor = 0.0f;
  float nominal_focal_mm   = 0.0f;
  float real_focal_mm      = 0.0f;
  float lens_center_x      = 0.0f;
  float lens_center_y      = 0.0f;

  std::int32_t source_projection =
      static_cast<std::int32_t>(LensCalibProjectionType::UNKNOWN);
  std::int32_t target_projection =
      static_cast<std::int32_t>(LensCalibProjectionType::UNKNOWN);

  std::int32_t distortion_model =
      static_cast<std::int32_t>(LensCalibDistortionModel::NONE);
  float distortion_terms[5] = {};

  std::int32_t tca_model = static_cast<std::int32_t>(LensCalibTCAModel::NONE);
  float tca_terms[12] = {};

  std::int32_t vignetting_model =
      static_cast<std::int32_t>(LensCalibVignettingModel::NONE);
  float vignetting_terms[3] = {};

  std::int32_t crop_mode = static_cast<std::int32_t>(LensCalibCropMode::NONE);
  float crop_bounds[4] = {};  // left right top bottom

  std::int32_t interpolation =
      static_cast<std::int32_t>(LensCalibInterpolation::BILINEAR);

  std::int32_t apply_vignetting  = 0;
  std::int32_t apply_distortion  = 0;
  std::int32_t apply_tca         = 0;
  std::int32_t apply_projection  = 0;
  std::int32_t apply_crop        = 0;
  std::int32_t apply_crop_circle = 0;

  std::int32_t use_user_scale = 0;
  std::int32_t use_auto_scale = 0;
  float user_scale            = 1.0f;
  float resolved_scale        = 1.0f;

  // Extension point: perspective correction is intentionally stubbed for now.
  std::int32_t perspective_mode = 0;
  float perspective_terms[8] = {};

  // Fast path hints
  std::int32_t fast_path_distortion_only = 0;
  std::int32_t fast_path_vignetting_only = 0;

  // Preview/export precision hook.
  std::int32_t low_precision_preview = 0;
};

}  // namespace alcedo
