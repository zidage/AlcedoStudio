//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>

#include "app/pipeline_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::ui {
namespace {

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
  RegisterAllOperators();
  EditorRenderSessionContext context;
  context.epoch                = epoch;
  context.element_id           = element_id;
  context.image_id             = image_id;
  context.presentation_sink_id = sink_id;
  context.image                = std::make_shared<alcedo::Image>(image_id);
  context.image->image_path_   = std::filesystem::path("D:/fixture/session-context.arw");
  context.input                = std::make_shared<alcedo::ImageBuffer>();
  context.pipeline_guard       = std::make_shared<alcedo::PipelineGuard>();
  context.pipeline_guard->id_  = element_id;
  context.pipeline_guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  return context;
}

TEST(EditorSessionRenderSchedulerPortTest,
     ProductionPipelinePathSchedulesInstalledContextWithoutAdapterBind) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(7, 22, 11));

  ASSERT_NE(scheduler->Schedule(MakeRequest(33, 7)), 0u);
  scheduler->WaitForSessionIdle(7);

  // Production Dispatch never EnsureSize; pipeline SetExecutorRenderParams binds.
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_GE(sink.bind_count(), 1);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, 33u);
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
}

TEST(EditorSessionRenderSchedulerPortTest, ViewDrivenReasonsDisableScopeFrameReplacement) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(20, 22, 11));

  const std::array view_reasons = {alcedo::EditorRenderReason::ZoomPan,
                                   alcedo::EditorRenderReason::Resize,
                                   alcedo::EditorRenderReason::DetailRefresh};
  for (std::size_t index = 0; index < view_reasons.size(); ++index) {
    auto request          = MakeRequest(60 + index, 20);
    request.intent.reason = view_reasons[index];
    ASSERT_NE(scheduler->Schedule(request), 0u);
    scheduler->WaitForSessionIdle(20);
    EXPECT_FALSE(sink.last_submission.metadata.scope_update_allowed);
    EXPECT_EQ(sink.ensure_size_count(), 0);
  }
}

TEST(EditorSessionRenderSchedulerPortTest, ScopeRefreshMarksFrameAsRequestedScopeInput) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(32, 22, 11));

  auto request          = MakeRequest(72, 32);
  request.intent.reason = alcedo::EditorRenderReason::ScopeRefresh;
  ASSERT_NE(scheduler->Schedule(request), 0u);
  scheduler->WaitForSessionIdle(32);

  EXPECT_TRUE(sink.last_submission.metadata.scope_update_allowed);
  EXPECT_TRUE(sink.last_submission.metadata.scope_refresh_requested);
  EXPECT_EQ(sink.last_submission.metadata.preview_generation, 0u);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, request.request_id);
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_GE(sink.bind_count(), 1);
}

TEST(EditorSessionRenderSchedulerPortTest, SessionDoesNotStampPreviewGenerationFromIntent) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(41, 22, 11));

  auto request = MakeRequest(88, 41);
  ASSERT_NE(scheduler->Schedule(request), 0u);
  scheduler->WaitForSessionIdle(41);

  EXPECT_EQ(sink.last_submission.metadata.preview_generation, 0u);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, request.request_id);
}

TEST(EditorSessionRenderSchedulerPortTest, RequestWithoutFrameSourceIsRejected) {
  EditorSessionRenderSchedulerPort scheduler;
  const auto                       request = MakeRequest(44, 8);

  const auto job_id = scheduler.Schedule(request);
  EXPECT_EQ(job_id, 0u);
  EXPECT_TRUE(scheduler.last_scheduled().empty());
  scheduler.WaitForSessionIdle(8);
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
  EXPECT_EQ(context->pipeline_guard, nullptr);
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
  EXPECT_EQ(context->pipeline_guard, nullptr);
}

