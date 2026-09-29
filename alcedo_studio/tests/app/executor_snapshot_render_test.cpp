//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Executor that receives an immutable snapshot per request
// (docs/refactor/2026-09-27-executor-ownership-refactor-plan.md, phase P3).
//
// These tests render imported RAW files on test-owned executors, one per role, from a test-owned
// copy of the document the editor lease loads. They check:
// - an executor serves only its role;
// - the interactive renderer keeps sources, plans, and results for one binding (lineage +
//   element), and releases all of them when the binding changes;
// - a batch render releases its results and keeps its device;
// - each render case gives the same pixels on a long-lived executor of its role, where the cases
//   interleave, as on a new executor of its role, so the renders of one executor do not change
//   the renders of another.
//
// Set ALCEDO_RENDER_OUTPUT_DIR to a directory to also write the host pixels of every render case
// (`<image>_<case>.pixels`: int32 rows, cols, OpenCV type, then the pixel bytes). The phase
// record compares these files between two builds.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/renderer.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
#include "support/render_snapshot_source.hpp"
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
using raw_import_test::ImportRawFile;
using raw_import_test::LinearDngPath;
using raw_import_test::LoadEncodedInput;

auto SampleRawPath(const char* relative) -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / relative;
}

/// Sample images: linear DNG, Bayer ARW, X-Trans RAF.
struct SampleImage {
  std::string           name;
  std::filesystem::path path;
};

auto SampleImages() -> std::vector<SampleImage> {
  return {{"linear_dng", LinearDngPath()},
          {"bayer_arw", SampleRawPath("_DSC0726.ARW")},
          {"xtrans_raf", SampleRawPath("xtrans_edge/DSCF0019.RAF")}};
}

auto UniqueTempPath(const std::string& prefix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

auto MaxAbsDifference(const cv::Mat& lhs, const cv::Mat& rhs) -> double {
  if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size() || lhs.type() != rhs.type()) {
    return std::numeric_limits<double>::infinity();
  }
  return cv::norm(lhs, rhs, cv::NORM_INF);
}

/// Same-backend renders of the same values match to within float rounding.
auto SameValuesTolerance(const cv::Mat& pixels) -> double {
  return pixels.depth() == CV_32F ? 1e-4 : 1.0;
}

/// Renderer of @p role that @p executor created for its backend; null before its first render.
template <class Visit>
auto VisitRenderer(PipelineExecutor& executor, ExecutorRole role, Visit&& visit) -> bool {
  const bool interactive = role == ExecutorRole::Interactive;
#ifdef HAVE_CUDA
  if (auto* renderer =
          interactive ? executor.DebugCudaRenderer() : executor.DebugCudaBatchRenderer()) {
    visit(*renderer);
    return true;
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer =
          interactive ? executor.DebugMetalRenderer() : executor.DebugMetalBatchRenderer()) {
    visit(*renderer);
    return true;
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer =
          interactive ? executor.DebugOpenClRenderer() : executor.DebugOpenClBatchRenderer()) {
    visit(*renderer);
    return true;
  }
#endif
  (void)interactive;
  (void)visit;
  return false;
}

auto RendererStats(PipelineExecutor& executor, ExecutorRole role) -> RenderSessionStats {
  RenderSessionStats stats;
  (void)VisitRenderer(executor, role, [&](auto& renderer) { stats = renderer.Stats(); });
  return stats;
}

auto RendererResources(PipelineExecutor& executor, ExecutorRole role) -> RenderSessionResources {
  RenderSessionResources resources;
  (void)VisitRenderer(executor, role, [&](auto& renderer) { resources = renderer.Resources(); });
  return resources;
}

auto RendererDeviceIdentity(PipelineExecutor& executor, ExecutorRole role) -> std::uintptr_t {
  std::uintptr_t identity = 0;
  (void)VisitRenderer(executor, role,
                      [&](auto& renderer) { identity = renderer.DebugDeviceIdentity(); });
  return identity;
}

auto InteractiveBinding(PipelineExecutor& executor) -> std::optional<RenderBindingKey> {
  std::optional<RenderBindingKey> binding;
  (void)VisitRenderer(executor, ExecutorRole::Interactive,
                      [&](auto& renderer) { binding = renderer.Binding(); });
  return binding;
}

