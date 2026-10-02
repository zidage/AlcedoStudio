//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_comparison_inputs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_session_ports.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "grade_owned_mask_support.hpp"
#include "json.hpp"
#include "support/editor_history_port_test_reads.hpp"
#include "support/editor_lease_test_support.hpp"
#include "support/editor_parameter_target_test.hpp"
#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {
namespace {

constexpr sl_element_id_t kElementId = 42;

auto ComparisonTestPath(std::string_view name, std::string_view ext) -> std::filesystem::path {
  const auto stamp =
      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  auto dir = std::filesystem::path{"build/tmp/editor_comparison"};
  std::filesystem::create_directories(dir);
  return dir / (std::string{name} + "_" + stamp + std::string{ext});
}

auto CommitPanelField(EditorSessionHistoryPort& port, const EditorHistoryGuardHandle& handle,
                      const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  const auto preview = test::PatchFromJson(field, after_json, false);
  const auto settled = test::PatchFromJson(field, after_json, true);
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto CommitGradeField(EditorSessionHistoryPort& port, const EditorHistoryGuardHandle& handle,
                      const std::string& field, const std::string& after_json, std::string* error)
    -> bool {
  const auto preview = test::WithColorGradeTarget(test::PatchFromJson(field, after_json, false));
  const auto settled = test::WithColorGradeTarget(test::PatchFromJson(field, after_json, true));
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto CommitOdtEotf(EditorSessionHistoryPort& port, const EditorHistoryGuardHandle& handle,
                   const std::string& eotf, std::string* error) -> bool {
  const auto json    = R"({"odt":{"encoding_eotf":")" + eotf + R"("}})";
  const auto preview = test::WithDrtPostTarget(test::PatchFromJson("odt", json, false));
  const auto settled = test::WithDrtPostTarget(test::PatchFromJson("odt", json, true));
  if (!port.CaptureAdjustmentBeforePreview(handle, preview, error)) return false;
  return port.CommitAdjustment(handle, settled, error);
}

auto CommitAddColorGrade(EditorSessionHistoryPort& port, const EditorHistoryGuardHandle& handle,
                         const PipelineDocument& document, const NodeId& new_id, std::string* error)
    -> bool {
  try {
    auto change = CaptureAddColorGradeChange(document, NodeId{"drt"}, new_id);
    return port.CommitPipelineEditBatch(handle, MakeAddColorGradeBatch(std::move(change)), error);
  } catch (const std::exception& ex) {
    if (error != nullptr) *error = ex.what();
    return false;
  }
}

auto DevelopPayloadOf(const PipelineGraphSnapshot& snapshot) -> DevelopPayload {
  const auto* develop = snapshot.Document().Develop();
  EXPECT_NE(develop, nullptr);
  return develop == nullptr ? DevelopPayload{} : develop->Params().Params();
}

auto DevelopStamp(const PipelineGraphSnapshot& snapshot, DevelopDirty field) -> ParameterRevision {
  return snapshot.Document().Develop()->Params().FieldsRevision(DirtyFieldMask{field});
}

auto ExposureEv(const PipelineDocument& document) -> float {
  nlohmann::json json;
  std::string    error;
  EXPECT_TRUE(
      ReadEditorParameterJson(document, test::ColorGradeFieldTarget("exposure"), &json, &error))
      << error;
  return json.at("exposure_ev").get<float>();
}

auto RotationDegrees(const PipelineDocument& document) -> float {
  return document.Geometry().RotationDegrees();
}

/// Observable live and persistent state of the held history.
struct HeldHistoryState {
  head_commit_hash_t                           head;
  version_ref_id_t                             active_version;
  std::size_t                                  commit_count   = 0;
  std::size_t                                  version_count  = 0;
  bool                                         unmaterialized = false;
  std::uintmax_t                               journal_bytes  = 0;
  std::string                                  working_json;
  std::uint64_t                                working_fingerprint = 0;
  PipelineLineageId                            lineage;
  std::shared_ptr<const PipelineGraphSnapshot> published_preview;
};

class EditorComparisonInputsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    journal_path_ = ComparisonTestPath("comparison_inputs", ".wal");
    lease_    = test::MakeInMemoryEditorLease(kElementId, test::WorkingSpaceBoundDefaultDocument());
    pipeline_ = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        {}, [this](sl_element_id_t) { return test::CopyEditorLease(lease_); }});
    history_.SetServices(
        EditorSessionHistoryPort::Services{[this](sl_element_id_t) { return journal_path_; }});
    history_.SetPipelinePort(pipeline_);
    std::string error;
    handle_ = history_.Acquire(kElementId, &error);
    ASSERT_TRUE(handle_.valid) << error;
  }

  void TearDown() override {
    history_.Release(handle_);
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);
  }

  auto Graph() -> std::shared_ptr<const CommitGraph> {
    return test::EditorHistoryGraph(history_, kElementId);
  }
  auto Working() -> std::shared_ptr<const PipelineGraphSnapshot> {
    return test::EditorWorkingPreview(*pipeline_, kElementId);
  }

  auto ReadHeldState() -> HeldHistoryState {
    HeldHistoryState state;
    const auto       graph = Graph();
    state.head             = graph->GetActiveVersionRef().head_commit_hash;
    state.active_version   = graph->GetActiveVersionId();
    state.commit_count     = graph->CommitCount();
    state.version_count    = graph->GetAllVersionRefs().size();
    std::string error;
    state.unmaterialized = history_.HasUnmaterializedChanges(handle_, &error);
    std::error_code ec;
    state.journal_bytes       = std::filesystem::exists(journal_path_, ec)
                                    ? std::filesystem::file_size(journal_path_, ec)
                                    : 0;
    state.published_preview   = Working();
    state.working_json        = CanonicalPipelineDocumentJson(state.published_preview->Document());
    state.working_fingerprint = DocumentRevisionFingerprint(state.published_preview->Document());
    state.lineage             = state.published_preview->Lineage();
    return state;
  }

  void ExpectHeldStateUnchanged(const HeldHistoryState& before) {
    const auto after = ReadHeldState();
    EXPECT_EQ(after.head, before.head);
    EXPECT_EQ(after.active_version, before.active_version);
    EXPECT_EQ(after.commit_count, before.commit_count);
    EXPECT_EQ(after.version_count, before.version_count);
    EXPECT_EQ(after.unmaterialized, before.unmaterialized);
    EXPECT_EQ(after.journal_bytes, before.journal_bytes) << "no WAL record was written";
    EXPECT_EQ(after.working_json, before.working_json);
    EXPECT_EQ(after.working_fingerprint, before.working_fingerprint);
    EXPECT_EQ(after.lineage, before.lineage);
    EXPECT_EQ(after.published_preview, before.published_preview) << "no preview was published";
  }

  auto Build(const std::shared_ptr<const PipelineGraphSnapshot>& captured,
             const EditorComparisonSource& a, const EditorComparisonSource& b,
             EditorComparisonInputPair* pair, std::string* error) -> bool {
    return history_.BuildComparisonInputs(handle_, captured, a, b, pair, error);
  }

  /// Commit the state of Version "Look A" on the default Version, then open Version "Look B"
  /// from the root and commit different sensor, white-balance, geometry, and Grade values on it.
  /// Returns the identity of "Look A"; "Look B" stays active.
  auto MakeTwoDifferentVersions() -> version_ref_id_t {
    std::string error;
    const auto  look_a = Graph()->GetActiveVersionId();
    EXPECT_TRUE(CommitPanelField(history_, handle_, "raw_decode",
                                 R"({"raw":{"highlights_reconstruct":false}})", &error))
        << error;
    EXPECT_TRUE(CommitPanelField(history_, handle_, "lens_calib",
                                 R"({"lens_calib":{"enabled":true,"apply_tca":false}})", &error))
        << error;
    EXPECT_TRUE(CommitPanelField(history_, handle_, "color_temp",
                                 R"({"wb_mode":"custom","custom_cct":4100.0,"custom_tint":12.0})",
                                 &error))
        << error;
    EXPECT_TRUE(
        CommitPanelField(history_, handle_, "crop_rotate", R"({"rotation_degrees":7.0})", &error))
        << error;
    EXPECT_TRUE(CommitGradeField(history_, handle_, "exposure", R"({"exposure":0.5})", &error))
        << error;

    version_ref_id_t look_b{};
    EXPECT_TRUE(history_.CreateRootVersionAndCheckout(handle_, "Look B", &look_b, &error)) << error;
    EXPECT_TRUE(CommitPanelField(history_, handle_, "raw_decode",
                                 R"({"raw":{"demosaic_method":"neural_engine"}})", &error))
        << error;
    EXPECT_TRUE(CommitPanelField(history_, handle_, "color_temp",
                                 R"({"wb_mode":"custom","custom_cct":6900.0,"custom_tint":-4.0})",
                                 &error))
        << error;
    EXPECT_TRUE(CommitGradeField(history_, handle_, "exposure", R"({"exposure":1.25})", &error))
        << error;
    EXPECT_TRUE(
        CommitAddColorGrade(history_, handle_, Working()->Document(), NodeId{"grade.b"}, &error))
        << error;
    EXPECT_TRUE(history_.AddMask(handle_, NodeId{"grade.b"},
                                 grade_mask_test::MakeRadialMask(MaskId{"mask.b"}), 0, &error))
        << error;
    return look_a;
  }

  std::filesystem::path                      journal_path_;
  EditorHistoryLease                         lease_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_;
  EditorSessionHistoryPort                   history_;
  EditorHistoryGuardHandle                   handle_;
};

