//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "decoders/processor/raw_color_context.hpp"
#include "image/dng_color_profile.hpp"

namespace alcedo::gpu_dag_test {

/**
 * @brief One import-time RAW color context and the name of its stored expected Develop JSON.
 *
 * The expected files under tests/resources/expected_json/ were written from
 * `PipelineExecutor::InjectRawMetadata` (anonymous `ApplyImportedCameraProfile`) at
 * revision 92085ffe on a default document. They hold `DevelopNodeModel::Params()` JSON only.
 */
struct ImportedCameraProfileFixture {
  std::string            name_;
  std::string            expected_file_;
  RawRuntimeColorContext context_;
};

inline void FillDualIlluminantMatrices(RawRuntimeColorContext& ctx) {
  constexpr std::array<double, 9> kCm1 = {1.85, 0.22, 0.08, 0.36, 1.62, 0.14, 0.06, 0.28, 1.76};
  constexpr std::array<double, 9> kCm2 = {1.42, 0.42, 0.24, 0.32, 1.48, 0.26, 0.16, 0.38, 1.66};
  ctx.valid_                           = true;
  ctx.color_matrices_valid_            = true;
  ctx.calibration_illuminants_valid_   = true;
  ctx.color_matrix_1_cct_              = 2856.0;
  ctx.color_matrix_2_cct_              = 6504.0;
  for (int i = 0; i < 9; ++i) {
    ctx.color_matrix_1_[i] = kCm1[static_cast<std::size_t>(i)];
    ctx.color_matrix_2_[i] = kCm2[static_cast<std::size_t>(i)];
  }
  ctx.as_shot_neutral_valid_ = true;
  ctx.as_shot_neutral_[0]    = 0.45;
  ctx.as_shot_neutral_[1]    = 1.0;
  ctx.as_shot_neutral_[2]    = 0.62;
  ctx.cam_mul_[0]            = 2.0f;
  ctx.cam_mul_[1]            = 1.0f;
  ctx.cam_mul_[2]            = 1.5f;
}

/** @brief DNG, non-DNG RAW and RGB (non-RAW) working-space contexts. */
inline auto MakeImportedCameraProfileFixtures() -> std::vector<ImportedCameraProfileFixture> {
  std::vector<ImportedCameraProfileFixture> fixtures;

  RawRuntimeColorContext                    dng;
  FillDualIlluminantMatrices(dng);
  dng.forward_matrices_valid_          = true;
  constexpr std::array<double, 9> kFm1 = {0.61, 0.24, 0.11, 0.27, 0.69, 0.04, 0.02, 0.11, 0.70};
  constexpr std::array<double, 9> kFm2 = {0.66, 0.20, 0.10, 0.29, 0.66, 0.05, 0.03, 0.08, 0.72};
  for (int i = 0; i < 9; ++i) {
    dng.forward_matrix_1_[i] = kFm1[static_cast<std::size_t>(i)];
    dng.forward_matrix_2_[i] = kFm2[static_cast<std::size_t>(i)];
  }
  DngColorProfile profile;
  profile.name              = "Fixture Standard";
  profile.analog_balance    = {1.02, 1.0, 0.98};
  profile.baseline_exposure = 0.25;
  dng.dng_profile_          = MakeDngColorProfile(std::move(profile));
  fixtures.push_back({"dng", "imported_camera_profile_dng_expected_develop.json", dng});

  RawRuntimeColorContext raw;
  FillDualIlluminantMatrices(raw);
  raw.camera_make_  = "Fixture";
  raw.camera_model_ = "Bayer One";
  fixtures.push_back({"non_dng_raw", "imported_camera_profile_raw_expected_develop.json", raw});

  RawRuntimeColorContext rgb;
  rgb.valid_                         = true;
  rgb.output_in_camera_space_        = false;
  rgb.color_matrices_valid_          = true;
  rgb.as_shot_neutral_valid_         = true;
  rgb.calibration_illuminants_valid_ = false;
  rgb.color_matrix_1_cct_            = 6504.0;
  rgb.color_matrix_2_cct_            = 6504.0;
  for (int i = 0; i < 3; ++i) {
    rgb.as_shot_neutral_[i]        = 1.0;
    rgb.color_matrix_1_[i * 3 + i] = 1.0;
    rgb.color_matrix_2_[i * 3 + i] = 1.0;
  }
  fixtures.push_back({"non_raw_rgb", "imported_camera_profile_rgb_expected_develop.json", rgb});
  return fixtures;
}

}  // namespace alcedo::gpu_dag_test
