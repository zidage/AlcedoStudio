//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/halation_op.hpp"

#include <gtest/gtest.h>

#include "edit/operators/GPU_kernels/fused_param.hpp"
#include "edit/operators/operator_factory.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"

namespace alcedo {

TEST(HalationOpTest, DefaultParamsExposeOnlyStrength) {
  HalationOp op;
  const auto params = op.GetParams();

  ASSERT_TRUE(params.contains("halation"));
  ASSERT_TRUE(params.at("halation").is_object());
  EXPECT_EQ(params.at("halation").size(), 1U);
  EXPECT_FLOAT_EQ(params.at("halation").at("strength").get<float>(), 0.0f);
  EXPECT_FLOAT_EQ(op.strength_scale(), 0.0f);
}

TEST(HalationOpTest, ReadsStrengthAndClampsToUserRange) {
  HalationOp op({{"halation", {{"strength", 135.0f}}}});
  EXPECT_FLOAT_EQ(op.strength(), 100.0f);
  EXPECT_FLOAT_EQ(op.strength_scale(), 2.0f);

  op.SetParams({{"halation", {{"strength", -10.0f}}}});
  EXPECT_FLOAT_EQ(op.strength(), 0.0f);
  EXPECT_FLOAT_EQ(op.strength_scale(), 0.0f);
}

TEST(HalationOpTest, GlobalParamsWritesHiddenDefaultsAndNormalizedStrength) {
  HalationOp     op({{"halation", {{"strength", 50.0f}}}});
  OperatorParams params;

  op.SetGlobalParams(params);

  EXPECT_TRUE(params.halation_.enabled_);
  EXPECT_FLOAT_EQ(params.halation_.strength_, 1.0f);
  EXPECT_FLOAT_EQ(params.halation_.low_threshold_, 0.7f);
  EXPECT_FLOAT_EQ(params.halation_.high_threshold_, 0.8f);
  EXPECT_FLOAT_EQ(params.halation_.sigma_, 7.0f);
  EXPECT_FLOAT_EQ(params.halation_.redshift_[0], 1.0f);
  EXPECT_FLOAT_EQ(params.halation_.redshift_[1], 0.05f);
  EXPECT_FLOAT_EQ(params.halation_.redshift_[2], 0.02f);
  EXPECT_FLOAT_EQ(params.halation_.additive_scale_, 2.0f);
}

TEST(HalationOpTest, EnableGlobalParamsTogglesHalationPayload) {
  HalationOp     op({{"halation", {{"strength", 25.0f}}}});
  OperatorParams params;

  op.EnableGlobalParams(params, false);
  op.SetGlobalParams(params);

  EXPECT_FALSE(params.halation_.enabled_);
  EXPECT_FLOAT_EQ(params.halation_.strength_, 0.5f);

  op.EnableGlobalParams(params, true);
  op.SetGlobalParams(params);

  EXPECT_TRUE(params.halation_.enabled_);
  EXPECT_FLOAT_EQ(params.halation_.strength_, 0.5f);
}

TEST(HalationOpTest, FusedParamsCarryHalationPayload) {
  HalationOp     op({{"halation", {{"strength", 75.0f}}}});
  OperatorParams params;

  op.SetGlobalParams(params);
  const auto fused = FusedParamsConverter::ConvertFromCPU(params);

  EXPECT_TRUE(fused.halation_.enabled_);
  EXPECT_FLOAT_EQ(fused.halation_.strength_, 1.5f);
  EXPECT_FLOAT_EQ(fused.halation_.low_threshold_, 0.7f);
  EXPECT_FLOAT_EQ(fused.halation_.high_threshold_, 0.8f);
  EXPECT_FLOAT_EQ(fused.halation_.sigma_, 7.0f);
  EXPECT_FLOAT_EQ(fused.halation_.redshift_[0], 1.0f);
  EXPECT_FLOAT_EQ(fused.halation_.redshift_[1], 0.05f);
  EXPECT_FLOAT_EQ(fused.halation_.redshift_[2], 0.02f);
  EXPECT_FLOAT_EQ(fused.halation_.additive_scale_, 2.0f);
}

TEST(HalationOpTest, FactoryCreatesHalationOperator) {
  RegisterAllOperators();
  auto op = OperatorFactory::Instance().Create(OperatorType::HALATION,
                                               pipeline_defaults::MakeDefaultHalationParams());
  ASSERT_NE(op, nullptr);
  EXPECT_EQ(op->GetScriptName(), "halation");
  EXPECT_EQ(op->GetStage(), PipelineStageName::Output_Transform);
}

}  // namespace alcedo