TEST_F(EditorComparisonInputsTest, RootAndCurrentInputsKeepHistoryAndWorkingDocumentUnchanged) {
  std::string error;
  ASSERT_TRUE(CommitPanelField(history_, handle_, "raw_decode",
                               R"({"raw":{"highlights_reconstruct":false}})", &error))
      << error;
  ASSERT_TRUE(CommitGradeField(history_, handle_, "exposure", R"({"exposure":0.75})", &error))
      << error;
  // An unsettled preview value is part of the captured working state.
  ASSERT_TRUE(history_.CaptureAdjustmentBeforePreview(
      handle_,
      test::WithColorGradeTarget(test::PatchFromJson("exposure", R"({"exposure":2.0})", false)),
      &error))
      << error;
  ASSERT_TRUE(history_.CommitAdjustment(
      handle_,
      test::WithColorGradeTarget(test::PatchFromJson("exposure", R"({"exposure":2.0})", false)),
      &error))
      << error;
  const auto captured = Working();
  ASSERT_FLOAT_EQ(ExposureEv(captured->Document()), 2.0f);
  const auto                before = ReadHeldState();

  EditorComparisonInputPair pair;
  ASSERT_TRUE(Build(captured, EditorComparisonSource::Root(), EditorComparisonSource::Current(),
                    &pair, &error))
      << error;
  ExpectHeldStateUnchanged(before);

  EXPECT_EQ(pair.b.snapshot, captured) << "Current uses the captured preview directly";
  EXPECT_EQ(pair.b.source.kind, EditorComparisonSourceKind::Current);
  EXPECT_FALSE(pair.b.source_head.has_value());

  ASSERT_NE(pair.a.snapshot, nullptr);
  EXPECT_EQ(pair.a.source.kind, EditorComparisonSourceKind::Root);
  EXPECT_FALSE(pair.a.source_head.has_value());
  EXPECT_FALSE(pair.a.snapshot->IsCommitted());
  EXPECT_FALSE(pair.a.snapshot->Head().has_value());
  EXPECT_EQ(pair.a.snapshot->ElementId(), kElementId);
  EXPECT_EQ(pair.a.snapshot->Lineage(), captured->Lineage());
  EXPECT_FLOAT_EQ(ExposureEv(pair.a.snapshot->Document()), ExposureEv(lease_.root_->document));
  // Root adjustments with the current sensor setting.
  EXPECT_FALSE(DevelopPayloadOf(*pair.a.snapshot).highlights_reconstruct);
}

