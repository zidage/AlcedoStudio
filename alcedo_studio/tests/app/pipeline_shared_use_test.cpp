//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
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
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/renderer.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "io/image/export_recipe.hpp"
#include "io/image/image_loader.hpp"
#include "json.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "sleeve/storage.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
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
using raw_import_test::BindImportedRawColor;
using raw_import_test::HostPixels;
using raw_import_test::ImportLinearDng;
using raw_import_test::ImportRawFile;
using raw_import_test::LinearDngPath;

/// Read every channel of an 8- or 16-bit image file at its stored depth; empty on failure.
auto ReadImagePixels(const std::filesystem::path& path) -> cv::Mat {
  auto input = OIIO::ImageInput::open(path.string());
  if (!input) {
    return {};
  }
  const OIIO::ImageSpec spec    = input->spec();
  const bool            sixteen = spec.format == OIIO::TypeDesc::UINT16;
  cv::Mat pixels(spec.height, spec.width, CV_MAKETYPE(sixteen ? CV_16U : CV_8U, spec.nchannels));
  const bool ok = input->read_image(0, 0, 0, spec.nchannels,
                                    sixteen ? OIIO::TypeDesc::UINT16 : OIIO::TypeDesc::UINT8,
                                    pixels.data);
  input->close();
  return ok ? pixels : cv::Mat{};
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

/// Resources of the interactive (editor) renderer of @p executor; empty when none exists.
auto InteractiveWorkspace(PipelineExecutor& executor) -> RenderSessionResources {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->Resources();
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->Resources();
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->Resources();
  }
#endif
  return {};
}

/// Resources of the batch (thumbnail, analysis, export) renderer of @p executor; empty when none
/// exists.
auto BatchWorkspace(PipelineExecutor& executor) -> RenderSessionResources {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaBatchRenderer()) {
    return renderer->Resources();
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalBatchRenderer()) {
    return renderer->Resources();
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClBatchRenderer()) {
    return renderer->Resources();
  }
#endif
  return {};
}

/// True when @p executor created a batch renderer, i.e. a thumbnail, analysis, or export render
/// ran on it.
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

auto InteractivePreparedSourceCount(PipelineExecutor& executor) -> std::size_t {
  return InteractiveWorkspace(executor).prepared_source_entry_count;
}

auto InteractiveTexturePoolEntries(PipelineExecutor& executor) -> std::size_t {
  return InteractiveWorkspace(executor).texture_pool_entry_count;
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
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }
};

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
    task.snapshot_under_render_lock_        = MakeLiveSnapshotSource(extra);
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
}

