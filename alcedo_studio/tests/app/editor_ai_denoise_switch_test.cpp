//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_ai_denoise_switch_test.cpp
/// @brief The AI Denoise switch as the `ai_denoise` key of the `raw_decode` field (integration
///        plan, DN7): the write, the complete value of a history replay, the owned JSON, the
///        panel projection, and the paste of the RAW Decode item.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <variant>

#include "app/adjustment_transfer_package_builder.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/editor_panel_projection.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

const std::string kVersion(64, 'a');
const std::string kOtherVersion(64, 'b');

auto              RawDecodeTarget(const PipelineDocument& document) -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind = EditorParameterOwnerKind::Develop;
  target.node_id    = document.Develop()->Id();
  target.field_key  = "raw_decode";
  return target;
}

auto VersionJson(const std::string& version) -> nlohmann::json {
  return {{"model_version", version}};
}

TEST(EditorAiDenoiseSwitchTest, RawDecodeWriteTurnsAiDenoiseOnAndOff) {
  auto        document = CreateDefaultPipelineDocument();
  auto&       params   = document.Develop()->Params();
  const auto  target   = RawDecodeTarget(document);
  std::string error;

  ASSERT_TRUE(ApplyEditorParameterPatch(document, target,
                                        {{"raw", {{"ai_denoise", VersionJson(kVersion)}}}}, &error))
      << error;
  ASSERT_TRUE(params.AiDenoise().has_value());
  EXPECT_EQ(params.AiDenoise()->model_version, kVersion);
  // The demosaic method stays: it returns when the switch goes off.
  EXPECT_EQ(params.DemosaicMethod(), "default");

  // A write without the key (the method or the highlight toggle of the panel) keeps the switch.
  auto revision = params.Revision();
  ASSERT_TRUE(ApplyEditorParameterPatch(
      document, target, {{"raw", {{"method", "legacy"}, {"highlights_reconstruct", false}}}},
      &error))
      << error;
  EXPECT_NE(params.Revision(), revision);
  ASSERT_TRUE(params.AiDenoise().has_value());
  EXPECT_EQ(params.DemosaicMethod(), "legacy");

  // The same version again changes nothing.
  revision = params.Revision();
  ASSERT_TRUE(ApplyEditorParameterPatch(document, target,
                                        {{"raw", {{"ai_denoise", VersionJson(kVersion)}}}}, &error))
      << error;
  EXPECT_EQ(params.Revision(), revision);

  // Null turns it off; the stored method is unchanged.
  ASSERT_TRUE(
      ApplyEditorParameterPatch(document, target, {{"raw", {{"ai_denoise", nullptr}}}}, &error))
      << error;
  EXPECT_FALSE(params.AiDenoise().has_value());
  EXPECT_EQ(params.DemosaicMethod(), "legacy");
  EXPECT_FALSE(params.ToJson().contains("ai_denoise"));
}

TEST(EditorAiDenoiseSwitchTest, TypedWriteOfTheSwitchUsesTheRawDecodeUpdate) {
  auto                   document = CreateDefaultPipelineDocument();
  const auto             target   = RawDecodeTarget(document);
  std::string            error;
  DevelopRawDecodeUpdate on;
  on.ai_denoise = DevelopAiDenoise{kVersion};
  ASSERT_TRUE(ApplyEditorParameterWrite(document, target, on, &error)) << error;
  EXPECT_TRUE(document.Develop()->Params().AiDenoise().has_value());

  DevelopRawDecodeUpdate off;
  off.ai_denoise = std::optional<DevelopAiDenoise>{};
  ASSERT_TRUE(ApplyEditorParameterWrite(document, target, off, &error)) << error;
  EXPECT_FALSE(document.Develop()->Params().AiDenoise().has_value());
}

TEST(EditorAiDenoiseSwitchTest, RawDecodeValueWithoutTheKeyTurnsAiDenoiseOff) {
  auto        document = CreateDefaultPipelineDocument();
  auto&       params   = document.Develop()->Params();
  const auto  target   = RawDecodeTarget(document);
  std::string error;

  // A complete value with the key (a history replay of the switch on).
  auto        stored_on   = FieldOwnedParameterJson("raw_decode", params.ToJson());
  stored_on["ai_denoise"] = VersionJson(kVersion);
  ASSERT_TRUE(ApplyEditorParameterValue(document, target, stored_on, &error)) << error;
  ASSERT_TRUE(params.AiDenoise().has_value());

  // A complete value without the key: the stored JSON of the switch off, or of a project before
  // DN7.
  const auto stored_off = FieldOwnedParameterJson(
      "raw_decode", CreateDefaultPipelineDocument().Develop()->Params().ToJson());
  ASSERT_FALSE(stored_off.contains("ai_denoise"));
  ASSERT_TRUE(ApplyEditorParameterValue(document, target, stored_off, &error)) << error;
  EXPECT_FALSE(params.AiDenoise().has_value());

  // An invalid version is rejected; the Model keeps its state.
  ASSERT_TRUE(ApplyEditorParameterValue(document, target, stored_on, &error)) << error;
  const auto revision   = params.Revision();
  auto       invalid    = stored_on;
  invalid["ai_denoise"] = VersionJson("not a version");
  EXPECT_FALSE(ApplyEditorParameterValue(document, target, invalid, &error));
  EXPECT_FALSE(ApplyEditorParameterPatch(document, target, {{"raw", invalid}}, &error));
  EXPECT_EQ(params.Revision(), revision);
  ASSERT_TRUE(params.AiDenoise().has_value());
  EXPECT_EQ(params.AiDenoise()->model_version, kVersion);
}

