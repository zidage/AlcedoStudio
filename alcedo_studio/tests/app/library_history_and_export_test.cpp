//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Executor ownership refactor P5: export, import, Copy, and Paste to library images never
// construct or borrow an executor other than their own; the pipeline service holds none.
// - Export renders the committed snapshot captured at enqueue on the ExportService's own batch
//   executor and reads the output color from the DRT of the same snapshot.
// - Import creates the history root on a private document.
// - Copy reads the stored history; Paste edits a private copy of it and writes the result in one
//   storage transaction.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <chrono>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/export_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/runtime/drt_display.hpp"
#include "io/image/export_icc_profile_resolver.hpp"
#include "io/image/export_recipe.hpp"
#include "json.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;
using raw_import_test::ImportLinearDng;
using raw_import_test::LinearDngPath;

/// 8-bit mean absolute difference below which two exports of one state count as equal
/// (the JPEG encoder is deterministic; this allows for decode rounding only).
constexpr double kSameStateDifference     = 1.0;
/// 8-bit mean absolute difference above which two exports show different committed states.
constexpr double kDistinctStateDifference = 5.0;

auto             MeanAbsoluteDifference(const cv::Mat& a, const cv::Mat& b) -> double {
  if (a.empty() || b.empty() || a.size() != b.size() || a.type() != b.type()) {
    return std::numeric_limits<double>::infinity();
  }
  cv::Mat a64;
  cv::Mat b64;
  a.convertTo(a64, CV_64F);
  b.convertTo(b64, CV_64F);
  const auto per_channel = cv::mean(cv::abs(a64 - b64));
  double     sum         = 0.0;
  for (int c = 0; c < a.channels(); ++c) {
    sum += per_channel[c];
  }
  return sum / a.channels();
}

auto ReadExportedPixels(const std::filesystem::path& path) -> cv::Mat {
  auto input = OIIO::ImageInput::open(path.string());
  if (!input) {
    return {};
  }
  const OIIO::ImageSpec spec = input->spec();
  cv::Mat               pixels(spec.height, spec.width, CV_MAKETYPE(CV_8U, spec.nchannels));
  const bool ok = input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, pixels.data);
  input->close();
  return ok ? pixels : cv::Mat{};
}

auto ReadIccProfile(const std::filesystem::path& path) -> std::vector<uint8_t> {
  auto image = Exiv2::ImageFactory::open(path.string());
  image->readMetadata();
  if (!image->iccProfileDefined() || image->iccProfile().empty()) {
    return {};
  }
  const auto& profile = image->iccProfile();
  return {profile.c_data(), profile.c_data() + profile.size()};
}

/// 256 px JPEG export task of @p ids to @p path with an empty output color (read from the DRT).
auto MakeExportTask(const std::pair<sl_element_id_t, image_id_t>& ids,
                    const std::filesystem::path&                  path) -> ExportTask {
  ExportTask task;
  task.sleeve_id_                = ids.first;
  task.image_id_                 = ids.second;
  task.options_.format_          = ImageFormatType::JPEG;
  task.options_.export_path_     = path;
  task.options_.resize_enabled_  = true;
  task.options_.max_length_side_ = 256;
  task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
  return task;
}

/// Run every queued task of @p service and return the results.
auto RunQueuedExports(ExportService& service) -> std::vector<ExportResult> {
  std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
  auto                                                     done_future = done.get_future();
  service.ExportAll([&done](std::shared_ptr<std::vector<ExportResult>> results) {
    done.set_value(std::move(results));
  });
  if (done_future.wait_for(180s) != std::future_status::ready) {
    ADD_FAILURE() << "export did not finish";
    return {};
  }
  const auto results = done_future.get();
  return results ? *results : std::vector<ExportResult>{};
}

/// Materialized (head, chain) label of @p element_id in storage.
auto StoredLabel(ProjectService& project, sl_element_id_t element_id)
    -> std::optional<MaterializedHistoryLabel> {
  auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  return graph_service.GetMaterializedHistoryLabel(element_id);
}

