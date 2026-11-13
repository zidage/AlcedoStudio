//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DCP camera profile reference goes through the editor field write, the history, and every
// load path (dcp_camera_profile_plan.md, phase C4). The project stores the reference only; each
// load binds the profile from the DCP library.
//
// Each case writes synthetic DCP files into a scratch DCP folder and scans it into the
// process-wide DcpProfileLibrary::Shared(), the library that production lookups read. The cases
// import a copy of a CI RAW fixture (non-DNG, so no embedded profile takes part) and skip when it
// is missing. The PipelineDcpProfileDngSourceTest cases (phase C6) import a CI DNG fixture
// instead, so the DCP tables meet an embedded profile.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <QString>
#include <array>
#include <cstdint>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support/storage_test_access.hpp"
#include "app/document_transfer.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "image/dcp_profile_library.hpp"
#include "image/dcp_test_file_writer.hpp"
#include "image/dng_camera_matrix.hpp"
#include "image/dng_color_profile.hpp"
#include "json.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace {

using test::DcpTestFileWriter;

namespace tag {
constexpr std::uint16_t kUniqueCameraModel        = 50708;
constexpr std::uint16_t kColorMatrix1             = 50721;
constexpr std::uint16_t kColorMatrix2             = 50722;
constexpr std::uint16_t kCalibrationIlluminant1   = 50778;
constexpr std::uint16_t kCalibrationIlluminant2   = 50779;
constexpr std::uint16_t kProfileCalibrationSignature = 50932;
constexpr std::uint16_t kProfileName              = 50936;
constexpr std::uint16_t kForwardMatrix1           = 50964;
constexpr std::uint16_t kForwardMatrix2           = 50965;
constexpr std::uint16_t kProfileLookTableDims     = 50981;
constexpr std::uint16_t kProfileLookTableData     = 50982;
constexpr std::uint16_t kProfileLookTableEncoding = 51108;
}  // namespace tag

constexpr const char*                kCameraModel = "Alcedo Test Camera";
constexpr const char*                kProfileName = "Camera Standard";

// A RAW file without ForwardMatrix gets none from import, so a forward matrix element in a project
// table can only come from the DCP file. The look table values are not values of a Develop field.
const std::vector<double>            kForwardMatrix1{0.7978, 0.1352, 0.0313, 0.288, 0.7119,
                                          0.0001, 0.0,    0.0,    0.8251};
const std::vector<double>            kForwardMatrix2{0.7978, 0.1352, 0.0313,  0.288, 0.7119,
                                          0.0001, 0.0,    -0.0001, 0.8253};
constexpr std::array<const char*, 3> kForwardMatrixProbes{"0.7978", "0.1352", "0.7119"};
constexpr float                      kLookHueShift = 37.4219f;
constexpr float                      kLookSatScale = 1.3917f;
constexpr float                      kLookValScale = 0.8713f;
constexpr std::array<const char*, 3> kLookTableProbes{"37.42", "1.391", "0.871"};

auto                                 RawFixturePath() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "ci_rawfiles" /
         "credit @ryanbreitkreutz _DSC0027.ARW";
}

/// Dual-illuminant DCP with forward matrices and a 3x2x2 look table. @p matrix_scale changes the
/// color matrices, so that two files with one identity have different content and fingerprints.
void WriteDcp(const std::filesystem::path& path, const std::string& profile_name,
              double matrix_scale = 1.0, const std::string& calibration_signature = {}) {
  std::vector<double> color_matrix_1{0.6722, -0.0635, -0.0963, -0.4287, 1.246,
                                     0.2028, -0.0908, 0.2162,  0.5668};
  std::vector<double> color_matrix_2{0.6875, -0.097,  -0.0932, -0.4691, 1.2459,
                                     0.2501, -0.0874, 0.1953,  0.5809};
  for (double& value : color_matrix_1) value *= matrix_scale;
  for (double& value : color_matrix_2) value *= matrix_scale;
  // DNG order: saturation is the innermost index. A zero-saturation entry keeps the value scale 1.
  std::vector<float> look;
  for (int slice = 0; slice < 3 * 2; ++slice) {
    look.insert(look.end(), {kLookHueShift, kLookSatScale, 1.0f});
    look.insert(look.end(), {kLookHueShift, kLookSatScale, kLookValScale});
  }
  DcpTestFileWriter writer;
  writer.Ascii(tag::kUniqueCameraModel, kCameraModel)
      .Ascii(tag::kProfileName, profile_name)
      .SRationals(tag::kColorMatrix1, color_matrix_1)
      .SRationals(tag::kColorMatrix2, color_matrix_2)
      .Shorts(tag::kCalibrationIlluminant1, {17})
      .Shorts(tag::kCalibrationIlluminant2, {21})
      .SRationals(tag::kForwardMatrix1, kForwardMatrix1)
      .SRationals(tag::kForwardMatrix2, kForwardMatrix2)
      .Longs(tag::kProfileLookTableDims, {3, 2, 2})
      .Floats(tag::kProfileLookTableData, look)
      .Longs(tag::kProfileLookTableEncoding, {0});
  if (!calibration_signature.empty()) {
    writer.Ascii(tag::kProfileCalibrationSignature, calibration_signature);
  }
  writer.Write(path);
}

/// Log messages of the cases that check a warning. The handler runs on any thread.
std::mutex               g_log_mutex;
std::vector<std::string> g_log_messages;

