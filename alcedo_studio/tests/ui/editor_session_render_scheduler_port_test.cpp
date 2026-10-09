//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../support/editor_lease_test_support.hpp"
#include "app/editor_comparison_inputs.hpp"
#include "app/editor_comparison_types.hpp"
#include "app/editor_image_render_port.hpp"
#include "app/editor_working_document.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/drt_display.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"
#include "image/dng_color_profile_import.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#ifdef HAVE_CUDA
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#endif

namespace alcedo::ui {
namespace {

using namespace std::chrono_literals;

/// Upper bound for one frame, including the first GPU device creation of a debug build.
constexpr auto           kFrameTimeout = 180s;

class RecordingFrameSink final : public alcedo::IFrameSink {
 public:
  void EnsureSize(int width, int height) override {
    width_  = width;
    height_ = height;
    ++ensure_size_count_;
  }

  auto MapResourceForWrite(alcedo::FrameMemoryDomain /*preferred_domain*/)
      -> alcedo::FrameWriteMapping override {
    return {};
  }

  void UnmapResource() override {}

  void NotifyFrameReady(const alcedo::FrameCompletionSubmission& submission) override {
    last_submission = submission;
    ++ready_count_;
  }

  void BindFrameSubmission(const alcedo::FrameCompletionSubmission& submission) override {
    last_submission = submission;
    ++bind_count_;
  }

  [[nodiscard]] auto GetWidth() const -> int override { return width_; }
  [[nodiscard]] auto GetHeight() const -> int override { return height_; }

