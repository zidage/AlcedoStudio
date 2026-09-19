//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/document_transfer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <variant>

#include "app/adjustment_transfer_package_builder.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/history/pipeline_history_format.hpp"
#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_asset.hpp"
#endif
#include "edit/mask/mask_id.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_type_id.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"

namespace alcedo {
namespace {

auto CollectIds(const TransferColorGradeValue& grade) -> std::set<std::string> {
  std::set<std::string> ids;
  ids.insert(std::string{grade.source_node_id.Value()});
  for (const auto& adjustment : grade.adjustments) {
    ids.insert(std::string{adjustment.source_id.Value()});
  }
  if (grade.masks.has_value()) {
    for (const auto& mask : *grade.masks) {
      ids.insert(std::string{mask.id.Value()});
#ifdef ALCEDO_ENABLE_BRUSH_MASK
      if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
        for (const auto& stroke : brush->strokes) {
          ids.insert(std::string{stroke.id.Value()});
        }
      }
#endif
    }
  }
  return ids;
}

auto JsonKeys(const nlohmann::json& json, std::set<std::string>* keys) -> void {
  if (json.is_object()) {
    for (const auto& [key, value] : json.items()) {
      keys->insert(key);
      JsonKeys(value, keys);
    }
    return;
  }
  if (json.is_array()) {
    for (const auto& item : json) {
      JsonKeys(item, keys);
    }
  }
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
  EXPECT_EQ(package.color_grades_.front().source_node_id, NodeId{"grade.primary"});
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
  EXPECT_TRUE(imported.color_grades_.front().deletion_protected);
  for (const bool mask_field : {false, true}) {
    auto  missing = encoded;
    auto& owner   = mask_field ? missing.at("color_grades").at(0).at("masks").at(0)
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
    auto malformed                = encoded;
    malformed["default_grade_id"] = invalid;
    EXPECT_THROW((void)ImportDocumentTransfer(malformed), std::runtime_error);
  }
}

TEST(DocumentTransferTest, PasteKeepsTargetDevelopRawLensAndGeometry) {
  auto target = CreateDefaultPipelineDocument();
  target.Geometry().SetRotationDegrees(27.0f);
  target.Geometry().SetExpandToFit(true);
  auto develop_payload                                 = target.Develop()->Params().Params();
  develop_payload.highlights_reconstruct               = false;
  develop_payload.demosaic_method                      = "AMaZE";
  develop_payload.lens_enabled                         = true;
  develop_payload.apply_tca                            = false;
  develop_payload.use_camera_wb                        = false;
  develop_payload.camera_profile.as_shot_neutral_valid = true;
  develop_payload.camera_profile.as_shot_neutral       = {0.5, 1.0, 0.75};
  develop_payload.camera_profile.cam_mul               = {0.9f, 1.0f, 1.1f};
  target.Develop()->Params().ReplaceParams(develop_payload);
  const auto develop_before  = target.Develop()->Params().ToJson();
  const auto geometry_before = target.Geometry().ToJson();

  auto       source          = test::DocumentWithExposureEv(-0.5);
  source.Geometry().SetRotationDegrees(90.0f);
  auto source_develop                   = source.Develop()->Params().Params();
  source_develop.highlights_reconstruct = true;
  source_develop.demosaic_method        = "default";
  source.Develop()->Params().ReplaceParams(source_develop);
  const auto                     package = CaptureDocumentTransfer(source);
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  const auto  prepared    = DocumentTransferPlanner::Plan(package, target, options);
  auto        working     = ClonePipelineDocument(target);
  std::string error;
  ASSERT_TRUE(
      ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward, &error))
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
  auto             source = test::DocumentWithExposureEv(0.75);
  RadialMaskSource radial;
  radial.major_radius = 0.3f;
  radial.minor_radius = 0.2f;
  grade_mask_test::AddRadialMask(source, MaskId{"mask.radial"}, radial);
  const auto            package = CaptureDocumentTransfer(source);
  std::set<std::string> source_ids;
  for (const auto& grade : package.color_grades_) {
    const auto ids = CollectIds(grade);
    source_ids.insert(ids.begin(), ids.end());
  }
  EXPECT_EQ(source_ids.count("mask.radial"), 1u);

