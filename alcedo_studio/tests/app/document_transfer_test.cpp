//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <variant>

#include "app/document_transfer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

auto CollectIds(const nlohmann::json& grade) -> std::set<std::string> {
  std::set<std::string> ids;
  ids.insert(grade.at("id").get<std::string>());
  for (const auto& adjustment : grade.at("adjustments")) {
    ids.insert(adjustment.at("id").get<std::string>());
  }
  for (const auto& mask : grade.at("masks")) {
    ids.insert(mask.at("id").get<std::string>());
    if (!mask.contains("source") || !mask.at("source").is_object() ||
        !mask.at("source").contains("strokes") || !mask.at("source").at("strokes").is_array()) {
      continue;
    }
    for (const auto& stroke : mask.at("source").at("strokes")) {
      if (stroke.is_object() && stroke.contains("id") && stroke.at("id").is_string()) {
        ids.insert(stroke.at("id").get<std::string>());
      }
    }
  }
  return ids;
}

TEST(DocumentTransferTest, CaptureOmitsDevelopGeometryAndHistory) {
  auto document = test::DocumentWithExposureEv(2.25);
  document.Geometry().SetRotationDegrees(15.0f);
  const auto package = CaptureDocumentTransfer(document);
  const auto json    = ExportDocumentTransfer(package);
  EXPECT_FALSE(json.contains("operators"));
  EXPECT_FALSE(json.contains("develop"));
  EXPECT_FALSE(json.contains("geometry"));
  EXPECT_FALSE(json.contains("root_id"));
  EXPECT_FALSE(json.contains("version"));
  EXPECT_EQ(json.at("schema").get<std::string>(), std::string{kAdjustmentTransferSchema});
  EXPECT_EQ(json.at("document_format_version").get<std::uint32_t>(),
            kPipelineDocumentFormatVersion);
  ASSERT_FALSE(package.color_grades_.empty());
  EXPECT_EQ(package.color_grades_.front().at("id").get<std::string>(), "grade.primary");
}

TEST(DocumentTransferTest, ImportRejectsOperatorListPackages) {
  const nlohmann::json json = {{"operators", nlohmann::json::array({nlohmann::json::object()})},
                               {"schema", std::string{kAdjustmentTransferSchema}}};
  EXPECT_THROW((void)ImportDocumentTransfer(json), std::runtime_error);
}

TEST(DocumentTransferTest, ExportImportRoundTripPreservesFingerprint) {
  const auto original = CaptureDocumentTransfer(test::DocumentWithExposureEv(0.25));
  const auto exported = ExportDocumentTransfer(original);
  const auto imported = ImportDocumentTransfer(exported);
  EXPECT_EQ(imported.fingerprint_, original.fingerprint_);
  EXPECT_EQ(ExportDocumentTransfer(imported).dump(), exported.dump());
}

TEST(DocumentTransferTest, PasteKeepsTargetDevelopRawDataAndGeometry) {
  auto target = CreateDefaultPipelineDocument();
  target.Geometry().SetRotationDegrees(27.0f);
  target.Geometry().SetExpandToFit(true);
  auto develop_payload = target.Develop()->Params().Params();
  develop_payload.highlights_reconstruct = false;
  develop_payload.demosaic_method        = "AMaZE";
  target.Develop()->Params().ReplaceParams(develop_payload);
  const auto develop_before  = target.Develop()->Params().ToJson();
  const auto geometry_before = target.Geometry().ToJson();

  auto source = test::DocumentWithExposureEv(-0.5);
  source.Geometry().SetRotationDegrees(90.0f);
  auto source_develop = source.Develop()->Params().Params();
  source_develop.highlights_reconstruct = true;
  source_develop.demosaic_method        = "default";
  source.Develop()->Params().ReplaceParams(source_develop);
  const auto package = CaptureDocumentTransfer(source);
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  const auto prepared     = PrepareDocumentPaste(package, target, options);
  auto       working      = ClonePipelineDocument(target);
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward,
                                     &error))
      << error;
  EXPECT_EQ(working.Develop()->Params().ToJson().dump(), develop_before.dump());
  EXPECT_EQ(working.Geometry().ToJson().dump(), geometry_before.dump());
  nlohmann::json exposure;
  ASSERT_TRUE(ReadEditorParameterJson(working, test::ColorGradeFieldTarget("exposure", "grade.t1"),
                                      &exposure, &error))
      << error;
  EXPECT_DOUBLE_EQ(exposure.at("exposure_ev").get<double>(), -0.5);
}