/// A copy of the stored history of @p element_id with one more commit that raises exposure by
/// @p delta_ev on the active Version, as a Paste to a library image records its edit.
struct EditedHistory {
  ImageHistorySnapshot base;
  CommitGraph          graph;
  commit_hash_t        head;
};

auto AddExposureCommit(PipelineMgmtService& pipelines, sl_element_id_t element_id, float delta_ev)
    -> EditedHistory {
  auto           base    = pipelines.LoadHistorySnapshot(element_id);
  const auto     current = pipelines.AcquireCommittedSnapshot(element_id);
  nlohmann::json before =
      current->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure())->ToJson();
  nlohmann::json after = before;
  after["exposure_ev"] = before.at("exposure_ev").get<float>() + delta_ev;

  PipelineEditBatch  batch;
  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  change.target.field_key              = "exposure";
  change.before_value                  = std::move(before);
  change.after_value                   = std::move(after);
  change.before_enabled                = true;
  change.after_enabled                 = true;
  batch.operation_kind                 = PipelineEditOperationKind::SetParameter;
  batch.presentation_key               = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));

  CommitGraph graph  = *base.graph_;
  auto        commit = EditCommit::MakePipelineEdit(graph.GetRootId(),
                                                    graph.GetActiveVersionRef().head_commit_hash, batch);
  const auto  head   = commit.GetCommitHash();
  if (!graph.InsertCommit(std::move(commit))) {
    throw std::runtime_error("exposure commit was not inserted");
  }
  graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
  return {std::move(base), std::move(graph), head};
}

class LibraryHistoryAndExportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(LinearDngPath())) {
      GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
    }
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base  = std::filesystem::temp_directory_path();
    db_path_         = base / ("library_history_export_" + stamp + ".db");
    meta_path_       = base / ("library_history_export_" + stamp + ".json");
    export_dir_      = base / ("library_history_export_out_" + stamp);
    std::filesystem::create_directories(export_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(export_dir_, ec);
  }

  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
  std::filesystem::path export_dir_;
};

// Import builds the root on a private document, with no executor. The stored root carries the
// RAW camera profile, and the element pipeline JSON (kept for older versions of the application)
// is the same document.
TEST_F(LibraryHistoryAndExportTest, ImportCreatesTheRootOnAPrivateDocument) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  const auto history = pipelines->LoadHistorySnapshot(ids.first);
  ASSERT_TRUE(history.root_ != nullptr);
  EXPECT_TRUE(history.root_->raw_color_context.has_value());
  ASSERT_NE(history.root_->document.Develop(), nullptr);
  EXPECT_TRUE(
      history.root_->document.Develop()->Params().Params().camera_profile.color_matrices_valid);
  EXPECT_FALSE(history.graph_->GetImageEditState().materialized_head_commit_hash.has_value());

  const auto element_json =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(ids.first);
  ASSERT_TRUE(element_json.has_value());
  const auto committed = pipelines->AcquireCommittedSnapshot(ids.first);
  EXPECT_EQ(PipelineDocument::FromJson(*element_json).ToJson(), committed->Document().ToJson());
}

// The root is immutable: a second initialization is an error and the stored root stays.
TEST_F(LibraryHistoryAndExportTest, InitializeImageRootRefusesAnImageThatAlreadyHasARoot) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  const auto root_before = pipelines->LoadHistorySnapshot(ids.first).graph_->GetRootId();

  EXPECT_THROW(pipelines->InitializeImageRoot(ids.first, CreateDefaultPipelineDocument(), nullptr),
               std::runtime_error);
  EXPECT_EQ(pipelines->LoadHistorySnapshot(ids.first).graph_->GetRootId(), root_before);
}

// Copy source: the stored history is read from storage, and the snapshot does not change when
// storage changes later.
TEST_F(LibraryHistoryAndExportTest, HistorySnapshotIsReadFromStorageAndStaysImmutable) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  const auto copy_source = pipelines->LoadHistorySnapshot(ids.first);
  const auto commits     = copy_source.graph_->CommitCount();
  auto       edited      = AddExposureCommit(*pipelines, ids.first, 1.0f);
  (void)pipelines->PersistHistory(edited.base, edited.graph);

  EXPECT_EQ(copy_source.graph_->CommitCount(), commits);
  EXPECT_EQ(pipelines->LoadHistorySnapshot(ids.first).graph_->CommitCount(), commits + 1);
}

