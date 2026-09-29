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
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../support/editor_lease_test_support.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "image/dng_color_profile_import.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"
#include "renderer/pipeline_scheduler.hpp"
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
  void NotifyFrameReady(const alcedo::FrameCompletionSubmission& /*submission*/) override {}
  [[nodiscard]] auto GetWidth() const -> int override { return pixels.cols; }
  [[nodiscard]] auto GetHeight() const -> int override { return pixels.rows; }

  cv::Mat            pixels;
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
    project_   = std::make_unique<alcedo::ProjectService>(db_path_, meta_path_);
    service_   = std::make_shared<alcedo::PipelineMgmtService>(project_->GetStorage());
    try {
      service_->SetAcceleratorBackendPreference(alcedo::AcceleratorBackendPreference::CUDA);
    } catch (const std::exception& ex) {
      GTEST_SKIP() << "No CUDA device available: " << ex.what();
    }
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

    held_ =
        HoldImage(22, EditorSessionPipelineMappers{[service = service_] { return service; },
                                                   [root = root_document_](sl_element_id_t id) {
                                                     return alcedo::test::MakeInMemoryEditorLease(
                                                         id, alcedo::ClonePipelineDocument(*root));
                                                   }});
    ASSERT_TRUE(held_.lease.has_value()) << held_.error;

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

  static constexpr std::uint64_t                    kEpoch = 70;
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

  ASSERT_TRUE(ScheduleAndWait(*scheduler, MakeRequest(33, 7)).has_value());

  // Production Dispatch never calls EnsureSize.
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_GE(sink.bind_count(), 1);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, 33u);
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
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(20, 22, 11));

  const std::array view_reasons = {alcedo::EditorRenderReason::ZoomPan,
                                   alcedo::EditorRenderReason::Resize,
                                   alcedo::EditorRenderReason::DetailRefresh};
  for (std::size_t index = 0; index < view_reasons.size(); ++index) {
    auto request          = MakeRequest(60 + index, 20);
    request.intent.reason = view_reasons[index];
    ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());
    EXPECT_FALSE(sink.last_submission.metadata.scope_update_allowed);
    EXPECT_EQ(sink.ensure_size_count(), 0);
  }
}

TEST(EditorSessionRenderSchedulerPortTest, ScopeRefreshMarksFrameAsRequestedScopeInput) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(32, 22, 11));

  auto request          = MakeRequest(72, 32);
  request.intent.reason = alcedo::EditorRenderReason::ScopeRefresh;
  ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());

  EXPECT_TRUE(sink.last_submission.metadata.scope_update_allowed);
  EXPECT_TRUE(sink.last_submission.metadata.scope_refresh_requested);
  EXPECT_EQ(sink.last_submission.metadata.preview_generation, 0u);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, request.request_id);
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_GE(sink.bind_count(), 1);
}

TEST(EditorSessionRenderSchedulerPortTest, SessionDoesNotStampPreviewGenerationFromIntent) {
  HeldImage held;
  auto      scheduler = MakeSchedulerHoldingImage(held);
  ASSERT_TRUE(held.lease.has_value()) << held.error;
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(41, 22, 11));

  auto request = MakeRequest(88, 41);
  ASSERT_TRUE(ScheduleAndWait(*scheduler, request).has_value());

  EXPECT_EQ(sink.last_submission.metadata.preview_generation, 0u);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, request.request_id);
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
  EXPECT_GE(sink.bind_count(), 1);
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

TEST(EditorSessionRenderSchedulerPortTest,
     GeometryOverlayIntentSetsUncroppedSourceOnlyForEditorRequests) {
  auto overlay                         = MakeRequest(91, 5);
  overlay.intent.geometry_overlay_only = true;
  const auto overlay_desc              = MakeEditorRenderDesc(overlay);
  EXPECT_EQ(overlay_desc.document_geometry_, alcedo::DocumentGeometryUse::UncroppedSource);
  EXPECT_EQ(overlay_desc.frame_metadata_.presentation_request_id, 91u);

  const auto closed_desc = MakeEditorRenderDesc(MakeRequest(92, 5));
  EXPECT_EQ(closed_desc.document_geometry_, alcedo::DocumentGeometryUse::ApplyCropAndRotation);

  // The value reaches the apply request unchanged.
  alcedo::PipelineTask editor_task;
  editor_task.pipeline_executor_    = std::make_shared<alcedo::PipelineExecutor>();
  editor_task.options_.render_desc_ = overlay_desc;
  EXPECT_EQ(editor_task.MakeApplyRequest().geometry.document_geometry,
            alcedo::DocumentGeometryUse::UncroppedSource);

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

}  // namespace
}  // namespace alcedo::ui
