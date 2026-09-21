//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/geometry/lens_calib_op.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/geometry/lens_calib_runtime.hpp"

namespace alcedo {
namespace {

auto TouitPayload(bool enabled) -> DevelopPayload {
  DevelopPayload payload;
  payload.lens_enabled         = enabled;
  payload.apply_vignetting     = true;
  payload.apply_distortion     = true;
  payload.apply_tca            = false;
  payload.apply_crop           = false;
  payload.projection_enabled   = false;
  payload.lens_maker           = "Zeiss";
  payload.lens_model           = "Touit 1.8/32";
  payload.lens_profile_db_path = (std::filesystem::path(CONFIG_PATH) / "lens_calib").string();
  return payload;
}

auto FocalOnlyContext() -> RawRuntimeColorContext {
  RawRuntimeColorContext context;
  context.valid_               = true;
  context.lens_metadata_valid_ = false;
  context.focal_length_mm_     = 32.0f;
  context.aperture_f_number_   = 1.8f;
  context.focus_distance_m_    = 10.0f;
  context.crop_factor_hint_    = 1.534f;
  return context;
}

}  // namespace

TEST(LensCalibDevelopResolve, UserCatalogIdentityResolvesWhenRawLensNameIsEmpty) {
  LensCalibOp op(TouitPayload(true));
  const auto  runtime = op.ResolveRuntimeForImage(FocalOnlyContext(), Extent2D{96, 64}, false);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->apply_vignetting, 1);
  EXPECT_EQ(runtime->vignetting_model,
            static_cast<std::int32_t>(LensCalibVignettingModel::PA));
  EXPECT_GT(runtime->src_width, 0);
  EXPECT_GT(runtime->src_height, 0);
}

TEST(LensCalibDevelopResolve, DisabledPayloadReturnsNoRuntime) {
  LensCalibOp op(TouitPayload(false));
  const auto  runtime = op.ResolveRuntimeForImage(FocalOnlyContext(), Extent2D{96, 64}, false);
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, EmptyCatalogIdentityWithoutRawLensNameReturnsNoRuntime) {
  auto payload         = TouitPayload(true);
  payload.lens_maker   = {};
  payload.lens_model   = {};
  LensCalibOp op(payload);
  const auto  runtime = op.ResolveRuntimeForImage(FocalOnlyContext(), Extent2D{96, 64}, false);
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, CatalogIdentityWithoutFocalLengthReturnsNoRuntime) {
  auto context                 = FocalOnlyContext();
  context.focal_length_mm_     = 0.0f;
  context.aperture_f_number_   = 0.0f;
  context.lens_metadata_valid_ = false;
  LensCalibOp op(TouitPayload(true));
  const auto  runtime = op.ResolveRuntimeForImage(context, Extent2D{96, 64}, false);
  EXPECT_FALSE(runtime.has_value());
}

}  // namespace alcedo