/// One render case: how the production scheduler would build the request.
struct RenderCase {
  std::string                             name;
  RenderType                              type = RenderType::FAST_PREVIEW;
  std::uint32_t                           max_edge   = 0;
  DecodeRes                               decode_res = DecodeRes::FULL;
  std::optional<ViewportRenderRegion>     viewport;
  std::optional<ExportColorProfileConfig> export_color;
};

/// Editor frames (three roles), thumbnails (four tiers), and export (SDR, HDR), in the order an
/// editor session with background work interleaves them.
auto RenderCases() -> std::vector<RenderCase> {
  std::vector<RenderCase> cases;
  cases.push_back({.name = "editor_interactive_primary", .type = RenderType::FAST_PREVIEW});
  cases.push_back({.name = "thumbnail_256",
                   .type       = RenderType::THUMBNAIL,
                   .max_edge   = 256,
                   .decode_res = DecodeRes::EIGHTH});
  cases.push_back({.name = "editor_quality_base", .type = RenderType::QUALITY_BASE_PREVIEW});
  cases.push_back({.name = "thumbnail_512",
                   .type       = RenderType::THUMBNAIL,
                   .max_edge   = 512,
                   .decode_res = DecodeRes::QUARTER});
  // Viewport over the centre quarter of a 2000 x 1500 reference, presented at 800 x 600.
  cases.push_back({.name     = "editor_detail_patch",
                   .type     = RenderType::DETAIL_ROI_PREVIEW,
                   .viewport = ViewportRenderRegion{.x_                = 500,
                                                    .y_                = 375,
                                                    .scale_x_          = 0.5f,
                                                    .scale_y_          = 0.5f,
                                                    .reference_width_  = 2000,
                                                    .reference_height_ = 1500,
                                                    .target_width_     = 800,
                                                    .target_height_    = 600}});
  cases.push_back({.name = "thumbnail_1024",
                   .type       = RenderType::THUMBNAIL,
                   .max_edge   = 1024,
                   .decode_res = DecodeRes::QUARTER});
  cases.push_back({.name = "thumbnail_2048",
                   .type       = RenderType::THUMBNAIL,
                   .max_edge   = 2048,
                   .decode_res = DecodeRes::HALF});
  cases.push_back({.name         = "export_sdr_rec709",
                   .type         = RenderType::FULL_RES_EXPORT,
                   .export_color = ExportColorProfileConfig{ColorUtils::ColorSpace::REC709,
                                                            ColorUtils::EOTF::GAMMA_2_2, 100.0f}});
  cases.push_back({.name         = "export_hdr_rec2020_pq",
                   .type         = RenderType::FULL_RES_EXPORT,
                   .export_color = ExportColorProfileConfig{ColorUtils::ColorSpace::REC2020,
                                                            ColorUtils::EOTF::ST2084, 1000.0f}});
  return cases;
}

/// The request the scheduler builds for @p render_case, with host output so pixels can be read.
auto MakeRequest(const std::shared_ptr<PipelineExecutor>& executor, const RenderCase& render_case)
    -> PipelineApplyRequest {
  PipelineTask task;
  task.pipeline_executor_                       = executor;
  task.options_.render_desc_.render_type_       = render_case.type;
  task.options_.render_desc_.max_edge_          = render_case.max_edge;
  task.options_.render_desc_.decode_res_        = render_case.decode_res;
  task.options_.render_desc_.viewport_region_   = render_case.viewport;
  task.options_.render_desc_.use_viewport_region_ = render_case.viewport.has_value();
  task.options_.export_output_color_            = render_case.export_color;
  auto request                                  = task.MakeApplyRequest();
  request.require_host_output                   = true;
  return request;
}

/// Render @p snapshot on @p executor under its render lock and return the host pixels.
auto RenderPixels(PipelineExecutor& executor, const PipelineGraphSnapshot& snapshot,
                  const std::shared_ptr<ImageBuffer>& input, const PipelineApplyRequest& request)
    -> cv::Mat {
  std::shared_ptr<ImageBuffer> output;
  {
    std::lock_guard<std::mutex> render_lock(executor.GetRenderLock());
    output = executor.Apply(snapshot, input, request);
  }
  return output ? HostPixels(*output) : cv::Mat{};
}

