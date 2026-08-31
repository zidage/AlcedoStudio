//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "app/export_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_disk_cache_service.hpp"
#include "app/thumbnail_service.hpp"
#include "app/thumbnail_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/runtime/renderer.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "io/image/export_recipe.hpp"
#include "io/image/image_loader.hpp"
#include "json.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "sleeve/storage.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

#ifdef HAVE_CUDA
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#endif
#ifdef HAVE_METAL
#include "edit/runtime/metal/metal_renderer.hpp"
#endif
#ifdef HAVE_OPENCL
#include "edit/runtime/opencl/opencl_renderer.hpp"
#endif

namespace alcedo {
namespace {
using namespace std::chrono_literals;

auto LinearDngPath() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng" / "mfzoty.dng";
}

auto ImportRawFile(ProjectService& project, std::shared_ptr<PipelineMgmtService> pipelines,
                   const std::filesystem::path& raw_path) -> std::pair<sl_element_id_t, image_id_t> {
  if (!std::filesystem::exists(raw_path) || !pipelines) {
    return {0, 0};
  }
  auto              fs_service = project.GetSleeveService();
  auto              img_pool   = project.GetImagePoolService();
  ImportServiceImpl import_service(fs_service, img_pool, pipelines);
  auto              import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> imported;
  auto                       imported_future = imported.get_future();
  import_job->on_finished_                   = [&imported](const ImportResult& result) {
    imported.set_value(result);
  };
  import_job = import_service.ImportToFolder({raw_path}, L"", {}, import_job);
  if (!import_job) {
    return {0, 0};
  }
  if (imported_future.wait_for(60s) != std::future_status::ready) {
    return {0, 0};
  }
  if (imported_future.get().imported_ != 1u || !import_job->import_log_) {
    return {0, 0};
  }
  const auto snapshot = import_job->import_log_->Snapshot();
  if (snapshot.created_.size() != 1u) {
    return {0, 0};
  }
  import_service.SyncImports(snapshot, L"");
  project.GetSleeveService()->Sync();
  project.GetImagePoolService()->SyncWithStorage();
  return {snapshot.created_.front().element_id_, snapshot.created_.front().image_id_};
}

auto ImportLinearDng(ProjectService& project, std::shared_ptr<PipelineMgmtService> pipelines)
    -> std::pair<sl_element_id_t, image_id_t> {
  return ImportRawFile(project, pipelines, LinearDngPath());
}

auto GetThumbnailDetailedBlocking(ThumbnailService& service, sl_element_id_t id,
                                   image_id_t image_id, ThumbnailResolution resolution)
    -> ThumbnailRequestResult {
  std::promise<ThumbnailRequestResult> done;
  auto                                fut = done.get_future();
  service.GetThumbnailDetailed(
      id, image_id, [&done](ThumbnailRequestResult result) { done.set_value(std::move(result)); },
      true, nullptr, resolution);
  EXPECT_EQ(fut.wait_for(60s), std::future_status::ready);
  return fut.get();
}

auto SessionPreparedSourceCount(CPUPipelineExecutor& executor) -> std::size_t {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->SessionResources().prepared_source_entry_count;
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->SessionResources().prepared_source_entry_count;
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->SessionResources().prepared_source_entry_count;
  }
#endif
  return 0;
}

auto OneShotWorkspace(CPUPipelineExecutor& executor) -> RenderSessionResources {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->OneShotResources();
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->OneShotResources();
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->OneShotResources();
  }
#endif
  return {};
}

auto SessionWorkspace(CPUPipelineExecutor& executor) -> RenderSessionResources {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->SessionResources();
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->SessionResources();
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->SessionResources();
  }
#endif
  return {};
}

auto SessionTexturePoolEntries(CPUPipelineExecutor& executor) -> std::size_t {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->SessionResources().texture_pool_entry_count;
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->SessionResources().texture_pool_entry_count;
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->SessionResources().texture_pool_entry_count;
  }
#endif
  return 0;
}

auto OneShotPublishedResultCount(CPUPipelineExecutor& executor) -> std::size_t {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->OneShotPublishedResultCount();
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->OneShotPublishedResultCount();
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->OneShotPublishedResultCount();
  }
#endif
  return 0;
}

void BindImportedRawColor(const std::shared_ptr<PipelineGuard>& live, ImagePoolService& pool,
                           image_id_t image_id) {
  if (!live || !live->pipeline_) {
    return;
  }
  auto img = pool.Read<std::shared_ptr<Image>>(
      image_id, [](const std::shared_ptr<Image>& image) { return image; });
  if (!img || !img->HasRawColorContext()) {
    return;
  }
  std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
  live->pipeline_->InjectRawMetadata(img->GetRawColorContext());
}

}  // namespace

class PipelineSharedUseTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void SetUp() override {
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    db_path_   = std::filesystem::temp_directory_path() / ("pipeline_shared_use_" + stamp + ".db");
    meta_path_ = std::filesystem::temp_directory_path() / ("pipeline_shared_use_" + stamp + ".json");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    RegisterAllOperators();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }
};

TEST_F(PipelineSharedUseTest, ThumbnailDiskCacheWriteAllowedRejectsStalePreviewAndDirtyLabels) {
  EXPECT_TRUE(ThumbnailDiskCacheWriteAllowed("abc", "abc", false, false));
  EXPECT_FALSE(ThumbnailDiskCacheWriteAllowed("h1", "h2", false, false));
  EXPECT_FALSE(ThumbnailDiskCacheWriteAllowed("abc", "abc", true, false));
  EXPECT_FALSE(ThumbnailDiskCacheWriteAllowed("abc", "abc", false, true));
  EXPECT_FALSE(ThumbnailDiskCacheWriteAllowed("", "abc", false, false));
  EXPECT_FALSE(ThumbnailDiskCacheWriteAllowed("abc", "", false, false));
}

TEST_F(PipelineSharedUseTest, BackgroundCacheMissUsesNormalDocumentLoad) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  constexpr int       kThreads = 8;
  std::barrier        start(kThreads);
  std::vector<std::thread> workers;
  std::vector<std::shared_ptr<PipelineGuard>> guards(kThreads);
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&pipelines, &guards, &start, i] {
      start.arrive_and_wait();
      guards[i] = pipelines.LoadPipeline(1);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_NE(guards[0], nullptr);
  ASSERT_NE(guards[0]->pipeline_, nullptr);
  ASSERT_NE(guards[0]->document_, nullptr);
  for (int i = 1; i < kThreads; ++i) {
    ASSERT_NE(guards[i], nullptr);
    EXPECT_EQ(guards[i].get(), guards[0].get());
    EXPECT_EQ(guards[i]->pipeline_.get(), guards[0]->pipeline_.get());
    EXPECT_EQ(guards[i]->document_.get(), guards[0]->document_.get());
  }
  EXPECT_EQ(guards[0]->pin_count_, static_cast<size_t>(kThreads));
  for (auto& guard : guards) {
    pipelines.ReleasePipelineUse(guard);
  }
  EXPECT_EQ(guards[0]->pin_count_, size_t{0});
}

TEST_F(PipelineSharedUseTest, DocumentMutationWaitsForSharedRender) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  auto                live = pipelines.LoadPipeline(1);
  ASSERT_NE(live, nullptr);
  ASSERT_NE(live->document_, nullptr);
  auto* exposure = live->document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  const float before_ev = exposure->ToJson().at("exposure_ev").get<float>();

  PipelineScheduler scheduler(1);
  std::promise<void> in_render;
  std::promise<void> allow_finish;
  auto               in_render_fut   = in_render.get_future();
  auto               allow_finish_fut = allow_finish.get_future();

  PipelineTask task;
  task.pipeline_executor_                 = live->pipeline_;
  task.input_                             = std::make_shared<ImageBuffer>();
  task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  task.options_.is_blocking_              = false;
  task.options_.is_callback_              = false;
  task.configure_under_render_lock_      = [&](PipelineTask&) -> bool {
    in_render.set_value();
    allow_finish_fut.wait();
    return false;
  };

  scheduler.ScheduleTask(std::move(task));
  ASSERT_EQ(in_render_fut.wait_for(5s), std::future_status::ready);

  std::promise<void> mutation_started;
  auto               mutation_started_fut = mutation_started.get_future();
  auto               mutation               = std::async(std::launch::async, [&] {
    mutation_started.set_value();
    std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
    exposure->LoadJson({{"exposure_ev", 2.5f}});
  });
  mutation_started_fut.wait();
  EXPECT_EQ(mutation.wait_for(100ms), std::future_status::timeout);
  EXPECT_FLOAT_EQ(exposure->ToJson().at("exposure_ev").get<float>(), before_ev);

  allow_finish.set_value();
  mutation.wait();
  EXPECT_FLOAT_EQ(exposure->ToJson().at("exposure_ev").get<float>(), 2.5f);
  pipelines.ReleasePipelineUse(live);
}