TEST_F(EditorComparisonInputsTest, VersionInputsRetainOwnWhiteBalanceAndUseCurrentSensorSettings) {
  const auto look_a   = MakeTwoDifferentVersions();
  const auto captured = Working();
  const auto current  = DevelopPayloadOf(*captured);
  ASSERT_EQ(current.demosaic_method, "neural_engine");
  ASSERT_TRUE(current.highlights_reconstruct);
  ASSERT_FALSE(current.lens_enabled);
  const auto                before = ReadHeldState();

  std::string               error;
  EditorComparisonInputPair pair;
  ASSERT_TRUE(Build(captured, EditorComparisonSource::Version(look_a),
                    EditorComparisonSource::Current(), &pair, &error))
      << error;
  ExpectHeldStateUnchanged(before);

  const auto& a = *pair.a.snapshot;
  EXPECT_EQ(pair.a.source.version_id, look_a);
  EXPECT_EQ(pair.a.source_head, Graph()->GetVersionRef(look_a).head_commit_hash);
  // Sensor settings of the current working state.
  const auto payload = DevelopPayloadOf(a);
  EXPECT_EQ(payload.demosaic_method, current.demosaic_method);
  EXPECT_EQ(payload.highlights_reconstruct, current.highlights_reconstruct);
  EXPECT_EQ(payload.lens_enabled, current.lens_enabled);
  EXPECT_EQ(payload.apply_tca, current.apply_tca);
  EXPECT_EQ(payload.camera_profile, current.camera_profile);
  // White balance, geometry, and Grades of Look A.
  EXPECT_EQ(payload.wb_mode, "custom");
  EXPECT_FLOAT_EQ(payload.custom_cct, 4100.0f);
  EXPECT_FLOAT_EQ(payload.custom_tint, 12.0f);
  EXPECT_FLOAT_EQ(current.custom_cct, 6900.0f);
  EXPECT_FLOAT_EQ(RotationDegrees(a.Document()), 7.0f);
  EXPECT_FLOAT_EQ(RotationDegrees(captured->Document()), 0.0f);
  EXPECT_FLOAT_EQ(ExposureEv(a.Document()), 0.5f);
  EXPECT_FLOAT_EQ(ExposureEv(captured->Document()), 1.25f);
  EXPECT_EQ(a.Document().Graph().FindNode(NodeId{"grade.b"}), nullptr);
  EXPECT_EQ(a.Lineage(), captured->Lineage());
  EXPECT_FALSE(a.IsCommitted());
}

