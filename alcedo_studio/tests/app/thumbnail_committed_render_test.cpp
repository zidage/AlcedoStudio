//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Executor ownership refactor P4: thumbnails and analysis renditions render committed pipeline
// graph snapshots on the ThumbnailService's own batch executors. They never load a
// PipelineGuard, never take the editor's render lock, and never see uncommitted editor values.
// Since P6 the editor session owns its working document and its Interactive executor; these tests
// stand in for it with an EditorStandIn that owns the same objects.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/editor_working_document.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;
using raw_import_test::HostPixels;
using raw_import_test::ImportLinearDng;
using raw_import_test::LinearDngPath;

auto RequestThumbnail(ThumbnailService& service, sl_element_id_t element_id, image_id_t image_id,
                      ThumbnailResolution resolution) -> ThumbnailRequestResult {
  std::promise<ThumbnailRequestResult> done;
  auto                                 done_future = done.get_future();
  service.GetThumbnailDetailed(
      element_id, image_id,
      [&done](ThumbnailRequestResult result) { done.set_value(std::move(result)); }, true, nullptr,
      resolution);
  if (done_future.wait_for(120s) != std::future_status::ready) {
    ADD_FAILURE() << "thumbnail request did not finish";
    return {};
  }
  return done_future.get();
}

/// 8-bit mean absolute difference below which two thumbnails count as the same state after the
/// disk cache's JPEG encoding.
constexpr double kJpegDifference          = 3.0;
/// 8-bit mean absolute difference above which two thumbnails show different committed states
/// (+1.5 EV and +2 EV change the tone-mapped linear DNG thumbnail by more than this).
constexpr double kDistinctStateDifference = 5.0;

auto             ThumbnailPixels(const ThumbnailRequestResult& result) -> cv::Mat {
  if (!result.guard || !result.guard->thumbnail_buffer_) {
    return {};
  }
  return HostPixels(*result.guard->thumbnail_buffer_);
}

/// Mean absolute difference over all channels of two images of the same size and type.
auto MeanAbsoluteDifference(const cv::Mat& a, const cv::Mat& b) -> double {
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

/// What the editor session owns for the image it holds: the history lease, the working document,
/// and the only Interactive executor. The service holds none of them.
struct EditorStandIn {
  sl_element_id_t                        id = 0;
  EditorHistoryLease                     lease;
  std::unique_ptr<EditorWorkingDocument> working;
  std::shared_ptr<PipelineExecutor>      executor;

  [[nodiscard]] auto                     Head() const -> head_commit_hash_t {
    return lease.graph_.GetActiveVersionRef().head_commit_hash;
  }
};

/// Take the editor lease of @p id and build the editor's working document and executor.
auto OpenEditor(PipelineMgmtService& pipelines, sl_element_id_t id) -> EditorStandIn {
  EditorStandIn editor;
  editor.id       = id;
  editor.lease    = pipelines.AcquireEditorLease(id);
  editor.working  = std::make_unique<EditorWorkingDocument>(id, editor.lease.document_);
  editor.executor = std::make_shared<PipelineExecutor>(ExecutorRole::Interactive);
  return editor;
}

/// Exposure of the editor's working document.
auto WorkingExposureJson(const EditorStandIn& editor) -> nlohmann::json {
  return std::as_const(*editor.working)
      .Document()
      .PrimaryGrade()
      ->FindAdjustmentByType(type_ids::Exposure())
      ->ToJson();
}

/// Raise exposure on the editor's working document by @p delta_ev and record it as one commit on
/// its CommitGraph, as the editor does when a slider settles. Returns the new head. The imported
/// DNG already carries a non-zero exposure, so callers state a change, not a value.
auto CommitExposure(EditorStandIn& editor, float delta_ev) -> commit_hash_t {
  nlohmann::json before = WorkingExposureJson(editor);
  nlohmann::json after  = before;
  after["exposure_ev"]  = before.at("exposure_ev").get<float>() + delta_ev;
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
  std::string error;
  if (!ApplyPipelineEditBatch(editor.working->Document(), batch,
                              PipelineEditApplyDirection::Forward, &error)) {
    throw std::runtime_error("exposure batch did not apply: " + error);
  }
  auto&      graph  = editor.lease.graph_;
  auto       commit = EditCommit::MakePipelineEdit(graph.GetRootId(), editor.Head(), batch);
  const auto head   = commit.GetCommitHash();
  if (!graph.InsertCommit(std::move(commit))) {
    throw std::runtime_error("commit was not inserted");
  }
  graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
  (void)editor.working->PublishPreview();
  return head;
}

/// Publish the editor's working document as the committed state at its working head, as the
/// editor history does after a settled change.
auto PublishWorkingAsCommitted(PipelineMgmtService& pipelines, const EditorStandIn& editor)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  const auto head     = editor.Head();
  auto       snapshot = PipelineGraphSnapshot::Committed(
      std::as_const(*editor.working).Document().Freeze(), editor.id, PipelineLineageId::Next(),
      head, editor.lease.graph_.ChainHashForHead(head));
  pipelines.PublishCommitted(snapshot);
  return snapshot;
}