  [[nodiscard]] auto ready_count() const -> int {
    return ready_count_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto bind_count() const -> int {
    return bind_count_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto ensure_size_count() const -> int {
    return ensure_size_count_.load(std::memory_order_acquire);
  }

  alcedo::FrameCompletionSubmission last_submission{};

 private:
  std::atomic<int> ready_count_       = 0;
  std::atomic<int> bind_count_        = 0;
  std::atomic<int> ensure_size_count_ = 0;
  int              width_             = 0;
  int              height_            = 0;
};

/// Frame sink that receives the presented RGBA32F pixels in host memory, without a GUI.
class HostPixelFrameSink final : public alcedo::IFrameSink {
 public:
  void EnsureSize(int width, int height) override { pixels.create(height, width, CV_32FC4); }

  auto MapResourceForWrite(alcedo::FrameMemoryDomain /*preferred_domain*/)
      -> alcedo::FrameWriteMapping override {
    alcedo::FrameWriteMapping mapping;
    mapping.data          = pixels.data;
    mapping.row_bytes     = pixels.step;
    mapping.pixel_format  = alcedo::FramePixelFormat::RGBA32F;
    mapping.memory_domain = alcedo::FrameMemoryDomain::HostVisible;
    mapping.target_type   = alcedo::FrameWriteTargetType::LinearBuffer;
    return mapping;
  }

  void UnmapResource() override {}
  void NotifyFrameReady(const alcedo::FrameCompletionSubmission& /*submission*/) override {
    ready_count_.fetch_add(1, std::memory_order_acq_rel);
  }
  void BindFrameSubmission(const alcedo::FrameCompletionSubmission& submission) override {
    bound_submission = submission;
  }
  [[nodiscard]] auto GetWidth() const -> int override { return pixels.cols; }
  [[nodiscard]] auto GetHeight() const -> int override { return pixels.rows; }
  [[nodiscard]] auto ready_count() const -> int {
    return ready_count_.load(std::memory_order_acquire);
  }

  cv::Mat                           pixels;
  /// Submission the presenter bound with the last frame.
  alcedo::FrameCompletionSubmission bound_submission{};

 private:
  std::atomic<int> ready_count_ = 0;
};

/// Result that the Schedule `on_complete` callback reported for one frame.
struct FrameCompletion {
  bool        success = false;
  std::string message;
};

/**
 * @brief Schedule @p request and wait for its `on_complete` callback.
 * @return nullopt when the port rejected the request (job id 0) or the frame did not complete
 *         within kFrameTimeout (reported as a test failure).
 */
auto ScheduleAndWait(EditorSessionRenderSchedulerPort&  scheduler,
                     const alcedo::EditorRenderRequest& request) -> std::optional<FrameCompletion> {
  // Shared with the callback: a frame that completes after a timeout must not write a
  // destroyed promise.
  auto       done   = std::make_shared<std::promise<FrameCompletion>>();
  auto       result = done->get_future();
  const auto job_id = scheduler.Schedule(request, [done](bool success, std::string message) {
    done->set_value(FrameCompletion{success, std::move(message)});
  });
  if (job_id == 0) {
    return std::nullopt;
  }
  if (result.wait_for(kFrameTimeout) != std::future_status::ready) {
    ADD_FAILURE() << "Frame " << request.request_id << " did not complete";
    return std::nullopt;
  }
  return result.get();
}

/**
 * @brief Schedule @p request as an image job and wait for its completion.
 * @return nullopt when the port rejected the job (reason in @p error) or the job did not complete
 *         within kFrameTimeout (reported as a test failure).
 */
auto ScheduleImagesAndWait(EditorSessionRenderSchedulerPort& scheduler,
                           alcedo::EditorImageRenderRequest request, std::string* error = nullptr)
    -> std::optional<alcedo::EditorImageRenderResult> {
  auto       done   = std::make_shared<std::promise<alcedo::EditorImageRenderResult>>();
  auto       result = done->get_future();
  const auto job_id = scheduler.ScheduleImages(
      std::move(request),
      [done](alcedo::EditorImageRenderResult images) { done->set_value(std::move(images)); },
      error);
  if (job_id == 0) {
    return std::nullopt;
  }
  if (result.wait_for(kFrameTimeout) != std::future_status::ready) {
    ADD_FAILURE() << "Image job " << job_id << " did not complete";
    return std::nullopt;
  }
  return result.get();
}

/// Image job of the bound fixture image 22 / 11 for @p snapshots.
auto MakeImageRequest(std::uint64_t                                                     epoch,
                      std::vector<std::shared_ptr<const alcedo::PipelineGraphSnapshot>> snapshots)
    -> alcedo::EditorImageRenderRequest {
  alcedo::EditorImageRenderRequest request;
  request.element_id            = 22;
  request.image_id              = 11;
  request.image_load_request_id = alcedo::ImageLoadRequestId{epoch};
  request.snapshots             = std::move(snapshots);
  return request;
}

/// Wait until every work item queued on @p pipeline_scheduler before this call has run. The
/// editor port uses a scheduler with one worker, so the queue runs in order.
void WaitForQueuedWork(alcedo::PipelineScheduler& pipeline_scheduler) {
  auto done   = std::make_shared<std::promise<void>>();
  auto result = done->get_future();
  pipeline_scheduler.ScheduleWork([done] { done->set_value(); });
  ASSERT_EQ(result.wait_for(kFrameTimeout), std::future_status::ready);
}

auto MakeRequest(std::uint64_t request_id, std::uint64_t image_load_request,
                 alcedo::PresentationSinkId sink_id = 7) -> alcedo::EditorRenderRequest {
  alcedo::EditorRenderRequest request;
  request.request_id                   = request_id;
  request.intent.element_id            = 22;
  request.intent.image_id              = 11;
  request.intent.image_load_request_id = alcedo::ImageLoadRequestId{image_load_request};
  request.intent.reason                = alcedo::EditorRenderReason::InitialFrame;
  request.intent.quality               = alcedo::EditorRenderQuality::Interactive;
  request.intent.frame_role            = alcedo::FrameRole::InteractivePrimary;
  request.intent.requested_width       = 320;
  request.intent.requested_height      = 180;
  request.intent.presentation_sink_id  = sink_id;
  return request;
}

/// Submission the renderer hands to the frame presenter for @p request: the port's render
/// description through the pipeline task. The presenter binds it to the sink with the frame.
auto PresentedSubmission(const alcedo::EditorRenderRequest& request)
    -> alcedo::FrameCompletionSubmission {
  alcedo::PipelineTask task;
  task.pipeline_executor_ =
      std::make_shared<alcedo::PipelineExecutor>(alcedo::ExecutorRole::Interactive);
  task.options_.render_desc_ = MakeEditorRenderDesc(request);
  return task.MakeApplyRequest().submission;
}

auto MakeReadyContext(std::uint64_t epoch, sl_element_id_t element_id, image_id_t image_id,
                      alcedo::PresentationSinkId sink_id = 7) -> EditorRenderSessionContext {
  EditorRenderSessionContext context;
  context.epoch                = epoch;
  context.element_id           = element_id;
  context.image_id             = image_id;
  context.presentation_sink_id = sink_id;
  context.image                = std::make_shared<alcedo::Image>(image_id);
  context.image->image_path_   = std::filesystem::path("D:/fixture/session-context.arw");
  context.input                = std::make_shared<alcedo::ImageBuffer>();
  return context;
}

/// Editor lease port of one session, with the image it holds.
struct HeldImage {
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  std::optional<EditorImageLease>            lease;
  std::string                                error;
};

/// Take the editor lease of @p element_id through a new pipeline port that uses @p mappers.
auto HoldImage(sl_element_id_t element_id, EditorSessionPipelineMappers mappers) -> HeldImage {
  HeldImage held;
  held.pipeline_port = std::make_shared<EditorSessionPipelinePort>();
  held.pipeline_port->SetServices(std::move(mappers));
  held.lease = held.pipeline_port->AcquireLease(element_id, &held.error);
  return held;
}

/// Take the editor lease of @p element_id with an in-memory history of the default document.
auto HoldImageInMemory(sl_element_id_t element_id) -> HeldImage {
  return HoldImage(element_id,
                   EditorSessionPipelineMappers{{}, [](sl_element_id_t id) {
                                                  return alcedo::test::MakeInMemoryEditorLease(id);
                                                }});
}

/// Render port that renders the preview of an in-memory held image 22.
auto MakeSchedulerHoldingImage(HeldImage& held)
    -> std::shared_ptr<EditorSessionRenderSchedulerPort> {
  held           = HoldImageInMemory(22);
  auto scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  scheduler->SetPipelinePort(held.pipeline_port);
  return scheduler;
}

void WriteExposure(alcedo::PipelineDocument& document, float ev) {
  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  auto* exposure = grade->FindAdjustmentByType(alcedo::type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  exposure->LoadJson(nlohmann::json{{"exposure_ev", ev}});
}

auto MeanOfColorChannels(const cv::Mat& pixels) -> double {
  const auto mean = cv::mean(pixels);
  return mean[0] + mean[1] + mean[2];
}

/// Custom white balance: a different CCT and tint than the as-shot white balance of the root.
void WriteCustomWhiteBalance(alcedo::PipelineDocument& document, float cct, float tint) {
  auto develop          = document.Develop()->Params().Params();
  develop.use_camera_wb = false;
  develop.wb_mode       = "custom";
  develop.custom_cct    = cct;
  develop.custom_tint   = tint;
  document.Develop()->Params().ReplaceParams(develop);
}

/// A sensor setting: changing it changes the sensor result of the image.
void WriteHighlightsReconstruct(alcedo::PipelineDocument& document, bool enabled) {
  auto develop                   = document.Develop()->Params().Params();
  develop.highlights_reconstruct = enabled;
  document.Develop()->Params().ReplaceParams(develop);
}

void WriteLmtCubePath(alcedo::PipelineDocument& document, const std::string& path) {
  auto* lmt = dynamic_cast<alcedo::LmtModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(alcedo::type_ids::Lmt()));
  ASSERT_NE(lmt, nullptr);
  lmt->SetCubePath(path);
}

/**
 * @brief LUT resolver of the port's executor that can hold the render worker inside one render.
 *
 * Resolves like the default resolver. When armed, the first lookup of any LUT reference blocks
 * the calling render thread until @ref Release. The editor executor resolves a Color Grade LUT
 * while it renders the document that references it, so the test can act while exactly that
 * document is rendering.
 */
class BlockingLutResolver final : public alcedo::LutResourceResolver {
 public:
  /// Block the next LUT lookup; @ref Started becomes ready when it starts.
  void Arm() {
    std::scoped_lock lock(mutex_);
    armed_          = true;
    released_       = false;
    started_        = std::promise<void>();
    release_        = std::promise<void>();
    started_future_ = started_.get_future().share();
    release_future_ = release_.get_future().share();
  }

  [[nodiscard]] auto Started() -> std::shared_future<void> {
    std::scoped_lock lock(mutex_);
    return started_future_;
  }

  /// Let the blocked lookup continue. Idempotent; also disarms a lookup that did not start.
  void Release() {
    std::scoped_lock lock(mutex_);
    armed_ = false;
    if (release_future_.valid() && !released_) {
      released_ = true;
      release_.set_value();
    }
  }

  [[nodiscard]] auto Resolve(const alcedo::LutReference& reference) const
      -> alcedo::LutResourceResolution override {
    BlockIfArmed(reference);
    return default_->Resolve(reference);
  }

  void ReadResource(
      const alcedo::LutReference&                                      reference,
      const std::function<void(const alcedo::LutResourceResolution&)>& visitor) const override {
    BlockIfArmed(reference);
    default_->ReadResource(reference, visitor);
  }

 private:
  void BlockIfArmed(const alcedo::LutReference& reference) const {
    if (alcedo::IsEmptyLutReference(reference)) {
      return;
    }
    std::shared_future<void> release;
    {
      std::scoped_lock lock(mutex_);
      if (!armed_) {
        return;
      }
      armed_ = false;
      started_.set_value();
      release = release_future_;
    }
    release.wait();
  }

  std::shared_ptr<const alcedo::LutResourceResolver> default_ =
      alcedo::DefaultLutResourceResolver();
  mutable std::mutex         mutex_;
  mutable bool               armed_    = false;
  bool                       released_ = false;
  mutable std::promise<void> started_;
  std::promise<void>         release_;
  std::shared_future<void>   started_future_;
  std::shared_future<void>   release_future_;
};

#ifdef HAVE_CUDA
/// Binding of the interactive CUDA renderer of @p executor; nullopt when it has none.
auto InteractiveCudaBinding(alcedo::PipelineExecutor& executor)
    -> std::optional<alcedo::RenderBindingKey> {
  auto* renderer = executor.DebugCudaRenderer();
  if (renderer == nullptr) {
    return std::nullopt;
  }
  return renderer->Binding();
}
#endif

/**
 * @brief Editor port frames of a real linear DNG on the CUDA backend, presented to a host sink.
 *
 * The pipeline service only carries the CUDA accelerator preference to the port's executor; the
 * lease comes from an in-memory history whose root document has the DNG camera profile bound.
 * Skipped when CUDA is not compiled or no CUDA device exists, or the sample DNG is missing.
 */
class EditorSessionRenderSchedulerPortGpuTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifndef HAVE_CUDA
    GTEST_SKIP() << "CUDA backend is not compiled.";
#else
    // The service needs a storage; the lease never reads it (the history is in memory).
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    db_path_   = std::filesystem::temp_directory_path() / ("editor_render_port_" + stamp + ".db");
    meta_path_ = std::filesystem::temp_directory_path() / ("editor_render_port_" + stamp + ".json");
    project_         = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_);
    try {
      (void)alcedo::ResolveAcceleratorBackend(alcedo::AcceleratorBackendPreference::CUDA);
    } catch (const std::exception& ex) {
      GTEST_SKIP() << "No CUDA device available: " << ex.what();
    }
    service_ = std::make_shared<alcedo::PipelineMgmtService>(
        project_->GetStorage(), alcedo::AcceleratorBackendPreference::CUDA, lut_resolver_);
    const auto path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng" / "mfzoty.dng";
    if (!std::filesystem::exists(path)) {
      GTEST_SKIP() << "Sample DNG file is missing: " << path.string();
    }
    std::ifstream             stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                                    std::istreambuf_iterator<char>()};
    ASSERT_FALSE(bytes.empty());
    auto raw = std::make_unique<LibRaw>();
    ASSERT_EQ(raw->open_buffer(bytes.data(), bytes.size()), LIBRAW_SUCCESS);
    ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
    alcedo::RawRuntimeColorContext imported;
    alcedo::MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw, imported);
    const auto exif = alcedo::MetadataExtractor::ExtractEXIFFromBuffer(bytes.data(), bytes.size());
    ASSERT_NE(exif, nullptr);
    imported.dng_profile_ = alcedo::ReadDngColorProfile(exif->exifData());
    ASSERT_TRUE(imported.color_matrices_valid_);
    root_document_ =
        std::make_shared<alcedo::PipelineDocument>(alcedo::CreateDefaultPipelineDocument());
    alcedo::BindImportedCameraProfile(*root_document_, imported);
    input_ = std::make_shared<alcedo::ImageBuffer>(std::move(bytes));

    HoldRoot(alcedo::ClonePipelineDocument(*root_document_));
    ASSERT_FALSE(HasFatalFailure());

    pipeline_scheduler_ = std::make_shared<alcedo::PipelineScheduler>(1);
    scheduler_          = std::make_shared<EditorSessionRenderSchedulerPort>(pipeline_scheduler_);
    scheduler_->SetPipelinePort(held_.pipeline_port);
    scheduler_->SetSinkResolver([this] { return static_cast<alcedo::IFrameSink*>(&sink_); });
    auto context  = MakeReadyContext(kEpoch, 22, 11);
    context.input = input_;
    scheduler_->InstallSessionContext(std::move(context));
#endif
  }

  void TearDown() override {
    // A failed assertion can leave the worker blocked in a LUT lookup; let it finish first.
    lut_resolver_->Release();
    // The port and the lease use the service; the service uses the project storage.
    scheduler_.reset();
    pipeline_scheduler_.reset();
    held_ = {};
    service_.reset();
    project_.reset();
    std::error_code ec;
    if (!db_path_.empty()) {
      std::filesystem::remove(db_path_, ec);
      std::filesystem::remove(meta_path_, ec);
    }
  }

