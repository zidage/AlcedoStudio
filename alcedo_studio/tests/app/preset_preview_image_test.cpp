//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file preset_preview_image_test.cpp
/// @brief The preset preview document (edit presets plan 2.4, PS3): the import root with the
///        preset applied by the Paste planner and the sensor settings of the current Version, and
///        the sensor key that the preview cache uses.

#include "app/preset_preview_image.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/document_transfer.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "json.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/editor_history_port_test_reads.hpp"

namespace alcedo {
namespace {

constexpr sl_element_id_t    kElementId = 42;
const std::string            kDenoiseVersion(64, 'a');

/// Identities that start again at @ref Reset, so two plans of one package give equal documents.
class CountingIdentitySource final : public TransferIdentitySource {
 public:
  void Reset() { next_ = 0; }
  auto NextNodeId() -> NodeId override { return NodeId{"preset.grade." + std::to_string(++next_)}; }
  auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
      -> AdjustmentInstanceId override {
    return MakeAdjustmentInstanceId(node_id, type);
  }
  auto NextMaskId() -> MaskId override { return MaskId{"preset.mask." + std::to_string(++next_)}; }
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  auto NextStrokeId() -> StrokeId override {
    return StrokeId{"preset.stroke." + std::to_string(++next_)};
  }
#endif

 private:
  int next_ = 0;
};

auto DevelopTarget(const PipelineDocument& document, std::string field) -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind = EditorParameterOwnerKind::Develop;
  target.node_id    = document.Develop()->Id();
  target.field_key  = std::move(field);
  return target;
}

void PatchDevelop(PipelineDocument* document, const std::string& field,
                  const nlohmann::json& params) {
  test::PatchDocumentField(document, DevelopTarget(*document, field), params);
}

/// Exposure of the primary Grade; a Paste gives the Grades of the preset new ids.
auto ExposureEv(const PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  const auto     target =
      test::ColorGradeFieldTarget("exposure", std::string{document.PrimaryGrade()->Id().Value()});
  EXPECT_TRUE(ReadEditorParameterJson(document, target, &json, &error)) << error;
  return json.at("exposure_ev").get<float>();
}

/// A preset with an exposure, a custom white balance, another demosaic method, and AI Denoise.
auto PresetWithSensorAndLook() -> AdjustmentTransferPackage {
  auto source = test::DocumentWithExposureEv(0.75);
  PatchDevelop(&source, "raw_decode",
               {{"raw",
                 {{"method", "legacy"},
                  {"highlights_reconstruct", false},
                  {"ai_denoise", {{"model_version", kDenoiseVersion}}}}}});
  PatchDevelop(&source, "color_temp",
               {{"color_temp", {{"mode", "custom"}, {"custom_cct", 4300.0}}}});
  return CaptureDocumentTransfer(source);
}

class PresetPreviewImageTest : public ::testing::Test {
 protected:
  void SetUp() override { SetDocumentTransferIdentitySourceForTesting(&identity_); }
  void TearDown() override { SetDocumentTransferIdentitySourceForTesting(nullptr); }

  /// Working values of the current Version: the root with @p edit applied.
  template <typename Edit>
  auto Current(Edit&& edit) -> std::shared_ptr<const PipelineGraphSnapshot> {
    auto document = std::make_shared<PipelineDocument>(ClonePipelineDocument(root_.document));
    edit(document.get());
    return PipelineGraphSnapshot::Preview(std::move(document), kElementId,
                                          PipelineLineageId::Next(), transaction_chain_hash_t{});
  }

  auto Preview(const std::shared_ptr<const PipelineGraphSnapshot>& current,
               const AdjustmentTransferPackage&                    package)
      -> std::shared_ptr<const PipelineGraphSnapshot> {
    identity_.Reset();
    return BuildPresetPreviewDocument(graph_, root_, current, kElementId, package);
  }