auto HasBatchRenderer(PipelineExecutor& executor) -> bool {
#ifdef HAVE_CUDA
  if (executor.DebugCudaBatchRenderer() != nullptr) {
    return true;
  }
#endif
#ifdef HAVE_METAL
  if (executor.DebugMetalBatchRenderer() != nullptr) {
    return true;
  }
#endif
#ifdef HAVE_OPENCL
  if (executor.DebugOpenClBatchRenderer() != nullptr) {
    return true;
  }
#endif
  return false;
}

class ThumbnailCommittedRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base  = std::filesystem::temp_directory_path();
    db_path_         = base / ("thumbnail_committed_" + stamp + ".db");
    meta_path_       = base / ("thumbnail_committed_" + stamp + ".json");
    cache_root_      = base / ("thumbnail_committed_cache_" + stamp);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(cache_root_, ec);
  }

  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
  std::filesystem::path cache_root_;
};

// ── PipelineMgmtService committed snapshots ────────────────────────────────────────────────────

TEST_F(ThumbnailCommittedRenderTest, StoredSnapshotEqualsTheDocumentTheEditorLoads) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  const auto snapshot = pipelines->AcquireCommittedSnapshot(ids.first);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_TRUE(snapshot->IsCommitted());
  EXPECT_EQ(snapshot->ElementId(), ids.first);
  EXPECT_FALSE(snapshot->Head().has_value()) << "an imported image has only its root";

  const auto lease = pipelines->AcquireEditorLease(ids.first);
  ASSERT_NE(lease.document_, nullptr);
  EXPECT_EQ(snapshot->Chain(),
            lease.graph_.ChainHashForHead(lease.graph_.GetActiveVersionRef().head_commit_hash));
  EXPECT_EQ(snapshot->Document().ToJson(), lease.document_->ToJson());
  pipelines->ReleaseEditorLease(ids.first);
}

TEST_F(ThumbnailCommittedRenderTest, StoredSnapshotIsReusedUntilTheStoredHistoryChanges) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);

  const auto first  = pipelines->AcquireCommittedSnapshot(ids.first);
  const auto second = pipelines->AcquireCommittedSnapshot(ids.first);
  EXPECT_EQ(first, second);
  EXPECT_EQ(pipelines->CommittedSnapshotStorageLoadCount(), 1u);

  // A writer that persists history directly needs no invalidation call: the next acquire sees
  // that the stored labels moved. The writer here is an editor that publishes nothing.
  auto        writer       = OpenEditor(*pipelines, ids.first);
  const auto  before_state = writer.lease.graph_.GetImageEditState();
  const float stored_ev    = WorkingExposureJson(writer).at("exposure_ev").get<float>();
  const auto  head         = CommitExposure(writer, 1.0f);
  std::string error;
  ASSERT_TRUE(pipelines->PersistEditorHistory(writer.lease.graph_, before_state,
                                              writer.working->Document(), &error))
      << error;
  pipelines->ReleaseEditorLease(ids.first);

  const auto after = pipelines->AcquireCommittedSnapshot(ids.first);
  ASSERT_NE(after, first);
  EXPECT_EQ(after->Head(), head);
  EXPECT_EQ(pipelines->CommittedSnapshotStorageLoadCount(), 2u);
  const auto* exposure =
      after->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->ToJson().at("exposure_ev").get<float>(), stored_ev + 1.0f);
}