TEST_F(EditorComparisonInputsTest, SensorSettingsDerivativeKeepsCurrentSensorFieldRevisions) {
  const auto                look_a   = MakeTwoDifferentVersions();
  const auto                captured = Working();

  std::string               error;
  EditorComparisonInputPair pair;
  ASSERT_TRUE(Build(captured, EditorComparisonSource::Version(look_a),
                    EditorComparisonSource::Root(), &pair, &error))
      << error;

  for (const auto* side : {&pair.a, &pair.b}) {
    const auto& snapshot = *side->snapshot;
    EXPECT_EQ(DevelopStamp(snapshot, DevelopDirty::Demosaic),
              DevelopStamp(*captured, DevelopDirty::Demosaic));
    EXPECT_EQ(DevelopStamp(snapshot, DevelopDirty::Highlights),
              DevelopStamp(*captured, DevelopDirty::Highlights));
    EXPECT_EQ(DevelopStamp(snapshot, DevelopDirty::Lens),
              DevelopStamp(*captured, DevelopDirty::Lens));
  }
  // Look A's white balance differs from current, so its stamp differs from current's; Root's
  // as-shot white balance differs too. The two derivatives never share a white-balance stamp.
  EXPECT_NE(DevelopStamp(*pair.a.snapshot, DevelopDirty::WhiteBalance),
            DevelopStamp(*captured, DevelopDirty::WhiteBalance));
  EXPECT_NE(DevelopStamp(*pair.b.snapshot, DevelopDirty::WhiteBalance),
            DevelopStamp(*captured, DevelopDirty::WhiteBalance));
  EXPECT_NE(DevelopStamp(*pair.a.snapshot, DevelopDirty::WhiteBalance),
            DevelopStamp(*pair.b.snapshot, DevelopDirty::WhiteBalance));
  // The captured working document was not written.
  EXPECT_EQ(DevelopPayloadOf(*captured).custom_cct, 6900.0f);
}