#ifdef HAVE_CUDA
  /// Hold image 22 with an in-memory history whose root is @p root; the working document starts
  /// as the root. Replaces the image held before (a new lineage) and rebinds the render port.
  void HoldRoot(alcedo::PipelineDocument root) {
    if (scheduler_) {
      scheduler_->SetPipelinePort(nullptr);
    }
    held_            = {};
    auto shared_root = std::make_shared<alcedo::PipelineDocument>(std::move(root));
    held_            = HoldImage(
        22, EditorSessionPipelineMappers{[service = service_] { return service; },
                                         [shared_root](sl_element_id_t id) {
                                           return alcedo::test::MakeInMemoryEditorLease(
                                               id, alcedo::ClonePipelineDocument(*shared_root));
                                         }});
    ASSERT_TRUE(held_.lease.has_value()) << held_.error;
    if (scheduler_) {
      scheduler_->SetPipelinePort(held_.pipeline_port);
    }
  }

  auto Working() -> alcedo::EditorWorkingDocument& { return *held_.lease->document_; }

  /// Interactive CUDA renderer of the port's executor; null before the first frame.
  auto EditorRenderer() -> alcedo::CudaRenderer* {
    const auto executor = scheduler_->interactive_executor();
    return executor ? executor->DebugCudaRenderer() : nullptr;
  }

  /// Render a viewport frame of the current preview and return the presented pixels.
  auto RenderFrame(std::uint64_t request_id) -> cv::Mat {
    const auto frame = ScheduleAndWait(*scheduler_, MakeRequest(request_id, kEpoch));
    EXPECT_TRUE(frame.has_value() && frame->success) << (frame ? frame->message : "rejected");
    return sink_.pixels.clone();
  }

  /// Comparison inputs built by the production builder from the held history and current preview.
  auto BuildInputs(const alcedo::EditorComparisonSource& a, const alcedo::EditorComparisonSource& b)
      -> alcedo::EditorComparisonInputPair {
    const auto  current = held_.pipeline_port->CurrentPreview(22);
    std::string error;
    auto pair = alcedo::BuildEditorComparisonInputs(*held_.lease->graph_, *held_.lease->root_,
                                                    current, 22, a, b, &error);
    EXPECT_TRUE(pair.has_value()) << error;
    return pair.value_or(alcedo::EditorComparisonInputPair{});
  }

  auto PairRequest(const alcedo::EditorComparisonInputPair& pair)
      -> alcedo::EditorImageRenderRequest {
    return MakeImageRequest(kEpoch, {pair.a.snapshot, pair.b.snapshot});
  }

  /// @p snapshot rendered by a new Interactive executor of its own: no cache, prepared source,
  /// or binding is shared with the editor executor. Same full-image Quality Base request as the
  /// image job.
  auto FreshImage(const alcedo::PipelineGraphSnapshot& snapshot) -> cv::Mat {
    alcedo::FramePreviewMetadata metadata;
    metadata.scope_update_allowed = false;
    auto request                  = alcedo::MakeQualityBaseApplyRequest(
        metadata, alcedo::DocumentGeometryUse::ApplyCropAndRotation);
    request.require_host_output = true;
    alcedo::PipelineExecutor executor(alcedo::ExecutorRole::Interactive);
    executor.SetAcceleratorBackendPreference(alcedo::AcceleratorBackendPreference::CUDA);
    std::unique_lock lock(executor.GetRenderLock());
    return executor.ApplyImage(snapshot, input_, request).pixels->GetCPUData().clone();
  }
#endif

  static constexpr std::uint64_t                    kEpoch = 70;
  std::shared_ptr<BlockingLutResolver> lut_resolver_ = std::make_shared<BlockingLutResolver>();
  std::filesystem::path                             db_path_;
  std::filesystem::path                             meta_path_;
  std::unique_ptr<alcedo::ProjectService>           project_;
  HostPixelFrameSink                                sink_;
  std::shared_ptr<alcedo::PipelineMgmtService>      service_;
  std::shared_ptr<alcedo::PipelineDocument>         root_document_;
  std::shared_ptr<alcedo::ImageBuffer>              input_;
  HeldImage                                         held_;
  std::shared_ptr<alcedo::PipelineScheduler>        pipeline_scheduler_;
  std::shared_ptr<EditorSessionRenderSchedulerPort> scheduler_;
};

TEST(EditorSessionRenderSchedulerPortTest,
     ProductionPipelinePathSchedulesInstalledContextWithoutAdapterBind) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(7, 22, 11));

  const auto request = MakeRequest(33, 7);
  ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());

  // The frame reached the port's executor with the resolved sink attached.
  const auto executor = scheduler->interactive_executor();
  ASSERT_NE(executor, nullptr);
  EXPECT_EQ(executor->GetFrameSink(), &sink);
  // The port never binds or sizes the sink: the presenter binds the submission with the frame.
  // The fixture has no image bytes, so the render fails before it presents.
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_EQ(sink.bind_count(), 0);
  EXPECT_EQ(PresentedSubmission(request).metadata.presentation_request_id, 33u);
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
}

// The port creates the editor's only Interactive executor on the first frame, attaches the sink
// resolved at submit to it on the worker, and keeps the same executor for later frames.
TEST(EditorSessionRenderSchedulerPortTest,
     FrameRendersOnThePortExecutorWithTheResolvedSinkAttached) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(51, 22, 11));
  ASSERT_EQ(scheduler->interactive_executor(), nullptr);

  auto request          = MakeRequest(90, 51);
  request.intent.reason = alcedo::EditorRenderReason::SettledAdjustment;
  ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());

  const auto executor = scheduler->interactive_executor();
  ASSERT_NE(executor, nullptr);
  EXPECT_TRUE(executor->Serves(alcedo::ExecutorRole::Interactive));
  EXPECT_FALSE(executor->Serves(alcedo::ExecutorRole::Batch));
  EXPECT_EQ(executor->GetFrameSink(), &sink);

  auto next          = MakeRequest(91, 51);
  next.intent.reason = alcedo::EditorRenderReason::SettledAdjustment;
  ASSERT_TRUE(ScheduleAndWait(*scheduler, next).has_value());
  EXPECT_EQ(scheduler->interactive_executor(), executor);
  EXPECT_EQ(executor->GetFrameSink(), &sink);
}

TEST(EditorSessionRenderSchedulerPortTest, ViewDrivenReasonsDisableScopeFrameReplacement) {
  const std::array view_reasons = {alcedo::EditorRenderReason::ZoomPan,
                                   alcedo::EditorRenderReason::Resize,
                                   alcedo::EditorRenderReason::DetailRefresh};
  for (std::size_t index = 0; index < view_reasons.size(); ++index) {
    auto request          = MakeRequest(60 + index, 20);
    request.intent.reason = view_reasons[index];
    EXPECT_FALSE(PresentedSubmission(request).metadata.scope_update_allowed) << index;
  }
}

TEST(EditorSessionRenderSchedulerPortTest, ScopeRefreshMarksFrameAsRequestedScopeInput) {
  auto request          = MakeRequest(72, 32);
  request.intent.reason = alcedo::EditorRenderReason::ScopeRefresh;

  const auto metadata = PresentedSubmission(request).metadata;
  EXPECT_TRUE(metadata.scope_update_allowed);
  EXPECT_TRUE(metadata.scope_refresh_requested);
  EXPECT_EQ(metadata.preview_generation, 0u);
  EXPECT_EQ(metadata.presentation_request_id, request.request_id);
}

TEST(EditorSessionRenderSchedulerPortTest, SessionDoesNotStampPreviewGenerationFromIntent) {
  const auto request  = MakeRequest(88, 41);
  const auto metadata = PresentedSubmission(request).metadata;
  EXPECT_EQ(metadata.preview_generation, 0u);
  EXPECT_EQ(metadata.presentation_request_id, request.request_id);
}

TEST(EditorSessionRenderSchedulerPortTest, RequestWithoutFrameSourceIsRejected) {
  EditorSessionRenderSchedulerPort scheduler;
  const auto                       request = MakeRequest(44, 8);

  const auto job_id = scheduler.Schedule(request);
  EXPECT_EQ(job_id, 0u);
  EXPECT_TRUE(scheduler.last_scheduled().empty());
  EXPECT_EQ(scheduler.interactive_executor(), nullptr);
}

// Rendering reads only the preview of an image the session holds. Without the lease the request
// is stale: it fails without loading the image and without creating the executor.
TEST(EditorSessionRenderSchedulerPortTest, RenderOfAnImageWithoutAHeldLeaseFailsAsStale) {
  auto pipeline_port = std::make_shared<EditorSessionPipelinePort>();
  pipeline_port->SetServices(EditorSessionPipelineMappers{
      {}, [](sl_element_id_t id) { return alcedo::test::MakeInMemoryEditorLease(id); }});
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetPipelinePort(pipeline_port);
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(13, 22, 11));
  ASSERT_EQ(pipeline_port->CurrentPreview(22), nullptr);

  const auto frame = ScheduleAndWait(*scheduler, MakeRequest(130, 13));
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->success);
  EXPECT_NE(frame->message.find("stale"), std::string::npos) << frame->message;
  EXPECT_EQ(sink.bind_count(), 0);
  EXPECT_EQ(scheduler->interactive_executor(), nullptr);
}

