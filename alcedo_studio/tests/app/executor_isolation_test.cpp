//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Isolation requirements for the executor ownership refactor
// (docs/refactor/2026-09-27-executor-ownership-refactor-plan.md, phase P0).
//
// Each test states a requirement that the shared per-image executor model broke before this
// refactor, and its comment names the phase that fixed it. The control test next to it runs the
// same steps without the conflicting consumer, so it proves that the measurement itself is
// correct. The editor is modelled by what it owns since P6: the lease, an EditorWorkingDocument,
// and a test-owned Interactive executor.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

#include "app/editor_working_document.hpp"
#include "app/export_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/renderer.hpp"
#include "image/image_buffer.hpp"
#include "io/image/export_recipe.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
#include "support/render_snapshot_source.hpp"
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

// IEC 61966-2-1 XYZ -> Rec.709 first coefficient that BindRgbWorkingSpaceCameraProfile stores.
constexpr double kRec709ColorMatrixFirstCoefficient = 3.2404542;

auto IsWorkingSpaceRec709Profile(const DevelopCameraProfile& profile) -> bool {
  return profile.color_matrices_valid &&
         std::abs(profile.color_matrix_1[0] - kRec709ColorMatrixFirstCoefficient) < 1e-6 &&
         std::abs(profile.color_matrix_2[0] - kRec709ColorMatrixFirstCoefficient) < 1e-6;
}

auto PrimaryExposure(PipelineDocument& document) -> ExposureModel* {
  auto* grade = document.PrimaryGrade();
  if (grade == nullptr) {
    return nullptr;
  }
  return dynamic_cast<ExposureModel*>(grade->FindAdjustmentByType(type_ids::Exposure()));
}

/// Largest per-channel difference; +inf when the images differ in size or type.
auto MaxAbsDifference(const cv::Mat& lhs, const cv::Mat& rhs) -> double {
  if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size() || lhs.type() != rhs.type()) {
    return std::numeric_limits<double>::infinity();
  }
  return cv::norm(lhs, rhs, cv::NORM_INF);
}

/// Same-backend renders of the same values are expected to match to within float rounding.
auto SameValuesTolerance(const cv::Mat& pixels) -> double {
  return pixels.depth() == CV_32F ? 1e-4 : 1.0;
}

auto MeanOfAllChannels(const cv::Mat& pixels) -> double {
  const auto mean = cv::mean(pixels);
  double     sum  = 0.0;
  for (int c = 0; c < pixels.channels() && c < 3; ++c) {
    sum += mean[c];
  }
  return sum;
}

/// Read an exported 8-bit image at its stored depth; empty on failure.
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

auto UniqueTempPath(const std::string& prefix) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         (prefix + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

/// Counters of the interactive renderer that @p executor uses; zero when none exists.
auto SessionRenderStats(PipelineExecutor& executor) -> RenderSessionStats {
#ifdef HAVE_CUDA
  if (auto* renderer = executor.DebugCudaRenderer()) {
    return renderer->Stats();
  }
#endif
#ifdef HAVE_METAL
  if (auto* renderer = executor.DebugMetalRenderer()) {
    return renderer->Stats();
  }
#endif
#ifdef HAVE_OPENCL
  if (auto* renderer = executor.DebugOpenClRenderer()) {
    return renderer->Stats();
  }
#endif
  return {};
}

/// Pass and plan-cache counters of one editor frame (difference of two cumulative snapshots).
struct EditorFramePassCounts {
  std::uint64_t sensor_develop_execute = 0;
  std::uint64_t sensor_develop_skip    = 0;
  std::uint64_t camera_color_execute   = 0;
  std::uint64_t camera_color_skip      = 0;
  std::uint64_t primary_grade_execute  = 0;
  std::uint64_t primary_grade_skip     = 0;
  std::uint64_t drt_execute            = 0;
  std::uint64_t drt_skip               = 0;
  std::uint64_t result_content_hits    = 0;
  std::uint64_t result_revision_misses = 0;
  std::uint64_t plan_cache_hits        = 0;
  std::uint64_t plan_cache_misses      = 0;
};

auto FramePassCounts(const RenderSessionStats& before, const RenderSessionStats& after)
    -> EditorFramePassCounts {
  EditorFramePassCounts counts;
  counts.sensor_develop_execute =
      after.pass.sensor_develop_execute - before.pass.sensor_develop_execute;
  counts.sensor_develop_skip  = after.pass.sensor_develop_skip - before.pass.sensor_develop_skip;
  counts.camera_color_execute = after.pass.camera_color_execute - before.pass.camera_color_execute;
  counts.camera_color_skip    = after.pass.camera_color_skip - before.pass.camera_color_skip;
  counts.primary_grade_execute =
      after.pass.primary_grade_execute - before.pass.primary_grade_execute;
  counts.primary_grade_skip  = after.pass.primary_grade_skip - before.pass.primary_grade_skip;
  counts.drt_execute         = after.pass.drt_execute - before.pass.drt_execute;
  counts.drt_skip            = after.pass.drt_skip - before.pass.drt_skip;
  counts.result_content_hits = after.pass.result_content_hits - before.pass.result_content_hits;
  counts.result_revision_misses =
      after.pass.result_revision_misses - before.pass.result_revision_misses;
  counts.plan_cache_hits   = after.plan_cache_hits - before.plan_cache_hits;
  counts.plan_cache_misses = after.plan_cache_misses - before.plan_cache_misses;
  return counts;
}

auto FormatFramePassCounts(const std::string& frame, const EditorFramePassCounts& counts)
    -> std::string {
  return "[editor-frame-counts] frame=" + frame +
         " sensor_exec=" + std::to_string(counts.sensor_develop_execute) +
         " sensor_skip=" + std::to_string(counts.sensor_develop_skip) +
         " camera_exec=" + std::to_string(counts.camera_color_execute) +
         " camera_skip=" + std::to_string(counts.camera_color_skip) +
         " grade_exec=" + std::to_string(counts.primary_grade_execute) +
         " grade_skip=" + std::to_string(counts.primary_grade_skip) +
         " drt_exec=" + std::to_string(counts.drt_execute) +
         " drt_skip=" + std::to_string(counts.drt_skip) +
         " content_hits=" + std::to_string(counts.result_content_hits) +
         " revision_misses=" + std::to_string(counts.result_revision_misses) +
         " plan_hits=" + std::to_string(counts.plan_cache_hits) +
         " plan_misses=" + std::to_string(counts.plan_cache_misses);
}

}  // namespace

class ExecutorIsolationTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
  std::filesystem::path export_dir_;

  void SetUp() override {
    if (!std::filesystem::exists(LinearDngPath())) {
      GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
    }
    TimeProvider::Refresh();
    db_path_    = UniqueTempPath("executor_isolation_") += ".db";
    meta_path_  = UniqueTempPath("executor_isolation_") += ".json";
    export_dir_ = UniqueTempPath("executor_isolation_export_");
    std::filesystem::create_directories(export_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(export_dir_, ec);
  }

  static auto MakeRequest(ExecutorRole role) -> PipelineApplyRequest {
    PipelineApplyRequest request;
    request.geometry.resolution.max_edge = 4096;
    request.geometry.resolution.quality  = RenderQuality::Export;
    request.decode_res                   = DecodeRes::EIGHTH;
    request.role                         = role;
    request.require_host_output          = true;
    return request;
  }

  /// Render @p snapshot on @p executor in @p role and return the host pixels.
  static auto Render(PipelineExecutor& executor, const PipelineGraphSnapshot& snapshot,
                     const std::shared_ptr<ImageBuffer>& input, ExecutorRole role) -> cv::Mat {
    const auto                   request = MakeRequest(role);
    std::shared_ptr<ImageBuffer> output;
    {
      std::lock_guard<std::mutex> render_lock(executor.GetRenderLock());
      output = executor.Apply(snapshot, input, request);
    }
    return output ? HostPixels(*output) : cv::Mat{};
  }

  /// Render the committed-state reference: a new executor on a frozen copy of @p document.
  static auto RenderOnNewExecutor(const PipelineDocument&             document,
                                  const std::shared_ptr<ImageBuffer>& input) -> cv::Mat {
    PipelineExecutor executor(ExecutorRole::Interactive);
    const auto       snapshot = test::FreezeInNewLineage(document);
    const auto       request  = MakeRequest(ExecutorRole::Interactive);
    std::shared_ptr<ImageBuffer> output;
    {
      std::lock_guard<std::mutex> render_lock(executor.GetRenderLock());
      output = executor.Apply(*snapshot, input, request);
    }
    return output ? HostPixels(*output) : cv::Mat{};
  }

  /// Render @p snapshot on @p executor in the Interactive role, as the editor render port does
  /// for one frame, and return the host pixels.
  static auto RenderSnapshot(PipelineExecutor& executor, const PipelineGraphSnapshot& snapshot,
                             const std::shared_ptr<ImageBuffer>& input) -> cv::Mat {
    const auto                   request = MakeRequest(ExecutorRole::Interactive);
    std::shared_ptr<ImageBuffer> output;
    {
      std::lock_guard<std::mutex> render_lock(executor.GetRenderLock());
      output = executor.Apply(snapshot, input, request);
    }
    return output ? HostPixels(*output) : cv::Mat{};
  }

  /// Export @p ids through ExportService as a 256 px JPEG and read the pixels back. The output
  /// color comes from the DRT of the committed snapshot the export renders.
  auto Export(ProjectService& project, const std::shared_ptr<PipelineMgmtService>& pipelines,
              const std::pair<sl_element_id_t, image_id_t>& ids, const std::string& file_name)
      -> cv::Mat {
    ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(),
                                 pipelines);
    ExportTask    task;
    task.sleeve_id_                = ids.first;
    task.image_id_                 = ids.second;
    task.options_.format_          = ImageFormatType::JPEG;
    task.options_.export_path_     = export_dir_ / file_name;
    task.options_.resize_enabled_  = true;
    task.options_.max_length_side_ = 256;
    task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
    export_service.EnqueueExportTask(task);
    std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
    auto                                                     done_future = done.get_future();
    export_service.ExportAll([&done](std::shared_ptr<std::vector<ExportResult>> results) {
      done.set_value(std::move(results));
    });
    EXPECT_EQ(done_future.wait_for(120s), std::future_status::ready);
    if (done_future.wait_for(0s) != std::future_status::ready) {
      return {};
    }
    const auto results = done_future.get();
    if (results == nullptr || results->size() != 1u || !(*results)[0].success_) {
      ADD_FAILURE() << "export failed: "
                    << (results != nullptr && !results->empty() ? (*results)[0].message_ : "");
      return {};
    }
    return ReadExportedPixels(export_dir_ / file_name);
  }
};