/// Write @p pixels for the cross-build comparison when ALCEDO_RENDER_OUTPUT_DIR is set.
void WritePixelsWhenRequested(const std::string& file_stem, const cv::Mat& pixels) {
  const char* dir = std::getenv("ALCEDO_RENDER_OUTPUT_DIR");
  if (dir == nullptr || dir[0] == '\0' || pixels.empty()) {
    return;
  }
  std::filesystem::create_directories(dir);
  const cv::Mat continuous = pixels.isContinuous() ? pixels : pixels.clone();
  std::ofstream out(std::filesystem::path(dir) / (file_stem + ".pixels"), std::ios::binary);
  const std::int32_t header[3] = {continuous.rows, continuous.cols, continuous.type()};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  out.write(reinterpret_cast<const char*>(continuous.data),
            static_cast<std::streamsize>(continuous.total() * continuous.elemSize()));
}

void RaiseExposure(PipelineDocument& document, float value) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(value);
}

/// Role of the executor that owns renders of @p type: the editor port renders the preview types,
/// ThumbnailService and ExportService render thumbnails and exports.
auto RoleOf(RenderType type) -> ExecutorRole {
  return type == RenderType::THUMBNAIL || type == RenderType::FULL_RES_EXPORT
             ? ExecutorRole::Batch
             : ExecutorRole::Interactive;
}

/// One long-lived executor per role, as the editor port and the background services own them.
struct RoleExecutors {
  std::shared_ptr<PipelineExecutor> interactive =
      std::make_shared<PipelineExecutor>(ExecutorRole::Interactive);
  std::shared_ptr<PipelineExecutor> batch = std::make_shared<PipelineExecutor>(ExecutorRole::Batch);

  [[nodiscard]] auto For(RenderType type) const -> const std::shared_ptr<PipelineExecutor>& {
    return RoleOf(type) == ExecutorRole::Interactive ? interactive : batch;
  }
};

}  // namespace

class ExecutorSnapshotRenderTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void SetUp() override {
    if (!std::filesystem::exists(LinearDngPath())) {
      GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
    }
    TimeProvider::Refresh();
    db_path_   = UniqueTempPath("executor_snapshot_render_") += ".db";
    meta_path_ = UniqueTempPath("executor_snapshot_render_") += ".json";
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
  }

  struct LoadedImage {
    std::optional<test::RenderSnapshotSource> source;
    std::shared_ptr<ImageBuffer>              input;
  };

  /// Import @p path, take the document the editor lease loads (camera profile of the image bound)
  /// as the test's own working document, and read the encoded bytes.
  static auto Load(ProjectService& project, const std::shared_ptr<PipelineMgmtService>& pipelines,
                   const std::filesystem::path& path) -> LoadedImage {
    const auto ids = ImportRawFile(project, pipelines, path);
    if (ids.first == 0) {
      return {};
    }
    auto document = pipelines->AcquireEditorLease(ids.first).document_;
    pipelines->ReleaseEditorLease(ids.first);
    if (!document) {
      return {};
    }
    LoadedImage loaded;
    loaded.source.emplace(std::move(document), ids.first);
    loaded.input = LoadEncodedInput(*project.GetImagePoolService(), ids.second);
    return loaded;
  }
};

// Every render case (three editor frame roles, four thumbnail tiers, SDR and HDR export) gives the
// same pixels on the long-lived executor of its role, where the cases interleave, as on a new
// executor of the case's role that renders only that case.
TEST_F(ExecutorSnapshotRenderTest, EveryRenderCaseMatchesARenderOnANewExecutorOfItsRole) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  for (const auto& sample : SampleImages()) {
    SCOPED_TRACE(sample.name);
    if (!std::filesystem::exists(sample.path)) {
      ADD_FAILURE() << "Sample RAW file is missing: " << sample.path.string();
      continue;
    }
    auto loaded = Load(project, pipelines, sample.path);
    ASSERT_TRUE(loaded.source.has_value());
    ASSERT_NE(loaded.input, nullptr);
    RaiseExposure(loaded.source->Document(), 0.7f);
    ASSERT_FALSE(HasFatalFailure());
    const RoleExecutors executors;

    for (const auto& render_case : RenderCases()) {
      SCOPED_TRACE(render_case.name);
      const auto  snapshot   = loaded.source->Freeze();
      const auto& long_lived = executors.For(render_case.type);
      const auto  request    = MakeRequest(long_lived, render_case);
      ASSERT_EQ(request.role, RoleOf(render_case.type));
      const auto shared = RenderPixels(*long_lived, *snapshot, loaded.input, request);
      ASSERT_FALSE(shared.empty());
      if (render_case.type == RenderType::THUMBNAIL) {
        EXPECT_LE(std::max(shared.cols, shared.rows), static_cast<int>(render_case.max_edge));
      }
      WritePixelsWhenRequested(sample.name + "_" + render_case.name, shared);

      auto       isolated = std::make_shared<PipelineExecutor>(request.role);
      const auto reference =
          RenderPixels(*isolated, *snapshot, loaded.input, MakeRequest(isolated, render_case));
      // Bayer and X-Trans renders of equal values vary by up to ~1e-5 between any two runs
      // (measured on CUDA); state carried between cases would show as a much larger difference.
      EXPECT_LE(MaxAbsDifference(shared, reference), SameValuesTolerance(shared))
          << "long-lived executor output differs from a new " << render_case.name << " executor";
    }
  }
}