TEST(EditorSessionRenderSchedulerPortTest, BindSessionContextRecordsIdentityWithoutPoolRead) {
  EditorSessionRenderSchedulerPort scheduler;
  std::atomic<int>                 pool_resolve_count = 0;
  scheduler.SetServices(EditorSessionSchedulerServices{
      [&pool_resolve_count]() -> std::shared_ptr<alcedo::ImagePoolService> {
        ++pool_resolve_count;
        return nullptr;
      }});

  scheduler.BindSessionContext(/*epoch=*/7, /*element_id=*/22, /*image_id=*/11,
                               /*presentation_sink_id=*/42);

  const auto context = scheduler.session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 7u);
  EXPECT_EQ(context->element_id, 22u);
  EXPECT_EQ(context->image_id, 11u);
  EXPECT_EQ(context->presentation_sink_id, 42u);
  EXPECT_EQ(context->image, nullptr);
  EXPECT_EQ(context->input, nullptr);
  EXPECT_EQ(scheduler.context_payload_load_count(), 0u);
  EXPECT_EQ(scheduler.sink_resolve_count(), 0u);
  EXPECT_EQ(pool_resolve_count.load(std::memory_order_acquire), 0);
}

TEST(EditorSessionRenderSchedulerPortTest, ClearSessionContextDropsBoundIdentity) {
  EditorSessionRenderSchedulerPort scheduler;
  scheduler.BindSessionContext(3, 22, 11, 9);
  ASSERT_TRUE(scheduler.session_context().has_value());

  scheduler.ClearSessionContext();
  EXPECT_FALSE(scheduler.session_context().has_value());
}

TEST(EditorSessionRenderSchedulerPortTest, ImageSwitchBindReplacesPriorContextPayload) {
  EditorSessionRenderSchedulerPort scheduler;
  scheduler.InstallSessionContext(MakeReadyContext(/*epoch=*/1, /*element_id=*/22, /*image_id=*/11));
  ASSERT_TRUE(scheduler.session_context().has_value());
  ASSERT_NE(scheduler.session_context()->input, nullptr);

  scheduler.BindSessionContext(/*epoch=*/2, /*element_id=*/33, /*image_id=*/44,
                               /*presentation_sink_id=*/55);
  const auto context = scheduler.session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 2u);
  EXPECT_EQ(context->element_id, 33u);
  EXPECT_EQ(context->image_id, 44u);
  EXPECT_EQ(context->presentation_sink_id, 55u);
  EXPECT_EQ(context->image, nullptr);
  EXPECT_EQ(context->input, nullptr);
}

TEST(EditorSessionRenderSchedulerPortTest, RebindSameImageIdentityKeepsPayloadWithoutReload) {
  EditorSessionRenderSchedulerPort scheduler;
  auto                             ready = MakeReadyContext(/*epoch=*/1, /*element_id=*/22,
                                                            /*image_id=*/11, /*sink_id=*/7);
  const auto                       input_ptr = ready.input;
  const auto                       image_ptr = ready.image;
  scheduler.InstallSessionContext(std::move(ready));
  const auto loads_before = scheduler.context_payload_load_count();

  // RouteInitialRender / undo rebind the same element+image with a new epoch.
  scheduler.BindSessionContext(/*epoch=*/9, /*element_id=*/22, /*image_id=*/11,
                               /*presentation_sink_id=*/77);

  const auto context = scheduler.session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 9u);
  EXPECT_EQ(context->element_id, 22u);
  EXPECT_EQ(context->image_id, 11u);
  EXPECT_EQ(context->presentation_sink_id, 77u);
  EXPECT_EQ(context->input, input_ptr);
  EXPECT_EQ(context->image, image_ptr);
  EXPECT_EQ(scheduler.context_payload_load_count(), loads_before);
}

TEST(EditorSessionRenderSchedulerPortTest,
     InstalledContextAllowsScheduleWithoutImagePoolService) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(9, 22, 11));

  ASSERT_TRUE(ScheduleAndWait(*scheduler, MakeRequest(100, 9)).has_value());
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
  // The frame was submitted to the port's executor without an image pool.
  ASSERT_NE(scheduler->interactive_executor(), nullptr);
  EXPECT_EQ(scheduler->interactive_executor()->GetFrameSink(), &sink);
  EXPECT_EQ(sink.ensure_size_count(), 0);
}

TEST(EditorSessionRenderSchedulerPortTest,
     HotPathAfterInstalledContextDoesNotInvokeImagePoolResolver) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  std::atomic<int>   pool_resolve_count = 0;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->SetServices(EditorSessionSchedulerServices{
      [&pool_resolve_count]() -> std::shared_ptr<alcedo::ImagePoolService> {
        ++pool_resolve_count;
        return nullptr;
      }});
  scheduler->InstallSessionContext(MakeReadyContext(12, 22, 11));

  for (std::uint64_t request_id = 200; request_id < 203; ++request_id) {
    auto request          = MakeRequest(request_id, 12);
    request.intent.reason = alcedo::EditorRenderReason::InteractiveAdjustment;
    ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());
  }

  EXPECT_EQ(pool_resolve_count.load(std::memory_order_acquire), 0);
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
  const auto context = scheduler->session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 12u);
  EXPECT_NE(context->input, nullptr);
  EXPECT_NE(context->image, nullptr);
}

TEST(EditorSessionRenderSchedulerPortTest,
     BoundIdentityWithoutPayloadStillRejectsWhenPoolUnavailable) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  scheduler->BindSessionContext(5, 22, 11, 7);
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });

  const auto frame = ScheduleAndWait(*scheduler, MakeRequest(55, 5));
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->success);
  EXPECT_NE(frame->message.find("Image pool is unavailable"), std::string::npos) << frame->message;
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
  EXPECT_EQ(sink.ensure_size_count(), 0);
}

TEST(EditorSessionRenderSchedulerPortTest,
     SinkIdentityStableAcrossInteractiveFramesForBoundContext) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(12, 22, 11, /*sink_id=*/99));

  const auto resolves_before = scheduler->sink_resolve_count();
  for (std::uint64_t request_id = 300; request_id < 303; ++request_id) {
    auto request          = MakeRequest(request_id, 12, /*sink_id=*/99);
    request.intent.reason = alcedo::EditorRenderReason::InteractiveAdjustment;
    ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());
  }

  const auto context = scheduler->session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->presentation_sink_id, 99u);
  EXPECT_EQ(scheduler->sink_resolve_count(), resolves_before + 3u);
  EXPECT_EQ(sink.ensure_size_count(), 0);
}

TEST(EditorSessionRenderSchedulerPortTest,
     MismatchedPresentationSinkIdentityFailsWithoutAdapterEnsureSize) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(4, 22, 11, /*sink_id=*/10));

  auto       request = MakeRequest(401, 4, /*sink_id=*/99);
  const auto frame   = ScheduleAndWait(*scheduler, request);
  ASSERT_TRUE(frame.has_value());
  EXPECT_FALSE(frame->success);

  EXPECT_EQ(sink.bind_count(), 0);
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_EQ(scheduler->session_context()->presentation_sink_id, 10u);
}

TEST(EditorSessionRenderSchedulerPortTest,
     ForwardScheduleCompletionInvokedWithoutReverseCoordinator) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(7, 22, 11));

  auto             done   = std::make_shared<std::promise<void>>();
  auto             result = done->get_future();
  std::atomic<int> completed{0};
  ASSERT_NE(scheduler->Schedule(MakeRequest(77, 7),
                                [&completed, done](bool /*success*/, std::string /*message*/) {
                                  completed.fetch_add(1, std::memory_order_relaxed);
                                  done->set_value();
                                }),
            0u);
  ASSERT_EQ(result.wait_for(kFrameTimeout), std::future_status::ready);

  // Fixture context has no real RAW bytes; pipeline may fail — the residual
  // cleanup claim is forward completion without SetCoordinator / weak_ptr.
  EXPECT_EQ(completed.load(), 1);
}