void                     CollectLogRecord(const log::Record& record) {
  if (record.level < log::Level::Warn) return;
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log_messages.emplace_back(record.message);
}

auto CountLogMessages(std::string_view text) -> int {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  int                         count = 0;
  for (const auto& message : g_log_messages) {
    if (message.find(text) != std::string::npos) ++count;
  }
  return count;
}

/// Text of every row of every table of the project database, one string per row.
auto AllTableRowTexts(ProjectService& project) -> std::vector<std::string> {
  auto       guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
  auto       lock  = guard.Lock();
  const auto query = [&guard](const std::string& sql) {
    duckdb_result            result;
    std::vector<std::string> texts;
    if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
      ADD_FAILURE() << "Query failed: " << sql << ": " << duckdb_result_error(&result);
      duckdb_destroy_result(&result);
      return texts;
    }
    for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
      char* value = duckdb_value_varchar(&result, 0, row);
      texts.emplace_back(value != nullptr ? value : "");
      duckdb_free(value);
    }
    duckdb_destroy_result(&result);
    return texts;
  };
  std::vector<std::string> rows;
  for (const auto& table : query("SELECT table_name FROM duckdb_tables() WHERE NOT internal")) {
    const auto table_rows =
        query("SELECT CAST(row_value AS VARCHAR) FROM \"" + table + "\" AS row_value");
    rows.insert(rows.end(), table_rows.begin(), table_rows.end());
  }
  return rows;
}

auto DevelopTarget(const PipelineDocument& document) -> EditorParameterTarget {
  std::string error;
  auto        target = CompleteCurrentPanelParameterTarget(document, "camera_profile_file", &error);
  if (!target.has_value()) throw std::runtime_error(error);
  return *target;
}

auto Payload(const PipelineDocument& document) -> DevelopPayload {
  return document.Develop()->Params().Params();
}

class PipelineDcpProfileBindingTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;

  void                    SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    if (!std::filesystem::exists(RawFixturePath())) {
      GTEST_SKIP() << "CI RAW fixture missing: " << RawFixturePath().string();
    }
    dcp_dir_ = temp_dir_ / "dcp";
    std::filesystem::create_directories(dcp_dir_);
    dcp_file_ = dcp_dir_ / "Alcedo Test Camera Camera Standard.dcp";
    WriteDcp(dcp_file_, kProfileName);
    WriteDcp(dcp_dir_ / "Alcedo Test Camera Camera Vivid.dcp", "Camera Vivid", 0.5);
    ASSERT_EQ(DcpProfileLibrary::Shared().Scan(dcp_dir_).profile_count, 2u);
    library_profile_ = DcpProfileLibrary::Shared().Find(kCameraModel, kProfileName);
    ASSERT_NE(library_profile_, nullptr);

    db_path_          = temp_dir_ / "project.db";
    meta_path_        = temp_dir_ / "project.json";
    const auto source = temp_dir_ / "DSC0027.ARW";
    std::filesystem::copy_file(RawFixturePath(), source);
    project_ = std::make_unique<ProjectService>(db_path_, meta_path_);
    auto pipelines = std::make_shared<PipelineMgmtService>(project_->GetStorage());
    element_id_ = raw_import_test::ImportRawFile(*project_, pipelines, source).first;
    ASSERT_NE(element_id_, 0u);
  }

  void TearDown() override {
    project_.reset();
    log::SetHandler(nullptr);
    (void)DcpProfileLibrary::Shared().Scan({});
  }

  /// Apply a `camera_profile_file` patch to the lease document and record it as the next commit,
  /// as EditorHistoryMutation::CommitAdjustment does: read the field, apply the write, read it
  /// again, and append one SetParameter batch. Returns the batch, or nullopt with @p error set
  /// when the write is rejected (no commit then).
  static auto CommitPatch(EditorHistoryLease& lease, const nlohmann::json& patch,
                          std::string* error) -> std::optional<PipelineEditBatch> {
    auto&          document = *lease.document_;
    const auto     target   = DevelopTarget(document);
    nlohmann::json before;
    nlohmann::json after;
    if (!ReadEditorParameterJson(document, target, &before, error) ||
        !ApplyEditorParameterPatch(document, target, patch, error) ||
        !ReadEditorParameterJson(document, target, &after, error)) {
      return std::nullopt;
    }
    auto batch = MakeSetParameterBatch(target, before, after, true, true, "Develop");
    AppendCommit(lease, batch);
    return batch;
  }

  static auto CommitSelection(EditorHistoryLease& lease) -> PipelineEditBatch {
    std::string error;
    auto        batch = CommitPatch(
        lease, {{"camera_model", kCameraModel}, {"profile_name", kProfileName}}, &error);
    if (!batch.has_value()) throw std::runtime_error("selection was rejected: " + error);
    return *batch;
  }

  static void AppendCommit(EditorHistoryLease& lease, const PipelineEditBatch& batch) {
    auto&      graph  = lease.graph_;
    auto       commit = EditCommit::MakePipelineEdit(graph.GetRootId(),
                                                     graph.GetActiveVersionRef().head_commit_hash, batch);
    const auto head   = commit.GetCommitHash();
    if (!graph.InsertCommit(std::move(commit))) throw std::runtime_error("commit not inserted");
    graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
  }

  /// Open the editor, select the profile, persist history and checkpoint, and release the editor.
  void CommitSelectionAndRelease() {
    PipelineMgmtService pipelines(project_->GetStorage());
    auto                lease  = pipelines.AcquireEditorLease(element_id_);
    const auto          before = lease.graph_.GetImageEditState();
    (void)CommitSelection(lease);
    std::string error;
    ASSERT_TRUE(pipelines.PersistEditorHistory(lease.graph_, before, *lease.document_, &error))
        << error;
    pipelines.ReleaseEditorLease(element_id_);
  }

  void ExpectBoundToLibraryProfile(const PipelineDocument& document) const {
    const auto payload = Payload(document);
    ASSERT_TRUE(payload.camera_profile_file.has_value());
    EXPECT_EQ(payload.camera_profile_file->camera_model, kCameraModel);
    EXPECT_EQ(payload.camera_profile_file->profile_name, kProfileName);
    ASSERT_TRUE(payload.camera_profile_file->IsBound());
    EXPECT_EQ(payload.camera_profile_file->fingerprint, library_profile_->fingerprint);
    EXPECT_EQ(payload.camera_profile_file->profile->fingerprint, library_profile_->fingerprint);
    const auto resolved = ResolveDevelopColorTransform(payload);
    EXPECT_TRUE(resolved.ok) << static_cast<int>(resolved.error);
    EXPECT_EQ(EffectiveCameraProfile(payload).forward_matrix_1, library_profile_->forward_matrix_1);
  }

  std::filesystem::path           dcp_dir_;
  std::filesystem::path           dcp_file_;
  DcpCameraProfilePtr             library_profile_;
  std::filesystem::path           db_path_;
  std::filesystem::path           meta_path_;
  std::unique_ptr<ProjectService> project_;
  sl_element_id_t                 element_id_ = 0;
};