TEST_F(ThumbnailCommittedRenderTest, ImageWithoutHistoryRootFailsWithTheRealError) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  try {
    (void)pipelines.AcquireCommittedSnapshot(424242);
    FAIL() << "an image without a history root must not produce a snapshot";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string{error.what()}.find("has no edit history root"), std::string::npos)
        << error.what();
  }
}

TEST_F(ThumbnailCommittedRenderTest, PublishRejectsAPreviewSnapshot) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  EXPECT_THROW(pipelines.PublishCommitted(PipelineGraphSnapshot::Preview(
                   document, 7, PipelineLineageId::Next(), transaction_chain_hash_t{})),
               std::invalid_argument);
}

// ── ThumbnailService ───────────────────────────────────────────────────────────────────────────

TEST_F(ThumbnailCommittedRenderTest, ThumbnailRendersTheCommittedStateNotUncommittedEditorValues) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto editor = OpenEditor(*pipelines, ids.first);
  (void)PublishWorkingAsCommitted(*pipelines, editor);

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       committed = ThumbnailPixels(
      RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k256));
  ASSERT_FALSE(committed.empty());

  // An editor drag writes the working document and publishes its preview without committing.
  float committed_ev = 0.0f;
  {
    auto* exposure =
        editor.working->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
    ASSERT_NE(exposure, nullptr);
    committed_ev = exposure->ToJson().at("exposure_ev").get<float>();
    exposure->LoadJson({{"exposure_ev", committed_ev + 2.0f}});
    (void)editor.working->PublishPreview();
  }
  thumbnails.InvalidateThumbnail(ids.first);
  const auto during_drag = ThumbnailPixels(
      RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k256));
  EXPECT_EQ(MeanAbsoluteDifference(during_drag, committed), 0.0)
      << "uncommitted editor values must not reach a thumbnail";

  // The drag settles: the value is committed and the editor publishes the new state.
  editor.working->Document()
      .PrimaryGrade()
      ->FindAdjustmentByType(type_ids::Exposure())
      ->LoadJson({{"exposure_ev", committed_ev}});
  (void)CommitExposure(editor, 2.0f);
  (void)PublishWorkingAsCommitted(*pipelines, editor);
  thumbnails.InvalidateThumbnail(ids.first);
  const auto settled = ThumbnailPixels(
      RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k256));
  const double settled_difference = MeanAbsoluteDifference(settled, committed);
  std::cout << "[P4 pixels] committed +2 EV vs the opened state, mean absolute difference "
            << settled_difference << '\n';
  EXPECT_GT(settled_difference, kDistinctStateDifference)
      << "the committed +2 EV state must reach the thumbnail";

  thumbnails.ReleaseThumbnail(ids.first);
  pipelines->ReleaseEditorLease(ids.first);
}

