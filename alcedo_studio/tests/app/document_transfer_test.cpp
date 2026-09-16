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
#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_asset.hpp"
#endif
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
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

TEST(DocumentTransferTest, ImportRejectsMissingOrInvalidProtectionAndDefaultIdentity) {
  auto document = test::DocumentWithExposureEv(0.25);
  grade_mask_test::AddRadialMask(document, MaskId{"mask.transfer"});
  auto encoded = ExportDocumentTransfer(CaptureDocumentTransfer(document));
  encoded.erase("fingerprint");
  const auto imported = ImportDocumentTransfer(encoded);
  EXPECT_EQ(imported.default_grade_id_, document.DefaultGradeId());
  EXPECT_TRUE(imported.color_grades_.front().at("deletion_protected").get<bool>());
  for (const bool mask_field : {false, true}) {
    auto missing = encoded;
    auto& owner = mask_field ? missing.at("color_grades").at(0).at("masks").at(0)
                             : missing.at("color_grades").at(0);
    owner.erase("deletion_protected");
    EXPECT_THROW((void)ImportDocumentTransfer(missing), std::runtime_error);
    owner["deletion_protected"] = "false";
    EXPECT_THROW((void)ImportDocumentTransfer(missing), std::runtime_error);
  }
  auto missing_identity = encoded;
  missing_identity.erase("default_grade_id");
  EXPECT_THROW((void)ImportDocumentTransfer(missing_identity), std::runtime_error);
  for (const auto& invalid : {nlohmann::json{7}, nlohmann::json("grade.absent"),
                              nlohmann::json("develop"), nlohmann::json("")}) {
    auto malformed = encoded;
    malformed["default_grade_id"] = invalid;
    EXPECT_THROW((void)ImportDocumentTransfer(malformed), std::runtime_error);
  }
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
  RadialMaskSource radial;
  radial.major_radius = 0.3f;
  radial.minor_radius = 0.2f;
  grade_mask_test::AddRadialMask(source, MaskId{"mask.radial"}, radial);
  const auto package = CaptureDocumentTransfer(source);
  std::set<std::string> source_ids;
  for (const auto& grade : package.color_grades_) {
    const auto ids = CollectIds(grade);
    source_ids.insert(ids.begin(), ids.end());
  }
  EXPECT_EQ(source_ids.count("mask.radial"), 1u);

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
  EXPECT_EQ(prepared.package.color_grades_.front().at("masks").front().at("source").at("kind"),
            "radial");

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
  const auto* pasted_radial = std::get_if<RadialMaskSource>(&mask->source);
  ASSERT_NE(pasted_radial, nullptr);
  EXPECT_FLOAT_EQ(pasted_radial->major_radius, 0.3f);
  EXPECT_FLOAT_EQ(pasted_radial->minor_radius, 0.2f);
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
#ifdef ALCEDO_ENABLE_BRUSH_MASK
    auto NextStrokeId() -> StrokeId override { return StrokeId{"stroke.t1"}; }
#endif
  } colliding;
  DocumentTransferPasteOptions options;
  options.identity_source = &colliding;
  EXPECT_THROW((void)PrepareDocumentPaste(package, target, options), std::runtime_error);
  EXPECT_EQ(CanonicalPipelineDocumentJson(target), before);
}

}  // namespace
}  // namespace alcedo