// Control for the next test: with no batch (one-shot) render in between, the editor session render
// shows a parameter change and matches a render of the same values on a new executor.
TEST_F(ExecutorIsolationTest, EditorSessionRenderShowsParameterChangeWithoutInterleavedOneShot) {
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
  auto* exposure = PrimaryExposure(working.Document());
  ASSERT_NE(exposure, nullptr);

  const cv::Mat before =
      Render(editor_executor, *working.CurrentPreview(), input, ExecutorRole::Interactive);
  ASSERT_FALSE(before.empty());
  exposure->SetValue(exposure->Value() + 1.5f);
  (void)working.PublishPreview();
  const cv::Mat after =
      Render(editor_executor, *working.CurrentPreview(), input, ExecutorRole::Interactive);
  const cv::Mat reference = RenderOnNewExecutor(working.Document(), input);

  EXPECT_GT(MeanOfAllChannels(after), MeanOfAllChannels(before) * 1.2);
  EXPECT_LE(MaxAbsDifference(after, reference), SameValuesTolerance(reference));
  pipelines->ReleaseEditorLease(ids.first);
}

// Audit R7. Before P1, a one-shot (thumbnail/export) render on the same document took the Model
// dirty bits, so the editor session arena and invalidation state saw no change and kept the old
// parameters. Enabled by P1: each render workspace compares Model revisions with its own record.
// Since P7 the one-shot render runs on its own Batch executor, as thumbnails and export do; it
// reads a snapshot of the same working document between the edit and the editor frame.
TEST_F(ExecutorIsolationTest, EditorSessionRenderShowsParameterChangeAfterInterleavedOneShot) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  const auto lease = pipelines->AcquireEditorLease(ids.first);
  ASSERT_NE(lease.document_, nullptr);
  EditorWorkingDocument working(ids.first, lease.document_);
  PipelineExecutor      editor_executor(ExecutorRole::Interactive);
  PipelineExecutor      one_shot_executor(ExecutorRole::Batch);
  const auto            input = LoadEncodedInput(*project.GetImagePoolService(), ids.second);
  ASSERT_NE(input, nullptr);
  auto* exposure = PrimaryExposure(working.Document());
  ASSERT_NE(exposure, nullptr);

  const cv::Mat before =
      Render(editor_executor, *working.CurrentPreview(), input, ExecutorRole::Interactive);
  ASSERT_FALSE(before.empty());
  exposure->SetValue(exposure->Value() + 1.5f);
  (void)working.PublishPreview();
  const cv::Mat one_shot =
      Render(one_shot_executor, *working.CurrentPreview(), input, ExecutorRole::Batch);
  ASSERT_FALSE(one_shot.empty());
  const cv::Mat after =
      Render(editor_executor, *working.CurrentPreview(), input, ExecutorRole::Interactive);
  const cv::Mat reference = RenderOnNewExecutor(working.Document(), input);

  EXPECT_GT(MeanOfAllChannels(after), MeanOfAllChannels(before) * 1.2);
  EXPECT_LE(MaxAbsDifference(after, reference), SameValuesTolerance(reference));
  pipelines->ReleaseEditorLease(ids.first);
}