TEST_F(ThumbnailCommittedRenderTest, DiskCacheEntriesAreLabelledWithTheRenderedCommittedState) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto       editor     = OpenEditor(*pipelines, ids.first);
  const auto root_state = PublishWorkingAsCommitted(*pipelines, editor);

  cv::Mat    root_pixels;
  cv::Mat    committed_pixels;
  std::shared_ptr<const PipelineGraphSnapshot> committed_state;
  {
    ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(),
                                pipelines, project.GetStorage(), project.GetProjectUUID(),
                                cache_root_);
    root_pixels = ThumbnailPixels(
        RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k256));
    ASSERT_FALSE(root_pixels.empty());

    // A render queued behind an editor commit renders, and is stored under, the state it read.
    (void)CommitExposure(editor, 1.5f);
    committed_state = PublishWorkingAsCommitted(*pipelines, editor);
    ASSERT_EQ(pipelines->AcquireCommittedSnapshot(ids.first), committed_state);
    // Drop only the memory entry; InvalidateThumbnail would also delete the disk entries.
    thumbnails.ReleaseThumbnail(ids.first);
    committed_pixels = ThumbnailPixels(
        RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k256));
    ASSERT_FALSE(committed_pixels.empty());
    thumbnails.ReleaseThumbnail(ids.first);
    thumbnails.FlushDiskCacheMetadata();
  }  // the destructor waits for the queued disk writes of this service's cache

  // A new service has an empty memory cache, so each request below is answered from the disk
  // entry of the label of whatever state is published at that moment.
  ThumbnailService reopened(project.GetSleeveService(), project.GetImagePoolService(), pipelines,
                            project.GetStorage(), project.GetProjectUUID(), cache_root_);
  const auto       hits_before = reopened.GetDiskCacheStats().hit_count;
  const auto       committed_from_disk =
      ThumbnailPixels(RequestThumbnail(reopened, ids.first, ids.second, ThumbnailResolution::k256));
  reopened.ReleaseThumbnail(ids.first);
  pipelines->PublishCommitted(root_state);
  const auto root_from_disk =
      ThumbnailPixels(RequestThumbnail(reopened, ids.first, ids.second, ThumbnailResolution::k256));
  reopened.ReleaseThumbnail(ids.first);
  EXPECT_EQ(reopened.GetDiskCacheStats().hit_count, hits_before + 2)
      << "both states must be served from their own disk entries";

  // The disk cache stores JPEG; its pixels match the delivered ones within JPEG error.
  const double root_error      = MeanAbsoluteDifference(root_from_disk, root_pixels);
  const double committed_error = MeanAbsoluteDifference(committed_from_disk, committed_pixels);
  const double states          = MeanAbsoluteDifference(root_pixels, committed_pixels);
  std::cout << "[P4 pixels] disk vs delivered: root " << root_error << ", committed "
            << committed_error << "; opened state vs +1.5 EV " << states << '\n';
  EXPECT_GT(states, kDistinctStateDifference) << "the two labels must hold different states";
  EXPECT_LT(root_error, kJpegDifference) << "the root label holds the root pixels";
  EXPECT_LT(committed_error, kJpegDifference) << "the head label holds the committed pixels";

  pipelines->ReleaseEditorLease(ids.first);
}

TEST_F(ThumbnailCommittedRenderTest, ThumbnailRendersWhileTheEditorHoldsTheRenderLockOfTheImage) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto editor = OpenEditor(*pipelines, ids.first);
  (void)PublishWorkingAsCommitted(*pipelines, editor);
  pipelines->ResetPipelineAcquireCountsForTesting();
  // One editor frame binds the editor executor to the image before the thumbnail request.
  {
    const auto input =
        raw_import_test::LoadEncodedInput(*project.GetImagePoolService(), ids.second);
    ASSERT_NE(input, nullptr);
    PipelineApplyRequest request;
    request.geometry.resolution.max_edge = 1024;
    request.geometry.resolution.quality  = RenderQuality::Export;
    request.decode_res                   = DecodeRes::EIGHTH;
    request.role                         = ExecutorRole::Interactive;
    request.require_host_output          = true;
    std::lock_guard<std::mutex> render_lock(editor.executor->GetRenderLock());
    ASSERT_NE(editor.executor->Apply(*editor.working->CurrentPreview(), input, request), nullptr);
  }

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  // An editor frame holds this lock from configure through present. Before P4 a thumbnail of
  // the same image queued on it for the whole frame.
  std::unique_lock<std::mutex>         editor_frame(editor.executor->GetRenderLock());
  std::promise<ThumbnailRequestResult> done;
  auto                                 done_future = done.get_future();
  thumbnails.GetThumbnailDetailed(
      ids.first, ids.second,
      [&done](ThumbnailRequestResult result) { done.set_value(std::move(result)); }, true, nullptr,
      ThumbnailResolution::k512);
  ASSERT_EQ(done_future.wait_for(120s), std::future_status::ready)
      << "the thumbnail waited for the editor's render lock";
  const auto result = done_future.get();
  editor_frame.unlock();
  EXPECT_EQ(result.status, ThumbnailRequestStatus::kReady) << result.message;
  EXPECT_FALSE(HasBatchRenderer(*editor.executor));
  EXPECT_EQ(pipelines->PipelineLoadCount(), 0u);

  thumbnails.ReleaseThumbnail(ids.first);
  pipelines->ReleaseEditorLease(ids.first);
}