// Successive snapshots of one loaded history keep the interactive binding: the second frame
// after an edit reuses the prepared source and the compiled plan and re-executes only passes
// downstream of the edit.
TEST_F(ExecutorSnapshotRenderTest, InteractiveRendersOfOneLineageReuseSourceAndPlan) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto           loaded    = Load(project, pipelines, LinearDngPath());
  ASSERT_TRUE(loaded.source.has_value());
  auto       executor = std::make_shared<PipelineExecutor>(ExecutorRole::Interactive);
  const auto frame    = RenderCases().front();

  const auto first    = loaded.source->Freeze();
  ASSERT_FALSE(RenderPixels(*executor, *first, loaded.input, MakeRequest(executor, frame)).empty());
  const auto before = RendererStats(*executor, ExecutorRole::Interactive);

  RaiseExposure(loaded.source->Document(), 1.0f);
  ASSERT_FALSE(HasFatalFailure());
  const auto second = loaded.source->Freeze();
  ASSERT_NE(&first->Document(), &second->Document());
  ASSERT_EQ(first->Lineage(), second->Lineage());
  ASSERT_FALSE(
      RenderPixels(*executor, *second, loaded.input, MakeRequest(executor, frame)).empty());
  const auto after = RendererStats(*executor, ExecutorRole::Interactive);

  EXPECT_EQ(after.prepared_source_misses, before.prepared_source_misses);
  EXPECT_EQ(after.prepared_source_hits, before.prepared_source_hits + 1);
  EXPECT_EQ(after.plan_cache_misses, before.plan_cache_misses);
  EXPECT_EQ(after.plan_cache_hits, before.plan_cache_hits + 1);
  EXPECT_EQ(after.pass.sensor_develop_execute, before.pass.sensor_develop_execute);
  EXPECT_EQ(after.pass.sensor_develop_skip, before.pass.sensor_develop_skip + 1);
  const auto binding = InteractiveBinding(*executor);
  ASSERT_TRUE(binding.has_value());
  EXPECT_EQ(binding->lineage, second->Lineage());
}

// A snapshot of another lineage (reload, Version checkout, paste rebuild) releases every resource
// of the previous binding before it renders: the source is unpacked and the plan compiled again,
// and no result of the previous binding is reused.
TEST_F(ExecutorSnapshotRenderTest, InteractiveRenderOfAnotherLineageReleasesThePreviousBinding) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto           loaded    = Load(project, pipelines, LinearDngPath());
  ASSERT_TRUE(loaded.source.has_value());
  auto       executor = std::make_shared<PipelineExecutor>(ExecutorRole::Interactive);
  const auto frame    = RenderCases().front();

  const auto first    = loaded.source->Freeze();
  ASSERT_FALSE(RenderPixels(*executor, *first, loaded.input, MakeRequest(executor, frame)).empty());
  const auto before = RendererStats(*executor, ExecutorRole::Interactive);
  ASSERT_GT(RendererResources(*executor, ExecutorRole::Interactive).published_result_count, 0u);

  // The owner replaces its document with a rebuilt copy and takes a new lineage.
  loaded.source->Rebind(
      std::make_shared<PipelineDocument>(ClonePipelineDocument(loaded.source->Document())));
  const auto rebound = loaded.source->Freeze();
  ASSERT_NE(rebound->Lineage(), first->Lineage());
  const auto frame_pixels =
      RenderPixels(*executor, *rebound, loaded.input, MakeRequest(executor, frame));
  ASSERT_FALSE(frame_pixels.empty());
  const auto after = RendererStats(*executor, ExecutorRole::Interactive);

  // The release resets the pass counters and clears the source and plan caches, so the rebound
  // frame is the first frame of a new binding.
  EXPECT_EQ(after.prepared_source_misses, before.prepared_source_misses + 1);
  EXPECT_EQ(after.libraw_open_unpack_count, before.libraw_open_unpack_count + 1);
  EXPECT_EQ(after.plan_cache_misses, before.plan_cache_misses + 1);
  EXPECT_EQ(after.pass.sensor_develop_execute, 1u);
  EXPECT_EQ(after.pass.sensor_develop_skip, 0u);
  EXPECT_EQ(after.pass.result_content_hits, 0u);
  EXPECT_EQ(RendererResources(*executor, ExecutorRole::Interactive).prepared_source_entry_count,
            1u);
  const auto binding = InteractiveBinding(*executor);
  ASSERT_TRUE(binding.has_value());
  EXPECT_EQ(binding->lineage, rebound->Lineage());
}