TEST_F(PipelineSharedUseTest, ThumbnailAndAnalysisLeaveTheLiveGuardUntouched) {
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
  PipelineExecutor* const    executor = live->pipeline_.get();
  PipelineDocument* const    document = live->document_.get();
  pipelines->ResetPipelineAcquireCountsForTesting();
  ThumbnailService           thumbnails(project.GetSleeveService(), project.GetImagePoolService(),
                                          pipelines);
  const auto first = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                 ThumbnailResolution::k256);
  EXPECT_EQ(first.status, ThumbnailRequestStatus::kReady) << first.message;
  ASSERT_NE(first.guard, nullptr);
  ASSERT_NE(first.guard->thumbnail_buffer_, nullptr);
  EXPECT_EQ(live->pipeline_.get(), executor);
  EXPECT_EQ(live->document_.get(), document);
  EXPECT_EQ(InteractivePreparedSourceCount(*live->pipeline_), 0u);
  EXPECT_EQ(InteractiveTexturePoolEntries(*live->pipeline_), 0u);
  EXPECT_FALSE(HasBatchRenderer(*live->pipeline_));

  std::promise<ThumbnailRequestResult> analysis_done;
  auto                                 analysis_fut = analysis_done.get_future();
  const auto                           rendition =
      thumbnails.RequestAnalysisRendition(ids.first, ids.second, ThumbnailResolution::k256,
                                          [&analysis_done](ThumbnailRequestResult result) {
                                            analysis_done.set_value(std::move(result));
                                          });
  ASSERT_EQ(analysis_fut.wait_for(60s), std::future_status::ready);
  const auto analysis = analysis_fut.get();
  EXPECT_EQ(analysis.status, ThumbnailRequestStatus::kReady) << analysis.message;
  ASSERT_NE(analysis.guard, nullptr);
  ASSERT_NE(analysis.guard->thumbnail_buffer_, nullptr);
  EXPECT_EQ(live->pipeline_.get(), executor);
  EXPECT_EQ(live->document_.get(), document);
  EXPECT_EQ(live->pin_count_, size_t{1});
  EXPECT_FALSE(HasBatchRenderer(*live->pipeline_));
  // Neither render loaded a guard: both read the committed snapshot.
  EXPECT_EQ(pipelines->PipelineLoadCount(), 0u);

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  thumbnails.ReleaseAnalysisRendition(rendition);
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, TaskRenderOptionsDoNotLeakIntoLaterEditorRequests) {
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

  {
    auto extra = pipelines->LoadPipeline(ids.first);
    PipelineScheduler scheduler(1);
    std::promise<void> done;
    auto                done_fut = done.get_future();
    PipelineTask        task;
    task.pipeline_executor_                  = extra->pipeline_;
    task.snapshot_under_render_lock_         = MakeLiveSnapshotSource(extra);
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
  // Thumbnail options live only in their own apply request. A later editor request on the same
  // executor keeps the editor settings.
  PipelineTask editor;
  editor.pipeline_executor_                 = live->pipeline_;
  editor.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
  const auto editor_request                 = editor.MakeApplyRequest();
  EXPECT_EQ(editor_request.role, ExecutorRole::Interactive);
  EXPECT_FALSE(editor_request.require_host_output);
  EXPECT_EQ(editor_request.decode_res, DecodeRes::FULL);

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
    PipelineApplyRequest request;
    request.geometry.resolution.max_edge = 4096;
    request.geometry.resolution.quality  = RenderQuality::Export;
    request.decode_res                   = DecodeRes::EIGHTH;
    request.role                         = ExecutorRole::Interactive;
    request.require_host_output          = true;
    const auto snapshot                  = live->FreezeLiveSnapshot();
    ASSERT_NO_THROW(live->pipeline_->Apply(*snapshot, input, request));
  }
  const auto prepared_before = InteractivePreparedSourceCount(*live->pipeline_);
  const auto session_textures_before = InteractiveTexturePoolEntries(*live->pipeline_);
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
  EXPECT_EQ(InteractivePreparedSourceCount(*live->pipeline_), prepared_before);
  EXPECT_EQ(InteractiveTexturePoolEntries(*live->pipeline_), session_textures_before);
  EXPECT_FALSE(HasBatchRenderer(*live->pipeline_));

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
}