TEST_F(ThumbnailCommittedRenderTest, EditorFrameLatencyStaysUnchangedWhileThumbnailsRender) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto editor = OpenEditor(*pipelines, ids.first);
  (void)PublishWorkingAsCommitted(*pipelines, editor);

  PipelineScheduler editor_worker(1);
  const auto        run_editor_frame = [&]() -> double {
    PipelineTask task;
    task.pipeline_executor_          = editor.executor;
    task.snapshot_under_render_lock_ = [&editor]() { return editor.working->CurrentPreview(); };
    task.input_desc_ = std::make_shared<Image>(LinearDngPath(), ImageType::DEFAULT);
    task.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
    task.options_.is_blocking_              = true;
    task.result_        = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
    auto       blocking = task.result_->get_future();
    const auto start    = std::chrono::steady_clock::now();
    editor_worker.ScheduleTask(std::move(task));
    EXPECT_EQ(blocking.wait_for(120s), std::future_status::ready);
    EXPECT_NE(blocking.get(), nullptr);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
  };
  const auto median = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };

  (void)run_editor_frame();  // first frame decodes the source
  std::vector<double> idle;
  for (int i = 0; i < 5; ++i) {
    idle.push_back(run_editor_frame());
  }

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  std::vector<double> thumbnail_ms;
  {
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(
        RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k2048).status,
        ThumbnailRequestStatus::kReady);
    thumbnail_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count());
  }

  // Keep the thumbnail workers rendering the same image at the largest tier while the editor
  // renders frames.
  std::atomic<bool> stop{false};
  std::atomic<int>  thumbnails_rendered{0};
  std::thread       load([&] {
    while (!stop.load()) {
      thumbnails.InvalidateThumbnail(ids.first);
      const auto start = std::chrono::steady_clock::now();
      const auto result =
          RequestThumbnail(thumbnails, ids.first, ids.second, ThumbnailResolution::k2048);
      if (result.status == ThumbnailRequestStatus::kReady) {
        thumbnails_rendered.fetch_add(1);
        thumbnail_ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
      }
    }
  });
  std::this_thread::sleep_for(50ms);
  std::vector<double> loaded;
  for (int i = 0; i < 5; ++i) {
    loaded.push_back(run_editor_frame());
  }
  stop = true;
  load.join();

  const double idle_ms      = median(idle);
  const double loaded_ms    = median(loaded);
  const double thumbnail_md = median(thumbnail_ms);
  std::cout << "[P4 latency] editor frame idle median " << idle_ms << " ms, during thumbnails "
            << loaded_ms << " ms; k2048 thumbnail median " << thumbnail_md << " ms; thumbnails "
            << thumbnails_rendered.load() << '\n';
  EXPECT_GT(thumbnails_rendered.load(), 0);
  // An editor frame shares the GPU with the thumbnail render but never waits for it to finish:
  // before P4 each frame queued on the shared render lock behind a whole thumbnail render.
  EXPECT_LT(loaded_ms, idle_ms + thumbnail_md)
      << "editor frames are serialized behind thumbnail renders";

  pipelines->ReleaseEditorLease(ids.first);
}

}  // namespace
}  // namespace alcedo
