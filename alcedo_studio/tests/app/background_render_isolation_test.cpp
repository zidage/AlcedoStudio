//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Background renders (thumbnails, analysis renditions, export) run on the executors of their own
// services and read committed snapshots
// (docs/refactor/2026-09-27-executor-ownership-refactor-plan.md, phase P7). The editor is
// modelled by what it owns since P6: the lease, an EditorWorkingDocument, and a test-owned
// Interactive executor.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_working_document.hpp"
#include "app/export_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_service.hpp"
#include "app/thumbnail_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
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
using raw_import_test::HostPixels;
using raw_import_test::ImportLinearDng;
using raw_import_test::LinearDngPath;
using raw_import_test::LoadEncodedInput;

auto GetThumbnailDetailedBlocking(ThumbnailService& service, sl_element_id_t id,
                                  image_id_t image_id, ThumbnailResolution resolution)
    -> ThumbnailRequestResult {
  std::promise<ThumbnailRequestResult> done;
  auto                                 fut = done.get_future();
  service.GetThumbnailDetailed(
      id, image_id, [&done](ThumbnailRequestResult result) { done.set_value(std::move(result)); },
      true, nullptr, resolution);
  EXPECT_EQ(fut.wait_for(60s), std::future_status::ready);
  return fut.get();
}

/// Interactive renderer of @p executor for its backend; @p visit is not called before the first
/// render.
template <class Visit>
void VisitInteractiveRenderer(PipelineExecutor& executor, Visit&& visit) {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    visit(*renderer);
    return;
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    visit(*renderer);
    return;
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    visit(*renderer);
    return;
  }
#endif
  (void)executor;
  (void)visit;
}

auto InteractiveResources(PipelineExecutor& executor) -> RenderSessionResources {
  RenderSessionResources resources;
  VisitInteractiveRenderer(executor, [&](auto& renderer) { resources = renderer.Resources(); });
  return resources;
}

auto InteractiveStats(PipelineExecutor& executor) -> RenderSessionStats {
  RenderSessionStats stats;
  VisitInteractiveRenderer(executor, [&](auto& renderer) { stats = renderer.Stats(); });
  return stats;
}

auto InteractiveBinding(PipelineExecutor& executor) -> std::optional<RenderBindingKey> {
  std::optional<RenderBindingKey> binding;
  VisitInteractiveRenderer(executor, [&](auto& renderer) { binding = renderer.Binding(); });
  return binding;
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

/// Largest per-channel difference; +inf when the images differ in size or type.
auto MaxAbsDifference(const cv::Mat& lhs, const cv::Mat& rhs) -> double {
  if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size() || lhs.type() != rhs.type()) {
    return std::numeric_limits<double>::infinity();
  }
  return cv::norm(lhs, rhs, cv::NORM_INF);
}

/// Render @p snapshot as one editor frame on @p executor and return the host pixels.
auto RenderEditorFrame(PipelineExecutor& executor, const PipelineGraphSnapshot& snapshot,
                       const std::shared_ptr<ImageBuffer>& input) -> cv::Mat {
  PipelineApplyRequest request;
  request.geometry.resolution.max_edge = 4096;
  request.geometry.resolution.quality  = RenderQuality::Export;
  request.decode_res                   = DecodeRes::EIGHTH;
  request.role                         = ExecutorRole::Interactive;
  request.require_host_output          = true;
  std::shared_ptr<ImageBuffer> output;
  {
    std::lock_guard<std::mutex> render_lock(executor.GetRenderLock());
    output = executor.Apply(snapshot, input, request);
  }
  return output ? HostPixels(*output) : cv::Mat{};
}

}  // namespace

class BackgroundRenderIsolationTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
  std::filesystem::path export_dir_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path();
    db_path_        = base / ("background_render_isolation_" + stamp + ".db");
    meta_path_      = base / ("background_render_isolation_" + stamp + ".json");
    export_dir_     = base / ("background_render_isolation_export_" + stamp);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(export_dir_, ec);
  }
};