TEST_F(PipelineDcpProfileBindingTest, CommittedSelectionStoresReferenceOnlyInProjectTables) {
  CommitSelectionAndRelease();
  project_->SaveProject(meta_path_);

  const auto fingerprint    = DngColorProfileFingerprintToText(library_profile_->fingerprint);
  int        reference_rows = 0;
  for (const auto& row : AllTableRowTexts(*project_)) {
    if (row.find("camera_profile_file") != std::string::npos) {
      ++reference_rows;
      EXPECT_NE(row.find(fingerprint), std::string::npos) << row.substr(0, 400);
      EXPECT_NE(row.find(kProfileName), std::string::npos) << row.substr(0, 400);
    }
    for (const char* probe : kLookTableProbes) {
      EXPECT_EQ(row.find(probe), std::string::npos) << "look table value " << probe;
    }
    for (const char* probe : kForwardMatrixProbes) {
      EXPECT_EQ(row.find(probe), std::string::npos) << "forward matrix value " << probe;
    }
    EXPECT_EQ(row.find("look_table"), std::string::npos);
    EXPECT_EQ(row.find("hue_sat_map"), std::string::npos);
  }
  // The checkpoint and the history commit hold the reference.
  EXPECT_GE(reference_rows, 2);
}

TEST_F(PipelineDcpProfileBindingTest, EditorLeaseCheckpointAndReplayBindTheReference) {
  CommitSelectionAndRelease();

  PipelineMgmtService reopened(project_->GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto lease = reopened.AcquireEditorLease(element_id_);
  ASSERT_NE(lease.document_, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 0u)
      << "the checkpoint path must load and bind the stored document";
  ExpectBoundToLibraryProfile(*lease.document_);
  // The root never holds a reference: import selects no profile.
  EXPECT_FALSE(Payload(lease.root_->document).camera_profile_file.has_value());

  std::string error;
  const auto  replayed = BuildDocumentFromRoot(
      lease.graph_, *lease.root_, lease.graph_.GetActiveVersionRef().head_commit_hash, &error);
  ASSERT_NE(replayed, nullptr) << error;
  ExpectBoundToLibraryProfile(*replayed);
  EXPECT_EQ(Payload(*replayed).as_shot_cct, Payload(*lease.document_).as_shot_cct);
  reopened.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileBindingTest, CommittedDocumentForExportBindsTheReference) {
  CommitSelectionAndRelease();

  PipelineMgmtService pipelines(project_->GetStorage());
  const auto          snapshot = pipelines.AcquireCommittedSnapshot(element_id_);
  ASSERT_NE(snapshot, nullptr);
  ExpectBoundToLibraryProfile(snapshot->Document());
  const auto committed = ResolveDevelopColorTransform(Payload(snapshot->Document()));

  auto       lease     = pipelines.AcquireEditorLease(element_id_);
  ASSERT_NE(lease.document_, nullptr);
  const auto editor = ResolveDevelopColorTransform(Payload(*lease.document_));
  pipelines.ReleaseEditorLease(element_id_);
  ASSERT_TRUE(committed.ok);
  ASSERT_TRUE(editor.ok);
  EXPECT_EQ(committed.transform.camera_to_ap1, editor.transform.camera_to_ap1);
  EXPECT_EQ(committed.transform.resolved_cct, editor.transform.resolved_cct);
  EXPECT_EQ(committed.transform.resolved_tint, editor.transform.resolved_tint);
}

TEST_F(PipelineDcpProfileBindingTest, UndoRestoresImportProfileAndRedoRestoresDcpProfile) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease          = pipelines.AcquireEditorLease(element_id_);
  auto&               document       = *lease.document_;
  const auto          import_profile = Payload(document).camera_profile;
  const auto          import_json    = document.Develop()->Params().ToJson();
  const auto          batch          = CommitSelection(lease);
  const auto          selected_json  = document.Develop()->Params().ToJson();
  ExpectBoundToLibraryProfile(document);
  EXPECT_EQ(EffectiveCameraProfile(Payload(document)).color_matrix_1,
            library_profile_->color_matrix_1);

  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_FALSE(Payload(document).camera_profile_file.has_value());
  EXPECT_EQ(EffectiveCameraProfile(Payload(document)).color_matrix_1,
            import_profile.color_matrix_1);
  EXPECT_EQ(document.Develop()->Params().ToJson(), import_json);

  ASSERT_TRUE(ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  ExpectBoundToLibraryProfile(document);

  // The editor history restores a field from the complete stored Develop JSON (an unsettled
  // preview, a same-session head move); that value also clears and sets the reference.
  const auto target = DevelopTarget(document);
  ASSERT_TRUE(ApplyEditorParameterValue(document, target, import_json, &error)) << error;
  EXPECT_FALSE(Payload(document).camera_profile_file.has_value());
  ASSERT_TRUE(ApplyEditorParameterValue(document, target, selected_json, &error)) << error;
  ExpectBoundToLibraryProfile(document);
  // The other Develop fields restore from the same complete JSON while a reference is set.
  for (const char* field : {"raw_decode", "color_temp", "lens_calib"}) {
    const auto field_target = CompleteCurrentPanelParameterTarget(document, field, &error);
    ASSERT_TRUE(field_target.has_value()) << error;
    EXPECT_TRUE(ApplyEditorParameterValue(document, *field_target, selected_json, &error))
        << field << ": " << error;
  }
  pipelines.ReleaseEditorLease(element_id_);
}

// `camera_profile_file.profile_tone` (dcp_profile_tone_curve_study.md, phase T2): the write, the
// history in both directions, the replay, and the binding keep the switch.
TEST_F(PipelineDcpProfileBindingTest, ProfileToneSwitchCommitsUndoesAndReplaysWithTheBinding) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease    = pipelines.AcquireEditorLease(element_id_);
  auto&               document = *lease.document_;
  (void)CommitSelection(lease);
  ASSERT_TRUE(Payload(document).camera_profile_file->profile_tone);

  std::string error;
  const auto  tone_off = CommitPatch(
      lease,
      {{"camera_model", kCameraModel}, {"profile_name", kProfileName}, {"profile_tone", false}},
      &error);
  ASSERT_TRUE(tone_off.has_value()) << error;
  ExpectBoundToLibraryProfile(document);
  EXPECT_FALSE(Payload(document).camera_profile_file->profile_tone);
  EXPECT_EQ(document.Develop()->Params().ToJson().at("camera_profile_file").at("profile_tone"),
            false);

  ASSERT_TRUE(
      ApplyPipelineEditBatch(document, *tone_off, PipelineEditApplyDirection::Inverse, &error))
      << error;
  ExpectBoundToLibraryProfile(document);
  EXPECT_TRUE(Payload(document).camera_profile_file->profile_tone);
  EXPECT_FALSE(
      document.Develop()->Params().ToJson().at("camera_profile_file").contains("profile_tone"));
  ASSERT_TRUE(
      ApplyPipelineEditBatch(document, *tone_off, PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_FALSE(Payload(document).camera_profile_file->profile_tone);

  const auto head   = lease.graph_.GetActiveVersionRef().head_commit_hash;
  const auto replay = BuildDocumentFromRoot(lease.graph_, *lease.root_, head, &error);
  ASSERT_NE(replay, nullptr) << error;
  ExpectBoundToLibraryProfile(*replay);
  EXPECT_FALSE(Payload(*replay).camera_profile_file->profile_tone);

  // A switch that is not a boolean is rejected; the document stays.
  const auto json = document.ToJson().dump();
  EXPECT_FALSE(
      CommitPatch(
          lease,
          {{"camera_model", kCameraModel}, {"profile_name", kProfileName}, {"profile_tone", "off"}},
          &error)
          .has_value());
  EXPECT_EQ(document.ToJson().dump(), json);
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileBindingTest, CheckoutOfEarlierVersionClearsTheReference) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease = pipelines.AcquireEditorLease(element_id_);
  // An earlier edit of another Develop field, then the selection.
  std::string         error;
  {
    auto&       document = *lease.document_;
    std::string target_error;
    const auto  target = CompleteCurrentPanelParameterTarget(document, "color_temp", &target_error);
    ASSERT_TRUE(target.has_value()) << target_error;
    nlohmann::json before;
    nlohmann::json after;
    ASSERT_TRUE(ReadEditorParameterJson(document, *target, &before, &error)) << error;
    ASSERT_TRUE(ApplyEditorParameterPatch(document, *target,
                                          {{"wb_mode", "custom"}, {"custom_cct", 4321.0f}}, &error))
        << error;
    ASSERT_TRUE(ReadEditorParameterJson(document, *target, &after, &error)) << error;
    AppendCommit(lease, MakeSetParameterBatch(*target, before, after, true, true, "Develop"));
  }
  const auto earlier_head = lease.graph_.GetActiveVersionRef().head_commit_hash;
  (void)CommitSelection(lease);
  const auto selection_head = lease.graph_.GetActiveVersionRef().head_commit_hash;

  const auto earlier = BuildDocumentFromRoot(lease.graph_, *lease.root_, earlier_head, &error);
  ASSERT_NE(earlier, nullptr) << error;
  EXPECT_FALSE(Payload(*earlier).camera_profile_file.has_value());
  EXPECT_EQ(Payload(*earlier).custom_cct, 4321.0f);
  EXPECT_TRUE(ResolveDevelopColorTransform(Payload(*earlier)).ok);

  const auto latest = BuildDocumentFromRoot(lease.graph_, *lease.root_, selection_head, &error);
  ASSERT_NE(latest, nullptr) << error;
  ExpectBoundToLibraryProfile(*latest);
  EXPECT_EQ(Payload(*latest).custom_cct, 4321.0f);
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileBindingTest, SelectionOfUnknownIdentityIsRejectedAndDocumentIsUnchanged) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease         = pipelines.AcquireEditorLease(element_id_);
  const auto          document_json = lease.document_->ToJson().dump();
  const auto          head_before   = lease.graph_.GetActiveVersionRef().head_commit_hash;

  std::string         error;
  EXPECT_FALSE(
      CommitPatch(lease, {{"camera_model", kCameraModel}, {"profile_name", "Camera Flat"}}, &error)
          .has_value());
  EXPECT_NE(error.find("\"Camera Flat\""), std::string::npos) << error;
  EXPECT_NE(error.find(dcp_dir_.string()), std::string::npos) << error;
  EXPECT_EQ(lease.document_->ToJson().dump(), document_json);

  error.clear();
  EXPECT_FALSE(
      CommitPatch(lease, {{"camera_model", "Other Camera"}, {"profile_name", kProfileName}}, &error)
          .has_value());
  EXPECT_NE(error.find("\"Other Camera\""), std::string::npos) << error;

  // A selection needs both strings; unknown keys are rejected.
  error.clear();
  EXPECT_FALSE(CommitPatch(lease, {{"camera_model", kCameraModel}}, &error).has_value());
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_FALSE(CommitPatch(lease,
                           {{"camera_model", kCameraModel},
                            {"profile_name", kProfileName},
                            {"fingerprint", "0000000000000000"}},
                           &error)
                   .has_value());
  EXPECT_FALSE(error.empty());

  EXPECT_EQ(lease.document_->ToJson().dump(), document_json);
  EXPECT_EQ(lease.graph_.GetActiveVersionRef().head_commit_hash, head_before);
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileBindingTest, MissingLibraryLeavesReferenceUnboundAndRenderFails) {
  CommitSelectionAndRelease();
  ASSERT_EQ(DcpProfileLibrary::Shared().Scan({}).profile_count, 0u);

  PipelineMgmtService pipelines(project_->GetStorage());
  // Checkpoint path: the editor opens, and the render of the document fails with the named error.
  auto                lease = pipelines.AcquireEditorLease(element_id_);
  ASSERT_NE(lease.document_, nullptr);
  auto&      document  = *lease.document_;
  const auto reference = Payload(document).camera_profile_file;
  ASSERT_TRUE(reference.has_value());
  EXPECT_FALSE(reference->IsBound());
  EXPECT_EQ(reference->fingerprint, library_profile_->fingerprint);
  const auto failed = ResolveDevelopColorTransform(Payload(document));
  EXPECT_FALSE(failed.ok);
  EXPECT_EQ(failed.error, ColorTransformError::UnboundCameraProfileFile);
  const auto message = ColorTransformFailureMessage(Payload(document),
                                                    ColorTransformError::UnboundCameraProfileFile);
  EXPECT_NE(message.find(kProfileName), std::string::npos) << message;
  EXPECT_NE(message.find(kCameraModel), std::string::npos) << message;

  // Replay path: the history apply keeps the stored reference unbound instead of failing.
  std::string error;
  const auto  replayed = BuildDocumentFromRoot(
      lease.graph_, *lease.root_, lease.graph_.GetActiveVersionRef().head_commit_hash, &error);
  ASSERT_NE(replayed, nullptr) << error;
  ASSERT_TRUE(Payload(*replayed).camera_profile_file.has_value());
  EXPECT_FALSE(Payload(*replayed).camera_profile_file->IsBound());
  EXPECT_EQ(ResolveDevelopColorTransform(Payload(*replayed)).error,
            ColorTransformError::UnboundCameraProfileFile);

  // Selecting the default entry commits a clear edit, and the render uses the import profile.
  const auto clear = CommitPatch(lease, nlohmann::json(nullptr), &error);
  ASSERT_TRUE(clear.has_value()) << error;
  EXPECT_FALSE(Payload(document).camera_profile_file.has_value());
  EXPECT_TRUE(ResolveDevelopColorTransform(Payload(document)).ok);
  // Undo of the clear restores the stored reference, still unbound.
  ASSERT_TRUE(ApplyPipelineEditBatch(document, *clear, PipelineEditApplyDirection::Inverse, &error))
      << error;
  ASSERT_TRUE(Payload(document).camera_profile_file.has_value());
  EXPECT_FALSE(Payload(document).camera_profile_file->IsBound());
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileBindingTest, ChangedLibraryFileWinsAndLogsOneWarning) {
  PipelineMgmtService pipelines(project_->GetStorage());
  PipelineEditBatch   selection;
  {
    auto       lease  = pipelines.AcquireEditorLease(element_id_);
    const auto before = lease.graph_.GetImageEditState();
    selection         = CommitSelection(lease);
    std::string error;
    ASSERT_TRUE(pipelines.PersistEditorHistory(lease.graph_, before, *lease.document_, &error))
        << error;
    pipelines.ReleaseEditorLease(element_id_);
  }
  // The same identity with other matrices, so another fingerprint.
  WriteDcp(dcp_file_, kProfileName, 1.25);
  ASSERT_EQ(DcpProfileLibrary::Shared().Scan(dcp_dir_).profile_count, 2u);
  const auto changed = DcpProfileLibrary::Shared().Find(kCameraModel, kProfileName);
  ASSERT_NE(changed, nullptr);
  ASSERT_NE(changed->fingerprint, library_profile_->fingerprint);

  {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_messages.clear();
  }
  log::SetHandler(&CollectLogRecord);
  PipelineMgmtService reopened(project_->GetStorage());
  auto                lease = reopened.AcquireEditorLease(element_id_);
  log::SetHandler(nullptr);
  ASSERT_NE(lease.document_, nullptr);
  const auto reference = Payload(*lease.document_).camera_profile_file;
  ASSERT_TRUE(reference.has_value());
  ASSERT_TRUE(reference->IsBound());
  EXPECT_EQ(reference->fingerprint, changed->fingerprint);
  EXPECT_EQ(EffectiveCameraProfile(Payload(*lease.document_)).color_matrix_1,
            changed->color_matrix_1);
  EXPECT_EQ(CountLogMessages("changed in the DCP folder"), 1);

  // Undo still applies: the stored side holds the earlier fingerprint of the same identity.
  std::string error;
  ASSERT_TRUE(ApplyPipelineEditBatch(*lease.document_, selection,
                                     PipelineEditApplyDirection::Inverse, &error))
      << error;
  EXPECT_FALSE(Payload(*lease.document_).camera_profile_file.has_value());
  reopened.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileBindingTest, TransferAndPasteDoNotCarryTheReference) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease = pipelines.AcquireEditorLease(element_id_);
  (void)CommitSelection(lease);
  auto& source = *lease.document_;
  ExpectBoundToLibraryProfile(source);

  const auto package  = CaptureDocumentTransfer(source);
  const auto exported = ExportDocumentTransfer(package).dump();
  EXPECT_EQ(exported.find("camera_profile_file"), std::string::npos);
  EXPECT_EQ(exported.find(kProfileName), std::string::npos);

  // Paste of all Develop fields onto the import root of the same image.
  auto                           target = ClonePipelineDocument(lease.root_->document);
  CountingTransferIdentitySource identity;
  DocumentTransferPasteOptions   options;
  options.identity_source = &identity;
  const auto  prepared    = DocumentTransferPlanner::Plan(package, target, options);
  std::string error;
  ASSERT_TRUE(
      ApplyPipelineEditBatch(target, prepared.batch, PipelineEditApplyDirection::Forward, &error))
      << error;
  EXPECT_FALSE(Payload(target).camera_profile_file.has_value());
  EXPECT_EQ(prepared.batch.ToJSON().dump().find("camera_profile_file"), std::string::npos);
  pipelines.ReleaseEditorLease(element_id_);
}

