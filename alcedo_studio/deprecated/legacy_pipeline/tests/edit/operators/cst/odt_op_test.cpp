//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/cst/odt_op.hpp"

#include <gtest/gtest.h>

#include "edit/pipeline/default_pipeline_params.hpp"

namespace alcedo {

TEST(ODTOpTests, DefaultRoundTripUsesOpenDRT) {
  const nlohmann::json params = pipeline_defaults::MakeDefaultODTParams();
  ODT_Op               op(params);

  const nlohmann::json exported = op.GetParams();
  ASSERT_TRUE(exported.contains("odt"));
  EXPECT_EQ(exported["odt"]["method"], "open_drt");
  EXPECT_EQ(exported["odt"]["encoding_space"], "rec709");
  EXPECT_EQ(exported["odt"]["encoding_eotf"], "gamma_2_2");
  EXPECT_EQ(exported["odt"]["open_drt"]["look_preset"], "standard");
}

TEST(ODTOpTests, MethodSwitchesToACES2) {
  nlohmann::json params           = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["method"]         = "aces_2_0";
  params["odt"]["limiting_space"] = "rec709";

  ODT_Op         op(params);
  OperatorParams global_params;
  op.SetGlobalParams(global_params);

  EXPECT_EQ(global_params.to_output_params_.method_, ColorUtils::ODTMethod::ACES_2_0);
  EXPECT_EQ(op.GetParams()["odt"]["method"], "aces_2_0");
  EXPECT_EQ(op.GetParams()["odt"]["limiting_space"], "rec709");
}

TEST(ODTOpTests, ACES2St2084UsesReferenceLuminanceScale) {
  nlohmann::json params           = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["method"]         = "aces_2_0";
  params["odt"]["encoding_space"] = "rec2020";
  params["odt"]["encoding_eotf"]  = "st2084";
  params["odt"]["limiting_space"] = "rec2020";
  params["odt"]["peak_luminance"] = 600.0f;

  ODT_Op         op(params);
  OperatorParams global_params;
  op.SetGlobalParams(global_params);

  EXPECT_EQ(global_params.to_output_params_.method_, ColorUtils::ODTMethod::ACES_2_0);
  EXPECT_EQ(global_params.to_output_params_.eotf_, ColorUtils::EOTF::ST2084);
  EXPECT_FLOAT_EQ(global_params.to_output_params_.display_linear_scale_, ColorUtils::ref_lum);
}

TEST(ODTOpTests, ACES2GammaOutputKeepsUnitScale) {
  nlohmann::json params           = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["method"]         = "aces_2_0";
  params["odt"]["encoding_space"] = "rec709";
  params["odt"]["encoding_eotf"]  = "gamma_2_2";
  params["odt"]["limiting_space"] = "rec709";

  ODT_Op         op(params);
  OperatorParams global_params;
  op.SetGlobalParams(global_params);

  EXPECT_EQ(global_params.to_output_params_.method_, ColorUtils::ODTMethod::ACES_2_0);
  EXPECT_FLOAT_EQ(global_params.to_output_params_.display_linear_scale_, 1.0f);
}

TEST(ODTOpTests, UnsupportedOpenDRTOutputCombinationThrows) {
  nlohmann::json params           = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["encoding_space"] = "prophoto";
  EXPECT_THROW({ ODT_Op op(params); }, std::runtime_error);
}

TEST(ODTOpTests, PresetExpansionProducesStableResolvedRuntimeValues) {
  nlohmann::json params                         = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["open_drt"]["tonescale_preset"] = "aces_2_0";

  ODT_Op         op(params);
  OperatorParams global_params;
  op.SetGlobalParams(global_params);

  const auto& runtime = global_params.to_output_params_.open_drt_params_;
  EXPECT_NEAR(runtime.tn_con_, 1.15f, 1e-6f);
  EXPECT_NEAR(runtime.tn_toe_, 0.04f, 1e-6f);
  EXPECT_NEAR(runtime.ts_x0_, 0.18f, 1e-6f);
  EXPECT_NEAR(runtime.ts_dsc_, 1.0f, 1e-6f);
}

TEST(ODTOpTests, MethodRoundTripPreservesBothMethodSpecificSettings) {
  nlohmann::json params                         = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["method"]                       = "open_drt";
  params["odt"]["limiting_space"]               = "p3_d65";
  params["odt"]["open_drt"]["look_preset"]      = "umbra";
  params["odt"]["open_drt"]["tonescale_preset"] = "aces_2_0";
  params["odt"]["open_drt"]["creative_white"]   = "d60";

  ODT_Op open_drt_op(params);
  auto   exported_open_drt = open_drt_op.GetParams();
  EXPECT_EQ(exported_open_drt["odt"]["limiting_space"], "p3_d65");
  EXPECT_EQ(exported_open_drt["odt"]["open_drt"]["look_preset"], "umbra");
  EXPECT_EQ(exported_open_drt["odt"]["open_drt"]["tonescale_preset"], "aces_2_0");
  EXPECT_EQ(exported_open_drt["odt"]["open_drt"]["creative_white"], "d60");

  exported_open_drt["odt"]["method"] = "aces_2_0";
  ODT_Op     aces_op(exported_open_drt);
  const auto exported_aces = aces_op.GetParams();
  EXPECT_EQ(exported_aces["odt"]["limiting_space"], "p3_d65");
  EXPECT_EQ(exported_aces["odt"]["open_drt"]["look_preset"], "umbra");
  EXPECT_EQ(exported_aces["odt"]["open_drt"]["tonescale_preset"], "aces_2_0");
  EXPECT_EQ(exported_aces["odt"]["open_drt"]["creative_white"], "d60");
}

TEST(ODTOpTests, CustomOpenDRTLookUsesDetailedSliderParameters) {
  nlohmann::json params                               = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["open_drt"]["look_preset"]            = "custom";
  params["odt"]["open_drt"]["parameters"]["brl_r"]    = -4.0f;
  params["odt"]["open_drt"]["parameters"]["hs_b_rng"] = 2.5f;

  ODT_Op         op(params);
  OperatorParams global_params;
  op.SetGlobalParams(global_params);

  const auto& runtime = global_params.to_output_params_.open_drt_params_;
  EXPECT_NEAR(runtime.brl_r_, -4.0f, 1e-6f);
  EXPECT_NEAR(runtime.hs_b_rng_, 2.5f, 1e-6f);
  EXPECT_EQ(op.GetParams()["odt"]["open_drt"]["look_preset"], "custom");
  EXPECT_NEAR(op.GetParams()["odt"]["open_drt"]["parameters"]["brl_r"].get<float>(), -4.0f, 1e-6f);
}

TEST(ODTOpTests, CustomTonescaleOverridesLookPresetToneParameters) {
  nlohmann::json params                              = pipeline_defaults::MakeDefaultODTParams();
  params["odt"]["open_drt"]["look_preset"]           = "umbra";
  params["odt"]["open_drt"]["tonescale_preset"]      = "custom";
  params["odt"]["open_drt"]["parameters"]["tn_con"]  = 1.23f;
  params["odt"]["open_drt"]["parameters"]["tn_lcon"] = 0.77f;
  params["odt"]["open_drt"]["parameters"]["brl_r"]   = -3.25f;

  ODT_Op         op(params);
  OperatorParams global_params;
  op.SetGlobalParams(global_params);

  const auto& runtime = global_params.to_output_params_.open_drt_params_;
  EXPECT_NEAR(runtime.tn_con_, 1.23f, 1e-6f);
  EXPECT_NEAR(runtime.tn_lcon_, 0.77f, 1e-6f);
  EXPECT_NEAR(runtime.brl_r_, -4.5f, 1e-6f);
}

TEST(ODTOpTests, PresetAutomataWritesPresetValuesIntoSliderState) {
  odt_cpu::OpenDRTSettings settings;
  settings.detailed_.tn_con_ = 1.01f;
  settings.detailed_.brl_r_  = 0.5f;

  odt_cpu::ApplyOpenDRTLookPresetToSettings(odt_cpu::OpenDRTLookPreset::UMBRA, &settings);
  EXPECT_EQ(settings.look_preset_, odt_cpu::OpenDRTLookPreset::UMBRA);
  EXPECT_NEAR(settings.detailed_.tn_con_, 1.8f, 1e-6f);
  EXPECT_NEAR(settings.detailed_.brl_r_, -4.5f, 1e-6f);

  odt_cpu::ApplyOpenDRTTonescalePresetToSettings(odt_cpu::OpenDRTTonescalePreset::ACES_2_0,
                                                 &settings);
  EXPECT_EQ(settings.tonescale_preset_, odt_cpu::OpenDRTTonescalePreset::ACES_2_0);
  EXPECT_NEAR(settings.detailed_.tn_con_, 1.15f, 1e-6f);
  EXPECT_NEAR(settings.detailed_.brl_r_, -4.5f, 1e-6f);
}

}  // namespace alcedo