TEST(EditorSessionRenderSchedulerPortTest, RebindSameImageIdentityKeepsPayloadWithoutReload) {
  EditorSessionRenderSchedulerPort scheduler;
  auto                             ready = MakeReadyContext(/*epoch=*/1, /*element_id=*/22,
                                                            /*image_id=*/11, /*sink_id=*/7);
  const auto                       input_ptr = ready.input;
  const auto                       image_ptr = ready.image;
  const auto                       guard_ptr = ready.pipeline_guard;
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
  EXPECT_EQ(context->pipeline_guard, guard_ptr);
  EXPECT_EQ(scheduler.context_payload_load_count(), loads_before);
}

TEST(EditorSessionRenderSchedulerPortTest,
     InstalledContextAllowsScheduleWithoutImagePoolService) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(9, 22, 11));

  ASSERT_NE(scheduler->Schedule(MakeRequest(100, 9)), 0u);
  scheduler->WaitForSessionIdle(9);
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
  EXPECT_GE(sink.bind_count(), 1);
  EXPECT_EQ(sink.ensure_size_count(), 0);
}

TEST(EditorSessionRenderSchedulerPortTest,
     HotPathAfterInstalledContextDoesNotInvokeImagePoolResolver) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
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
    ASSERT_NE(scheduler->Schedule(request), 0u);
    scheduler->WaitForSessionIdle(12);
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
  EditorSessionRenderSchedulerPort scheduler;
  scheduler.BindSessionContext(5, 22, 11, 7);
  RecordingFrameSink sink;
  scheduler.SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });

  const auto job_id = scheduler.Schedule(MakeRequest(55, 5));
  ASSERT_NE(job_id, 0u);
  scheduler.WaitForSessionIdle(5);
  EXPECT_EQ(scheduler.context_payload_load_count(), 0u);
  EXPECT_EQ(sink.ensure_size_count(), 0);
}

TEST(EditorSessionRenderSchedulerPortTest,
     SinkIdentityStableAcrossInteractiveFramesForBoundContext) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(12, 22, 11, /*sink_id=*/99));

  const auto resolves_before = scheduler->sink_resolve_count();
  for (std::uint64_t request_id = 300; request_id < 303; ++request_id) {
    auto request          = MakeRequest(request_id, 12, /*sink_id=*/99);
    request.intent.reason = alcedo::EditorRenderReason::InteractiveAdjustment;
    ASSERT_NE(scheduler->Schedule(request), 0u);
    scheduler->WaitForSessionIdle(12);
  }

  const auto context = scheduler->session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->presentation_sink_id, 99u);
  EXPECT_EQ(scheduler->sink_resolve_count(), resolves_before + 3u);
  EXPECT_EQ(sink.ensure_size_count(), 0);
}

TEST(EditorSessionRenderSchedulerPortTest,
     MismatchedPresentationSinkIdentityFailsWithoutAdapterEnsureSize) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(4, 22, 11, /*sink_id=*/10));

  auto request = MakeRequest(401, 4, /*sink_id=*/99);
  ASSERT_NE(scheduler->Schedule(request), 0u);
  scheduler->WaitForSessionIdle(4);

  EXPECT_EQ(sink.bind_count(), 0);
  EXPECT_EQ(sink.ensure_size_count(), 0);
  EXPECT_EQ(scheduler->session_context()->presentation_sink_id, 10u);
}

TEST(EditorSessionRenderSchedulerPortTest,
     ForwardScheduleCompletionInvokedWithoutReverseCoordinator) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->InstallSessionContext(MakeReadyContext(7, 22, 11));

  std::atomic<int> completed{0};
  ASSERT_NE(scheduler->Schedule(
                MakeRequest(77, 7),
                [&](bool /*success*/, std::string /*message*/) {
                  completed.fetch_add(1, std::memory_order_relaxed);
                }),
            0u);
  scheduler->WaitForSessionIdle(7);

  // Fixture context has no real RAW bytes; pipeline may fail — the residual
  // cleanup claim is forward completion without SetCoordinator / weak_ptr.
  EXPECT_EQ(completed.load(), 1);
}

}  // namespace
}  // namespace alcedo::ui