// A project written before this plan (build abdb000c8, tests/resources/compat) keeps its root id:
// a document without the reference serializes as it did then.
TEST_F(PipelineDcpProfileBindingTest, RootIdOfProjectMadeBeforeThisPlanStillValidates) {
  nlohmann::json expected;
  {
    std::ifstream in(
        std::filesystem::path(ALCEDO_COMPAT_FIXTURE_DIR) / "raw_project_abdb000c8.json",
        std::ios::binary);
    ASSERT_TRUE(in.is_open());
    expected = nlohmann::json::parse(in);
  }
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               unpack_error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(
      std::filesystem::path(ALCEDO_COMPAT_FIXTURE_DIR) / "raw_project_abdb000c8.alcd",
      temp_dir_ / "compat_workspace", QStringLiteral("Compat"), &db_path, &meta_path,
      &unpack_error))
      << unpack_error.toStdString();

  const auto          element_id = expected.at("element_id").get<sl_element_id_t>();
  ProjectService      project(db_path, meta_path);
  PipelineMgmtService pipelines(project.GetStorage());
  // AcquireEditorLease throws when TryDecodeRootState rejects the stored root id.
  auto                lease = pipelines.AcquireEditorLease(element_id);
  ASSERT_NE(lease.root_, nullptr);
  ASSERT_NE(lease.document_, nullptr);
  EXPECT_FALSE(Payload(lease.root_->document).camera_profile_file.has_value());
  EXPECT_FALSE(Payload(*lease.document_).camera_profile_file.has_value());
  EXPECT_EQ(lease.document_->ToJson(), expected.at("committed_document"));
  pipelines.ReleaseEditorLease(element_id);
}