TEST_F(PipelineSharedUseTest, CanceledAndFailedTaskReleasesPipelineUse) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto           live      = pipelines->LoadPipeline(1);
  ASSERT_NE(live, nullptr);
  ASSERT_EQ(live->pin_count_, size_t{1});

  {
    auto extra = pipelines->LoadPipeline(1);
    ASSERT_EQ(live->pin_count_, size_t{2});
    PipelineScheduler  scheduler(1);
    std::promise<bool> done;
    auto                done_fut = done.get_future();
    PipelineTask        task;
    task.pipeline_executor_                 = extra->pipeline_;
    task.input_                              = std::make_shared<ImageBuffer>(std::vector<uint8_t>{1, 2, 3});
    task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
    task.options_.is_blocking_              = false;
    task.configure_under_render_lock_       = [](PipelineTask&) -> bool {
      throw std::runtime_error("configure failed");
    };
    task.on_complete_ = [pipelines, extra, &done](bool, std::string) {
      pipelines->ReleasePipelineUse(extra);
      done.set_value(true);
    };
    scheduler.ScheduleTask(std::move(task));
    ASSERT_EQ(done_fut.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(live->pin_count_, size_t{1});
  }

  {
    auto extra = pipelines->LoadPipeline(1);
    ASSERT_EQ(live->pin_count_, size_t{2});
    PipelineScheduler  scheduler(1);
    std::promise<bool> done;
    auto                done_fut = done.get_future();
    PipelineTask        task;
    task.pipeline_executor_                 = extra->pipeline_;
    task.input_                              = std::make_shared<ImageBuffer>(std::vector<uint8_t>{1, 2, 3});
    task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
    task.options_.is_blocking_              = false;
    task.on_complete_ = [pipelines, extra, &done](bool, std::string) {
      pipelines->ReleasePipelineUse(extra);
      done.set_value(true);
    };
    scheduler.ScheduleTask(std::move(task));
    ASSERT_EQ(done_fut.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(live->pin_count_, size_t{1});
  }

  pipelines->ReleasePipelineUse(live);
  const auto ids = ImportLinearDng(project, pipelines);
  if (ids.first == 0) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  auto imported = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(imported, nullptr);
  BindImportedRawColor(imported, *project.GetImagePoolService(), ids.second);
  ThumbnailService live_thumbnails(project.GetSleeveService(), project.GetImagePoolService(),
                                    pipelines);
  std::unique_lock held(imported->pipeline_->GetRenderLock());
  std::promise<ThumbnailRequestResult> canceled;
  auto                                 canceled_fut = canceled.get_future();
  live_thumbnails.GetThumbnailDetailed(
      ids.first, ids.second,
      [&canceled](ThumbnailRequestResult result) { canceled.set_value(std::move(result)); }, true,
      nullptr, ThumbnailResolution::k256);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(imported, 2, 10s));
  live_thumbnails.CancelPending(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  held.unlock();
  ASSERT_EQ(canceled_fut.wait_for(30s), std::future_status::ready);
  EXPECT_EQ(canceled_fut.get().status, ThumbnailRequestStatus::kCanceled);
  EXPECT_TRUE(pipelines->WaitUntilPinCount(imported, 1, 5s));
  EXPECT_EQ(imported->pin_count_, size_t{1});
  pipelines->ReleasePipelineUse(imported);
}

TEST_F(PipelineSharedUseTest, BackgroundTasksReuseLivePipelineAndDocument) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService      project(db_path_, meta_path_);
  auto                pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto          ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  CPUPipelineExecutor* const executor = live->pipeline_.get();
  PipelineDocument* const    document = live->document_.get();
  ThumbnailService           thumbnails(project.GetSleeveService(), project.GetImagePoolService(),
                                          pipelines);
  const auto first = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                 ThumbnailResolution::k256);
  EXPECT_EQ(first.status, ThumbnailRequestStatus::kReady) << first.message;
  ASSERT_NE(first.guard, nullptr);
  ASSERT_NE(first.guard->thumbnail_buffer_, nullptr);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));
  EXPECT_EQ(live->pipeline_.get(), executor);
  EXPECT_EQ(live->document_.get(), document);
  EXPECT_EQ(SessionPreparedSourceCount(*live->pipeline_), 0u);
  EXPECT_EQ(SessionTexturePoolEntries(*live->pipeline_), 0u);
  EXPECT_EQ(OneShotPublishedResultCount(*live->pipeline_), 0u);

  std::promise<ThumbnailRequestResult> analysis_done;
  auto                                 analysis_fut = analysis_done.get_future();
  thumbnails.RequestAnalysisRendition(
      ids.first, ids.second, ThumbnailResolution::k256,
      [&analysis_done](ThumbnailRequestResult result) { analysis_done.set_value(std::move(result)); });
  ASSERT_EQ(analysis_fut.wait_for(60s), std::future_status::ready);
  const auto analysis = analysis_fut.get();
  EXPECT_EQ(analysis.status, ThumbnailRequestStatus::kReady) << analysis.message;
  ASSERT_NE(analysis.guard, nullptr);
  ASSERT_NE(analysis.guard->thumbnail_buffer_, nullptr);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));
  EXPECT_EQ(live->pipeline_.get(), executor);
  EXPECT_EQ(live->document_.get(), document);
  EXPECT_EQ(live->pin_count_, size_t{1});

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  thumbnails.ReleaseAnalysisRendition(analysis.key);
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, TaskRenderOptionsDoNotPersistInExecutor) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService      project(db_path_, meta_path_);
  auto                pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto          ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  CPUPipelineExecutor::OneShotRenderParamsSnapshot prior;
  {
    std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
    live->pipeline_->SetForceCPUOutput(false);
    live->pipeline_->SetEnableCache(true);
    live->pipeline_->SetDecodeRes(DecodeRes::FULL);
    prior = live->pipeline_->CaptureOneShotRenderParams();
  }

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       result = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                         ThumbnailResolution::k256);
  EXPECT_EQ(result.status, ThumbnailRequestStatus::kReady) << result.message;
  ASSERT_NE(result.guard, nullptr);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));

  CPUPipelineExecutor::OneShotRenderParamsSnapshot restored;
  {
    std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
    restored = live->pipeline_->CaptureOneShotRenderParams();
  }
  EXPECT_EQ(restored.enable_cache_, prior.enable_cache_);
  EXPECT_EQ(restored.force_cpu_output_, prior.force_cpu_output_);
  EXPECT_EQ(restored.decode_res_, prior.decode_res_);

  {
    auto extra = pipelines->LoadPipeline(ids.first);
    PipelineScheduler scheduler(1);
    std::promise<void> done;
    auto                done_fut = done.get_future();
    PipelineTask        task;
    task.pipeline_executor_                  = extra->pipeline_;
    task.input_                              = std::make_shared<ImageBuffer>(std::vector<uint8_t>{0});
    task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
    task.options_.is_blocking_              = false;
    task.on_complete_ = [pipelines, extra, &done](bool, std::string) {
      pipelines->ReleasePipelineUse(extra);
      done.set_value();
    };
    scheduler.ScheduleTask(std::move(task));
    ASSERT_EQ(done_fut.wait_for(10s), std::future_status::ready);
  }
  CPUPipelineExecutor::OneShotRenderParamsSnapshot after_failure;
  {
    std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
    after_failure = live->pipeline_->CaptureOneShotRenderParams();
  }
  EXPECT_EQ(after_failure.enable_cache_, prior.enable_cache_);
  EXPECT_EQ(after_failure.force_cpu_output_, prior.force_cpu_output_);
  EXPECT_EQ(after_failure.decode_res_, prior.decode_res_);

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, BackgroundReleaseDoesNotSaveOrClearEditorState) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService      project(db_path_, meta_path_);
  auto                pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto          ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  auto* exposure = live->document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  exposure->LoadJson({{"exposure_ev", 1.25f}});
  live->dirty_         = true;
  const auto live_json = live->document_->ToJson();
  const auto stored_before =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(ids.first);

  auto img = project.GetImagePoolService()->Read<std::shared_ptr<Image>>(
      ids.second, [](const std::shared_ptr<Image>& image) { return image; });
  ASSERT_NE(img, nullptr);
  auto encoded = ByteBufferLoader::LoadByteBufferFromImage(img);
  auto input   = std::make_shared<ImageBuffer>(std::move(encoded));
  {
    std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
    live->pipeline_->SetForceCPUOutput(true);
    live->pipeline_->SetEnableCache(true);
    live->pipeline_->SetDecodeRes(DecodeRes::EIGHTH);
    ASSERT_NO_THROW(live->pipeline_->Apply(input));
  }
  const auto prepared_before = SessionPreparedSourceCount(*live->pipeline_);
  const auto session_textures_before = SessionTexturePoolEntries(*live->pipeline_);
  EXPECT_GT(prepared_before, 0u);

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       result = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                        ThumbnailResolution::k256);
  EXPECT_EQ(result.status, ThumbnailRequestStatus::kReady) << result.message;
  EXPECT_TRUE(live->dirty_);
  EXPECT_EQ(live->pin_count_, size_t{1});
  EXPECT_EQ(live->document_->ToJson(), live_json);
  const auto stored_after =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(ids.first);
  EXPECT_EQ(stored_after, stored_before);
  EXPECT_EQ(SessionPreparedSourceCount(*live->pipeline_), prepared_before);
  EXPECT_EQ(SessionTexturePoolEntries(*live->pipeline_), session_textures_before);
  EXPECT_EQ(OneShotPublishedResultCount(*live->pipeline_), 0u);

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, AnalysisAndExportUseSharedExecutorWithoutChangingEdits) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService      project(db_path_, meta_path_);
  auto                pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto          ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  CPUPipelineExecutor* const executor = live->pipeline_.get();
  PipelineDocument* const    document = live->document_.get();
  const auto                 live_json = live->document_->ToJson();

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  std::promise<ThumbnailRequestResult> analysis_done;
  auto                                 analysis_fut = analysis_done.get_future();
  thumbnails.RequestAnalysisRendition(
      ids.first, ids.second, ThumbnailResolution::k256,
      [&analysis_done](ThumbnailRequestResult result) { analysis_done.set_value(std::move(result)); });
  ASSERT_EQ(analysis_fut.wait_for(60s), std::future_status::ready);
  const auto analysis = analysis_fut.get();
  EXPECT_EQ(analysis.status, ThumbnailRequestStatus::kReady) << analysis.message;
  ASSERT_NE(analysis.guard, nullptr);
  EXPECT_EQ(live->pipeline_.get(), executor);
  EXPECT_EQ(live->document_.get(), document);

  const auto export_dir =
      std::filesystem::temp_directory_path() /
      ("pipeline_shared_export_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(export_dir);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  ExportTask    task;
  task.sleeve_id_                = ids.first;
  task.image_id_                 = ids.second;
  task.options_.format_          = ImageFormatType::JPEG;
  task.options_.export_path_     = export_dir / "shared-use.jpg";
  task.options_.resize_enabled_  = true;
  task.options_.max_length_side_ = 256;
  task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
  {
    std::lock_guard<std::mutex> lock(live->pipeline_->GetRenderLock());
    task.recipe_->output_color_ =
        ExportColorProfileFromDrt(live->document_->Drt()->Params().Params());
  }
  export_service.EnqueueExportTask(task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> export_done;
  auto export_fut = export_done.get_future();
  export_service.ExportAll(
      [&export_done](std::shared_ptr<std::vector<ExportResult>> results) {
        export_done.set_value(std::move(results));
      });
  ASSERT_EQ(export_fut.wait_for(120s), std::future_status::ready);
  auto export_results = export_fut.get();
  ASSERT_NE(export_results, nullptr);
  ASSERT_EQ(export_results->size(), 1u);
  EXPECT_TRUE((*export_results)[0].success_) << (*export_results)[0].message_;
  EXPECT_EQ(live->pipeline_.get(), executor);
  EXPECT_EQ(live->document_.get(), document);
  EXPECT_EQ(live->document_->ToJson(), live_json);
  EXPECT_EQ(live->pin_count_, size_t{1});

  thumbnails.ReleaseAnalysisRendition(analysis.key);
  pipelines->SavePipeline(live);
  std::error_code ec;
  std::filesystem::remove_all(export_dir, ec);
}

TEST_F(PipelineSharedUseTest, QueuedRenderDoesNotStorePixelsUnderStaleCommitLabel) {
  EXPECT_FALSE(ThumbnailDiskCacheWriteAllowed("old", "new", false, false));
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadEditorPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  ASSERT_NE(live->commit_graph_, nullptr);
  live->dirty_              = true;
  live->unsettled_preview_  = true;

  const auto cache_root =
      std::filesystem::temp_directory_path() /
      ("pipeline_shared_disk_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines,
                               project.GetStorage(), project.GetProjectUUID(), cache_root);
  const auto result = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                    ThumbnailResolution::k256);
  EXPECT_EQ(result.status, ThumbnailRequestStatus::kReady) << result.message;
  ASSERT_NE(result.guard, nullptr);
  EXPECT_TRUE(live->dirty_);
  EXPECT_TRUE(live->unsettled_preview_);
  EXPECT_EQ(live->pin_count_, size_t{1});

  thumbnails.FlushDiskCacheMetadata();
  ThumbnailDiskCacheKey key;
  key.project_uuid         = project.GetProjectUUID();
  key.element_id           = ids.first;
  key.resolution           = ThumbnailResolution::k256;
  key.purpose              = ThumbnailDiskCachePurpose::kThumbnail;
  key.cache_schema_version  = 2;
  const auto head          = live->working_head_commit_hash();
  key.edit_version_hash    = head.has_value() ? head->ToString() : live->root_id_.ToString();
  ThumbnailDiskCacheService disk(cache_root);
  disk.Initialize(project.GetProjectUUID());
  EXPECT_FALSE(disk.Lookup(key));

  live->dirty_             = false;
  live->unsettled_preview_ = false;
  std::unique_lock held(live->pipeline_->GetRenderLock());
  const auto queued_head = live->working_head_commit_hash();
  std::promise<ThumbnailRequestResult> queued;
  auto                                 queued_fut = queued.get_future();
  thumbnails.GetThumbnailDetailed(
      ids.first, ids.second,
      [&queued](ThumbnailRequestResult result) { queued.set_value(std::move(result)); }, true,
      nullptr, ThumbnailResolution::k256);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 2, 10s));
  OrdinaryEditPayload payload;
  payload.operator_type  = OperatorType::EXPOSURE;
  payload.stage_name     = PipelineStageName::Basic_Adjustment;
  payload.field_name     = "exposure";
  payload.before_value   = 0.0f;
  payload.after_value    = 0.25f;
  payload.before_enabled = true;
  payload.after_enabled  = true;
  auto commit = EditCommit::MakeEdit(live->root_id_, live->working_head_commit_hash(),
                                      std::move(payload));
  const auto new_head = commit.GetCommitHash();
  ASSERT_TRUE(live->commit_graph_->InsertCommit(std::move(commit)));
  live->commit_graph_->MoveWorkingHead(live->commit_graph_->GetActiveVersionId(), new_head);
  held.unlock();
  ASSERT_EQ(queued_fut.wait_for(60s), std::future_status::ready);
  EXPECT_EQ(queued_fut.get().status, ThumbnailRequestStatus::kReady);
  thumbnails.FlushDiskCacheMetadata();
  ThumbnailDiskCacheKey stale = key;
  if (queued_head.has_value()) {
    stale.edit_version_hash = queued_head->ToString();
  }
  ThumbnailDiskCacheKey fresh = key;
  fresh.edit_version_hash      = new_head.ToString();
  EXPECT_FALSE(disk.Lookup(stale));
  EXPECT_FALSE(disk.Lookup(fresh));

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
  std::error_code ec;
  std::filesystem::remove_all(cache_root, ec);
}

