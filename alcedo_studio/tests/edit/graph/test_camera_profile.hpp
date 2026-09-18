//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {
namespace gpu_dag_test {

/**
 * @brief Non-diagonal dual-illuminant XYZ→camera matrices for CameraColor tests.
 *
 * Invertible and off-diagonal so a cam_mul diagonal, rgb_cam identity, or linear
 * Kelvin interpolator cannot match the mired dual-illuminant result by accident.
 * No-op when @p profile.color_matrices_valid is already true.
 */
inline void FillTestCameraProfile(DevelopCameraProfile& profile) {
  if (profile.color_matrices_valid) {
    return;
  }
  profile.color_matrices_valid          = true;
  profile.calibration_illuminants_valid = true;
  profile.color_matrix_1_cct            = 2856.0;
  profile.color_matrix_2_cct            = 6504.0;
  profile.color_matrix_1                = {1.85, 0.22, 0.08, 0.36, 1.62, 0.14, 0.06, 0.28, 1.76};
  profile.color_matrix_2                = {1.42, 0.42, 0.24, 0.32, 1.48, 0.26, 0.16, 0.38, 1.66};
  profile.as_shot_neutral_valid         = true;
  profile.as_shot_neutral               = {0.45, 1.0, 0.62};
  profile.cam_mul                       = {2.0f, 1.0f, 1.5f};
}

inline void EnsureTestCameraProfile(PipelineDocument& document) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  auto payload = develop->Params().Params();
  FillTestCameraProfile(payload.camera_profile);
  develop->Params().ReplaceParams(std::move(payload));
}

inline void ApplyImportedCameraProfile(PipelineDocument& document,
                                       const RawRuntimeColorContext& imported) {
  auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  auto payload = develop->Params().Params();
  BindDevelopCameraProfile(payload, imported);
  develop->Params().ReplaceParams(std::move(payload));
}

}  // namespace gpu_dag_test
}  // namespace alcedo