// A batch render releases every result resource when it completes and keeps its device; it does
// not change the interactive executor that renders the same snapshot.
TEST_F(ExecutorSnapshotRenderTest, BatchRenderReleasesItsResultsAndKeepsItsDevice) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto           loaded    = Load(project, pipelines, LinearDngPath());
  ASSERT_TRUE(loaded.source.has_value());
  const RoleExecutors executors;
  auto&               interactive = *executors.interactive;
  auto&               batch_exec  = *executors.batch;
  const auto          cases       = RenderCases();
  const auto          frame       = cases[0];
  const auto          thumb       = cases[1];
  ASSERT_EQ(thumb.type, RenderType::THUMBNAIL);

  const auto snapshot = loaded.source->Freeze();
  ASSERT_FALSE(
      RenderPixels(interactive, *snapshot, loaded.input, MakeRequest(executors.interactive, frame))
          .empty());
  const auto interactive_before = RendererResources(interactive, ExecutorRole::Interactive);
  const auto interactive_stats  = RendererStats(interactive, ExecutorRole::Interactive);

  const auto thumb_request      = MakeRequest(executors.batch, thumb);
  ASSERT_EQ(thumb_request.role, ExecutorRole::Batch);
  ASSERT_FALSE(RenderPixels(batch_exec, *snapshot, loaded.input, thumb_request).empty());
  const auto device = RendererDeviceIdentity(batch_exec, ExecutorRole::Batch);
  ASSERT_NE(device, 0u);
  ASSERT_FALSE(RenderPixels(batch_exec, *snapshot, loaded.input, thumb_request).empty());

  const auto batch = RendererResources(batch_exec, ExecutorRole::Batch);
  EXPECT_EQ(batch.published_result_count, 0u);
  EXPECT_EQ(batch.texture_pool_used_bytes, 0u);
  EXPECT_EQ(batch.transient_used_bytes, 0u);
  EXPECT_EQ(batch.prepared_source_entry_count, 0u);
  EXPECT_EQ(RendererDeviceIdentity(batch_exec, ExecutorRole::Batch), device);
  EXPECT_NE(RendererDeviceIdentity(interactive, ExecutorRole::Interactive), device);

  const auto interactive_after = RendererResources(interactive, ExecutorRole::Interactive);
  EXPECT_EQ(interactive_after.published_result_count, interactive_before.published_result_count);
  EXPECT_EQ(interactive_after.session_value_ids, interactive_before.session_value_ids);
  EXPECT_EQ(interactive_after.prepared_source_entry_count,
            interactive_before.prepared_source_entry_count);
  const auto stats_after = RendererStats(interactive, ExecutorRole::Interactive);
  EXPECT_EQ(stats_after.prepared_source_misses, interactive_stats.prepared_source_misses);
  EXPECT_EQ(stats_after.plan_cache_misses, interactive_stats.plan_cache_misses);
}