TEST_F(PipelineSharedUseTest, ConcurrentPipelineAcquirePublishesOneReadyLiveInstance) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  pipelines.ResetPipelineAcquireCountsForTesting();
  constexpr int kThreads = 8;
  std::barrier  start(kThreads);
  std::vector<std::thread> workers;
  std::vector<std::shared_ptr<PipelineGuard>> guards(kThreads);
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&pipelines, &guards, &start, i] {
      start.arrive_and_wait();
      guards[i] = pipelines.LoadPipeline(1);
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_NE(guards[0], nullptr);
  ASSERT_NE(guards[0]->pipeline_, nullptr);
  ASSERT_NE(guards[0]->document_, nullptr);
  EXPECT_TRUE(guards[0]->live_ready_);
  for (int i = 1; i < kThreads; ++i) {
    ASSERT_NE(guards[i], nullptr);
    EXPECT_EQ(guards[i].get(), guards[0].get());
    EXPECT_EQ(guards[i]->pipeline_.get(), guards[0]->pipeline_.get());
    EXPECT_TRUE(guards[i]->live_ready_);
  }
  EXPECT_EQ(pipelines.PipelineConstructCount(), 1u);
  EXPECT_EQ(pipelines.PipelineLoadCount(), static_cast<std::uint64_t>(kThreads));
  EXPECT_EQ(guards[0]->pin_count_, static_cast<size_t>(kThreads));
  for (auto& guard : guards) {
    pipelines.ReleasePipelineUse(guard);
  }
  EXPECT_TRUE(pipelines.WaitUntilPinCount(guards[0], 0, 5s));
}

