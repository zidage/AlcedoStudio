//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
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

  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}), 0);
  grade->AddMask(grade_mask_test::MakeLinearGradientMask(MaskId{"mask.linear"}), 1);

  ASSERT_EQ(grade->MaskCount(), 2u);
  ASSERT_NE(grade->FindMask(MaskId{"mask.radial"}), nullptr);
  ASSERT_NE(grade->FindMask(MaskId{"mask.linear"}), nullptr);
  EXPECT_EQ(GetMaskSourceKind(grade->FindMask(MaskId{"mask.radial"})->source),
            MaskSourceKind::Radial);
  EXPECT_EQ(GetMaskSourceKind(grade->FindMask(MaskId{"mask.linear"})->source),
            MaskSourceKind::LinearGradient);
  EXPECT_EQ(grade->MaskAt(0).id, MaskId{"mask.radial"});
  EXPECT_EQ(grade->MaskAt(1).id, MaskId{"mask.linear"});
}

TEST(GpuDagModelGraph, DuplicateMaskIdLeavesGradeUnchanged) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.a"}), 0);
  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.b"}), 1);
  const auto before = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());

  EXPECT_THROW(grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.a"}), 0),
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
  radial.luminance_range      = LuminanceRangeModel{0.25f, 0.75f, 0.05f};

  MaskModel linear = grade_mask_test::MakeLinearGradientMask(MaskId{"mask.linear"});
  linear.display_name = "Sky";
  linear.enabled      = false;

  MaskModel second            = grade_mask_test::MakeRadialMask(MaskId{"mask.second"});
  second.display_name         = "Second";
  second.opacity              = 0.25f;
  second.invert               = true;
  second.color_range          = ColorRangeModel{300.0f, 40.0f, 12.0f};
  second.luminance_range.reset();

  grade->AddMask(std::move(radial), 0);
  grade->AddMask(std::move(linear), 1);
  grade->AddMask(std::move(second), 2);

  const auto json     = document.ToJson();
  const auto restored = PipelineDocument::FromJson(json);
  ASSERT_NE(restored.PrimaryGrade(), nullptr);
  const auto* restored_grade = restored.PrimaryGrade();
  ASSERT_EQ(restored_grade->MaskCount(), 3u);
  EXPECT_EQ(restored_grade->MaskAt(0), document.PrimaryGrade()->MaskAt(0));
  EXPECT_EQ(restored_grade->MaskAt(1), document.PrimaryGrade()->MaskAt(1));
  EXPECT_EQ(restored_grade->MaskAt(2), document.PrimaryGrade()->MaskAt(2));
  EXPECT_EQ(restored_grade->MaskAt(0).color_range.has_value(), false);
  EXPECT_EQ(restored_grade->MaskAt(0).luminance_range.has_value(), true);
  EXPECT_EQ(restored_grade->MaskAt(2).color_range.has_value(), true);
  EXPECT_EQ(restored_grade->MaskAt(2).luminance_range.has_value(), false);
  nlohmann::json grade_json;
  for (const auto& node : json["nodes"]) {
    if (node.at("id") == "grade.primary") {
      grade_json = node;
    }
  }
  ASSERT_FALSE(grade_json.is_null());
  EXPECT_EQ(grade_json["masks"][0]["source"]["kind"], "radial");
  EXPECT_EQ(grade_json["masks"][1]["source"]["kind"], "linear_gradient");
  EXPECT_EQ(grade_json["masks"][2]["source"]["kind"], "radial");
  EXPECT_TRUE(grade_json["masks"][1]["color_range"].is_null());
  EXPECT_TRUE(grade_json["masks"][2]["luminance_range"].is_null());
  EXPECT_EQ(grade_json["masks"][0]["luminance_range"].dump(),
            R"({"high":0.75,"low":0.25,"softness":0.05000000074505806})");
  EXPECT_EQ(grade_json["masks"][2]["color_range"].dump(),
            R"({"hue_end":40.0,"hue_start":300.0,"softness":12.0})");
  EXPECT_EQ(restored.ToJson().dump(), json.dump());
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

TEST(GpuDagModelGraph, InvalidRangeValuesFailBeforeDocumentMutation) {
  auto  document = CreateDefaultPipelineDocument();
  auto* grade    = document.PrimaryGrade();
  const auto add_with_color = [&](ColorRangeModel range) {
    MaskModel mask   = grade_mask_test::MakeRadialMask(MaskId{"mask.range"});
    mask.color_range = range;
    grade->AddMask(mask, 0);
  };
  const auto add_with_luminance = [&](LuminanceRangeModel range) {
    MaskModel mask       = grade_mask_test::MakeRadialMask(MaskId{"mask.luma"});
    mask.luminance_range = range;
    grade->AddMask(mask, 0);
  };
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  // A full range is stored as an empty optional, never as an object.
  EXPECT_THROW(add_with_color({0.0f, 360.0f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({kNaN, 90.0f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({-1.0f, 90.0f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({360.0f, 90.0f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({10.0f, 0.0f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({10.0f, 361.0f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({10.0f, 10.5f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({359.75f, 0.25f, 12.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({10.0f, 90.0f, 0.0f}), std::runtime_error);
  EXPECT_THROW(add_with_color({10.0f, 90.0f, 91.0f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({0.0f, 1.0f, 0.05f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({kNaN, 0.5f, 0.05f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({-0.1f, 0.5f, 0.05f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({0.2f, 1.1f, 0.05f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({0.5f, 0.25f, 0.05f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({0.5f, 0.505f, 0.05f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({0.2f, 0.5f, 0.0f}), std::runtime_error);
  EXPECT_THROW(add_with_luminance({0.2f, 0.5f, 0.6f}), std::runtime_error);
  EXPECT_EQ(grade->MaskCount(), 0u);

  grade->AddMask(grade_mask_test::MakeRadialMask(MaskId{"mask.keep"}), 0);
  const auto before = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());
  EXPECT_THROW(
      grade->SetMaskLuminanceRange(MaskId{"mask.keep"}, LuminanceRangeModel{0.0f, 1.0f, 0.05f}),
      std::runtime_error);
  EXPECT_THROW(grade->SetMaskColorRange(MaskId{"mask.keep"}, ColorRangeModel{10.0f, 10.0f, 12.0f}),
               std::runtime_error);
  EXPECT_THROW(grade->SetMaskColorRange(MaskId{"mask.missing"}, std::nullopt), std::runtime_error);
  const auto after = std::vector<MaskModel>(grade->Masks().begin(), grade->Masks().end());
  EXPECT_EQ(after, before);

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

TEST(GpuDagModelGraph, AiSegmentationSourceJsonRoundTripKeepsAllFields) {
  const auto mask = grade_mask_test::MakeAiSegmentationMask(MaskId{"mask.ai"});
  const auto json = MaskModelToJson(mask);
  EXPECT_EQ(json.at("source").at("kind"), "ai_segmentation");
  EXPECT_EQ(json.at("source").at("format_version"), 1);
  EXPECT_EQ(json.at("source").at("components").size(), 3u);

  const auto restored = MaskModelFromJson(json);
  EXPECT_EQ(restored, mask);
  EXPECT_EQ(GetMaskSourceKind(restored.source), MaskSourceKind::AiSegmentation);
  EXPECT_EQ(MaskSourceKindText(MaskSourceKind::AiSegmentation), "ai_segmentation");
  const auto& ai = std::get<AiSegmentationMaskSource>(restored.source);
  ASSERT_EQ(ai.recipe.components.size(), 3u);
  const auto& people = std::get<AiMaskTaskComponent>(ai.recipe.components[0].content);
  ASSERT_EQ(people.person_toggles.size(), 1u);
  EXPECT_EQ(people.person_toggles[0].index, 2u);
  EXPECT_EQ(people.person_toggles[0].operation, AiMaskComponentOperation::Subtract);
  const auto& stroke = std::get<AiMaskPromptGroup>(ai.recipe.components[2].content);
  ASSERT_EQ(stroke.steps.size(), 2u);
  EXPECT_TRUE(stroke.steps[1].box.has_value());
  EXPECT_EQ(stroke.steps[1].points[0].label, AiMaskComponentOperation::Subtract);
  // A second serialization gives the same bytes.
  EXPECT_EQ(MaskModelToJson(restored).dump(), json.dump());
}

TEST(GpuDagModelGraph, AiSegmentationSourceRejectsInvalidFields) {
  const auto valid = MaskModelToJson(grade_mask_test::MakeAiSegmentationMask(MaskId{"mask.ai"}));
  ASSERT_NO_THROW((void)MaskModelFromJson(valid));

  const auto point_list = [](std::size_t count) {
    auto points = nlohmann::json::array();
    for (std::size_t index = 0; index < count; ++index) {
      points.push_back({{"x", 0.5}, {"y", 0.5}, {"label", "add"}});
    }
    return points;
  };
  // The stroke group has one point in its second step; 63 more points give the limit of 64.
  auto at_limit                                                    = valid;
  at_limit["source"]["components"][2]["prompt_steps"][0]["points"] = point_list(63);
  EXPECT_NO_THROW((void)MaskModelFromJson(at_limit));

  struct Case {
    const char*                          name;
    std::function<void(nlohmann::json&)> change;
  };
  const std::vector<Case> cases = {
      {"key with 31 digits",
       [](nlohmann::json& source) { source["asset_key"] = std::string(31, 'a'); }},
      {"key with upper case digits",
       [](nlohmann::json& source) { source["asset_key"] = std::string(32, 'A'); }},
      {"asset extent 4097", [](nlohmann::json& source) { source["asset_width"] = 4097; }},
      {"analysis extent 4097", [](nlohmann::json& source) { source["analysis_height"] = 4097; }},
      {"asset larger than the analysis image",
       [](nlohmann::json& source) { source["analysis_width"] = 512; }},
      {"bounds past the right edge",
       [](nlohmann::json& source) { source["reference_bounds"]["x"] = 0.9; }},
      {"negative bounds", [](nlohmann::json& source) { source["reference_bounds"]["y"] = -0.1; }},
      {"empty bounds", [](nlohmann::json& source) { source["reference_bounds"]["h"] = 0.0; }},
      {"65 points in a group",
       [&point_list](nlohmann::json& source) {
         source["components"][2]["prompt_steps"][0]["points"] = point_list(64);
       }},
      {"level 1 with a box",
       [](nlohmann::json& source) { source["components"][2]["prompt_steps"][1]["level"] = 1; }},
      {"level 3",
       [](nlohmann::json& source) { source["components"][1]["prompt_steps"][0]["level"] = 3; }},
      {"step without points and box",
       [](nlohmann::json& source) {
         source["components"][1]["prompt_steps"][0]["points"] = nlohmann::json::array();
       }},
      {"point outside the image",
       [](nlohmann::json& source) {
         source["components"][1]["prompt_steps"][0]["points"][0]["x"] = 1.25;
       }},
      {"toggles on sky", [](nlohmann::json& source) { source["components"][0]["task"] = "sky"; }},
      {"unknown task", [](nlohmann::json& source) { source["components"][0]["task"] = "cat"; }},
      {"task and prompt steps",
       [](nlohmann::json& source) { source["components"][1]["task"] = "subject"; }},
      {"unknown operation",
       [](nlohmann::json& source) { source["components"][0]["operation"] = "multiply"; }},
      {"feather_px 51", [](nlohmann::json& source) { source["feather_px"] = 51.0; }},
      {"expand_px 1.5", [](nlohmann::json& source) { source["expand_px"] = 1.5; }},
      {"expand_px -51", [](nlohmann::json& source) { source["expand_px"] = -51; }},
      {"empty model set id", [](nlohmann::json& source) { source["model_set_id"] = ""; }},
      {"fingerprint with 33 digits",
       [](nlohmann::json& source) { source["geometry_fingerprint"] = std::string(33, 'b'); }},
      {"empty components",
       [](nlohmann::json& source) { source["components"] = nlohmann::json::array(); }},
      {"format version 2", [](nlohmann::json& source) { source["format_version"] = 2; }},
      {"unknown source field", [](nlohmann::json& source) { source["brush_strokes"] = 1; }},
      {"unknown component field",
       [](nlohmann::json& source) { source["components"][0]["weight"] = 1; }},
      {"unknown point field",
       [](nlohmann::json& source) {
         source["components"][1]["prompt_steps"][0]["points"][0]["z"] = 0.0;
       }},
      {"missing recipe", [](nlohmann::json& source) { source.erase("components"); }},
  };
  for (const auto& test_case : cases) {
    auto json = valid;
    test_case.change(json["source"]);
    EXPECT_THROW((void)MaskModelFromJson(json), std::runtime_error) << test_case.name;
  }
}

TEST(GpuDagModelGraph, RangeJsonReadsNullLegacyPlaceholderAndRangeObjects) {
  EXPECT_FALSE(ColorRangeFromJson(nullptr).has_value());
  EXPECT_FALSE(LuminanceRangeFromJson(nullptr).has_value());
  // The placeholder that builds before the range fields could hold reads as the full range.
  EXPECT_FALSE(ColorRangeFromJson({{"enabled", false}}).has_value());
  EXPECT_FALSE(LuminanceRangeFromJson({{"enabled", false}}).has_value());
  EXPECT_THROW((void)ColorRangeFromJson({{"enabled", true}}), std::runtime_error);
  EXPECT_THROW((void)LuminanceRangeFromJson({{"enabled", true}}), std::runtime_error);
  EXPECT_THROW((void)ColorRangeFromJson({{"enabled", false}, {"hue_start", 10.0}}),
               std::runtime_error);
  EXPECT_THROW((void)LuminanceRangeFromJson({{"enabled", 0}}), std::runtime_error);

  const auto wrap =
      ColorRangeFromJson({{"hue_start", 300.0}, {"hue_end", 40.0}, {"softness", 8.0}});
  ASSERT_TRUE(wrap.has_value());
  EXPECT_EQ(*wrap, (ColorRangeModel{300.0f, 40.0f, 8.0f}));
  EXPECT_FLOAT_EQ(ColorRangeWidthDegrees(*wrap), 100.0f);
  const auto luma = LuminanceRangeFromJson({{"low", 0.0}, {"high", 0.5}, {"softness", 0.1}});
  ASSERT_TRUE(luma.has_value());
  EXPECT_EQ(*luma, (LuminanceRangeModel{0.0f, 0.5f, 0.1f}));

  // A valid object that covers the full range reads as empty, so it serializes as null.
  EXPECT_FALSE(
      ColorRangeFromJson({{"hue_start", 0.0}, {"hue_end", 360.0}, {"softness", 12.0}}).has_value());
  EXPECT_FALSE(
      LuminanceRangeFromJson({{"low", 0.0}, {"high", 1.0}, {"softness", 0.05}}).has_value());
  EXPECT_THROW((void)LuminanceRangeFromJson({{"low", 0.0}, {"high", 1.0}, {"softness", 0.0}}),
               std::runtime_error);

  EXPECT_THROW((void)ColorRangeFromJson({{"hue_start", 10.0}, {"hue_end", 40.0}}),
               std::runtime_error);
  EXPECT_THROW((void)ColorRangeFromJson(
                   {{"hue_start", 10.0}, {"hue_end", 40.0}, {"softness", 8.0}, {"chroma", 0.1}}),
               std::runtime_error);
  EXPECT_THROW((void)LuminanceRangeFromJson({{"low", "0.1"}, {"high", 0.5}, {"softness", 0.1}}),
               std::runtime_error);
  EXPECT_THROW((void)LuminanceRangeFromJson(nlohmann::json::array()), std::runtime_error);
}

TEST(GpuDagModelGraph, RangeSettersStoreRangeAndTakeNewContentRevision) {
  auto         document = CreateDefaultPipelineDocument();
  auto*        grade    = document.PrimaryGrade();
  const MaskId mask_id{"mask.range"};
  grade->AddMask(grade_mask_test::MakeRadialMask(mask_id), 0);
  const auto initial = grade->MaskContentRevision(mask_id);

  grade->SetMaskLuminanceRange(mask_id, LuminanceRangeModel{0.1f, 0.6f, 0.05f});
  const auto after_luma = grade->MaskContentRevision(mask_id);
  EXPECT_NE(after_luma, initial);
  grade->SetMaskColorRange(mask_id, ColorRangeModel{200.0f, 260.0f, 12.0f});
  const auto after_hue = grade->MaskContentRevision(mask_id);
  EXPECT_NE(after_hue, after_luma);
  // Writing the stored value again is not a content change.
  grade->SetMaskColorRange(mask_id, ColorRangeModel{200.0f, 260.0f, 12.0f});
  EXPECT_EQ(grade->MaskContentRevision(mask_id), after_hue);
  EXPECT_EQ(grade->FindMask(mask_id)->luminance_range, (LuminanceRangeModel{0.1f, 0.6f, 0.05f}));
  EXPECT_EQ(grade->FindMask(mask_id)->color_range, (ColorRangeModel{200.0f, 260.0f, 12.0f}));

  grade->SetMaskLuminanceRange(mask_id, std::nullopt);
  grade->SetMaskColorRange(mask_id, std::nullopt);
  EXPECT_EQ(*grade->FindMask(mask_id), grade_mask_test::MakeRadialMask(mask_id));
  EXPECT_NE(grade->MaskContentRevision(mask_id), after_hue);
}

TEST(GpuDagModelGraph, ExistingMaskJsonBytesAreUnchanged) {
  // The serialized bytes of existing sources set the root id and the history byte comparison of
  // stored projects; adding a source kind must not change them.
  RadialMaskSource radial;
  radial.center_x          = 0.25f;
  radial.center_y          = 0.75f;
  radial.major_radius      = 0.5f;
  radial.minor_radius      = 0.125f;
  radial.inner_feather     = 0.0625f;
  radial.outer_feather     = 0.5f;
  auto radial_mask         = grade_mask_test::MakeRadialMask(MaskId{"mask.radial"}, radial);
  radial_mask.display_name = "Radial";
  radial_mask.opacity      = 0.75f;
  EXPECT_EQ(MaskModelToJson(radial_mask).dump(),
            R"({"color_range":null,"deletion_protected":false,"display_name":"Radial",)"
            R"("enabled":true,"id":"mask.radial","invert":false,"luminance_range":null,)"
            R"("opacity":0.75,"source":{"center_x":0.25,"center_y":0.75,"inner_feather":0.0625,)"
            R"("kind":"radial","major_radius":0.5,"minor_radius":0.125,"outer_feather":0.5,)"
            R"("rotation":0.0}})");

  auto linear_mask = grade_mask_test::MakeLinearGradientMask(MaskId{"mask.linear"}, {}, true);
  linear_mask.display_name = "Gradient";
  EXPECT_EQ(MaskModelToJson(linear_mask).dump(),
            R"({"color_range":null,"deletion_protected":false,"display_name":"Gradient",)"
            R"("enabled":true,"id":"mask.linear","invert":true,"luminance_range":null,)"
            R"("opacity":1.0,"source":{"end_value":0.0,"kind":"linear_gradient","normal_x":0.0,)"
            R"("normal_y":1.0,"origin_x":0.5,"origin_y":0.5,"start_value":1.0,)"
            R"("transition_distance":0.20000000298023224}})");
}

}  // namespace alcedo