// Phase C6: a DNG source keeps its AnalogBalance, BaselineExposure, and CameraCalibration with the
// tables of the DCP profile. The cases import a copy of a CI DNG fixture instead of the RAW file.
auto DngFixturePath() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "ci_rawfiles" /
         "tag @ryanbreitkreutz - free raws from @signatureeditsco - DSC06683.dng";
}

/// A DNG from another camera: its embedded profile has another fingerprint.
auto OtherCameraDngPath() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "raw/linear_dng/mfzoty.dng";
}

class PipelineDcpProfileDngSourceTest : public PipelineDcpProfileBindingTest {
 protected:
  void SetUp() override {
    PipelineDcpProfileBindingTest::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    if (!std::filesystem::exists(DngFixturePath())) {
      GTEST_SKIP() << "CI DNG fixture missing: " << DngFixturePath().string();
    }
    dng_source_ = temp_dir_ / "DSC06683.dng";
    std::filesystem::copy_file(DngFixturePath(), dng_source_);
    auto pipelines = std::make_shared<PipelineMgmtService>(project_->GetStorage());
    element_id_    = raw_import_test::ImportRawFile(*project_, pipelines, dng_source_).first;
    ASSERT_NE(element_id_, 0u);

    PipelineMgmtService service(project_->GetStorage());
    auto                lease = service.AcquireEditorLease(element_id_);
    source_profile_           = Payload(*lease.document_).camera_profile.dng_profile.Profile();
    service.ReleaseEditorLease(element_id_);
    ASSERT_NE(source_profile_, nullptr) << "the DNG fixture must embed a profile";
  }

