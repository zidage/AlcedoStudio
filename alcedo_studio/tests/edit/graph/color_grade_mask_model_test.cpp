//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_validation.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"

namespace alcedo {

TEST(GpuDagModelGraph, MultipleMasksBelongToOneColorGrade) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  EXPECT_EQ(grade->MaskCount(), 0u);

  grade->AddMask(grade_mask_test::MakeBrushMask(MaskId{"mask.brush"}, MaskAssetKey{}), 0);
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}), 1);
  grade->AddMask(grade_mask_test::MakeLinearGradientMask(MaskId{"mask.linear"}), 2);

  ASSERT_EQ(grade->MaskCount(), 3u);
  ASSERT_NE(grade->FindMask(MaskId{"mask.brush"}), nullptr);
  ASSERT_NE(grade->FindMask(MaskId{"mask.radial"}), nullptr);
  ASSERT_NE(grade->FindMask(MaskId{"mask.linear"}), nullptr);
  EXPECT_EQ(GetMaskSourceKind(grade->FindMask(MaskId{"mask.brush"})->source), MaskSourceKind::Brush);
  EXPECT_EQ(GetMaskSourceKind(grade->FindMask(MaskId{"mask.radial"})->source),
            MaskSourceKind::Radial);
  EXPECT_EQ(GetMaskSourceKind(grade->FindMask(MaskId{"mask.linear"})->source),
            MaskSourceKind::LinearGradient);
  EXPECT_EQ(grade->MaskAt(0).id, MaskId{"mask.brush"});
  EXPECT_EQ(grade->MaskAt(1).id, MaskId{"mask.radial"});
  EXPECT_EQ(grade->MaskAt(2).id, MaskId{"mask.linear"});
}

TEST(GpuDagModelGraph, DuplicateMaskIdLeavesGradeUnchanged) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.a"}), 0);
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.b"}), 1);
  const auto before = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());

  EXPECT_THROW(grade->AddMask(grade_mask_test::MakeBrushMask(MaskId{"mask.a"}, MaskAssetKey{}), 0),
               std::runtime_error);
  const auto after = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());
  EXPECT_EQ(after, before);
}

TEST(GpuDagModelGraph, InvalidMaskValuesFailBeforeDocumentMutation) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.keep"}), 0);
  const auto before = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());

  MaskModel nan_opacity = grade_mask_test::MakeRadialMask(MaskId{"mask.bad"});
  nan_opacity.opacity   = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(grade->AddMask(nan_opacity, 1), std::runtime_error);

  MaskModel bad_opacity = grade_mask_test::MakeRadialMask(MaskId{"mask.opacity"});
  bad_opacity.opacity   = 1.5f;
  EXPECT_THROW(grade->AddMask(bad_opacity, 1), std::runtime_error);

  RadialMaskSource negative_radius;
  negative_radius.major_radius = -0.1f;
  EXPECT_THROW(grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.radius"}, negative_radius),
                             1),
               std::runtime_error);

  LinearGradientMaskSource zero_normal;
  zero_normal.normal_x = 0.0f;
  zero_normal.normal_y = 0.0f;
  EXPECT_THROW(
      grade->AddMask(grade_mask_test::MakeLinearGradientMask(MaskId{"mask.dir"}, zero_normal), 1),
      std::runtime_error);

  EXPECT_THROW(grade->SetMaskOpacity(MaskId{"mask.keep"}, std::numeric_limits<float>::infinity()),
               std::runtime_error);

  const auto after = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());
  EXPECT_EQ(after, before);
  EXPECT_EQ(document.PrimaryGrade()->FindMask(MaskId{"mask.keep"})->id, MaskId{"mask.keep"});
}

TEST(GpuDagModelGraph, MaskListRoundTripPreservesSourcesOrderAndRangeFields) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();

  MaskModel brush = grade_mask_test::MakeBrushMask(MaskId{"mask.brush"}, MaskAssetKey{"asset_01"});
  brush.display_name   = "Brush";
  brush.opacity        = 0.25f;
  brush.invert         = true;
  brush.color_range    = ColorRangeModel{false};
  brush.luminance_range.reset();

  MaskModel radial          = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"});
  radial.display_name       = "Face";
  radial.opacity            = 0.8f;
  auto radial_source        = std::get<RadialMaskSource>(radial.source);
  radial_source.center_y    = 0.45f;
  radial_source.major_radius = 0.3f;
  radial_source.minor_radius = 0.2f;
  radial_source.outer_feather = 0.15f;
  radial.source             = radial_source;
  radial.color_range.reset();
  radial.luminance_range = LuminanceRangeModel{false};

  MaskModel linear = grade_mask_test::MakeLinearGradientMask(MaskId{"mask.linear"});
  linear.display_name = "Sky";
  linear.enabled      = false;

  grade->AddMask(std::move(brush), 0);
  grade->AddMask(std::move(radial), 1);
  grade->AddMask(std::move(linear), 2);

  const auto json     = document.ToJson();
  const auto restored = PipelineDocument::FromJson(json);
  ASSERT_NE(restored.PrimaryGrade(), nullptr);
  const auto* restored_grade = restored.PrimaryGrade();
  ASSERT_EQ(restored_grade->MaskCount(), 3u);
  EXPECT_EQ(restored_grade->MaskAt(0), document.PrimaryGrade()->MaskAt(0));
  EXPECT_EQ(restored_grade->MaskAt(1), document.PrimaryGrade()->MaskAt(1));
  EXPECT_EQ(restored_grade->MaskAt(2), document.PrimaryGrade()->MaskAt(2));
  EXPECT_EQ(restored_grade->MaskAt(0).color_range.has_value(), true);
  EXPECT_EQ(restored_grade->MaskAt(0).luminance_range.has_value(), false);
  EXPECT_EQ(restored_grade->MaskAt(1).color_range.has_value(), false);
  EXPECT_EQ(restored_grade->MaskAt(1).luminance_range.has_value(), true);
  nlohmann::json grade_json;
  for (const auto& node : json["nodes"]) {
    if (node.at("id") == "grade.primary") {
      grade_json = node;
    }
  }
  ASSERT_FALSE(grade_json.is_null());
  EXPECT_EQ(grade_json["masks"][0]["source"]["kind"], "brush");
  EXPECT_EQ(grade_json["masks"][1]["source"]["kind"], "radial");
  EXPECT_EQ(grade_json["masks"][2]["source"]["kind"], "linear_gradient");
  EXPECT_TRUE(grade_json["masks"][0]["luminance_range"].is_null());
  EXPECT_TRUE(grade_json["masks"][1]["color_range"].is_null());
}