TEST_F(EditorComparisonInputsTest, ReplayOfDifferentGradeTopologyKeepsStableDevelopIdentity) {
  const auto  look_a = MakeTwoDifferentVersions();
  // Look B (active) has grade.b with a Mask; Look A has only the primary Grade. Give Look A its own
  // Grade so that both sides differ in topology from each other and from the root.
  std::string error;
  ASSERT_TRUE(history_.CheckoutVersion(handle_, look_a, &error)) << error;
  ASSERT_TRUE(
      CommitAddColorGrade(history_, handle_, Working()->Document(), NodeId{"grade.a"}, &error))
      << error;
  version_ref_id_t look_b{};
  const auto       graph = Graph();
  for (const auto& [id, ref] : graph->GetAllVersionRefs()) {
    if (ref.display_name == "Look B") look_b = id;
  }
  ASSERT_TRUE(history_.CheckoutVersion(handle_, look_b, &error)) << error;
  const auto                captured = Working();

  EditorComparisonInputPair pair;
  ASSERT_TRUE(Build(captured, EditorComparisonSource::Version(look_a),
                    EditorComparisonSource::Root(), &pair, &error))
      << error;

  const auto& a    = pair.a.snapshot->Document();
  const auto& root = pair.b.snapshot->Document();
  EXPECT_TRUE(a.Graph().Validate().empty());
  EXPECT_TRUE(a.Graph().ValidateImageBackbone().empty());
  EXPECT_EQ(a.Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"grade.a"},
                                 NodeId{"drt"}}));
  EXPECT_EQ(root.Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"drt"}}));
  EXPECT_EQ(captured->Document().Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"grade.b"},
                                 NodeId{"drt"}}));
  const auto* current_grade_b = dynamic_cast<const ColorGradeNodeModel*>(
      captured->Document().Graph().FindNode(NodeId{"grade.b"}));
  ASSERT_NE(current_grade_b, nullptr);
  EXPECT_EQ(current_grade_b->Masks().size(), 1u);
  EXPECT_EQ(a.Graph().FindNode(NodeId{"grade.b"}), nullptr) << "Look A has no Mask Grade";
  EXPECT_EQ(a.Develop()->Id(), captured->Document().Develop()->Id());
  EXPECT_EQ(root.Develop()->Id(), captured->Document().Develop()->Id());
}

TEST_F(EditorComparisonInputsTest, ComparisonInputFailureDoesNotPublishPartialPair) {
  std::string error;
  ASSERT_TRUE(CommitGradeField(history_, handle_, "exposure", R"({"exposure":0.5})", &error))
      << error;
  const auto                captured = Working();
  const auto                before   = ReadHeldState();

  EditorComparisonInputPair pair;
  pair.a.source_head        = Hash128{0x5, 0x6};
  const auto untouched_head = pair.a.source_head;

  // A Version that is not in this history.
  EXPECT_FALSE(Build(captured, EditorComparisonSource::Root(),
                     EditorComparisonSource::Version(Hash128{0x77, 0x88}), &pair, &error));
  EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
  EXPECT_EQ(pair.a.snapshot, nullptr) << "a valid A side is not returned when B fails";
  EXPECT_EQ(pair.a.source_head, untouched_head);
  ExpectHeldStateUnchanged(before);

  // A Version whose head commit is not in the graph: replay fails.
  lease_.graph_ = *Graph();
  history_.Release(handle_);
  const auto bad_id = lease_.graph_.CreateVersionRefAtHead("Broken", before.head);
  lease_.graph_.GetVersionRef(bad_id).head_commit_hash = Hash128{0x11, 0x22};
  std::error_code ec;
  std::filesystem::remove(journal_path_, ec);
  lease_.document_ =
      std::make_shared<PipelineDocument>(ClonePipelineDocument(captured->Document()));
  handle_ = history_.Acquire(kElementId, &error);
  ASSERT_TRUE(handle_.valid) << error;
  const auto reopened        = Working();
  const auto reopened_before = ReadHeldState();

  error.clear();
  EXPECT_FALSE(Build(reopened, EditorComparisonSource::Root(),
                     EditorComparisonSource::Version(bad_id), &pair, &error));
  EXPECT_NE(error.find("Broken"), std::string::npos) << error;
  EXPECT_EQ(pair.a.snapshot, nullptr);
  ExpectHeldStateUnchanged(reopened_before);

  // A preview captured before the history was reloaded belongs to another lineage.
  error.clear();
  EXPECT_FALSE(Build(captured, EditorComparisonSource::Root(), EditorComparisonSource::Current(),
                     &pair, &error));
  EXPECT_NE(error.find("earlier load"), std::string::npos) << error;
  EXPECT_EQ(pair.a.snapshot, nullptr);
  ExpectHeldStateUnchanged(reopened_before);
}