TEST_F(PipelineSharedUseTest, PipelineReacquirePreventsStaleLastUseCleanup) {
  ProjectService      project(db_path_, meta_path_);
  PipelineMgmtService pipelines(project.GetStorage());
  auto                live = pipelines.LoadPipeline(1);
  ASSERT_NE(live, nullptr);
  ASSERT_TRUE(live->live_ready_);
  std::unique_lock<std::mutex> held(live->pipeline_->GetRenderLock());
  std::thread releaser([&pipelines, live] { pipelines.ReleasePipelineUse(live); });
  ASSERT_TRUE(pipelines.WaitUntilPinCount(live, 0, 5s));
  auto again = pipelines.LoadPipeline(1);
  ASSERT_EQ(again.get(), live.get());
  EXPECT_EQ(again->pin_count_, size_t{1});
  EXPECT_TRUE(again->live_ready_);
  held.unlock();
  releaser.join();
  EXPECT_TRUE(again->live_ready_);
  EXPECT_EQ(again->pin_count_, size_t{1});
  pipelines.ReleasePipelineUse(again);
}

TEST_F(PipelineSharedUseTest, BackgroundCompletionSignalsAfterPipelineUseRelease) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  std::atomic<int> completions{0};
  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  std::promise<ThumbnailRequestResult> done;
  auto                                 done_fut = done.get_future();
  thumbnails.GetThumbnailDetailed(
      ids.first, ids.second,
      [&done, &completions](ThumbnailRequestResult result) {
        completions.fetch_add(1, std::memory_order_relaxed);
        done.set_value(std::move(result));
      },
      true, nullptr, ThumbnailResolution::k256);
  ASSERT_EQ(done_fut.wait_for(60s), std::future_status::ready);
  EXPECT_EQ(done_fut.get().status, ThumbnailRequestStatus::kReady);
  EXPECT_EQ(completions.load(), 1);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));
  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, BackgroundRendersKeepEditorResultCacheReusable) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  PipelineScheduler editor(1);
  auto              run_preview = [&]() {
    PipelineTask task;
    task.pipeline_executor_                 = live->pipeline_;
    task.input_desc_                        = std::make_shared<Image>(LinearDngPath(), ImageType::DEFAULT);
    task.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
    task.options_.is_blocking_              = true;
    task.result_ = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
    auto blocking = task.result_->get_future();
    editor.ScheduleTask(std::move(task));
    EXPECT_EQ(blocking.wait_for(60s), std::future_status::ready);
    if (blocking.wait_for(0s) != std::future_status::ready) {
      return;
    }
    EXPECT_NE(blocking.get(), nullptr);
  };
  run_preview();
  const auto session_after_editor = SessionWorkspace(*live->pipeline_);
  EXPECT_GT(session_after_editor.prepared_source_entry_count +
                session_after_editor.published_result_count,
            0u);

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       thumb = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                        ThumbnailResolution::k256);
  EXPECT_EQ(thumb.status, ThumbnailRequestStatus::kReady) << thumb.message;
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));
  run_preview();
  const auto session_after_reuse = SessionWorkspace(*live->pipeline_);
  EXPECT_GE(session_after_reuse.prepared_source_entry_count,
            session_after_editor.prepared_source_entry_count);
  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, BackgroundTaskFailureReleasesTemporaryGpuResources) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  BindImportedRawColor(live, *project.GetImagePoolService(), ids.second);
  const auto session_before = SessionWorkspace(*live->pipeline_);
  PipelineScheduler scheduler(1);
  std::promise<void> done;
  auto               done_fut = done.get_future();
  auto extra                  = pipelines->LoadPipeline(ids.first);
  PipelineTask task;
  task.pipeline_executor_                 = extra->pipeline_;
  task.input_                             = std::make_shared<ImageBuffer>(std::vector<uint8_t>{0});
  task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  task.on_complete_                       = [pipelines, extra, &done](bool, std::string) {
    pipelines->ReleasePipelineUse(extra);
    done.set_value();
  };
  scheduler.ScheduleTask(std::move(task));
  ASSERT_EQ(done_fut.wait_for(30s), std::future_status::ready);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 5s));
  const auto one_shot = OneShotWorkspace(*live->pipeline_);
  EXPECT_EQ(one_shot.texture_pool_used_bytes, 0u);
  EXPECT_EQ(one_shot.transient_used_bytes, 0u);
  EXPECT_EQ(one_shot.transient_slab_count, 0u);
  EXPECT_EQ(one_shot.published_result_count, 0u);
  const auto session_after = SessionWorkspace(*live->pipeline_);
  EXPECT_EQ(session_after.texture_pool_used_bytes, session_before.texture_pool_used_bytes);
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, ParallelBackgroundRendersPreservePixelsAndReleaseWorkspaces) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     first     = ImportLinearDng(project, pipelines);
  ASSERT_NE(first.first, 0u);
  const auto copy_path =
      std::filesystem::temp_directory_path() /
      ("nm14r_parallel_b_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".dng");
  std::filesystem::copy_file(LinearDngPath(), copy_path);
  const auto second = ImportRawFile(project, pipelines, copy_path);
  ASSERT_NE(second.first, 0u);
  ASSERT_NE(second.first, first.first);
  auto live_a = pipelines->LoadPipeline(first.first);
  auto live_b = pipelines->LoadPipeline(second.first);
  BindImportedRawColor(live_a, *project.GetImagePoolService(), first.second);
  BindImportedRawColor(live_b, *project.GetImagePoolService(), second.second);

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  std::promise<ThumbnailRequestResult> thumb_done;
  auto                                 thumb_fut = thumb_done.get_future();
  const auto export_dir =
      std::filesystem::temp_directory_path() /
      ("nm14r_parallel_export_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(export_dir);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  ExportTask    export_task;
  export_task.sleeve_id_                = second.first;
  export_task.image_id_                 = second.second;
  export_task.options_.format_          = ImageFormatType::JPEG;
  export_task.options_.export_path_     = export_dir / "parallel.jpg";
  export_task.options_.resize_enabled_  = true;
  export_task.options_.max_length_side_ = 256;
  export_task.recipe_ = ExportRecipe::FromLegacyOptions(export_task.options_);
  {
    std::lock_guard<std::mutex> lock(live_b->pipeline_->GetRenderLock());
    export_task.recipe_->output_color_ =
        ExportColorProfileFromDrt(live_b->document_->Drt()->Params().Params());
  }
  export_service.EnqueueExportTask(export_task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> export_done;
  auto export_fut = export_done.get_future();

  std::atomic<std::size_t> peak_one_shot_bytes{0};
  std::thread observer([&] {
    while (thumb_fut.wait_for(0s) != std::future_status::ready ||
           export_fut.wait_for(0s) != std::future_status::ready) {
      const auto shot_a = OneShotWorkspace(*live_a->pipeline_);
      const auto shot_b = OneShotWorkspace(*live_b->pipeline_);
      const auto used   = shot_a.texture_pool_used_bytes + shot_a.transient_capacity_bytes +
                        shot_b.texture_pool_used_bytes + shot_b.transient_capacity_bytes;
      auto prev = peak_one_shot_bytes.load();
      while (used > prev && !peak_one_shot_bytes.compare_exchange_weak(prev, used)) {
      }
      std::this_thread::sleep_for(1ms);
    }
  });
  thumbnails.GetThumbnailDetailed(
      first.first, first.second,
      [&thumb_done](ThumbnailRequestResult result) { thumb_done.set_value(std::move(result)); },
      true, nullptr, ThumbnailResolution::k256);
  export_service.ExportAll([&export_done](std::shared_ptr<std::vector<ExportResult>> results) {
    export_done.set_value(std::move(results));
  });
  ASSERT_EQ(thumb_fut.wait_for(60s), std::future_status::ready);
  ASSERT_EQ(export_fut.wait_for(120s), std::future_status::ready);
  observer.join();
  EXPECT_EQ(thumb_fut.get().status, ThumbnailRequestStatus::kReady);
  auto export_results = export_fut.get();
  ASSERT_NE(export_results, nullptr);
  ASSERT_EQ(export_results->size(), 1u);
  EXPECT_TRUE((*export_results)[0].success_) << (*export_results)[0].message_;
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live_a, 1, 10s));
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live_b, 1, 10s));
  const auto one_shot_a = OneShotWorkspace(*live_a->pipeline_);
  const auto one_shot_b = OneShotWorkspace(*live_b->pipeline_);
  EXPECT_EQ(one_shot_a.texture_pool_used_bytes, 0u);
  EXPECT_EQ(one_shot_a.transient_slab_count, 0u);
  EXPECT_EQ(one_shot_a.published_result_count, 0u);
  EXPECT_EQ(one_shot_b.texture_pool_used_bytes, 0u);
  EXPECT_EQ(one_shot_b.transient_slab_count, 0u);
  EXPECT_EQ(one_shot_b.published_result_count, 0u);
  std::cout << "NM1.4R parallel one-shot peak bytes (allocator, two images): "
            << peak_one_shot_bytes.load() << '\n';
  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{first.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live_a);
  pipelines->SavePipeline(live_b);
  std::error_code ec;
  std::filesystem::remove_all(export_dir, ec);
  std::filesystem::remove(copy_path, ec);
}