  /// CameraCalibration1 that the DCP tables must hold for the DCP profile @p dcp.
  [[nodiscard]] auto ExpectedCalibration1(const DcpCameraProfile& dcp) const
      -> std::array<double, 9> {
    const auto& calibration = source_profile_->source_calibration;
    return CameraCalibrationSignaturesMatch(calibration.signature,
                                            dcp.profile_calibration_signature)
               ? calibration.camera_calibration_1
               : kDngIdentityMatrix;
  }

  /// The document renders the library profile with the values of the DNG source.
  void ExpectSourceTables(const PipelineDocument& document) const {
    ExpectBoundToLibraryProfile(document);
    const auto payload = Payload(document);
    EXPECT_EQ(payload.camera_profile.dng_profile.Profile(), source_profile_) << "import values";
    const auto& tables = payload.camera_profile_file->source_tables;
    ASSERT_NE(tables, nullptr);
    EXPECT_EQ(payload.camera_profile_file->source_fingerprint, source_profile_->fingerprint);
    EXPECT_EQ(EffectiveCameraProfile(payload).dng_profile.Profile(), tables);
    EXPECT_EQ(tables->analog_balance, source_profile_->analog_balance);
    EXPECT_EQ(tables->baseline_exposure, source_profile_->baseline_exposure);
    const auto& dcp = *library_profile_->tables;
    EXPECT_EQ(tables->look_table, dcp.look_table);
    EXPECT_EQ(tables->hue_sat_map_1, dcp.hue_sat_map_1);
    EXPECT_EQ(tables->hue_sat_map_2, dcp.hue_sat_map_2);
    EXPECT_EQ(tables->baseline_exposure_offset, dcp.baseline_exposure_offset);
    EXPECT_EQ(tables->camera_calibration_1, ExpectedCalibration1(*library_profile_));
  }