TEST_F(EditorComparisonInputsTest, HdrCurrentOrSelectedVersionRejectsComparisonWithoutSdrRewrite) {
  for (const std::string eotf : {"st2084", "hlg"}) {
    SCOPED_TRACE(eotf);
    // A Version whose DRT encodes HDR, compared from an SDR current state.
    std::string      error;
    version_ref_id_t hdr_version{};
    ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle_, "HDR " + eotf, &hdr_version, &error))
        << error;
    ASSERT_TRUE(CommitOdtEotf(history_, handle_, eotf, &error)) << error;
    ASSERT_TRUE(IsHdrExportEncoding(*Working()->Document().Drt()));
    version_ref_id_t sdr_version{};
    ASSERT_TRUE(history_.CreateRootVersionAndCheckout(handle_, "SDR " + eotf, &sdr_version, &error))
        << error;
    const auto sdr_current = Working();
    ASSERT_FALSE(IsHdrExportEncoding(*sdr_current->Document().Drt()));
    auto                      before = ReadHeldState();

    EditorComparisonInputPair pair;
    EXPECT_FALSE(Build(sdr_current, EditorComparisonSource::Current(),
                       EditorComparisonSource::Version(hdr_version), &pair, &error));
    EXPECT_NE(error.find("HDR"), std::string::npos) << error;
    EXPECT_NE(error.find("HDR " + eotf), std::string::npos) << "names the selected Version";
    EXPECT_EQ(pair.a.snapshot, nullptr);
    ExpectHeldStateUnchanged(before);

    // The HDR state is the current state.
    ASSERT_TRUE(history_.CheckoutVersion(handle_, hdr_version, &error)) << error;
    const auto hdr_current = Working();
    before                 = ReadHeldState();
    error.clear();
    EXPECT_FALSE(Build(hdr_current, EditorComparisonSource::Root(),
                       EditorComparisonSource::Version(sdr_version), &pair, &error));
    EXPECT_NE(error.find("current working state"), std::string::npos) << error;
    EXPECT_EQ(pair.a.snapshot, nullptr);
    ExpectHeldStateUnchanged(before);
    // The HDR encoding stays on the working document: nothing rewrote it to SDR.
    EXPECT_TRUE(IsHdrExportEncoding(*Working()->Document().Drt()));
  }
}

TEST_F(EditorComparisonInputsTest, ActiveVersionUsesCapturedWorkingValues) {
  const auto look_a = MakeTwoDifferentVersions();
  const auto look_b = Graph()->GetActiveVersionId();
  ASSERT_NE(look_a, look_b);
  const auto                captured = Working();

  std::string               error;
  EditorComparisonInputPair pair;
  ASSERT_TRUE(Build(captured, EditorComparisonSource::Version(look_b),
                    EditorComparisonSource::Version(look_a), &pair, &error))
      << error;
  EXPECT_EQ(pair.a.snapshot, captured);
  EXPECT_EQ(pair.a.source.kind, EditorComparisonSourceKind::Version);
  EXPECT_EQ(pair.a.source.version_id, look_b);
  EXPECT_NE(pair.b.snapshot, captured);
}