// The editor may hold commits that storage does not have yet, so storage is not a valid source
// for the image it holds, for reading or writing.
TEST_F(LibraryHistoryAndExportTest, HistoryReadAndWriteRefuseTheImageTheEditorHolds) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto       edited       = AddExposureCommit(*pipelines, ids.first, 1.0f);
  const auto label_before = StoredLabel(project, ids.first);

  const auto editor       = pipelines->AcquireEditorLease(ids.first);
  ASSERT_NE(editor.document_, nullptr);
  EXPECT_THROW((void)pipelines->LoadHistorySnapshot(ids.first), std::runtime_error);
  EXPECT_THROW((void)pipelines->PersistHistory(edited.base, edited.graph), std::runtime_error);
  pipelines->ReleaseEditorLease(ids.first);

  const auto label_after = StoredLabel(project, ids.first);
  ASSERT_TRUE(label_before.has_value() && label_after.has_value());
  EXPECT_EQ(label_after->head_commit_hash, label_before->head_commit_hash);
  EXPECT_EQ(label_after->transaction_chain_hash, label_before->transaction_chain_hash);
}

// Paste to a library image: the new commit, the Version tip, and the checkpoint of the replayed
// document are written together, and the returned snapshot is what thumbnails and export get next
// without another storage build.
TEST_F(LibraryHistoryAndExportTest, PersistHistoryWritesTheNewStateAndPublishesItsSnapshot) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  auto       edited    = AddExposureCommit(*pipelines, ids.first, 1.5f);
  const auto committed = pipelines->PersistHistory(edited.base, edited.graph);
  ASSERT_NE(committed, nullptr);
  EXPECT_TRUE(committed->IsCommitted());
  EXPECT_EQ(committed->Head(), head_commit_hash_t{edited.head});
  EXPECT_EQ(committed->Chain(), edited.graph.ChainHashForHead(edited.head));

  const auto label = StoredLabel(project, ids.first);
  ASSERT_TRUE(label.has_value());
  EXPECT_EQ(label->head_commit_hash, committed->Head());
  EXPECT_EQ(label->transaction_chain_hash, committed->Chain());
  {
    auto             db_guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    const auto       state = graph_service.GetImageEditState(ids.first);
    ASSERT_TRUE(state.has_value() && state->serialized_pipeline_state.has_value());
    const auto checkpoint = DecodePipelineDocumentCheckpoint(*state->serialized_pipeline_state);
    EXPECT_EQ(checkpoint.head_commit_hash, committed->Head());
    EXPECT_EQ(checkpoint.transaction_chain_hash, committed->Chain());
    EXPECT_EQ(checkpoint.document.ToJson(), committed->Document().ToJson());
  }

  const auto storage_loads = pipelines->CommittedSnapshotStorageLoadCount();
  EXPECT_EQ(pipelines->AcquireCommittedSnapshot(ids.first), committed);
  EXPECT_EQ(pipelines->CommittedSnapshotStorageLoadCount(), storage_loads);

  // A new service builds the same document from storage alone.
  PipelineMgmtService reopened(project.GetStorage());
  EXPECT_EQ(reopened.AcquireCommittedSnapshot(ids.first)->Document().ToJson(),
            committed->Document().ToJson());
}

// Two writers that read the same stored state: the second write is refused and storage keeps the
// first writer's state, so no commit is lost silently.
TEST_F(LibraryHistoryAndExportTest, PersistHistoryRejectsAStaleBaseAndLeavesStorageUnchanged) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  auto       first   = AddExposureCommit(*pipelines, ids.first, 1.0f);
  auto       second  = AddExposureCommit(*pipelines, ids.first, 2.0f);
  const auto written = pipelines->PersistHistory(first.base, first.graph);
  EXPECT_THROW((void)pipelines->PersistHistory(second.base, second.graph), std::runtime_error);

  const auto label = StoredLabel(project, ids.first);
  ASSERT_TRUE(label.has_value());
  EXPECT_EQ(label->head_commit_hash, head_commit_hash_t{first.head});
  EXPECT_EQ(pipelines->AcquireCommittedSnapshot(ids.first), written);
}