  auto                           target = CreateDefaultPipelineDocument();
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  const auto prepared     = DocumentTransferPlanner::Plan(package, target, options);
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
  EXPECT_EQ(prepared.package.color_grades_.front().source_node_id, NodeId{"grade.t1"});
  ASSERT_TRUE(prepared.package.color_grades_.front().masks.has_value());
  ASSERT_FALSE(prepared.package.color_grades_.front().masks->empty());
  EXPECT_EQ(prepared.package.color_grades_.front().masks->front().id, MaskId{"mask.t1"});
  EXPECT_NE(
      std::get_if<RadialMaskSource>(&prepared.package.color_grades_.front().masks->front().source),
      nullptr);

  auto        working = ClonePipelineDocument(target);
  std::string error;
  ASSERT_TRUE(
      ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward, &error))
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
  auto                    source  = test::DocumentWithExposureEv(1.0);
  const auto              package = CaptureDocumentTransfer(source);
  auto                    target  = CreateDefaultPipelineDocument();
  const auto              before  = CanonicalPipelineDocumentJson(target);
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
  EXPECT_THROW((void)DocumentTransferPlanner::Plan(package, target, options), std::runtime_error);
  EXPECT_EQ(CanonicalPipelineDocumentJson(target), before);
}

TEST(DocumentTransferTest, SelectedAdjustmentsAreTheOnlyValuesInTransferPackage) {
  auto        document = test::DocumentWithExposureEv(1.75);
  const auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {grade->Id(), {{AdjustmentTransferItemKind::Adjustment, *exposure_id}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(document, selection);

  ASSERT_EQ(package.color_grades_.size(), 1u);
  const auto& entry = package.color_grades_.front();
  EXPECT_EQ(entry.source_node_id, grade->Id());
  EXPECT_FALSE(entry.enabled.has_value());
  EXPECT_FALSE(entry.mix.has_value());
  EXPECT_FALSE(entry.masks.has_value());
  ASSERT_EQ(entry.adjustments.size(), 1u);
  EXPECT_EQ(entry.adjustments.front().source_id, *exposure_id);
  EXPECT_EQ(entry.adjustments.front().type, type_ids::Exposure());
  EXPECT_DOUBLE_EQ(entry.adjustments.front().params.at("exposure_ev").get<double>(), 1.75);
  EXPECT_TRUE(package.drt_post_.Empty());

  const auto encoded_grade = ExportDocumentTransfer(package).at("color_grades").at(0);
  EXPECT_FALSE(encoded_grade.contains("enabled"));
  EXPECT_FALSE(encoded_grade.contains("mix"));
  EXPECT_FALSE(encoded_grade.contains("masks"));
  EXPECT_EQ(encoded_grade.at("adjustments").size(), 1u);
}

TEST(DocumentTransferTest, ColorGradeSelectionKeepsSourceBackboneOrder) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.c"}).empty());
  ASSERT_EQ(ColorGradesOnImageBackbone(document).size(), 3u);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {NodeId{"grade.c"}, {{AdjustmentTransferItemKind::NodeEnabled, std::nullopt}}});
  selection.nodes.push_back(
      {NodeId{"grade.primary"}, {{AdjustmentTransferItemKind::NodeEnabled, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(document, selection);

  ASSERT_EQ(package.color_grades_.size(), 2u);
  EXPECT_EQ(package.color_grades_.at(0).source_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(package.color_grades_.at(1).source_node_id, NodeId{"grade.c"});
}

TEST(DocumentTransferTest, MasksSelectionCopiesEveryOwnedMaskOrNoMask) {
  auto document = test::DocumentWithExposureEv(0.5);
  grade_mask_test::AddRadialMask(document, MaskId{"mask.first"});
  grade_mask_test::AddLinearGradientMask(document, MaskId{"mask.second"});
  const auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  ASSERT_EQ(grade->MaskCount(), 2u);

  AdjustmentTransferSelection with_masks;
  with_masks.nodes.push_back({grade->Id(), {{AdjustmentTransferItemKind::Masks, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(document, with_masks);
  ASSERT_EQ(package.color_grades_.size(), 1u);
  ASSERT_TRUE(package.color_grades_.front().masks.has_value());
  ASSERT_EQ(package.color_grades_.front().masks->size(), 2u);
  EXPECT_EQ(package.color_grades_.front().masks->at(0).id, MaskId{"mask.first"});
  EXPECT_EQ(package.color_grades_.front().masks->at(1).id, MaskId{"mask.second"});

  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);
  AdjustmentTransferSelection without_masks;
  without_masks.nodes.push_back(
      {grade->Id(), {{AdjustmentTransferItemKind::Adjustment, *exposure_id}}});
  const auto sparse = AdjustmentTransferPackageBuilder::Build(document, without_masks);
  ASSERT_EQ(sparse.color_grades_.size(), 1u);
  EXPECT_FALSE(sparse.color_grades_.front().masks.has_value());

  auto clean_document = CreateDefaultPipelineDocument();
  ASSERT_EQ(clean_document.PrimaryGrade()->MaskCount(), 0u);
  AdjustmentTransferSelection empty_masks;
  empty_masks.nodes.push_back(
      {clean_document.PrimaryGrade()->Id(), {{AdjustmentTransferItemKind::Masks, std::nullopt}}});
  EXPECT_THROW((void)AdjustmentTransferPackageBuilder::Build(clean_document, empty_masks),
               std::runtime_error);
}

TEST(DocumentTransferTest, DrtOnlySelectionCreatesNonEmptyTransferPackage) {
  auto        document = CreateDefaultPipelineDocument();
  const auto* drt      = document.Drt();
  ASSERT_NE(drt, nullptr);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {drt->Id(), {{AdjustmentTransferItemKind::DrtParameters, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(document, selection);
  EXPECT_FALSE(package.Empty());
  EXPECT_TRUE(package.color_grades_.empty());
  EXPECT_TRUE(package.default_grade_id_.Empty());
  ASSERT_TRUE(package.drt_post_.params.has_value());
  EXPECT_TRUE(package.drt_post_.adjustments.empty());

  const auto round_tripped = ImportDocumentTransfer(ExportDocumentTransfer(package));
  EXPECT_EQ(round_tripped.fingerprint_, package.fingerprint_);
}

TEST(DocumentTransferTest, TransferPackageRejectsItemOwnedByAnotherNode) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  const auto backbone = ColorGradesOnImageBackbone(document);
  ASSERT_EQ(backbone.size(), 2u);
  const auto* other_exposure = backbone.at(1)->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(other_exposure, nullptr);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {NodeId{"grade.primary"}, {{AdjustmentTransferItemKind::Adjustment, *other_exposure}}});
  EXPECT_THROW((void)AdjustmentTransferPackageBuilder::Build(document, selection),
               std::runtime_error);
}

TEST(DocumentTransferTest, TransferPackageV6FingerprintCoversFieldPresence) {
  auto        document = CreateDefaultPipelineDocument();
  const auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);

  AdjustmentTransferSelection sparse;
  sparse.nodes.push_back({grade->Id(), {{AdjustmentTransferItemKind::Adjustment, *exposure_id}}});
  AdjustmentTransferSelection with_enabled;
  with_enabled.nodes.push_back({grade->Id(),
                                {{AdjustmentTransferItemKind::Adjustment, *exposure_id},
                                 {AdjustmentTransferItemKind::NodeEnabled, std::nullopt}}});
  const auto sparse_package  = AdjustmentTransferPackageBuilder::Build(document, sparse);
  const auto enabled_package = AdjustmentTransferPackageBuilder::Build(document, with_enabled);
  ASSERT_FALSE(sparse_package.color_grades_.front().enabled.has_value());
  ASSERT_TRUE(enabled_package.color_grades_.front().enabled.has_value());
  EXPECT_EQ(*enabled_package.color_grades_.front().enabled, grade->Enabled());
  EXPECT_NE(sparse_package.fingerprint_, enabled_package.fingerprint_);

  const auto sparse_json  = ExportDocumentTransfer(sparse_package).at("color_grades").at(0);
  const auto enabled_json = ExportDocumentTransfer(enabled_package).at("color_grades").at(0);
  EXPECT_FALSE(sparse_json.contains("enabled"));
  ASSERT_TRUE(enabled_json.contains("enabled"));
  EXPECT_EQ(enabled_json.at("enabled").get<bool>(), grade->Enabled());
}

TEST(DocumentTransferTest, TransferPackageV5IsRejectedWithoutConversion) {
  const auto package = CaptureDocumentTransfer(test::DocumentWithExposureEv(0.5));
  auto       json    = ExportDocumentTransfer(package);
  json["schema"]     = "alcedo.adjustment_transfer.v5";
  try {
    (void)ImportDocumentTransfer(json);
    FAIL() << "v5 package was accepted";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string{e.what()}.find("schema"), std::string::npos) << e.what();
  }
}

TEST(DocumentTransferTest, TransferPackageOmitsDevelopRawLensGeometryAndCaches) {
  auto document = test::DocumentWithLutPath("D:/cache/luts/example.cube");
  document.Geometry().SetRotationDegrees(33.0f);
  grade_mask_test::AddRadialMask(document, MaskId{"mask.full"});
  const auto            json = ExportDocumentTransfer(CaptureDocumentTransfer(document));

  std::set<std::string> actual_top;
  for (const auto& [key, value] : json.items()) {
    (void)value;
    actual_top.insert(key);
  }
  EXPECT_EQ(actual_top,
            (std::set<std::string>{"color_grades", "default_grade_id", "document_format_version",
                                   "drt_post", "fingerprint", "schema"}));

  std::set<std::string> all_keys;
  JsonKeys(json, &all_keys);
  for (const char* banned : {"develop", "geometry", "raw", "lens", "history", "version", "root_id",
                             "operators", "cache", "ui_state"}) {
    EXPECT_EQ(all_keys.count(banned), 0u) << banned;
  }
}

TEST(DocumentTransferTest, TransferPackageBuildDoesNotMutateSourceDocument) {
  auto       document = test::DocumentWithExposureEv(0.5);
  const auto before   = CanonicalPipelineDocumentJson(document);
  (void)AdjustmentTransferPackageBuilder::Build(document, SelectAllTransferableItems(document));
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);

  AdjustmentTransferSelection bad;
  bad.nodes.push_back(
      {NodeId{"grade.absent"}, {{AdjustmentTransferItemKind::NodeEnabled, std::nullopt}}});
  EXPECT_THROW((void)AdjustmentTransferPackageBuilder::Build(document, bad), std::runtime_error);
  EXPECT_EQ(CanonicalPipelineDocumentJson(document), before);
}

// ============================================================================
// NM10.2 selective paste planning
// ============================================================================

auto PlanSelectivePaste(const AdjustmentTransferPackage& package, const PipelineDocument& target)
    -> PreparedDocumentPaste {
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  return DocumentTransferPlanner::Plan(package, target, options);
}

auto ApplyPasteToClone(const PreparedDocumentPaste& prepared, const PipelineDocument& target)
    -> PipelineDocument {
  auto        working = ClonePipelineDocument(target);
  std::string error;
  if (!ApplyPipelineEditBatch(working, prepared.batch, PipelineEditApplyDirection::Forward,
                              &error)) {
    throw std::runtime_error("ApplyPipelineEditBatch failed: " + error);
  }
  return working;
}

TEST(DocumentTransferTest, PartialGradePasteUsesCleanValuesForUnselectedAdjustments) {
  auto  source = test::DocumentWithExposureEv(1.5);
  auto* grade  = source.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  grade->SetEnabled(false);
  grade->SetMix(0.4f);
  auto* contrast = grade->FindAdjustmentByType(type_ids::Contrast());
  ASSERT_NE(contrast, nullptr);
  contrast->LoadJson({{"contrast", 0.9}});
  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {grade->Id(), {{AdjustmentTransferItemKind::Adjustment, *exposure_id}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(source, selection);
  ASSERT_FALSE(package.color_grades_.front().enabled.has_value());
  ASSERT_FALSE(package.color_grades_.front().mix.has_value());
  ASSERT_EQ(package.color_grades_.front().adjustments.size(), 1u);

  const auto  target  = CreateDefaultPipelineDocument();
  const auto  working = ApplyPasteToClone(PlanSelectivePaste(package, target), target);
  const auto* pasted =
      dynamic_cast<const ColorGradeNodeModel*>(working.Graph().FindNode(NodeId{"grade.t1"}));
  ASSERT_NE(pasted, nullptr);
  EXPECT_TRUE(pasted->Enabled());
  EXPECT_FLOAT_EQ(pasted->Mix(), 1.0f);
  const auto clean = ColorGradeNodeModel::MakeClean(NodeId{"grade.clean"});
  EXPECT_EQ(pasted->FindAdjustmentByType(type_ids::Contrast())->ToJson().dump(),
            clean->FindAdjustmentByType(type_ids::Contrast())->ToJson().dump());
  const auto* exposure = pasted->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  EXPECT_DOUBLE_EQ(exposure->ToJson().at("exposure_ev").get<double>(), 1.5);
}

TEST(DocumentTransferTest, SelectedGradeSubsetKeepsSourceOrder) {
  auto source = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(source, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  ASSERT_TRUE(AddCleanColorGrade(source, NodeId{"drt"}, NodeId{"grade.c"}).empty());
  const auto backbone = ColorGradesOnImageBackbone(source);
  ASSERT_EQ(backbone.size(), 3u);
  const auto                  primary_name = backbone.at(0)->DisplayName();
  const auto                  c_name       = backbone.at(2)->DisplayName();

  // Selection order is reversed on purpose; backbone order decides the package order.
  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {NodeId{"grade.c"}, {{AdjustmentTransferItemKind::NodeEnabled, std::nullopt}}});
  selection.nodes.push_back(
      {NodeId{"grade.primary"}, {{AdjustmentTransferItemKind::NodeEnabled, std::nullopt}}});
  const auto package  = AdjustmentTransferPackageBuilder::Build(source, selection);
  const auto prepared = PlanSelectivePaste(package, CreateDefaultPipelineDocument());
  ASSERT_EQ(prepared.package.color_grades_.size(), 2u);
  EXPECT_EQ(prepared.package.color_grades_.at(0).source_node_id, NodeId{"grade.t1"});
  EXPECT_EQ(prepared.package.color_grades_.at(1).source_node_id, NodeId{"grade.t2"});
  EXPECT_EQ(prepared.package.color_grades_.at(0).display_name, primary_name);
  EXPECT_EQ(prepared.package.color_grades_.at(1).display_name, c_name);

  const auto working      = ApplyPasteToClone(prepared, CreateDefaultPipelineDocument());
  const auto pasted_chain = ColorGradesOnImageBackbone(working);
  ASSERT_EQ(pasted_chain.size(), 2u);
  EXPECT_EQ(pasted_chain.at(0)->Id(), NodeId{"grade.t1"});
  EXPECT_EQ(pasted_chain.at(1)->Id(), NodeId{"grade.t2"});
  EXPECT_EQ(pasted_chain.at(0)->DisplayName(), primary_name);
  EXPECT_EQ(pasted_chain.at(1)->DisplayName(), c_name);
}

TEST(DocumentTransferTest, MaskSetPasteRemapsAllMasksAsOneSelection) {
  auto source = test::DocumentWithExposureEv(0.5);
  grade_mask_test::AddRadialMask(source, MaskId{"mask.first"});
  grade_mask_test::AddLinearGradientMask(source, MaskId{"mask.second"});
  const auto* grade = source.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  ASSERT_EQ(grade->MaskCount(), 2u);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back({grade->Id(), {{AdjustmentTransferItemKind::Masks, std::nullopt}}});
  const auto  package = AdjustmentTransferPackageBuilder::Build(source, selection);

  const auto  target  = CreateDefaultPipelineDocument();
  const auto  working = ApplyPasteToClone(PlanSelectivePaste(package, target), target);
  const auto* pasted =
      dynamic_cast<const ColorGradeNodeModel*>(working.Graph().FindNode(NodeId{"grade.t1"}));
  ASSERT_NE(pasted, nullptr);
  ASSERT_EQ(pasted->MaskCount(), 2u);
  EXPECT_EQ(pasted->Masks()[0].id, MaskId{"mask.t1"});
  EXPECT_EQ(pasted->Masks()[1].id, MaskId{"mask.t2"});
}

TEST(DocumentTransferTest, UncheckedMaskSetAddsNoMask) {
  auto source = test::DocumentWithExposureEv(0.5);
  grade_mask_test::AddRadialMask(source, MaskId{"mask.first"});
  grade_mask_test::AddLinearGradientMask(source, MaskId{"mask.second"});
  const auto* grade       = source.PrimaryGrade();
  const auto* exposure_id = grade->FindAdjustmentIdByType(type_ids::Exposure());
  ASSERT_NE(exposure_id, nullptr);

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {grade->Id(), {{AdjustmentTransferItemKind::Adjustment, *exposure_id}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(source, selection);
  ASSERT_FALSE(package.color_grades_.front().masks.has_value());

  const auto  target  = CreateDefaultPipelineDocument();
  const auto  working = ApplyPasteToClone(PlanSelectivePaste(package, target), target);
  const auto* pasted =
      dynamic_cast<const ColorGradeNodeModel*>(working.Graph().FindNode(NodeId{"grade.t1"}));
  ASSERT_NE(pasted, nullptr);
  EXPECT_EQ(pasted->MaskCount(), 0u);
}

TEST(DocumentTransferTest, DrtOnlyPasteKeepsTargetRootGrades) {
  auto source = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(source, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  auto drt_payload                   = source.Drt()->Params().Params();
  drt_payload.peak_luminance         = 250.0f;
  drt_payload.display_grey_luminance = 42.0f;
  source.Drt()->Params().ReplaceParams(drt_payload);
  const auto                  source_drt_json = source.Drt()->Params().ToJson();

  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {source.Drt()->Id(), {{AdjustmentTransferItemKind::DrtParameters, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(source, selection);
  ASSERT_TRUE(package.color_grades_.empty());

  auto target = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(target, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  std::vector<std::string> target_grade_ids;
  std::vector<std::string> target_grade_jsons;
  for (const auto* grade : ColorGradesOnImageBackbone(target)) {
    target_grade_ids.push_back(std::string{grade->Id().Value()});
    target_grade_jsons.push_back(grade->ToJson().dump());
  }
  const auto target_default = target.DefaultGradeId();

  const auto working        = ApplyPasteToClone(PlanSelectivePaste(package, target), target);
  const auto pasted_chain   = ColorGradesOnImageBackbone(working);
  ASSERT_EQ(pasted_chain.size(), target_grade_ids.size());
  for (std::size_t index = 0; index < pasted_chain.size(); ++index) {
    EXPECT_EQ(std::string{pasted_chain.at(index)->Id().Value()}, target_grade_ids.at(index));
    EXPECT_EQ(pasted_chain.at(index)->ToJson().dump(), target_grade_jsons.at(index));
  }
  EXPECT_EQ(working.DefaultGradeId(), target_default);
  EXPECT_EQ(working.Drt()->Params().ToJson().dump(), source_drt_json.dump());
}

TEST(DocumentTransferTest, PartialDrtPasteKeepsUnselectedTargetRootValues) {
  auto source = CreateDefaultPipelineDocument();
  test::PatchDocumentField(&source, test::DrtPostFieldTarget("clarity"),
                           nlohmann::json{{"clarity", 30.0}});
  const auto* drt        = source.Drt();
  const auto* clarity_id = drt->FindAdjustmentIdByType(type_ids::Clarity());
  ASSERT_NE(clarity_id, nullptr);
  const auto source_clarity_json = drt->FindAdjustmentByType(type_ids::Clarity())->ToJson();

  AdjustmentTransferSelection selection;
  selection.nodes.push_back({drt->Id(), {{AdjustmentTransferItemKind::Adjustment, *clarity_id}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(source, selection);
  ASSERT_TRUE(package.color_grades_.empty());
  ASSERT_FALSE(package.drt_post_.params.has_value());
  ASSERT_EQ(package.drt_post_.adjustments.size(), 1u);

  const auto target                 = CreateDefaultPipelineDocument();
  const auto target_drt_params_json = target.Drt()->Params().ToJson();
  const auto target_sharpen_json =
      target.Drt()->FindAdjustmentByType(type_ids::Sharpen())->ToJson();
  const auto target_halation_json =
      target.Drt()->FindAdjustmentByType(type_ids::Halation())->ToJson();
  const auto target_film_grain_json =
      target.Drt()->FindAdjustmentByType(type_ids::FilmGrain())->ToJson();

  const auto  working    = ApplyPasteToClone(PlanSelectivePaste(package, target), target);
  const auto* pasted_drt = working.Drt();
  ASSERT_NE(pasted_drt, nullptr);
  EXPECT_EQ(pasted_drt->Params().ToJson().dump(), target_drt_params_json.dump());
  EXPECT_EQ(pasted_drt->FindAdjustmentByType(type_ids::Clarity())->ToJson().dump(),
            source_clarity_json.dump());
  EXPECT_EQ(pasted_drt->FindAdjustmentByType(type_ids::Sharpen())->ToJson().dump(),
            target_sharpen_json.dump());
  EXPECT_EQ(pasted_drt->FindAdjustmentByType(type_ids::Halation())->ToJson().dump(),
            target_halation_json.dump());
  EXPECT_EQ(pasted_drt->FindAdjustmentByType(type_ids::FilmGrain())->ToJson().dump(),
            target_film_grain_json.dump());
}

TEST(DocumentTransferTest, RemappedSourceDefaultGradeStaysDefault) {
  auto source = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(source, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  const auto package =
      AdjustmentTransferPackageBuilder::Build(source, SelectAllTransferableItems(source));
  ASSERT_EQ(package.default_grade_id_, NodeId{"grade.primary"});

  const auto prepared = PlanSelectivePaste(package, CreateDefaultPipelineDocument());
  EXPECT_EQ(prepared.package.default_grade_id_, NodeId{"grade.t1"});
  ASSERT_EQ(prepared.package.color_grades_.size(), 2u);
  EXPECT_TRUE(prepared.package.color_grades_.at(0).deletion_protected);

  const auto working = ApplyPasteToClone(prepared, CreateDefaultPipelineDocument());
  EXPECT_EQ(working.DefaultGradeId(), NodeId{"grade.t1"});
  const auto* pasted =
      dynamic_cast<const ColorGradeNodeModel*>(working.Graph().FindNode(NodeId{"grade.t1"}));
  ASSERT_NE(pasted, nullptr);
  EXPECT_TRUE(pasted->DeletionProtected());
}

TEST(DocumentTransferTest, FirstIncludedGradeBecomesTargetDefault) {
  auto source = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(source, NodeId{"drt"}, NodeId{"grade.b"}).empty());
  auto* grade_b = dynamic_cast<ColorGradeNodeModel*>(source.Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(grade_b, nullptr);
  ASSERT_FALSE(grade_b->DeletionProtected());
  auto mask = grade_mask_test::MakeRadialMask(MaskId{"mask.b"}, RadialMaskSource{}, false);
  mask.deletion_protected = false;
  grade_b->AddMask(mask, 0);

  // Only the non-default grade.b is selected; the package omits the source default.
  AdjustmentTransferSelection selection;
  selection.nodes.push_back({NodeId{"grade.b"},
                             {{AdjustmentTransferItemKind::NodeEnabled, std::nullopt},
                              {AdjustmentTransferItemKind::Masks, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(source, selection);
  ASSERT_TRUE(package.default_grade_id_.Empty());
  ASSERT_EQ(package.color_grades_.size(), 1u);
  ASSERT_FALSE(package.color_grades_.front().deletion_protected);
  ASSERT_TRUE(package.color_grades_.front().masks.has_value());
  ASSERT_FALSE(package.color_grades_.front().masks->front().deletion_protected);

  const auto prepared = PlanSelectivePaste(package, CreateDefaultPipelineDocument());
  EXPECT_EQ(prepared.package.default_grade_id_, NodeId{"grade.t1"});
  EXPECT_TRUE(prepared.package.color_grades_.front().deletion_protected);
  ASSERT_TRUE(prepared.package.color_grades_.front().masks.has_value());
  EXPECT_TRUE(prepared.package.color_grades_.front().masks->front().deletion_protected);

  const auto working = ApplyPasteToClone(prepared, CreateDefaultPipelineDocument());
  EXPECT_EQ(working.DefaultGradeId(), NodeId{"grade.t1"});
  const auto* pasted =
      dynamic_cast<const ColorGradeNodeModel*>(working.Graph().FindNode(NodeId{"grade.t1"}));
  ASSERT_NE(pasted, nullptr);
  EXPECT_TRUE(pasted->DeletionProtected());
  const auto* pasted_mask = pasted->FindMask(MaskId{"mask.t1"});
  ASSERT_NE(pasted_mask, nullptr);
  EXPECT_TRUE(pasted_mask->deletion_protected);
}

TEST(DocumentTransferTest, PlannerDoesNotMutatePackageOrTargetRoot) {
  auto source = test::DocumentWithExposureEv(0.75);
  grade_mask_test::AddRadialMask(source, MaskId{"mask.first"});
  const auto package        = CaptureDocumentTransfer(source);
  const auto package_before = ExportDocumentTransfer(package).dump();
  const auto target         = CreateDefaultPipelineDocument();
  const auto target_before  = CanonicalPipelineDocumentJson(target);

  const auto prepared       = PlanSelectivePaste(package, target);
  EXPECT_NO_THROW((void)prepared.batch.CanonicalJSON());
  EXPECT_EQ(ExportDocumentTransfer(package).dump(), package_before);
  EXPECT_EQ(CanonicalPipelineDocumentJson(target), target_before);
}

TEST(DocumentTransferTest, DrtOnlyPackageIdenticalToTargetFailsWithoutChanges) {
  const auto                  document = CreateDefaultPipelineDocument();
  AdjustmentTransferSelection selection;
  selection.nodes.push_back(
      {document.Drt()->Id(), {{AdjustmentTransferItemKind::DrtParameters, std::nullopt}}});
  const auto package = AdjustmentTransferPackageBuilder::Build(document, selection);
  const auto target  = CreateDefaultPipelineDocument();
  const auto before  = CanonicalPipelineDocumentJson(target);
  EXPECT_THROW((void)PlanSelectivePaste(package, target), std::runtime_error);
  EXPECT_EQ(CanonicalPipelineDocumentJson(target), before);
}

}  // namespace
}  // namespace alcedo