  CountingIdentitySource identity_;
  LoadedRootState        root_{test::WorkingSpaceBoundDefaultDocument(), std::nullopt};
  CommitGraph            graph_ = CommitGraph::CreateEmpty(kElementId);
};

TEST_F(PresetPreviewImageTest, PreviewDocumentEqualsPasteResultExceptSensorSettings) {
  const auto current        = Current([](PipelineDocument* document) {
    test::PatchDocumentField(document, test::ColorGradeFieldTarget("exposure"),
                                    nlohmann::json{{"exposure_ev", 2.0}});
    PatchDevelop(document, "raw_decode", {{"raw", {{"highlights_reconstruct", true}}}});
  });
  const auto package        = PresetWithSensorAndLook();
  const auto commits_before = graph_.CommitCount();
  const auto preview        = Preview(current, package);
  ASSERT_NE(preview, nullptr);
  EXPECT_EQ(graph_.CommitCount(), commits_before) << "a preview commits nothing";

  // A real Paste of the same package: a new root-relative Version.
  identity_.Reset();
  const auto pasted = AdjustmentTransferService::PasteAsRootRelativeVersion(graph_, root_.document,
                                                                            package, "Look");
  ASSERT_TRUE(pasted.pasted) << pasted.error;
  std::string error;
  auto        pasted_document = BuildDocumentFromRoot(graph_, root_, pasted.new_head, &error);
  ASSERT_NE(pasted_document, nullptr) << error;
  EXPECT_EQ(pasted_document->Develop()->Params().DemosaicMethod(), "legacy")
      << "the Paste itself applies the RAW Decode values of the preset";
  pasted_document->UseSensorSettingsFrom(current->Document());

  EXPECT_EQ(CanonicalPipelineDocumentJson(preview->Document()),
            CanonicalPipelineDocumentJson(*pasted_document));
  EXPECT_FLOAT_EQ(ExposureEv(preview->Document()), 0.75f) << "the look of the preset, not 2.0";
  EXPECT_EQ(preview->Document().Develop()->Params().Params().wb_mode, "custom")
      << "the white balance of the preset is in the preview";
  EXPECT_EQ(preview->ElementId(), kElementId);
  EXPECT_EQ(preview->Lineage(), current->Lineage()) << "the executor keeps the binding";
}

TEST_F(PresetPreviewImageTest, PreviewDocumentKeepsDemosaicAndDenoiseOfCurrentVersion) {
  const auto current = Current([](PipelineDocument* document) {
    PatchDevelop(document, "raw_decode", {{"raw", {{"highlights_reconstruct", true}}}});
  });
  const auto preview = Preview(current, PresetWithSensorAndLook());
  ASSERT_NE(preview, nullptr);

  const auto& params = preview->Document().Develop()->Params();
  EXPECT_EQ(params.DemosaicMethod(), current->Document().Develop()->Params().DemosaicMethod());
  EXPECT_FALSE(params.AiDenoise().has_value()) << "AI Denoise of the preset is not in the preview";
  EXPECT_TRUE(params.Params().highlights_reconstruct);
  EXPECT_EQ(PresetPreviewSensorKey(preview->Document()),
            PresetPreviewSensorKey(current->Document()))
      << "the preview reuses the sensor result of the current Version";
}

TEST_F(PresetPreviewImageTest, PreviewOfAnotherImageOrAnEmptyPresetIsRefused) {
  const auto current        = Current([](PipelineDocument*) {});
  const auto commits_before = graph_.CommitCount();
  EXPECT_THROW((void)BuildPresetPreviewDocument(graph_, root_, current, kElementId + 1,
                                                PresetWithSensorAndLook()),
               std::invalid_argument);
  EXPECT_THROW((void)Preview(current, AdjustmentTransferPackage{}), std::invalid_argument);
  EXPECT_EQ(graph_.CommitCount(), commits_before);
}

TEST_F(PresetPreviewImageTest, SensorKeyFollowsSensorSettingsAndIgnoresWhiteBalanceAndLook) {
  const auto base = PresetPreviewSensorKey(root_.document);
  ASSERT_FALSE(base.empty());

  auto look = ClonePipelineDocument(root_.document);
  test::PatchDocumentField(&look, test::ColorGradeFieldTarget("exposure"),
                           nlohmann::json{{"exposure_ev", 1.0}});
  PatchDevelop(&look, "color_temp", {{"color_temp", {{"mode", "custom"}, {"custom_cct", 5000.0}}}});
  EXPECT_EQ(PresetPreviewSensorKey(look), base) << "a Color Grade or white balance edit";

  auto demosaic = ClonePipelineDocument(root_.document);
  PatchDevelop(&demosaic, "raw_decode", {{"raw", {{"method", "legacy"}}}});
  EXPECT_NE(PresetPreviewSensorKey(demosaic), base);

  auto denoise = ClonePipelineDocument(root_.document);
  PatchDevelop(&denoise, "raw_decode",
               {{"raw", {{"ai_denoise", {{"model_version", kDenoiseVersion}}}}}});
  EXPECT_NE(PresetPreviewSensorKey(denoise), base);
  EXPECT_NE(PresetPreviewSensorKey(denoise), PresetPreviewSensorKey(demosaic));
}

}  // namespace
}  // namespace alcedo