  std::filesystem::path dng_source_;
  DngColorProfilePtr    source_profile_;
};

TEST_F(PipelineDcpProfileDngSourceTest,
       DngSourceKeepsAnalogBalanceAndBaselineExposureWithDcpProfile) {
  // The fixture has its own values, so equal values prove that they come from the source.
  EXPECT_NE(source_profile_->look_table, library_profile_->tables->look_table);
  RecordProperty("source_signature", source_profile_->source_calibration.signature);
  RecordProperty("source_baseline_exposure", std::to_string(source_profile_->baseline_exposure));
  {
    PipelineMgmtService pipelines(project_->GetStorage());
    auto                lease  = pipelines.AcquireEditorLease(element_id_);
    const auto          before = lease.graph_.GetImageEditState();
    (void)CommitSelection(lease);
    ExpectSourceTables(*lease.document_);
    std::string error;
    ASSERT_TRUE(pipelines.PersistEditorHistory(lease.graph_, before, *lease.document_, &error))
        << error;
    pipelines.ReleaseEditorLease(element_id_);
  }
  // Checkpoint, replay, and the committed snapshot of export bind the same values.
  PipelineMgmtService reopened(project_->GetStorage());
  auto                lease = reopened.AcquireEditorLease(element_id_);
  ExpectSourceTables(*lease.document_);
  std::string error;
  const auto  replayed = BuildDocumentFromRoot(
      lease.graph_, *lease.root_, lease.graph_.GetActiveVersionRef().head_commit_hash, &error);
  ASSERT_NE(replayed, nullptr) << error;
  ExpectSourceTables(*replayed);
  const auto snapshot = reopened.AcquireCommittedSnapshot(element_id_);
  ASSERT_NE(snapshot, nullptr);
  ExpectSourceTables(snapshot->Document());
  reopened.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileDngSourceTest, CameraCalibrationAppliesOnlyWhenSignaturesMatch) {
  const auto& signature = source_profile_->source_calibration.signature;
  WriteDcp(dcp_dir_ / "Alcedo Test Camera Camera Signed.dcp", "Camera Signed", 1.0, signature);
  WriteDcp(dcp_dir_ / "Alcedo Test Camera Camera Other.dcp", "Camera Other", 1.0,
           signature + ".other");
  ASSERT_EQ(DcpProfileLibrary::Shared().Scan(dcp_dir_).profile_count, 4u);

  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease = pipelines.AcquireEditorLease(element_id_);
  std::string         error;
  const auto          select = [&](const char* name) {
    const auto batch =
        CommitPatch(lease, {{"camera_model", kCameraModel}, {"profile_name", name}}, &error);
    EXPECT_TRUE(batch.has_value()) << error;
    return Payload(*lease.document_).camera_profile_file->source_tables;
  };
  const auto signed_tables = select("Camera Signed");
  ASSERT_NE(signed_tables, nullptr);
  EXPECT_EQ(signed_tables->camera_calibration_1,
            source_profile_->source_calibration.camera_calibration_1);
  EXPECT_EQ(signed_tables->camera_calibration_2,
            source_profile_->source_calibration.camera_calibration_2);
  const auto other_tables = select("Camera Other");
  ASSERT_NE(other_tables, nullptr);
  EXPECT_EQ(other_tables->camera_calibration_1, kDngIdentityMatrix);
  EXPECT_EQ(other_tables->camera_calibration_2, kDngIdentityMatrix);
  EXPECT_EQ(other_tables->analog_balance, source_profile_->analog_balance);
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileDngSourceTest, MergedProfileIsSharedByTwoDocumentsOfOneImage) {
  CommitSelectionAndRelease();

  PipelineMgmtService pipelines(project_->GetStorage());
  const auto          snapshot = pipelines.AcquireCommittedSnapshot(element_id_);
  ASSERT_NE(snapshot, nullptr);
  auto lease = pipelines.AcquireEditorLease(element_id_);
  ASSERT_NE(lease.document_, nullptr);
  std::string error;
  const auto  replayed = BuildDocumentFromRoot(
      lease.graph_, *lease.root_, lease.graph_.GetActiveVersionRef().head_commit_hash, &error);
  ASSERT_NE(replayed, nullptr) << error;

  const auto& committed = Payload(snapshot->Document()).camera_profile_file->source_tables;
  ASSERT_NE(committed, nullptr);
  EXPECT_EQ(Payload(*lease.document_).camera_profile_file->source_tables, committed);
  EXPECT_EQ(Payload(*replayed).camera_profile_file->source_tables, committed);
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileDngSourceTest, ClearingReferenceOnDngSourceRestoresEmbeddedProfile) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease     = pipelines.AcquireEditorLease(element_id_);
  auto&               document  = *lease.document_;
  const auto          before    = Payload(document);
  const auto          transform = ResolveDevelopColorTransform(before);
  ASSERT_TRUE(transform.ok);
  const auto json = document.Develop()->Params().ToJson();

  (void)CommitSelection(lease);
  ExpectSourceTables(document);
  EXPECT_NE(ResolveDevelopColorTransform(Payload(document)).transform.camera_to_ap1,
            transform.transform.camera_to_ap1);
  std::string error;
  ASSERT_TRUE(CommitPatch(lease, nlohmann::json(nullptr), &error).has_value()) << error;

  const auto after = Payload(document);
  EXPECT_FALSE(after.camera_profile_file.has_value());
  EXPECT_EQ(after, before);
  EXPECT_EQ(EffectiveCameraProfile(after).dng_profile.Profile(), source_profile_);
  const auto restored = ResolveDevelopColorTransform(after);
  ASSERT_TRUE(restored.ok);
  EXPECT_EQ(restored.transform.camera_to_xyz, transform.transform.camera_to_xyz);
  EXPECT_EQ(restored.transform.camera_to_ap1, transform.transform.camera_to_ap1);
  EXPECT_EQ(document.Develop()->Params().ToJson(), json);
  pipelines.ReleaseEditorLease(element_id_);
}