// P1 exit condition: the revision protocol must not turn editor incremental renders into full
// recomputes. Each frame's pass counts are printed with the prefix [editor-frame-counts] so the
// same sequence can be compared with a build of the dirty-bit protocol.
TEST_F(ExecutorIsolationTest, EditorSessionEditSequenceReexecutesOnlyPassesDownstreamOfTheEdit) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  const auto lease = pipelines->AcquireEditorLease(ids.first);
  ASSERT_NE(lease.document_, nullptr);
  EditorWorkingDocument working(ids.first, lease.document_);
  const auto            input = LoadEncodedInput(*project.GetImagePoolService(), ids.second);
  ASSERT_NE(input, nullptr);
  auto&      document   = working.Document();
  // Each edit looks its model up through the working document, as the history does, so a node
  // that a published preview shares is copied before the write.
  const auto shadows_of = [&document] {
    return dynamic_cast<ShadowsModel*>(
        document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  };
  ASSERT_NE(PrimaryExposure(document), nullptr);
  ASSERT_NE(shadows_of(), nullptr);

  PipelineExecutor                             executor(ExecutorRole::Interactive);
  std::map<std::string, EditorFramePassCounts> frames;
  const auto render_frame = [&](const std::string& name, const std::function<void()>& edit) {
    edit();
    const auto preview = working.PublishPreview();
    const auto before  = SessionRenderStats(executor);
    ASSERT_FALSE(Render(executor, *preview, input, ExecutorRole::Interactive).empty()) << name;
    frames[name] = FramePassCounts(before, SessionRenderStats(executor));
    std::cout << FormatFramePassCounts(name, frames[name]) << std::endl;
  };

  render_frame("0_first", [] {});
  render_frame("1_unchanged", [] {});
  render_frame("2_exposure", [&] {
    auto* exposure = PrimaryExposure(document);
    exposure->SetValue(exposure->Value() + 0.5f);
  });
  render_frame("3_shadows", [&] {
    auto* shadows = shadows_of();
    shadows->SetValue(shadows->Value() + 20.0f);
  });
  render_frame("4_grade_mix", [&] { document.PrimaryGrade()->SetMix(0.5f); });
  render_frame("5_white_balance", [&] {
    auto payload       = document.Develop()->Params().Params();
    payload.wb_mode    = "custom";
    payload.custom_cct = 4800.0f;
    document.Develop()->Params().ReplaceParams(payload);
  });
  render_frame("6_unchanged", [] {});
  pipelines->ReleaseEditorLease(ids.first);
  if (HasFatalFailure()) {
    return;
  }

  for (const auto& [name, counts] : frames) {
    if (name != "0_first") {
      EXPECT_EQ(counts.plan_cache_misses, 0u) << name;
    }
  }
  // Grade scene output is never published, so the Grade pass runs on every frame; the
  // published DRT display result shows whether the frame reused the previous output.
  for (const auto* name : {"1_unchanged", "6_unchanged"}) {
    EXPECT_EQ(frames[name].sensor_develop_execute, 0u) << name;
    EXPECT_EQ(frames[name].camera_color_execute, 0u) << name;
    EXPECT_EQ(frames[name].drt_execute, 0u) << name;
    EXPECT_EQ(frames[name].result_revision_misses, 0u) << name;
  }
  for (const auto* name : {"2_exposure", "3_shadows", "4_grade_mix"}) {
    EXPECT_EQ(frames[name].sensor_develop_execute, 0u) << name;
    EXPECT_EQ(frames[name].camera_color_execute, 0u) << name;
    EXPECT_EQ(frames[name].drt_execute, 1u) << name;
  }
  EXPECT_EQ(frames["5_white_balance"].sensor_develop_execute, 0u);
  EXPECT_EQ(frames["5_white_balance"].camera_color_execute, 1u);
  EXPECT_EQ(frames["5_white_balance"].drt_execute, 1u);
}

// Control for the next test: two exports of an image that the editor holds, with no uncommitted
// preview, produce the same pixels.
TEST_F(ExecutorIsolationTest, RepeatedExportOfEditorOwnedImageWithoutPreviewIsUnchanged) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  const auto lease = pipelines->AcquireEditorLease(ids.first);
  ASSERT_NE(lease.document_, nullptr);

  const cv::Mat first  = Export(project, pipelines, ids, "first.jpg");
  const cv::Mat second = Export(project, pipelines, ids, "second.jpg");
  ASSERT_FALSE(first.empty());
  EXPECT_LE(MaxAbsDifference(first, second), 1.0);
  pipelines->ReleaseEditorLease(ids.first);
}

