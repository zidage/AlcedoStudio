//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <utility>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "../graph/test_camera_profile.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo {
namespace {

auto BindFromSample(const std::filesystem::path& path, ImageType type)
    -> std::pair<RawRuntimeColorContext, DevelopPayload> {
  Image image(1, path, type);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  EXPECT_TRUE(image.HasRawColorContext());
  const auto ctx = image.GetRawColorContext();
  auto       document = CreateDefaultPipelineDocument();
  gpu_dag_test::ApplyImportedCameraProfile(document, ctx);
  return {ctx, document.Develop()->Params().Params()};
}

TEST(GpuDagCudaDrtProduct, BindDevelopCameraProfileCopiesDngColorMatricesFromMetadataExtractor) {
  const auto sample =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "bad_dng" / "bad_color_dng.dng";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample.string();
  }
  const auto [ctx, payload] = BindFromSample(sample, ImageType::DNG);
  EXPECT_TRUE(ctx.color_matrices_valid_);
  EXPECT_TRUE(payload.camera_profile.color_matrices_valid);
  EXPECT_EQ(payload.camera_profile.color_matrices_valid, ctx.color_matrices_valid_);
  EXPECT_EQ(payload.camera_profile.forward_matrices_valid, ctx.forward_matrices_valid_);
  EXPECT_EQ(payload.camera_profile.as_shot_neutral_valid, ctx.as_shot_neutral_valid_);
  EXPECT_EQ(payload.camera_profile.calibration_illuminants_valid,
            ctx.calibration_illuminants_valid_);
  EXPECT_DOUBLE_EQ(payload.camera_profile.color_matrix_1_cct, ctx.color_matrix_1_cct_);
  EXPECT_DOUBLE_EQ(payload.camera_profile.color_matrix_2_cct, ctx.color_matrix_2_cct_);
  for (int i = 0; i < 9; ++i) {
    EXPECT_DOUBLE_EQ(payload.camera_profile.color_matrix_1[static_cast<std::size_t>(i)],
                     ctx.color_matrix_1_[i]);
    EXPECT_DOUBLE_EQ(payload.camera_profile.color_matrix_2[static_cast<std::size_t>(i)],
                     ctx.color_matrix_2_[i]);
  }
  EXPECT_NEAR(ctx.as_shot_neutral_[0], 0.30864197, 1e-6);
  EXPECT_NEAR(payload.camera_profile.as_shot_neutral[0], ctx.as_shot_neutral_[0], 1e-6);
  EXPECT_GE(payload.as_shot_cct, 2000.0f);
  EXPECT_LE(payload.as_shot_cct, 15000.0f);

  const auto resolved = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(resolved.ok);
  EXPECT_NE(resolved.transform.camera_to_ap1[0], 1.0f);
}

TEST(GpuDagCudaDrtProduct, BindDevelopCameraProfileCopiesNonDngCameraMatricesFromMetadataExtractor) {
  const auto sample =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "cct_test" / "_DSC8085.ARW";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "Sample ARW not found: " << sample.string();
  }
  const auto [ctx, payload] = BindFromSample(sample, ImageType::ARW);
  EXPECT_TRUE(ctx.color_matrices_valid_);
  EXPECT_TRUE(payload.camera_profile.color_matrices_valid);
  EXPECT_DOUBLE_EQ(payload.camera_profile.color_matrix_1_cct, ctx.color_matrix_1_cct_);
  EXPECT_DOUBLE_EQ(payload.camera_profile.color_matrix_2_cct, ctx.color_matrix_2_cct_);
  EXPECT_GE(payload.as_shot_cct, 2000.0f);
  EXPECT_LE(payload.as_shot_cct, 15000.0f);

  auto custom = payload;
  custom.wb_mode    = "custom";
  custom.custom_cct = 4000.0f;
  const auto as_shot = ResolveDevelopColorTransform(payload);
  const auto edited  = ResolveDevelopColorTransform(custom);
  ASSERT_TRUE(as_shot.ok);
  ASSERT_TRUE(edited.ok);
  double delta = 0.0;
  for (int i = 0; i < 9; ++i) {
    delta += std::abs(as_shot.transform.camera_to_ap1[static_cast<std::size_t>(i)] -
                      edited.transform.camera_to_ap1[static_cast<std::size_t>(i)]);
  }
  EXPECT_GT(delta, 1.0e-4);
}

}  // namespace
}  // namespace alcedo