// Export renders the committed snapshot of an imported image on the ExportService's own executor
// and writes a readable image.
TEST_F(LibraryHistoryAndExportTest, ExportOfAnImportedImageWritesAReadableImage) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(),
                               pipelines);
  export_service.EnqueueExportTask(MakeExportTask(ids, export_dir_ / "export.jpg"));
  const auto results = RunQueuedExports(export_service);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].success_) << results[0].message_;
  EXPECT_FALSE(ReadExportedPixels(export_dir_ / "export.jpg").empty());
}

// An export renders the committed state at the time it was queued, not a state committed while
// it waits; an export queued afterwards renders the new state.
TEST_F(LibraryHistoryAndExportTest, ExportRendersTheCommittedStateCapturedAtEnqueue) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(),
                               pipelines);

  export_service.EnqueueExportTask(MakeExportTask(ids, export_dir_ / "reference.jpg"));
  ASSERT_EQ(RunQueuedExports(export_service).size(), 1u);

  export_service.EnqueueExportTask(MakeExportTask(ids, export_dir_ / "queued_before.jpg"));
  auto edited = AddExposureCommit(*pipelines, ids.first, 2.0f);
  (void)pipelines->PersistHistory(edited.base, edited.graph);
  export_service.EnqueueExportTask(MakeExportTask(ids, export_dir_ / "queued_after.jpg"));
  const auto results = RunQueuedExports(export_service);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_TRUE(results[0].success_) << results[0].message_;
  EXPECT_TRUE(results[1].success_) << results[1].message_;

  const auto reference     = ReadExportedPixels(export_dir_ / "reference.jpg");
  const auto queued_before = ReadExportedPixels(export_dir_ / "queued_before.jpg");
  const auto queued_after  = ReadExportedPixels(export_dir_ / "queued_after.jpg");
  ASSERT_FALSE(reference.empty());
  EXPECT_LE(MeanAbsoluteDifference(queued_before, reference), kSameStateDifference);
  EXPECT_GT(MeanAbsoluteDifference(queued_after, reference), kDistinctStateDifference);
}

// With no explicit output color in the recipe, the embedded ICC profile is the encoding of the
// DRT node of the committed snapshot the export renders.
TEST_F(LibraryHistoryAndExportTest, ExportIccProfileIsTheEncodingOfTheCommittedDrt) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(),
                               pipelines);

  const auto    path = export_dir_ / "icc.jpg";
  export_service.EnqueueExportTask(MakeExportTask(ids, path));
  const auto results = RunQueuedExports(export_service);
  ASSERT_EQ(results.size(), 1u);
  ASSERT_TRUE(results[0].success_) << results[0].message_;
  EXPECT_TRUE(results[0].icc_embedded_);

  const auto* drt = pipelines->AcquireCommittedSnapshot(ids.first)->Document().Drt();
  ASSERT_NE(drt, nullptr);
  const auto expected = ExportIccProfileResolver::ResolveIccProfileBytes(
      ExportColorProfileFromDrt(drt->Params().Params()));
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(ReadIccProfile(path), expected);
}

// An image without a history root cannot be exported: the task is refused at enqueue with the
// real error and nothing is queued.
TEST_F(LibraryHistoryAndExportTest, ExportOfAnImageWithoutHistoryIsRefusedAtEnqueue) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  ExportService  export_service(project.GetSleeveService(), project.GetImagePoolService(),
                                pipelines);

  try {
    export_service.EnqueueExportTask(MakeExportTask({987654u, 1u}, export_dir_ / "none.jpg"));
    ADD_FAILURE() << "export of an image without history was queued";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("has no edit history root"), std::string::npos)
        << error.what();
  }
  EXPECT_TRUE(RunQueuedExports(export_service).empty());
}

}  // namespace
}  // namespace alcedo