TEST(EditorComparisonSensorSettingsTest, DevelopModelTakesSensorStampsAndKeepsOwnWhiteBalance) {
  DevelopParamsModel current;
  current.ApplyRawDecodeUpdate(
      {.demosaic_method = "neural_engine", .highlights_reconstruct = false});
  current.ApplyLensCalibrationUpdate({.lens_enabled = true, .apply_tca = false});
  current.ApplyColorTemperatureUpdate({.wb_mode = "custom", .custom_cct = 5000.0f});

  // Same white balance as current: every stamp equals current's.
  DevelopParamsModel same_white_balance;
  same_white_balance.ApplyColorTemperatureUpdate({.wb_mode = "custom", .custom_cct = 5000.0f});
  same_white_balance.UseSensorSettingsFrom(current);
  EXPECT_EQ(same_white_balance.Params(), current.Params());
  EXPECT_EQ(same_white_balance.FieldsRevision(DirtyFieldMask{DevelopDirty::All}),
            current.FieldsRevision(DirtyFieldMask{DevelopDirty::All}));
  EXPECT_EQ(same_white_balance.Revision(), current.Revision());

  // Different white balance: sensor stamps from current, white balance keeps its values with a
  // new stamp.
  DevelopParamsModel selected;
  selected.ApplyRawDecodeUpdate({.use_camera_wb = false, .user_wb = 3200.0f});
  selected.ApplyColorTemperatureUpdate(
      {.wb_mode = "custom", .custom_cct = 3900.0f, .custom_tint = 9.0f});
  const auto selected_wb_stamp =
      selected.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});
  selected.UseSensorSettingsFrom(current);

  const auto payload = selected.Params();
  EXPECT_EQ(payload.demosaic_method, "neural_engine");
  EXPECT_FALSE(payload.highlights_reconstruct);
  EXPECT_TRUE(payload.lens_enabled);
  EXPECT_FALSE(payload.apply_tca);
  EXPECT_FALSE(payload.use_camera_wb);
  EXPECT_FLOAT_EQ(payload.user_wb, 3200.0f);
  EXPECT_FLOAT_EQ(payload.custom_cct, 3900.0f);
  EXPECT_FLOAT_EQ(payload.custom_tint, 9.0f);
  for (const auto field : {DevelopDirty::Demosaic, DevelopDirty::Highlights, DevelopDirty::Lens}) {
    EXPECT_EQ(selected.FieldsRevision(DirtyFieldMask{field}),
              current.FieldsRevision(DirtyFieldMask{field}));
  }
  const auto wb_stamp = selected.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});
  EXPECT_NE(wb_stamp, current.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance}));
  EXPECT_NE(wb_stamp, selected_wb_stamp);
  EXPECT_EQ(current.Params().custom_cct, 5000.0f) << "current is only read";

  EXPECT_THROW(selected.UseSensorSettingsFrom(selected), std::invalid_argument);
}

TEST(EditorComparisonSensorSettingsTest, DocumentOfAnotherImageRejectsSensorSettingsUnchanged) {
  auto current = test::WorkingSpaceBoundDefaultDocument();
  current.Develop()->Params().ApplyRawDecodeUpdate({.demosaic_method = "neural_engine"});

  // A RAW camera profile instead of the working-space profile: another image.
  auto other                                  = CreateDefaultPipelineDocument();
  auto profile                                = other.Develop()->Params().Params();
  profile.camera_profile.color_matrices_valid = true;
  profile.camera_profile.color_matrix_1[0]    = 0.75;
  other.Develop()->Params().ReplaceParams(profile);
  const auto before = DocumentRevisionFingerprint(other);
  const auto json   = CanonicalPipelineDocumentJson(other);

  EXPECT_THROW(other.UseSensorSettingsFrom(current), std::invalid_argument);
  EXPECT_EQ(DocumentRevisionFingerprint(other), before);
  EXPECT_EQ(CanonicalPipelineDocumentJson(other), json);
  EXPECT_EQ(other.Develop()->Params().DemosaicMethod(), "default");
}

}  // namespace
}  // namespace alcedo::ui