// Audit C6. Before P5, export borrowed the editor's live document and did not check
// unsettled_preview_, so an export of an open image during a slider drag wrote the drag value.
// Enabled by P5: export renders the committed snapshot captured at enqueue. Since P6 the editor
// owns its working document and its Interactive executor; export uses neither.
TEST_F(ExecutorIsolationTest, ExportDuringUnsettledEditorPreviewUsesCommittedState) {
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

  const cv::Mat committed_frame = RenderSnapshot(editor_executor, *working.CurrentPreview(), input);
  ASSERT_FALSE(committed_frame.empty());
  const cv::Mat committed = Export(project, pipelines, ids, "committed.jpg");
  ASSERT_FALSE(committed.empty());

  // A slider drag writes the uncommitted value into the working document and publishes the
  // preview that the editor renders. Nothing is committed or published to the service.
  auto* exposure = PrimaryExposure(working.Document());
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(exposure->Value() + 2.0f);
  const auto    drag_preview = working.PublishPreview();
  const cv::Mat drag_frame   = RenderSnapshot(editor_executor, *drag_preview, input);
  ASSERT_FALSE(drag_frame.empty());
  EXPECT_GT(MeanOfAllChannels(drag_frame), MeanOfAllChannels(committed_frame) * 1.2)
      << "the editor frame must show the drag value";

  const cv::Mat during_drag = Export(project, pipelines, ids, "during_drag.jpg");
  EXPECT_LE(MaxAbsDifference(committed, during_drag), 1.0);

  // The export did not use the editor executor: its next frame of the same preview is unchanged.
  const cv::Mat drag_frame_after_export = RenderSnapshot(editor_executor, *drag_preview, input);
  EXPECT_LE(MaxAbsDifference(drag_frame_after_export, drag_frame), SameValuesTolerance(drag_frame));
  pipelines->ReleaseEditorLease(ids.first);
}

// Audit section 3 item 3. Opening the editor must never expose the working-space Rec.709 profile
// on a RAW document that another consumer renders. Since P6 thumbnails, analysis, and export
// render the committed snapshot, and the editor lease builds its document privately. The observer
// reads the committed snapshot, as a thumbnail render does, for the whole editor-open call.
TEST_F(ExecutorIsolationTest, EditorOpenNeverExposesWorkingSpaceProfileOnCommittedRawSnapshot) {
  ProjectService project(db_path_, meta_path_);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     ids       = ImportLinearDng(project, pipelines);
  ASSERT_NE(ids.first, 0u);
  {
    const auto  snapshot = pipelines->AcquireCommittedSnapshot(ids.first);
    const auto& profile  = snapshot->Document().Develop()->Params().Params().camera_profile;
    ASSERT_TRUE(profile.color_matrices_valid);
    ASSERT_FALSE(IsWorkingSpaceRec709Profile(profile));
  }

  std::atomic<bool> editor_open_done{false};
  std::atomic<int>  observations{0};
  std::atomic<int>  rec709_observations{0};
  std::thread       observer([&] {
    do {
      const auto snapshot = pipelines->AcquireCommittedSnapshot(ids.first);
      if (IsWorkingSpaceRec709Profile(
              snapshot->Document().Develop()->Params().Params().camera_profile)) {
        rec709_observations.fetch_add(1, std::memory_order_relaxed);
      }
      observations.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::yield();
    } while (!editor_open_done.load(std::memory_order_acquire));
  });
  EditorHistoryLease lease;
  try {
    lease = pipelines->AcquireEditorLease(ids.first);
  } catch (...) {
    editor_open_done.store(true, std::memory_order_release);
    observer.join();
    throw;
  }
  editor_open_done.store(true, std::memory_order_release);
  observer.join();

  EXPECT_GT(observations.load(), 0);
  EXPECT_EQ(rec709_observations.load(), 0);
  ASSERT_NE(lease.document_, nullptr);
  ASSERT_NE(lease.root_, nullptr);
  EXPECT_FALSE(
      IsWorkingSpaceRec709Profile(lease.document_->Develop()->Params().Params().camera_profile));
  EXPECT_FALSE(IsWorkingSpaceRec709Profile(
      lease.root_->document.Develop()->Params().Params().camera_profile));
  pipelines->ReleaseEditorLease(ids.first);
}

}  // namespace alcedo