// Application exit destroys the viewport sink after Shutdown returns. Shutdown must cancel the
// in-flight frame and wait for it, so that the frame never presents to a destroyed sink.
TEST(EditorSessionRenderSchedulerPortTest, ShutdownCancelsAndWaitsForTheInFlightFrame) {
  HeldImage held = HoldImageInMemory(22);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  auto pipeline_scheduler = std::make_shared<alcedo::PipelineScheduler>(1);
  auto scheduler          = std::make_shared<EditorSessionRenderSchedulerPort>(pipeline_scheduler);
  scheduler->SetPipelinePort(held.pipeline_port);
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(8, 22, 11));

  // Occupy the single worker so that the frame stays in flight until the test releases it.
  auto release  = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  pipeline_scheduler->ScheduleWork([released] { released.wait(); });

  auto request                = MakeRequest(88, 8);
  request.intent.cancellation = std::make_shared<alcedo::EditorRenderCancellationToken>();
  auto       done             = std::make_shared<std::promise<FrameCompletion>>();
  auto       frame            = done->get_future();
  const auto job_id = scheduler->Schedule(request, [done](bool success, std::string message) {
    done->set_value(FrameCompletion{success, std::move(message)});
  });
  ASSERT_NE(job_id, 0u);

  auto shutdown = std::async(std::launch::async, [scheduler] { scheduler->Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(200ms), std::future_status::timeout);
  EXPECT_TRUE(request.intent.cancellation->IsCancelled());
  release->set_value();
  ASSERT_EQ(shutdown.wait_for(kFrameTimeout), std::future_status::ready);

  ASSERT_EQ(frame.wait_for(kFrameTimeout), std::future_status::ready);
  EXPECT_FALSE(frame.get().success);
  EXPECT_EQ(sink.bind_count(), 0);
  EXPECT_EQ(sink.ready_count(), 0);
  EXPECT_EQ(scheduler->Schedule(MakeRequest(89, 8)), 0u);
  scheduler->Shutdown();
}

TEST(EditorSessionRenderSchedulerPortTest,
     GeometryOverlayIntentSetsRotatedUncroppedSourceOnlyForEditorRequests) {
  auto overlay                         = MakeRequest(91, 5);
  overlay.intent.geometry_overlay_only = true;
  const auto overlay_desc              = MakeEditorRenderDesc(overlay);
  EXPECT_EQ(overlay_desc.document_geometry_, alcedo::DocumentGeometryUse::RotatedUncroppedSource);
  EXPECT_EQ(overlay_desc.frame_metadata_.presentation_request_id, 91u);

  const auto closed_desc = MakeEditorRenderDesc(MakeRequest(92, 5));
  EXPECT_EQ(closed_desc.document_geometry_, alcedo::DocumentGeometryUse::ApplyCropAndRotation);

  // The value reaches the apply request unchanged.
  alcedo::PipelineTask editor_task;
  editor_task.pipeline_executor_ =
      std::make_shared<alcedo::PipelineExecutor>(alcedo::ExecutorRole::Interactive);
  editor_task.options_.render_desc_ = overlay_desc;
  EXPECT_EQ(editor_task.MakeApplyRequest().geometry.document_geometry,
            alcedo::DocumentGeometryUse::RotatedUncroppedSource);

  // Thumbnail and export descriptions are built by their services from a default RenderDesc.
  for (const auto type : {alcedo::RenderType::THUMBNAIL, alcedo::RenderType::FULL_RES_EXPORT}) {
    alcedo::PipelineTask task;
    task.pipeline_executor_                 = editor_task.pipeline_executor_;
    task.options_.render_desc_.render_type_ = type;
    EXPECT_EQ(task.MakeApplyRequest().geometry.document_geometry,
              alcedo::DocumentGeometryUse::ApplyCropAndRotation);
  }
}

// Each frame renders the preview the history published last before dispatch: a published
// exposure change shows in the next frame, and a later write that was not published does not.
TEST_F(EditorSessionRenderSchedulerPortGpuTest, FrameRendersThePreviewPublishedLastAtDispatch) {
#ifdef HAVE_CUDA
  auto& working = *held_.lease->document_;
  WriteExposure(working.Document(), 0.0f);
  ASSERT_FALSE(HasFatalFailure());
  (void)working.PublishPreview();

  const auto first = ScheduleAndWait(*scheduler_, MakeRequest(700, kEpoch));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->success) << first->message;
  const cv::Mat before = sink_.pixels.clone();
  ASSERT_FALSE(before.empty());

  WriteExposure(working.Document(), 1.5f);
  ASSERT_FALSE(HasFatalFailure());
  const auto published = working.PublishPreview();
  // Written after the publication: the frame must not read it.
  WriteExposure(working.Document(), -3.0f);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(held_.pipeline_port->CurrentPreview(22), published);

  const auto second = ScheduleAndWait(*scheduler_, MakeRequest(701, kEpoch));
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->success) << second->message;
  const cv::Mat after = sink_.pixels.clone();
  ASSERT_EQ(after.size(), before.size());

  EXPECT_GT(MeanOfColorChannels(after), MeanOfColorChannels(before) * 1.2);
  // Both previews belong to one loaded history: the executor kept its binding.
  const auto executor = scheduler_->interactive_executor();
  ASSERT_NE(executor, nullptr);
  EXPECT_EQ(InteractiveCudaBinding(*executor),
            (alcedo::RenderBindingKey{.lineage = published->Lineage(), .element_id = 22}));
#endif
}

// The presenter binds the submission the port stamped from the request with the frame it presents.
TEST_F(EditorSessionRenderSchedulerPortGpuTest, PresentedFrameCarriesTheRequestMetadata) {
#ifdef HAVE_CUDA
  auto request          = MakeRequest(705, kEpoch);
  request.intent.reason = alcedo::EditorRenderReason::ScopeRefresh;
  const auto frame      = ScheduleAndWait(*scheduler_, request);
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->success) << frame->message;

  const auto& metadata = sink_.bound_submission.metadata;
  EXPECT_EQ(metadata.presentation_request_id, 705u);
  EXPECT_TRUE(metadata.scope_refresh_requested);
  EXPECT_TRUE(metadata.scope_update_allowed);
  EXPECT_EQ(metadata.preview_generation, 0u);
#endif
}

// Closing the image releases every resource the executor holds for it after the in-flight
// frame; the executor and its device stay for the next image.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       ClearSessionContextReleasesTheExecutorBindingAfterTheFrame) {
#ifdef HAVE_CUDA
  const auto frame = ScheduleAndWait(*scheduler_, MakeRequest(710, kEpoch));
  ASSERT_TRUE(frame.has_value());
  ASSERT_TRUE(frame->success) << frame->message;
  const auto executor = scheduler_->interactive_executor();
  ASSERT_NE(executor, nullptr);
  ASSERT_NE(executor->DebugCudaRenderer(), nullptr);
  EXPECT_EQ(
      InteractiveCudaBinding(*executor),
      (alcedo::RenderBindingKey{.lineage = held_.lease->document_->Lineage(), .element_id = 22}));
  const auto device_identity = executor->DebugCudaRenderer()->DebugDeviceIdentity();

  scheduler_->ClearSessionContext();
  WaitForQueuedWork(*pipeline_scheduler_);
  ASSERT_FALSE(HasFatalFailure());

  EXPECT_FALSE(scheduler_->session_context().has_value());
  EXPECT_EQ(scheduler_->interactive_executor(), executor);
  ASSERT_NE(executor->DebugCudaRenderer(), nullptr);
  EXPECT_EQ(InteractiveCudaBinding(*executor), std::nullopt);
  EXPECT_EQ(executor->DebugCudaRenderer()->Resources().published_result_count, 0u);
  EXPECT_EQ(executor->DebugCudaRenderer()->DebugDeviceIdentity(), device_identity);
#endif
}

// The editor's Quality Base frame and the image job start from one request builder, so the image
// job uses exactly the decode, long edge, resampling, and persistence of the Quality Base frame.
TEST(EditorSessionRenderSchedulerPortTest, QualityBaseFrameAndImageJobShareOneRequestBuilder) {
  alcedo::FramePreviewMetadata metadata;
  metadata.image_identity  = 11;
  metadata.source_roi_norm = {0.25f, 0.25f, 0.5f, 0.5f};
  const auto quality       = alcedo::MakeQualityBaseApplyRequest(
      metadata, alcedo::DocumentGeometryUse::ApplyCropAndRotation);
  EXPECT_EQ(quality.decode_res, alcedo::DecodeRes::FULL);
  EXPECT_EQ(quality.role, alcedo::ExecutorRole::Interactive);
  EXPECT_EQ(quality.geometry.resolution.max_edge, 4096u);
  EXPECT_EQ(quality.geometry.resolution.quality, alcedo::RenderQuality::Preview);
  EXPECT_TRUE(quality.geometry.view.visible_rect_in_edit_space.IsFullFrame());
  EXPECT_EQ(quality.submission.metadata.frame_role, alcedo::FrameRole::QualityBase);
  EXPECT_EQ(quality.submission.metadata.image_identity, 11u);
  EXPECT_FLOAT_EQ(quality.submission.metadata.source_roi_norm.width, 1.0f);
  EXPECT_EQ(quality.sink, nullptr);
  EXPECT_FALSE(quality.require_host_output);

  // The viewport's Quality Base frame: the same request plus the attached viewport sink.
  RecordingFrameSink   sink;
  alcedo::PipelineTask task;
  task.pipeline_executor_ =
      std::make_shared<alcedo::PipelineExecutor>(alcedo::ExecutorRole::Interactive);
  task.pipeline_executor_->AttachFrameSink(&sink);
  task.options_.render_desc_.render_type_ = alcedo::RenderType::QUALITY_BASE_PREVIEW;
  const auto frame                        = task.MakeApplyRequest();
  EXPECT_EQ(frame.decode_res, quality.decode_res);
  EXPECT_EQ(frame.role, quality.role);
  EXPECT_EQ(frame.geometry.resolution.max_edge, quality.geometry.resolution.max_edge);
  EXPECT_EQ(frame.geometry.resolution.quality, quality.geometry.resolution.quality);
  EXPECT_EQ(frame.submission.metadata.frame_role, alcedo::FrameRole::QualityBase);
  EXPECT_EQ(frame.submission.mode, quality.submission.mode);
  EXPECT_EQ(frame.sink, &sink);
  EXPECT_FALSE(frame.require_host_output);

  // The image job's default geometry is the same full-image Quality Base geometry.
  const alcedo::EditorImageRenderRequest images;
  EXPECT_EQ(images.geometry.resolution.max_edge, quality.geometry.resolution.max_edge);
  EXPECT_EQ(images.geometry.resolution.quality, quality.geometry.resolution.quality);
  EXPECT_EQ(images.geometry.document_geometry, alcedo::DocumentGeometryUse::ApplyCropAndRotation);
}