TEST_F(PipelineDcpProfileDngSourceTest, SourceFileWithAnotherProfileIsMergedAgainOnLoad) {
  if (!std::filesystem::exists(OtherCameraDngPath())) {
    GTEST_SKIP() << "Second DNG fixture missing: " << OtherCameraDngPath().string();
  }
  CommitSelectionAndRelease();
  std::filesystem::copy_file(OtherCameraDngPath(), dng_source_,
                             std::filesystem::copy_options::overwrite_existing);

  // The replay binds the DCP against the stored root profile; the import binding then binds the
  // profile of the new file, and BindDcpCameraProfile merges again.
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                lease = pipelines.AcquireEditorLease(element_id_);
  ASSERT_NE(lease.document_, nullptr);
  const auto payload  = Payload(*lease.document_);
  const auto replaced = payload.camera_profile.dng_profile.Profile();
  ASSERT_NE(replaced, nullptr);
  ASSERT_NE(replaced->fingerprint, source_profile_->fingerprint);
  ASSERT_TRUE(payload.camera_profile_file.has_value());
  ASSERT_NE(payload.camera_profile_file->source_tables, nullptr);
  EXPECT_EQ(payload.camera_profile_file->source_fingerprint, replaced->fingerprint);
  EXPECT_EQ(payload.camera_profile_file->source_tables->analog_balance, replaced->analog_balance);
  EXPECT_TRUE(ResolveDevelopColorTransform(payload).ok);
  pipelines.ReleaseEditorLease(element_id_);
}

}  // namespace
}  // namespace alcedo