TEST(DocumentTransferTest, PasteRemapsEveryNodeAdjustmentAndMaskId) {
  auto source = test::DocumentWithExposureEv(0.75);
  const auto first = grade_mask_test::MakePaintStroke("stroke.source", 8.0f, 12.0f, 4.0f);
  auto second      = grade_mask_test::MakePaintStroke("stroke.erase", 9.0f, 10.0f, 3.0f);
  second.mode      = BrushStrokeMode::Erase;
  grade_mask_test::AddParameterizedBrushMask(source, MaskId{"mask.brush"}, {first, second},
                                             Vector2{1.5f, -0.25f});
  const auto package = CaptureDocumentTransfer(source);
  EXPECT_TRUE(package.mask_assets_.empty());
  std::set<std::string> source_ids;
  for (const auto& grade : package.color_grades_) {
    const auto ids = CollectIds(grade);
    source_ids.insert(ids.begin(), ids.end());
  }
  EXPECT_EQ(source_ids.count("stroke.source"), 1u);
  EXPECT_EQ(source_ids.count("stroke.erase"), 1u);

  auto target = CreateDefaultPipelineDocument();
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  const auto prepared     = PrepareDocumentPaste(package, target, options);
  ASSERT_FALSE(prepared.package.color_grades_.empty());
  std::set<std::string> imported;
  for (const auto& grade : prepared.package.color_grades_) {
    const auto ids = CollectIds(grade);
    imported.insert(ids.begin(), ids.end());
  }
  for (const auto& id : imported) {
    EXPECT_EQ(source_ids.count(id), 0u) << id;
    EXPECT_EQ(id.find("grade.primary"), std::string::npos) << id;
  }
  EXPECT_EQ(prepared.package.color_grades_.front().at("id").get<std::string>(), "grade.t1");
  EXPECT_EQ(prepared.package.color_grades_.front().at("masks").front().at("id").get<std::string>(),
            "mask.t1");
  const auto& remapped_strokes =
      prepared.package.color_grades_.front().at("masks").front().at("source").at("strokes");
  ASSERT_EQ(remapped_strokes.size(), 2u);
  EXPECT_EQ(remapped_strokes.at(0).at("id").get<std::string>(), "stroke.t1");
  EXPECT_EQ(remapped_strokes.at(1).at("id").get<std::string>(), "stroke.t2");
  EXPECT_EQ(remapped_strokes.at(0).at("samples").at(0).at("local_x").get<float>(), 8.0f);
  EXPECT_EQ(remapped_strokes.at(1).at("mode").get<int>(), 1);
  EXPECT_TRUE(prepared.package.mask_assets_.empty());

  auto        working = ClonePipelineDocument(target);
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward,
                                     &error))
      << error;
  const auto* pasted = working.Graph().FindNode(NodeId{"grade.t1"});
  ASSERT_NE(pasted, nullptr);
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(pasted);
  ASSERT_NE(grade, nullptr);
  const auto* mask = grade->FindMask(MaskId{"mask.t1"});
  ASSERT_NE(mask, nullptr);
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  ASSERT_NE(brush, nullptr);
  ASSERT_EQ(brush->strokes.size(), 2u);
  EXPECT_EQ(brush->strokes[0].id, StrokeId{"stroke.t1"});
  EXPECT_EQ(brush->strokes[1].id, StrokeId{"stroke.t2"});
  EXPECT_EQ(BrushStrokeSamples(brush->strokes[0])[0].local_x, 8.0f);
  EXPECT_EQ(brush->placement_translation, (Vector2{1.5f, -0.25f}));
  EXPECT_FALSE(brush->asset_key.has_value());
}

TEST(DocumentTransferTest, IdentityCollisionIsRejectedBeforeDocumentMutation) {
  auto source = test::DocumentWithExposureEv(1.0);
  const auto package = CaptureDocumentTransfer(source);
  auto       target  = CreateDefaultPipelineDocument();
  const auto before  = CanonicalPipelineDocumentJson(target);
  class CollidingIdentity final : public TransferIdentitySource {
   public:
    auto NextNodeId() -> NodeId override { return NodeId{"grade.primary"}; }
    auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
        -> AdjustmentInstanceId override {
      return MakeAdjustmentInstanceId(node_id, type);
    }
    auto NextMaskId() -> MaskId override { return MaskId{"mask.t1"}; }
    auto NextStrokeId() -> StrokeId override { return StrokeId{"stroke.t1"}; }
  } colliding;
  DocumentTransferPasteOptions options;
  options.identity_source = &colliding;
  EXPECT_THROW((void)PrepareDocumentPaste(package, target, options), std::runtime_error);
  EXPECT_EQ(CanonicalPipelineDocumentJson(target), before);
}

}  // namespace
}  // namespace alcedo