// Admission: an image job renders only documents of the bound, held image, one job at a time,
// and a rejected job never completes.
TEST(EditorSessionRenderSchedulerPortTest, ImageJobRejectsDocumentsThatWouldRebindOrOverlap) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  const auto preview = held.pipeline_port->CurrentPreview(22);
  ASSERT_NE(preview, nullptr);
  std::atomic<int> completions{0};
  const auto       count = [&completions](alcedo::EditorImageRenderResult) { ++completions; };

  std::string      error;
  EXPECT_EQ(scheduler->ScheduleImages(MakeImageRequest(9, {preview}), count, &error), 0u);
  EXPECT_NE(error.find("not the bound editor image"), std::string::npos) << error;

  scheduler->InstallSessionContext(MakeReadyContext(9, 22, 11));
  EXPECT_EQ(scheduler->ScheduleImages(MakeImageRequest(9, {}), count, &error), 0u);
  EXPECT_NE(error.find("one or two"), std::string::npos) << error;
  EXPECT_EQ(
      scheduler->ScheduleImages(MakeImageRequest(9, {preview, preview, preview}), count, &error),
      0u);
  EXPECT_EQ(scheduler->ScheduleImages(MakeImageRequest(8, {preview}), count, &error), 0u);
  EXPECT_NE(error.find("not the bound editor image"), std::string::npos) << error;

  // A document of another lineage would release the image's prepared source and sensor result.
  const auto foreign = alcedo::PipelineGraphSnapshot::Preview(preview->Document().Freeze(), 22,
                                                              alcedo::PipelineLineageId::Next(),
                                                              alcedo::transaction_chain_hash_t{});
  EXPECT_EQ(scheduler->ScheduleImages(MakeImageRequest(9, {preview, foreign}), count, &error), 0u);
  EXPECT_NE(error.find("does not belong"), std::string::npos) << error;

  // Hold the single worker so the first job stays accepted.
  auto pipeline_scheduler = std::make_shared<alcedo::PipelineScheduler>(1);
  auto blocked            = std::make_shared<EditorSessionRenderSchedulerPort>(pipeline_scheduler);
  blocked->SetPipelinePort(held.pipeline_port);
  blocked->InstallSessionContext(MakeReadyContext(9, 22, 11));
  auto release  = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  pipeline_scheduler->ScheduleWork([released] { released.wait(); });
  const auto first = blocked->ScheduleImages(MakeImageRequest(9, {preview}), count, &error);
  ASSERT_NE(first, 0u) << error;
  EXPECT_EQ(blocked->ScheduleImages(MakeImageRequest(9, {preview}), count, &error), 0u);
  EXPECT_NE(error.find("Another image job"), std::string::npos) << error;
  blocked->CancelImages(first);
  release->set_value();
  blocked->Shutdown();
  EXPECT_EQ(blocked->ScheduleImages(MakeImageRequest(9, {preview}), count, &error), 0u);
  EXPECT_NE(error.find("shutting down"), std::string::npos) << error;

  // Only the accepted job completed.
  EXPECT_EQ(completions.load(), 1);
}

// Close while the job is still queued: no document renders and nothing is published.
TEST(EditorSessionRenderSchedulerPortTest, QueuedImageJobCancelledBeforeItStartsRendersNothing) {
  HeldImage held = HoldImageInMemory(22);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  auto pipeline_scheduler = std::make_shared<alcedo::PipelineScheduler>(1);
  auto scheduler          = std::make_shared<EditorSessionRenderSchedulerPort>(pipeline_scheduler);
  scheduler->SetPipelinePort(held.pipeline_port);
  scheduler->InstallSessionContext(MakeReadyContext(9, 22, 11));
  const auto preview  = held.pipeline_port->CurrentPreview(22);

  auto       release  = std::make_shared<std::promise<void>>();
  auto       released = release->get_future().share();
  pipeline_scheduler->ScheduleWork([released] { released.wait(); });
  auto        done   = std::make_shared<std::promise<alcedo::EditorImageRenderResult>>();
  auto        result = done->get_future();
  std::string error;
  const auto  job_id = scheduler->ScheduleImages(
      MakeImageRequest(9, {preview, preview}),
      [done](alcedo::EditorImageRenderResult images) { done->set_value(std::move(images)); },
      &error);
  ASSERT_NE(job_id, 0u) << error;
  scheduler->CancelImages(job_id);
  release->set_value();

  ASSERT_EQ(result.wait_for(kFrameTimeout), std::future_status::ready);
  const auto images = result.get();
  EXPECT_EQ(images.status, alcedo::EditorImageRenderStatus::Cancelled);
  EXPECT_TRUE(images.images.empty());
  // Neither document reached a renderer: the executor never created one.
  const auto executor = scheduler->interactive_executor();
  ASSERT_NE(executor, nullptr);
#ifdef HAVE_CUDA
  EXPECT_EQ(executor->DebugCudaRenderer(), nullptr);
#endif
}

// A render error ends the job with that error and no image; the port accepts the next job.
TEST(EditorSessionRenderSchedulerPortTest, ImageJobFailureReportsTheRenderErrorWithoutImages) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  // The fixture context has no encoded bytes, so the first document fails in the renderer.
  scheduler->InstallSessionContext(MakeReadyContext(9, 22, 11));
  const auto preview = held.pipeline_port->CurrentPreview(22);

  const auto first   = ScheduleImagesAndWait(*scheduler, MakeImageRequest(9, {preview, preview}));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->status, alcedo::EditorImageRenderStatus::Failed);
  EXPECT_FALSE(first->message.empty());
  EXPECT_TRUE(first->images.empty());

  const auto second = ScheduleImagesAndWait(*scheduler, MakeImageRequest(9, {preview}));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->status, alcedo::EditorImageRenderStatus::Failed);
}

// After a warm viewport frame of a real RAW, a Root/Current pair reuses the editor's prepared
// source and sensor result: no LibRaw unpack and no sensor develop. The next viewport frame
// still reads the editor's own results.
TEST_F(EditorSessionRenderSchedulerPortGpuTest, WarmComparisonPairReusesEditorSensorResult) {
#ifdef HAVE_CUDA
  // A current sensor setting that differs from the root.
  WriteHighlightsReconstruct(Working().Document(), false);
  WriteExposure(Working().Document(), 0.5f);
  ASSERT_FALSE(HasFatalFailure());
  (void)Working().PublishPreview();
  const cv::Mat frame_before = RenderFrame(800);
  ASSERT_FALSE(frame_before.empty());
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);
  const auto published_before = renderer->Resources().published_result_count;

  const auto inputs           = BuildInputs(alcedo::EditorComparisonSource::Root(),
                                            alcedo::EditorComparisonSource::Current());
  ASSERT_NE(inputs.a.snapshot, nullptr);
  renderer->ResetStats();
  const auto pair = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(pair.has_value());
  ASSERT_EQ(pair->status, alcedo::EditorImageRenderStatus::Completed) << pair->message;
  ASSERT_EQ(pair->images.size(), 2u);

  const auto stats = renderer->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 0u);
  EXPECT_EQ(stats.prepared_source_misses, 0u);
  EXPECT_EQ(stats.prepared_source_hits, 2u);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0u);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 2u);
  EXPECT_EQ(renderer->Resources().published_result_count, published_before);

  // The viewport resumes from the editor's results without a new sensor develop.
  renderer->ResetStats();
  const cv::Mat frame_after = RenderFrame(801);
  ASSERT_EQ(frame_after.size(), frame_before.size());
  EXPECT_LT(cv::norm(frame_after, frame_before, cv::NORM_INF), 2e-5);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0u);
  EXPECT_EQ(renderer->Stats().libraw_open_unpack_count, 0u);
#endif
}