TEST_F(PipelineSharedUseTest, ConcurrentThumbnailAndExportDoNotChangeDocumentOutputSettings) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live->document_->Drt(), nullptr);
  const auto before = live->document_->Drt()->Params().ToJson();
  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  std::promise<ThumbnailRequestResult> thumb_done;
  auto                                 thumb_fut = thumb_done.get_future();
  thumbnails.GetThumbnailDetailed(
      ids.first, ids.second,
      [&thumb_done](ThumbnailRequestResult result) { thumb_done.set_value(std::move(result)); },
      true, nullptr, ThumbnailResolution::k256);

  const auto export_dir =
      std::filesystem::temp_directory_path() /
      ("nm14r_export_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(export_dir);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  ExportTask    task;
  task.sleeve_id_                = ids.first;
  task.image_id_                 = ids.second;
  task.options_.format_          = ImageFormatType::JPEG;
  task.options_.export_path_     = export_dir / "overlay.jpg";
  task.options_.resize_enabled_  = true;
  task.options_.max_length_side_ = 256;
  task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
  task.recipe_->output_color_    = ExportColorProfileConfig{
      ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::GAMMA_2_2, 100.0f};
  export_service.EnqueueExportTask(task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> export_done;
  auto export_fut = export_done.get_future();
  export_service.ExportAll([&export_done](std::shared_ptr<std::vector<ExportResult>> results) {
    export_done.set_value(std::move(results));
  });
  ASSERT_EQ(thumb_fut.wait_for(60s), std::future_status::ready);
  ASSERT_EQ(export_fut.wait_for(120s), std::future_status::ready);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));
  EXPECT_EQ(live->document_->Drt()->Params().ToJson(), before);
  auto export_results = export_fut.get();
  ASSERT_NE(export_results, nullptr);
  ASSERT_EQ(export_results->size(), 1u);
  EXPECT_TRUE((*export_results)[0].success_) << (*export_results)[0].message_;
  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
  std::error_code ec;
  std::filesystem::remove_all(export_dir, ec);
}

}  // namespace alcedo