// ReleaseBinding drops every resource the executor's renderer holds for the last rendered image
// and keeps its device, so the next render starts a new binding on the same GPU streams.
TEST_F(ExecutorSnapshotRenderTest, ReleaseBindingReleasesTheRendererResourcesAndKeepsItsDevice) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto           loaded    = Load(project, pipelines, LinearDngPath());
  ASSERT_TRUE(loaded.source.has_value());
  const RoleExecutors executors;
  auto&               interactive = *executors.interactive;
  auto&               batch_exec  = *executors.batch;
  const auto          cases       = RenderCases();

  const auto          snapshot    = loaded.source->Freeze();
  ASSERT_FALSE(RenderPixels(interactive, *snapshot, loaded.input,
                            MakeRequest(executors.interactive, cases[0]))
                   .empty());
  ASSERT_FALSE(
      RenderPixels(batch_exec, *snapshot, loaded.input, MakeRequest(executors.batch, cases[1]))
          .empty());
  const auto interactive_device = RendererDeviceIdentity(interactive, ExecutorRole::Interactive);
  const auto batch_device       = RendererDeviceIdentity(batch_exec, ExecutorRole::Batch);
  ASSERT_NE(interactive_device, 0u);
  ASSERT_NE(batch_device, 0u);
  ASSERT_GT(RendererResources(interactive, ExecutorRole::Interactive).published_result_count, 0u);

  for (auto* executor : {&interactive, &batch_exec}) {
    std::lock_guard<std::mutex> render_lock(executor->GetRenderLock());
    executor->ReleaseBinding();
  }

  const auto released = RendererResources(interactive, ExecutorRole::Interactive);
  EXPECT_EQ(released.published_result_count, 0u);
  EXPECT_EQ(released.texture_pool_used_bytes, 0u);
  EXPECT_EQ(released.prepared_source_entry_count, 0u);
  EXPECT_EQ(released.prepared_source_host_bytes, 0u);
  EXPECT_FALSE(InteractiveBinding(interactive).has_value());
  EXPECT_EQ(RendererResources(batch_exec, ExecutorRole::Batch).published_result_count, 0u);
  EXPECT_EQ(RendererDeviceIdentity(interactive, ExecutorRole::Interactive), interactive_device);
  EXPECT_EQ(RendererDeviceIdentity(batch_exec, ExecutorRole::Batch), batch_device);
}

// An executor constructed for one role rejects requests of the other role before it touches the
// input or creates a renderer.
TEST(ExecutorRoleTest, ExecutorOfOneRoleRejectsRequestsOfTheOtherRole) {
  const auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  const auto snapshot = PipelineGraphSnapshot::Preview(document->Freeze(), 1,
                                                       PipelineLineageId::Next(), {});
  const auto input    = std::make_shared<ImageBuffer>();

  PipelineExecutor     batch_executor(ExecutorRole::Batch);
  PipelineApplyRequest interactive_request;
  interactive_request.role = ExecutorRole::Interactive;
  EXPECT_FALSE(batch_executor.Serves(ExecutorRole::Interactive));
  EXPECT_THROW((void)batch_executor.Apply(*snapshot, input, interactive_request),
               std::invalid_argument);

  PipelineExecutor     interactive_executor(ExecutorRole::Interactive);
  PipelineApplyRequest batch_request;
  batch_request.role = ExecutorRole::Batch;
  EXPECT_FALSE(interactive_executor.Serves(ExecutorRole::Batch));
  EXPECT_THROW((void)interactive_executor.Apply(*snapshot, input, batch_request),
               std::invalid_argument);
}

// A render task without a snapshot fails; the scheduler does not render some other document in
// its place.
TEST(ExecutorRoleTest, SchedulerFailsARenderTaskWithoutASnapshot) {
  PipelineScheduler scheduler;
  PipelineTask      task;
  task.pipeline_executor_                 = std::make_shared<PipelineExecutor>(ExecutorRole::Batch);
  task.input_                             = std::make_shared<ImageBuffer>();
  task.options_.is_blocking_              = true;
  task.options_.is_callback_              = false;
  task.options_.is_seq_callback_          = false;
  task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  auto result                             = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
  task.result_                            = result;
  auto               future               = result->get_future();
  std::promise<bool> completed;
  auto               completed_future = completed.get_future();
  task.on_complete_ = [&completed](bool success, std::string) { completed.set_value(success); };
  scheduler.ScheduleTask(std::move(task));

  ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
  EXPECT_THROW((void)future.get(), std::runtime_error);
  ASSERT_EQ(completed_future.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(completed_future.get());
}

}  // namespace alcedo