// The pair keeps each side's white balance: the pixels differ by white balance alone, and each
// side matches a fresh render of its own document, while the sensor result is reused.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       WhiteBalanceDifferenceChangesPairPixelsWithoutSensorExecution) {
#ifdef HAVE_CUDA
  WriteCustomWhiteBalance(Working().Document(), 3200.0f, 12.0f);
  (void)Working().PublishPreview();
  (void)RenderFrame(810);
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);

  const auto inputs = BuildInputs(alcedo::EditorComparisonSource::Root(),
                                  alcedo::EditorComparisonSource::Current());
  ASSERT_NE(inputs.a.snapshot, nullptr);
  renderer->ResetStats();
  const auto pair = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(pair.has_value());
  ASSERT_EQ(pair->status, alcedo::EditorImageRenderStatus::Completed) << pair->message;
  ASSERT_EQ(pair->images.size(), 2u);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0u);

  const cv::Mat root    = pair->images[0].pixels->GetCPUData();
  const cv::Mat current = pair->images[1].pixels->GetCPUData();
  ASSERT_EQ(root.type(), CV_32FC4);
  ASSERT_EQ(root.size(), current.size());
  EXPECT_TRUE(cv::checkRange(root));
  EXPECT_TRUE(cv::checkRange(current));
  // Mean absolute channel difference: a 3200 K custom white balance against as shot.
  EXPECT_GT(cv::norm(root, current, cv::NORM_L1) / (4.0 * static_cast<double>(root.total())), 0.01);
  EXPECT_LT(cv::norm(root, FreshImage(*inputs.a.snapshot), cv::NORM_INF), 2e-5);
  EXPECT_LT(cv::norm(current, FreshImage(*inputs.b.snapshot), cv::NORM_INF), 2e-5);
#endif
}

// The pair runs on the editor's own executor, device, and queue, keeps its binding and attached
// viewport sink, and presents nothing to that sink.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       ComparisonHostRequestsKeepEditorExecutorAndQueueIdentity) {
#ifdef HAVE_CUDA
  (void)RenderFrame(820);
  const auto executor = scheduler_->interactive_executor();
  ASSERT_NE(executor, nullptr);
  auto* renderer = executor->DebugCudaRenderer();
  ASSERT_NE(renderer, nullptr);
  const auto device_identity = renderer->DebugDeviceIdentity();
  const auto queue_identity  = renderer->DebugQueueIdentity();
  const auto binding         = renderer->Binding();
  const auto ready_before    = sink_.ready_count();
  ASSERT_EQ(executor->GetFrameSink(), &sink_);

  const auto inputs = BuildInputs(alcedo::EditorComparisonSource::Root(),
                                  alcedo::EditorComparisonSource::Current());
  const auto pair   = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(pair.has_value());
  ASSERT_EQ(pair->status, alcedo::EditorImageRenderStatus::Completed) << pair->message;

  EXPECT_EQ(scheduler_->interactive_executor(), executor);
  EXPECT_EQ(executor->DebugCudaRenderer(), renderer);
  EXPECT_EQ(executor->DebugCudaBatchRenderer(), nullptr);
  EXPECT_EQ(renderer->DebugDeviceIdentity(), device_identity);
  EXPECT_EQ(renderer->DebugQueueIdentity(), queue_identity);
  EXPECT_EQ(renderer->Binding(), binding);
  EXPECT_EQ(sink_.ready_count(), ready_before);
  EXPECT_EQ(executor->GetFrameSink(), &sink_);
  ASSERT_EQ(pair->images.size(), 2u);
  const std::array<const alcedo::PipelineGraphSnapshot*, 2> documents{inputs.a.snapshot.get(),
                                                                      inputs.b.snapshot.get()};
  for (std::size_t side = 0; side < documents.size(); ++side) {
    const auto& image = pair->images[side];
    // Geometry and display configuration of the executed full-image Quality Base plan.
    EXPECT_EQ(static_cast<std::uint32_t>(image.pixels->GetCPUData().cols),
              image.geometry.render_extent.width);
    EXPECT_EQ(static_cast<std::uint32_t>(image.pixels->GetCPUData().rows),
              image.geometry.render_extent.height);
    EXPECT_LE(std::max(image.geometry.render_extent.width, image.geometry.render_extent.height),
              alcedo::kQualityBaseMaxLongEdge);
    EXPECT_EQ(image.display, alcedo::ViewerDisplayConfigFromDrt(
                                 documents[side]->Document().Drt()->Params().Params()));
  }
#endif
}

// Worker order: a frame queued before the pair finishes first, a frame queued after the pair
// starts only after both images, and no frame runs between A and B.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       ComparisonPairDoesNotInterleaveAAndBWithNormalFrames) {
#ifdef HAVE_CUDA
  (void)RenderFrame(830);
  const auto               inputs       = BuildInputs(alcedo::EditorComparisonSource::Root(),
                                                      alcedo::EditorComparisonSource::Current());
  const auto               ready_before = sink_.ready_count();

  std::mutex               order_mutex;
  std::vector<std::string> order;
  int                      ready_at_pair = -1;
  const auto               record        = [&order_mutex, &order](std::string event) {
    std::scoped_lock lock(order_mutex);
    order.push_back(std::move(event));
  };

  // Hold the worker so that the frame and the pair are both queued before either runs.
  auto release  = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  pipeline_scheduler_->ScheduleWork([released] { released.wait(); });

  auto       last_done = std::make_shared<std::promise<void>>();
  auto       last      = last_done->get_future();
  // The second frame is scheduled from the first frame's completion, after the pair was queued,
  // as the coordinator schedules its next frame.
  const auto first     = scheduler_->Schedule(
      MakeRequest(831, kEpoch), [this, &record, last_done](bool success, std::string) {
        record(success ? "frame-1" : "frame-1-failed");
        const auto second = scheduler_->Schedule(MakeRequest(832, kEpoch),
                                                     [&record, last_done](bool ok, std::string) {
                                                   record(ok ? "frame-2" : "frame-2-failed");
                                                   last_done->set_value();
                                                 });
        if (second == 0) {
          record("frame-2-rejected");
          last_done->set_value();
        }
      });
  ASSERT_NE(first, 0u);
  std::string error;
  const auto  job_id = scheduler_->ScheduleImages(
      PairRequest(inputs),
      [this, &record, &ready_at_pair](alcedo::EditorImageRenderResult images) {
        ready_at_pair = sink_.ready_count();
        record(images.status == alcedo::EditorImageRenderStatus::Completed &&
                        images.images.size() == 2
                    ? "pair"
                    : "pair-failed");
      },
      &error);
  ASSERT_NE(job_id, 0u) << error;
  release->set_value();
  ASSERT_EQ(last.wait_for(kFrameTimeout), std::future_status::ready);

  std::scoped_lock lock(order_mutex);
  EXPECT_EQ(order, (std::vector<std::string>{"frame-1", "pair", "frame-2"}));
  // Only the first frame presented before the pair completed.
  EXPECT_EQ(ready_at_pair, ready_before + 1);
  EXPECT_EQ(sink_.ready_count(), ready_before + 2);
#endif
}

// Different Grade topology, crop, rotation, and white balance on the two sides: each side matches
// a fresh render of its own derived document, while both share the current sensor result.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       DifferentVersionTopologyRendersCorrectPixelsWithSharedSensor) {
#ifdef HAVE_CUDA
  auto& document = Working().Document();
  WriteHighlightsReconstruct(document, false);
  WriteCustomWhiteBalance(document, 4800.0f, -6.0f);
  ASSERT_TRUE(
      alcedo::AddCleanColorGrade(document, alcedo::NodeId{"drt"}, alcedo::NodeId{"grade.compare"})
          .empty());
  auto* grade = dynamic_cast<alcedo::ColorGradeNodeModel*>(
      document.Graph().FindNode(alcedo::NodeId{"grade.compare"}));
  ASSERT_NE(grade, nullptr);
  auto* contrast = dynamic_cast<alcedo::ContrastModel*>(
      grade->FindAdjustmentByType(alcedo::type_ids::Contrast()));
  ASSERT_NE(contrast, nullptr);
  contrast->SetValue(40.0f);
  document.Geometry().SetCropRect({0.1f, 0.15f, 0.6f, 0.7f});
  document.Geometry().SetRotationDegrees(4.0f);
  (void)Working().PublishPreview();
  (void)RenderFrame(840);
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);

  const auto inputs = BuildInputs(alcedo::EditorComparisonSource::Current(),
                                  alcedo::EditorComparisonSource::Root());
  ASSERT_NE(inputs.b.snapshot, nullptr);
  ASSERT_EQ(inputs.b.snapshot->Document().Graph().FindNode(alcedo::NodeId{"grade.compare"}),
            nullptr);
  renderer->ResetStats();
  const auto pair = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(pair.has_value());
  ASSERT_EQ(pair->status, alcedo::EditorImageRenderStatus::Completed) << pair->message;
  ASSERT_EQ(pair->images.size(), 2u);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0u);

  const auto& current = pair->images[0];
  const auto& root    = pair->images[1];
  // The cropped side covers part of the reference; the root covers all of it.
  EXPECT_NE(current.geometry.render_extent, root.geometry.render_extent);
  EXPECT_EQ(current.geometry.full_reference_extent, root.geometry.full_reference_extent);
  const cv::Mat fresh_current = FreshImage(*inputs.a.snapshot);
  const cv::Mat fresh_root    = FreshImage(*inputs.b.snapshot);
  ASSERT_EQ(current.pixels->GetCPUData().size(), fresh_current.size());
  ASSERT_EQ(root.pixels->GetCPUData().size(), fresh_root.size());
  EXPECT_LT(cv::norm(current.pixels->GetCPUData(), fresh_current, cv::NORM_INF), 2e-5);
  EXPECT_LT(cv::norm(root.pixels->GetCPUData(), fresh_root, cv::NORM_INF), 2e-5);