TEST(GpuDagModelGraph, TopLevelMaskNodesAndEdgesAreRejected) {
  const auto valid = CreateDefaultPipelineDocument().ToJson();

  auto with_node = valid;
  with_node["nodes"].push_back({{"id", "mask.old"},
                                {"type", std::string{type_ids::RasterMaskNode().Text()}},
                                {"params", nlohmann::json::object()}});
  try {
    (void)PipelineDocument::FromJson(with_node);
    FAIL() << "expected top-level Mask node rejection";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string{error.what()}.find("top-level Mask nodes"), std::string::npos);
  }

  auto with_analytic = valid;
  with_analytic["nodes"].push_back({{"id", "mask.analytic"},
                                    {"type", std::string{type_ids::AnalyticMaskNode().Text()}},
                                    {"params", nlohmann::json::object()}});
  EXPECT_THROW((void)PipelineDocument::FromJson(with_analytic), std::runtime_error);

  auto with_edge = valid;
  with_edge["edges"].push_back({{"from", nlohmann::json::array({"develop", "image"})},
                                {"to", nlohmann::json::array({"grade.primary", "mask"})}});
  try {
    (void)PipelineDocument::FromJson(with_edge);
    FAIL() << "expected Mask edge rejection";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string{error.what()}.find("Mask edges"), std::string::npos);
  }
}

TEST(GpuDagModelGraph, EnabledRangeFailsBeforeGpuWork) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  MaskModel enabled_color = grade_mask_test::MakeRadialMask(MaskId{"mask.range"});
  enabled_color.color_range = ColorRangeModel{true};
  EXPECT_THROW(grade->AddMask(enabled_color, 0), std::runtime_error);
  EXPECT_EQ(grade->MaskCount(), 0u);

  MaskModel enabled_luma = grade_mask_test::MakeRadialMask(MaskId{"mask.luma"});
  enabled_luma.luminance_range = LuminanceRangeModel{true};
  EXPECT_THROW(grade->AddMask(enabled_luma, 0), std::runtime_error);

  auto json = CreateDefaultPipelineDocument().ToJson();
  for (auto& node : json["nodes"]) {
    if (node.at("id") != "grade.primary") {
      continue;
    }
    node["masks"] = nlohmann::json::array(
        {nlohmann::json{{"id", "mask.range"},
                        {"display_name", ""},
                        {"enabled", true},
                        {"opacity", 1.0},
                        {"invert", false},
                        {"source",
                         {{"kind", "radial"},
                          {"center_x", 0.5},
                          {"center_y", 0.5},
                          {"major_radius", 0.5},
                          {"minor_radius", 0.5},
                          {"rotation", 0.0},
                          {"inner_feather", 0.0},
                          {"outer_feather", 0.0}}},
                        {"color_range", {{"enabled", true}}},
                        {"luminance_range", nullptr}}});
  }
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);

  for (auto& node : json["nodes"]) {
    if (node.at("id") != "grade.primary") {
      continue;
    }
    node["masks"][0]["color_range"]    = nullptr;
    node["masks"][0]["source"]["kind"] = "graduated_nd";
  }
  EXPECT_THROW((void)PipelineDocument::FromJson(json), std::runtime_error);
}

TEST(GpuDagModelGraph, ColorGradeHasNoMaskInputPort) {
  auto document = CreateDefaultPipelineDocument();
  document.Graph().Connect(NodeId{"develop"}, PortId{"image"}, NodeId{"grade.primary"},
                           PortId{"mask"});
  const auto errors = document.Graph().Validate();
  bool       unknown_port = false;
  for (const auto& error : errors) {
    if (error.code == GraphValidationCode::UnknownPort) {
      unknown_port = true;
    }
  }
  EXPECT_TRUE(unknown_port);
}

TEST(GpuDagModelGraph, MoveMaskForDisplayDoesNotChangeMaskIdentity) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.a"}), 0);
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.b"}), 1);
  grade->MoveMaskForDisplay(MaskId{"mask.b"}, 0);
  EXPECT_EQ(grade->MaskAt(0).id, MaskId{"mask.b"});
  EXPECT_EQ(grade->MaskAt(1).id, MaskId{"mask.a"});
  EXPECT_NE(grade->FindMask(MaskId{"mask.a"}), nullptr);
  EXPECT_NE(grade->FindMask(MaskId{"mask.b"}), nullptr);
}

}  // namespace alcedo