TEST(EditorAiDenoiseSwitchTest, RawDecodeOwnedJsonHasTheKeyOnlyWhileOn) {
  auto  document = CreateDefaultPipelineDocument();
  auto& params   = document.Develop()->Params();
  EXPECT_FALSE(FieldOwnedParameterJson("raw_decode", params.ToJson()).contains("ai_denoise"));

  params.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersion});
  const auto owned = FieldOwnedParameterJson("raw_decode", params.ToJson());
  ASSERT_TRUE(owned.contains("ai_denoise"));
  EXPECT_EQ(owned.at("ai_denoise"), VersionJson(kVersion));
  // The other Develop fields do not own it.
  for (const char* field : {"color_temp", "lens_calib"}) {
    EXPECT_FALSE(FieldOwnedParameterJson(field, params.ToJson()).contains("ai_denoise")) << field;
  }
}

TEST(EditorAiDenoiseSwitchTest, PanelProjectionReportsTheSwitchAndKeepsTheMethod) {
  auto                   document = CreateDefaultPipelineDocument();
  auto&                  params   = document.Develop()->Params();
  DevelopRawDecodeUpdate method;
  method.demosaic_method = "neural_engine";
  params.ApplyRawDecodeUpdate(method);

  const auto read = [&document] {
    EditorPanelFieldPresentation field;
    std::string                  error;
    EXPECT_TRUE(ReadEditorPanelField(document, RawDecodeTarget(document), &field, &error)) << error;
    return std::get<EditorPanelRawDecodeValue>(field.value);
  };
  EXPECT_FALSE(read().ai_denoise);
  params.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersion});
  const auto on = read();
  EXPECT_TRUE(on.ai_denoise);
  EXPECT_EQ(on.method, "neural_engine");
}

TEST(EditorAiDenoiseSwitchTest, PasteOfRawDecodeCarriesTheSwitch) {
  auto source = CreateDefaultPipelineDocument();
  source.Develop()->Params().ApplyAiDenoiseUpdate(DevelopAiDenoise{kOtherVersion});
  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {source.Develop()->Id(), {{AdjustmentTransferItemKind::RawDecode, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(source, selection);
  ASSERT_TRUE(package.develop_.raw_decode.has_value());
  EXPECT_EQ(package.develop_.raw_decode->at("ai_denoise"), VersionJson(kOtherVersion));

  // The paste turns it on in a target with the switch off.
  auto        target = CreateDefaultPipelineDocument();
  std::string error;
  {
    const auto prepared = DocumentTransferPlanner::Plan(package, target, {});
    auto       working  = ClonePipelineDocument(target);
    ASSERT_TRUE(ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward,
                                       &error))
        << error;
    ASSERT_TRUE(working.Develop()->Params().AiDenoise().has_value());
    EXPECT_EQ(working.Develop()->Params().AiDenoise()->model_version, kOtherVersion);
    // The inverse (undo of the paste) turns it off again.
    ASSERT_TRUE(ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Inverse,
                                       &error))
        << error;
    EXPECT_FALSE(working.Develop()->Params().AiDenoise().has_value());
  }

  // A RAW Decode item without the switch turns it off in a target with the switch on: a paste
  // has no merge semantics.
  const auto                  off_source = CreateDefaultPipelineDocument();
  AdjustmentTransferSelection off_selection;
  off_selection.nodes.push_back(
      {off_source.Develop()->Id(), {{AdjustmentTransferItemKind::RawDecode, std::nullopt}}});
  const auto off_package = AdjustmentTransferPackageBuilder::Build(off_source, off_selection);
  target.Develop()->Params().ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersion});
  const auto prepared = DocumentTransferPlanner::Plan(off_package, target, {});
  auto       working  = ClonePipelineDocument(target);
  ASSERT_TRUE(
      ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_FALSE(working.Develop()->Params().AiDenoise().has_value());
}

}  // namespace
}  // namespace alcedo