#endif
}

// A's host pixels stay valid after B renders and the renderer releases B's submission-local
// storage; the pair adds no persistent result to the editor cache.
TEST_F(EditorSessionRenderSchedulerPortGpuTest, AResultRemainsValidAfterBAndTransientRelease) {
#ifdef HAVE_CUDA
  WriteCustomWhiteBalance(Working().Document(), 5600.0f, 4.0f);
  (void)Working().PublishPreview();
  (void)RenderFrame(850);
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);
  const auto before = renderer->Resources();

  const auto inputs = BuildInputs(alcedo::EditorComparisonSource::Current(),
                                  alcedo::EditorComparisonSource::Root());
  const auto pair   = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(pair.has_value());
  ASSERT_EQ(pair->status, alcedo::EditorImageRenderStatus::Completed) << pair->message;
  ASSERT_EQ(pair->images.size(), 2u);
  ASSERT_NE(pair->images[0].pixels.get(), pair->images[1].pixels.get());

  const auto after = renderer->Resources();
  EXPECT_EQ(after.published_result_count, before.published_result_count);
  EXPECT_EQ(after.session_value_ids, before.session_value_ids);
  EXPECT_EQ(after.prepared_source_entry_count, before.prepared_source_entry_count);
  EXPECT_EQ(std::as_const(*renderer).Device().Workspace().Images().UnpublishedCount(), 0u);
  // A was downloaded before B ran; its pixels still equal a fresh render of A.
  EXPECT_LT(
      cv::norm(pair->images[0].pixels->GetCPUData(), FreshImage(*inputs.a.snapshot), cv::NORM_INF),
      2e-5);
#endif
}

// Close while A renders: B never starts, and the finished A is not published.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       ComparisonCloseDuringARenderSkipsBAndCannotPublish) {
#ifdef HAVE_CUDA
  // Only A (the current values) references a LUT; its lookup holds the worker inside A.
  WriteLmtCubePath(Working().Document(), "comparison_close_during_a_missing.cube");
  ASSERT_FALSE(HasFatalFailure());
  (void)Working().PublishPreview();
  (void)RenderFrame(860);
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);
  const auto inputs = BuildInputs(alcedo::EditorComparisonSource::Current(),
                                  alcedo::EditorComparisonSource::Root());
  renderer->ResetStats();

  lut_resolver_->Arm();
  auto        done   = std::make_shared<std::promise<alcedo::EditorImageRenderResult>>();
  auto        result = done->get_future();
  std::string error;
  const auto  job_id = scheduler_->ScheduleImages(
      PairRequest(inputs),
      [done](alcedo::EditorImageRenderResult images) { done->set_value(std::move(images)); },
      &error);
  ASSERT_NE(job_id, 0u) << error;
  ASSERT_EQ(lut_resolver_->Started().wait_for(kFrameTimeout), std::future_status::ready);
  // The owner closes the comparison while A renders on the worker.
  scheduler_->CancelImages(job_id);
  lut_resolver_->Release();

  ASSERT_EQ(result.wait_for(kFrameTimeout), std::future_status::ready);
  const auto images = result.get();
  EXPECT_EQ(images.status, alcedo::EditorImageRenderStatus::Cancelled);
  EXPECT_TRUE(images.images.empty());
  // Only A acquired the prepared source; B did not render.
  EXPECT_EQ(renderer->Stats().prepared_source_hits, 1u);

  // The port accepts the next job.
  const auto next = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next->status, alcedo::EditorImageRenderStatus::Completed) << next->message;
#endif
}

// Application shutdown during A: Shutdown cancels the job and returns only after its completion
// ran; B never starts.
TEST_F(EditorSessionRenderSchedulerPortGpuTest, ShutdownDuringAImageCancelsAndWaitsForTheJob) {
#ifdef HAVE_CUDA
  WriteLmtCubePath(Working().Document(), "comparison_shutdown_during_a_missing.cube");
  ASSERT_FALSE(HasFatalFailure());
  (void)Working().PublishPreview();
  (void)RenderFrame(870);
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);
  const auto inputs = BuildInputs(alcedo::EditorComparisonSource::Current(),
                                  alcedo::EditorComparisonSource::Root());
  renderer->ResetStats();

  lut_resolver_->Arm();
  std::atomic<bool> completed{false};
  auto              done   = std::make_shared<std::promise<alcedo::EditorImageRenderResult>>();
  auto              result = done->get_future();
  std::string       error;
  const auto        job_id = scheduler_->ScheduleImages(
      PairRequest(inputs),
      [done, &completed](alcedo::EditorImageRenderResult images) {
        completed = true;
        done->set_value(std::move(images));
      },
      &error);
  ASSERT_NE(job_id, 0u) << error;
  ASSERT_EQ(lut_resolver_->Started().wait_for(kFrameTimeout), std::future_status::ready);

  auto shutdown = std::async(std::launch::async, [this] { scheduler_->Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(200ms), std::future_status::timeout);
  lut_resolver_->Release();
  ASSERT_EQ(shutdown.wait_for(kFrameTimeout), std::future_status::ready);
  // Shutdown returned only after the completion ran.
  EXPECT_TRUE(completed.load());

  ASSERT_EQ(result.wait_for(0s), std::future_status::ready);
  const auto images = result.get();
  EXPECT_EQ(images.status, alcedo::EditorImageRenderStatus::Cancelled);
  EXPECT_TRUE(images.images.empty());
  EXPECT_EQ(renderer->Stats().prepared_source_hits, 1u);
  EXPECT_EQ(scheduler_->ScheduleImages(PairRequest(inputs), {}, &error), 0u);
#endif
}

// A failed B (a corrupted LUT in the root) fails the whole pair with the real error. The next
// viewport frame of the current document renders the same pixels as before, without a new
// sensor develop.
TEST_F(EditorSessionRenderSchedulerPortGpuTest,
       ComparisonFailureAllowsNormalCurrentDocumentRender) {
#ifdef HAVE_CUDA
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto directory = std::filesystem::path{"build/tmp/editor_comparison"};
  std::filesystem::create_directories(directory);
  const auto broken_cube = directory / ("broken_root_lut_" + stamp + ".cube");
  {
    std::ofstream out(broken_cube);
    out << "LUT_3D_SIZE 2\n0.1 0.2\nnot numbers\n";
  }
  auto root = alcedo::ClonePipelineDocument(*root_document_);
  WriteLmtCubePath(root, std::filesystem::absolute(broken_cube).string());
  ASSERT_FALSE(HasFatalFailure());
  HoldRoot(std::move(root));
  ASSERT_FALSE(HasFatalFailure());
  // The working values drop the root's LUT; only the root side references it.
  WriteLmtCubePath(Working().Document(), "");
  WriteCustomWhiteBalance(Working().Document(), 4300.0f, 3.0f);
  ASSERT_FALSE(HasFatalFailure());
  (void)Working().PublishPreview();
  const cv::Mat frame_before = RenderFrame(880);
  ASSERT_FALSE(frame_before.empty());
  auto* renderer = EditorRenderer();
  ASSERT_NE(renderer, nullptr);
  const auto binding = renderer->Binding();

  const auto inputs  = BuildInputs(alcedo::EditorComparisonSource::Current(),
                                   alcedo::EditorComparisonSource::Root());
  ASSERT_NE(inputs.b.snapshot, nullptr);
  const auto pair = ScheduleImagesAndWait(*scheduler_, PairRequest(inputs));
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->status, alcedo::EditorImageRenderStatus::Failed);
  EXPECT_FALSE(pair->message.empty());
  EXPECT_TRUE(pair->images.empty());
  EXPECT_EQ(std::as_const(*renderer).Device().Workspace().Images().UnpublishedCount(), 0u);

  renderer->ResetStats();
  const cv::Mat frame_after = RenderFrame(881);
  ASSERT_EQ(frame_after.size(), frame_before.size());
  EXPECT_LT(cv::norm(frame_after, frame_before, cv::NORM_INF), 2e-5);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0u);
  EXPECT_EQ(renderer->Binding(), binding);
  std::error_code ec;
  std::filesystem::remove(broken_cube, ec);
#endif
}

}  // namespace
}  // namespace alcedo::ui
