//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"

namespace alcedo {
namespace {

auto MakeSampleRawContext() -> RawRuntimeColorContext {
  RawRuntimeColorContext ctx;
  ctx.valid_                  = true;
  ctx.output_in_camera_space_ = true;
  ctx.camera_make_            = "Alcedo";
  ctx.camera_model_           = "TestCam";
  ctx.cam_mul_[0]             = 1.8f;
  ctx.cam_mul_[1]             = 1.0f;
  ctx.cam_mul_[2]             = 1.4f;
  ctx.color_matrices_valid_   = true;
  ctx.color_matrix_1_[0]      = 1.1;
  ctx.color_matrix_2_[4]      = 0.9;
  ctx.as_shot_neutral_valid_   = true;
  ctx.as_shot_neutral_[0]     = 0.45;
  ctx.as_shot_neutral_[1]     = 1.0;
  ctx.as_shot_neutral_[2]     = 0.72;
  ctx.lens_metadata_valid_    = true;
  ctx.lens_make_              = "Alcedo Optics";
  ctx.lens_model_             = "50mm f/1.8";
  ctx.focal_length_mm_        = 50.0f;
  ctx.aperture_f_number_      = 1.8f;
  return ctx;
}

TEST(RawDecodeOpParamsTest, GetParamsRoundTripsInherentRawColorContextFields) {
  const auto context = MakeSampleRawContext();

  nlohmann::json params = pipeline_defaults::MakeDefaultRawDecodeParams();
  const auto     context_json = RawColorContextToJson(context);
  for (auto it = context_json.begin(); it != context_json.end(); ++it) {
    params["raw"][it.key()] = it.value();
  }

  RawDecodeOp op(params);
  const auto  exported = op.GetParams();
  ASSERT_TRUE(exported.contains("raw"));
  ASSERT_TRUE(exported["raw"].is_object());
  EXPECT_FALSE(exported["raw"].contains("decode_res"));

  RawRuntimeColorContext loaded;
  ASSERT_TRUE(RawColorContextFromJson(exported["raw"], loaded));
  EXPECT_TRUE(loaded.valid_);
  EXPECT_EQ(loaded.camera_make_, "Alcedo");
  EXPECT_EQ(loaded.camera_model_, "TestCam");
  EXPECT_FLOAT_EQ(loaded.cam_mul_[0], 1.8f);
  EXPECT_TRUE(loaded.color_matrices_valid_);
  EXPECT_DOUBLE_EQ(loaded.color_matrix_1_[0], 1.1);
  EXPECT_TRUE(loaded.as_shot_neutral_valid_);
  EXPECT_DOUBLE_EQ(loaded.as_shot_neutral_[0], 0.45);
  EXPECT_EQ(loaded.lens_model_, "50mm f/1.8");
  EXPECT_FLOAT_EQ(loaded.focal_length_mm_, 50.0f);
}

TEST(RawDecodeOpParamsTest, GetParamsOmitsDecodeResEvenAfterSetParamsInstall) {
  RawDecodeOp op(pipeline_defaults::MakeDefaultRawDecodeParams());
  op.SetParams({{"raw", {{"decode_res", static_cast<int>(DecodeRes::QUARTER)}}}});
  EXPECT_EQ(op.params_.decode_res_, DecodeRes::QUARTER);

  const auto exported = op.GetParams();
  ASSERT_TRUE(exported.contains("raw"));
  EXPECT_FALSE(exported["raw"].contains("decode_res"));
}

TEST(RawDecodeOpParamsTest, SetGlobalParamsUsesInherentContextWithoutInject) {
  const auto context = MakeSampleRawContext();
  RawDecodeOp op(pipeline_defaults::MakeDefaultRawDecodeParams());
  op.SetInherentRawContext(context);

  OperatorParams global;
  op.SetGlobalParams(global);
  EXPECT_TRUE(global.raw_runtime_valid_);
  EXPECT_EQ(global.raw_camera_make_, "Alcedo");
  EXPECT_EQ(global.raw_camera_model_, "TestCam");
  EXPECT_TRUE(global.raw_as_shot_neutral_valid_);
  EXPECT_DOUBLE_EQ(global.raw_as_shot_neutral_[0], 0.45);
  EXPECT_EQ(global.raw_lens_model_, "50mm f/1.8");
}

}  // namespace
}  // namespace alcedo