// Before P4 and P5, thumbnails, analysis, and export rendered on the editor's per-image executor,
// and their completion could release or save the editor's state. Now each renders on its own
// service executor: after one thumbnail, one analysis rendition, and one P3 export of the image
// the editor holds, the editor executor has no batch renderer, keeps its binding, prepared
// source, and published results, and its next frame of the same preview reuses them. The editor's
// uncommitted value stays in its working document and never reaches storage.
TEST_F(BackgroundRenderIsolationTest,
       EditorExecutorKeepsItsBindingAndCachesWhileThumbnailAnalysisAndExportRender) {
  if (!std::filesystem::exists(LinearDngPath())) {
    GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
  }
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  const auto lease = pipelines->AcquireEditorLease(ids.first);
  ASSERT_NE(lease.document_, nullptr);
  EditorWorkingDocument working(ids.first, lease.document_);
  PipelineExecutor      editor_executor(ExecutorRole::Interactive);
  const auto            input = LoadEncodedInput(*project.GetImagePoolService(), ids.second);
  ASSERT_NE(input, nullptr);

  // A slider drag: the uncommitted value is written and published as the editor preview only.
  auto* exposure = working.Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  const float committed_ev = exposure->ToJson().at("exposure_ev").get<float>();
  exposure->LoadJson({{"exposure_ev", committed_ev + 1.25f}});
  const auto preview      = working.PublishPreview();
  const auto working_json = std::as_const(working).Document().ToJson();
  const auto stored_before =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(ids.first);

  const cv::Mat first_frame = RenderEditorFrame(editor_executor, *preview, input);
  ASSERT_FALSE(first_frame.empty());
  const auto binding_before = InteractiveBinding(editor_executor);
  ASSERT_TRUE(binding_before.has_value());
  EXPECT_EQ(*binding_before, RenderBindingKey::Of(*preview));
  const auto resources_before = InteractiveResources(editor_executor);
  ASSERT_GT(resources_before.prepared_source_entry_count, 0u);
  const auto       stats_before = InteractiveStats(editor_executor);

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       thumbnail =
      GetThumbnailDetailedBlocking(thumbnails, ids.first, ids.second, ThumbnailResolution::k256);
  EXPECT_EQ(thumbnail.status, ThumbnailRequestStatus::kReady) << thumbnail.message;
  ASSERT_NE(thumbnail.guard, nullptr);
  ASSERT_NE(thumbnail.guard->thumbnail_buffer_, nullptr);

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

  std::filesystem::create_directories(export_dir_);
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(),
                               pipelines);
  ExportTask    task;
  task.sleeve_id_                = ids.first;
  task.image_id_                 = ids.second;
  task.options_.format_          = ImageFormatType::JPEG;
  task.options_.export_path_     = export_dir_ / "background.jpg";
  task.options_.resize_enabled_  = true;
  task.options_.max_length_side_ = 256;
  task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
  task.recipe_->output_color_ =
      ExportColorProfileConfig{ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::GAMMA_2_2, 100.0f};
  export_service.EnqueueExportTask(task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> export_done;
  auto                                                     export_fut = export_done.get_future();
  export_service.ExportAll([&export_done](std::shared_ptr<std::vector<ExportResult>> results) {
    export_done.set_value(std::move(results));
  });
  ASSERT_EQ(export_fut.wait_for(120s), std::future_status::ready);
  const auto export_results = export_fut.get();
  ASSERT_NE(export_results, nullptr);
  ASSERT_EQ(export_results->size(), 1u);
  EXPECT_TRUE((*export_results)[0].success_) << (*export_results)[0].message_;

  // The editor executor is untouched by the three background renders.
  EXPECT_FALSE(HasBatchRenderer(editor_executor));
  EXPECT_EQ(InteractiveBinding(editor_executor), binding_before);
  const auto resources_after = InteractiveResources(editor_executor);
  EXPECT_EQ(resources_after.prepared_source_entry_count,
            resources_before.prepared_source_entry_count);
  EXPECT_EQ(resources_after.published_result_count, resources_before.published_result_count);
  EXPECT_EQ(resources_after.texture_pool_entry_count, resources_before.texture_pool_entry_count);
  EXPECT_EQ(resources_after.session_value_ids, resources_before.session_value_ids);
  // The editor's working document and its published preview are unchanged, and no background
  // completion wrote the editor's state to storage.
  EXPECT_EQ(std::as_const(working).Document().ToJson(), working_json);
  EXPECT_EQ(working.CurrentPreview(), preview);
  EXPECT_EQ(project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(ids.first),
            stored_before);

  // The next frame of the same preview reuses the prepared source and the compiled plan.
  const cv::Mat second_frame = RenderEditorFrame(editor_executor, *preview, input);
  ASSERT_FALSE(second_frame.empty());
  const auto stats_after = InteractiveStats(editor_executor);
  EXPECT_EQ(stats_after.prepared_source_misses, stats_before.prepared_source_misses);
  EXPECT_EQ(stats_after.libraw_open_unpack_count, stats_before.libraw_open_unpack_count);
  EXPECT_EQ(stats_after.plan_cache_misses, stats_before.plan_cache_misses);
  EXPECT_LE(MaxAbsDifference(second_frame, first_frame),
            first_frame.depth() == CV_32F ? 1e-4 : 1.0);

  thumbnails.ReleaseThumbnail(ThumbnailCacheKey{ids.first, ThumbnailResolution::k256});
  thumbnails.ReleaseAnalysisRendition(rendition);
  pipelines->ReleaseEditorLease(ids.first);
}

// A separate fixture with no operator registration: ctest runs each discovered test in its own
// process, so nothing in this process fills the legacy operator registry. At 5708f139 the
// executor constructor built the stage table through that registry and this test crashed.
class PipelineExecutorWithoutOperatorRegistryTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    db_path_ = std::filesystem::temp_directory_path() / ("executor_no_registry_" + stamp + ".db");
    meta_path_ =
        std::filesystem::temp_directory_path() / ("executor_no_registry_" + stamp + ".json");
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

  auto executor = std::make_shared<PipelineExecutor>(ExecutorRole::Batch);
  const std::shared_ptr<const PipelineGraphSnapshot> snapshot =
      pipelines->AcquireCommittedSnapshot(ids.first);
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
}

}  // namespace alcedo