TEST_F(PipelineSharedUseTest, AnalysisAndExportLeaveLiveEditsUnchanged) {
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
  PipelineExecutor* const    executor  = live->pipeline_.get();
  PipelineDocument* const    document = live->document_.get();
  const auto                 live_json = live->document_->ToJson();

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  std::promise<ThumbnailRequestResult> analysis_done;
  auto                                 analysis_fut = analysis_done.get_future();
  const auto                           rendition =
      thumbnails.RequestAnalysisRendition(ids.first, ids.second, ThumbnailResolution::k256,
                                          [&analysis_done](ThumbnailRequestResult result) {
                                            analysis_done.set_value(std::move(result));
                                          });
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

  thumbnails.ReleaseAnalysisRendition(rendition);
  pipelines->SavePipeline(live);
  std::error_code ec;
  std::filesystem::remove_all(export_dir, ec);
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
    task.snapshot_under_render_lock_        = MakeLiveSnapshotSource(live);
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
  const auto session_after_editor = InteractiveWorkspace(*live->pipeline_);
  EXPECT_GT(session_after_editor.prepared_source_entry_count +
                session_after_editor.published_result_count,
            0u);

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       thumb = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                        ThumbnailResolution::k256);
  EXPECT_EQ(thumb.status, ThumbnailRequestStatus::kReady) << thumb.message;
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 10s));
  run_preview();
  const auto session_after_reuse = InteractiveWorkspace(*live->pipeline_);
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
  const auto session_before = InteractiveWorkspace(*live->pipeline_);
  PipelineScheduler scheduler(1);
  std::promise<void> done;
  auto               done_fut = done.get_future();
  auto extra                  = pipelines->LoadPipeline(ids.first);
  PipelineTask task;
  task.pipeline_executor_                 = extra->pipeline_;
  task.snapshot_under_render_lock_        = MakeLiveSnapshotSource(extra);
  task.input_                             = std::make_shared<ImageBuffer>(std::vector<uint8_t>{0});
  task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  task.on_complete_                       = [pipelines, extra, &done](bool, std::string) {
    pipelines->ReleasePipelineUse(extra);
    done.set_value();
  };
  scheduler.ScheduleTask(std::move(task));
  ASSERT_EQ(done_fut.wait_for(30s), std::future_status::ready);
  ASSERT_TRUE(pipelines->WaitUntilPinCount(live, 1, 5s));
  const auto batch = BatchWorkspace(*live->pipeline_);
  EXPECT_EQ(batch.texture_pool_used_bytes, 0u);
  EXPECT_EQ(batch.transient_used_bytes, 0u);
  EXPECT_EQ(batch.transient_slab_count, 0u);
  EXPECT_EQ(batch.published_result_count, 0u);
  const auto session_after = InteractiveWorkspace(*live->pipeline_);
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
  export_task.recipe_                   = ExportRecipe::FromLegacyOptions(export_task.options_);
  export_service.EnqueueExportTask(export_task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> export_done;
  auto export_fut = export_done.get_future();

  std::atomic<std::size_t> peak_batch_bytes{0};
  std::thread observer([&] {
    while (thumb_fut.wait_for(0s) != std::future_status::ready ||
           export_fut.wait_for(0s) != std::future_status::ready) {
      const auto shot_a = BatchWorkspace(*live_a->pipeline_);
      const auto shot_b = BatchWorkspace(*live_b->pipeline_);
      const auto used   = shot_a.texture_pool_used_bytes + shot_a.transient_capacity_bytes +
                        shot_b.texture_pool_used_bytes + shot_b.transient_capacity_bytes;
      auto prev = peak_batch_bytes.load();
      while (used > prev && !peak_batch_bytes.compare_exchange_weak(prev, used)) {
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
  const auto batch_a = BatchWorkspace(*live_a->pipeline_);
  const auto batch_b = BatchWorkspace(*live_b->pipeline_);
  EXPECT_EQ(batch_a.texture_pool_used_bytes, 0u);
  EXPECT_EQ(batch_a.transient_slab_count, 0u);
  EXPECT_EQ(batch_a.published_result_count, 0u);
  EXPECT_EQ(batch_b.texture_pool_used_bytes, 0u);
  EXPECT_EQ(batch_b.transient_slab_count, 0u);
  EXPECT_EQ(batch_b.published_result_count, 0u);
  std::cout << "NM1.4R parallel batch peak bytes (allocator, two images): "
            << peak_batch_bytes.load() << '\n';
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

// Thumbnail (k256) and 16-bit PNG export (256 px long edge) of mfzoty.dng render from a document
// without the executor's legacy stage table. Both render the committed state; the exposure
// +0.75 EV, crop, and 3 degree rotation set on the live document reach neither.
TEST_F(PipelineSharedUseTest, ThumbnailAndExportRenderFromDocumentOnly) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);
  {
    std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
    auto* exposure = live->document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
    ASSERT_NE(exposure, nullptr);
    exposure->LoadJson({{"exposure_ev", 0.75f}});
    live->document_->Geometry().SetCropRect({0.1f, 0.15f, 0.7f, 0.6f});
    live->document_->Geometry().SetRotationDegrees(3.0f);
    live->dirty_ = true;
  }

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       thumbnail = GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second,
                                                            ThumbnailResolution::k256);
  ASSERT_EQ(thumbnail.status, ThumbnailRequestStatus::kReady) << thumbnail.message;
  ASSERT_NE(thumbnail.guard, nullptr);
  ASSERT_NE(thumbnail.guard->thumbnail_buffer_, nullptr);
  const cv::Mat thumbnail_pixels = HostPixels(*thumbnail.guard->thumbnail_buffer_);
  ASSERT_FALSE(thumbnail_pixels.empty());

  const auto export_dir =
      std::filesystem::temp_directory_path() /
      ("pipeline_document_only_export_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(export_dir);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  ExportTask    task;
  task.sleeve_id_                = ids.first;
  task.image_id_                 = ids.second;
  task.options_.format_          = ImageFormatType::PNG;
  task.options_.bit_depth_       = ExportFormatOptions::BIT_DEPTH::BIT_16;
  task.options_.export_path_     = export_dir / "document-only.png";
  task.options_.resize_enabled_  = true;
  task.options_.max_length_side_ = 256;
  task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
  export_service.EnqueueExportTask(task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> export_done;
  auto export_fut = export_done.get_future();
  export_service.ExportAll([&export_done](std::shared_ptr<std::vector<ExportResult>> results) {
    export_done.set_value(std::move(results));
  });
  ASSERT_EQ(export_fut.wait_for(120s), std::future_status::ready);
  auto export_results = export_fut.get();
  ASSERT_NE(export_results, nullptr);
  ASSERT_EQ(export_results->size(), 1u);
  ASSERT_TRUE((*export_results)[0].success_) << (*export_results)[0].message_;
  const cv::Mat export_pixels = ReadImagePixels((*export_results)[0].output_path_);
  ASSERT_FALSE(export_pixels.empty()) << (*export_results)[0].output_path_.string();
  ASSERT_EQ(export_pixels.depth(), CV_16U);
  EXPECT_EQ(std::max(export_pixels.cols, export_pixels.rows), 256);

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  pipelines->SavePipeline(live);
  std::error_code ec;
  std::filesystem::remove_all(export_dir, ec);
}

// A separate fixture with no operator registration: ctest runs each discovered test in its own
// process, so nothing in this process fills the legacy operator registry. At 5708f139 the
// executor constructor built the stage table through that registry and this test crashed.
class PipelineExecutorWithoutOperatorRegistryTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void SetUp() override {
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    db_path_   = std::filesystem::temp_directory_path() / ("executor_no_registry_" + stamp + ".db");
    meta_path_ = std::filesystem::temp_directory_path() / ("executor_no_registry_" + stamp + ".json");
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }
};

TEST_F(PipelineExecutorWithoutOperatorRegistryTest, ExecutorConstructsWithoutOperatorRegistry) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  auto live = pipelines->LoadPipeline(ids.first);
  ASSERT_NE(live, nullptr);

  auto executor = std::make_shared<PipelineExecutor>(ExecutorRole::Batch);
  std::shared_ptr<const PipelineGraphSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> live_lock(live->pipeline_->GetRenderLock());
    snapshot = live->FreezeLiveSnapshot();
  }
  ASSERT_NE(snapshot, nullptr);

  auto img = project.GetImagePoolService()->Read<std::shared_ptr<Image>>(
      ids.second, [](const std::shared_ptr<Image>& image) { return image; });
  ASSERT_NE(img, nullptr);
  auto input = std::make_shared<ImageBuffer>(ByteBufferLoader::LoadByteBufferFromImage(img));

  PipelineApplyRequest request;
  request.decode_res                   = DecodeRes::EIGHTH;
  request.role                         = ExecutorRole::Batch;
  request.require_host_output          = true;
  request.geometry.resolution.max_edge = 256;
  std::shared_ptr<ImageBuffer> output;
  {
    std::lock_guard<std::mutex> render_lock(executor->GetRenderLock());
    ASSERT_NO_THROW(output = executor->Apply(*snapshot, input, request));
  }
  ASSERT_NE(output, nullptr);
  const cv::Mat pixels = HostPixels(*output);
  ASSERT_FALSE(pixels.empty());
  EXPECT_LE(std::max(pixels.cols, pixels.rows), 256);
  pipelines->SavePipeline(live);
}

}  // namespace alcedo
