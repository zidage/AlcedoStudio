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
  }

  auto MapResourceForWrite(alcedo::FrameMemoryDomain /*preferred_domain*/)
      -> alcedo::FrameWriteMapping override {
    return {};
  }

  void               UnmapResource() override {}

  void NotifyFrameReady(const alcedo::FrameCompletionSubmission& submission) override {
    // Empty Notify from a test producer keeps BindFrameSubmission metadata so
    // request-id / scope flags stamped by the scheduler port remain observable.
    if (submission.metadata.presentation_request_id != 0 ||
        submission.metadata.scope_refresh_requested || !submission.metadata.scope_update_allowed ||
        submission.metadata.frame_role != alcedo::FrameRole::InteractivePrimary) {
      last_submission = submission;
    }
    ++ready_count_;
  }

  void BindFrameSubmission(const alcedo::FrameCompletionSubmission& submission) override {
    last_submission = submission;
  }

  [[nodiscard]] auto GetWidth() const -> int override { return width_; }
  [[nodiscard]] auto GetHeight() const -> int override { return height_; }

  [[nodiscard]] auto ready_count() const -> int {
    return ready_count_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto width() const -> int { return width_; }
  [[nodiscard]] auto height() const -> int { return height_; }

  alcedo::FrameCompletionSubmission last_submission{};

 private:
  std::atomic<int> ready_count_ = 0;
  int              width_       = 0;
  int              height_      = 0;
};

auto MakeRequest(std::uint64_t request_id, std::uint64_t image_load_request)
    -> alcedo::EditorRenderRequest {
  alcedo::EditorRenderRequest request;
  request.request_id                     = request_id;
  request.intent.element_id              = 22;
  request.intent.image_id                = 11;
  request.intent.image_load_request_id   = alcedo::ImageLoadRequestId{image_load_request};
  request.intent.reason             = alcedo::EditorRenderReason::InitialFrame;
  request.intent.quality            = alcedo::EditorRenderQuality::Interactive;
  request.intent.frame_role         = alcedo::FrameRole::InteractivePrimary;
  request.intent.requested_width    = 320;
  request.intent.requested_height   = 180;
  return request;
}

auto MakeReadyContext(std::uint64_t epoch, sl_element_id_t element_id, image_id_t image_id)
    -> EditorRenderSessionContext {
  RegisterAllOperators();
  EditorRenderSessionContext context;
  context.epoch      = epoch;
  context.element_id = element_id;
  context.image_id   = image_id;
  context.image      = std::make_shared<alcedo::Image>(image_id);
  context.image->image_path_ = std::filesystem::path("D:/fixture/session-context.arw");
  context.input              = std::make_shared<alcedo::ImageBuffer>();
  context.pipeline_guard     = std::make_shared<alcedo::PipelineGuard>();
  context.pipeline_guard->id_       = element_id;
  context.pipeline_guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  return context;
}

TEST(EditorSessionRenderSchedulerPortTest, TestProducerPublishesOneReadyFrame) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  std::atomic<int>   producer_count = 0;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->SetTestFrameProducer(
      [&producer_count](alcedo::IFrameSink* frame_sink, const alcedo::EditorRenderRequest&) {
        if (!frame_sink) return false;
        ++producer_count;
        frame_sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
        return true;
      });

  ASSERT_NE(scheduler->Schedule(MakeRequest(33, 7)), 0u);
  scheduler->WaitForSessionIdle(7);

  EXPECT_EQ(producer_count.load(std::memory_order_acquire), 1);
  EXPECT_EQ(sink.ready_count(), 1);
  EXPECT_EQ(sink.width(), 320);
  EXPECT_EQ(sink.height(), 180);
}

TEST(EditorSessionRenderSchedulerPortTest, ViewDrivenReasonsDisableScopeFrameReplacement) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->SetTestFrameProducer(
      [](alcedo::IFrameSink* frame_sink, const alcedo::EditorRenderRequest&) {
        if (!frame_sink) return false;
        frame_sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
        return true;
      });

  const std::array view_reasons = {alcedo::EditorRenderReason::ZoomPan,
                                   alcedo::EditorRenderReason::Resize,
                                   alcedo::EditorRenderReason::DetailRefresh};
  for (std::size_t index = 0; index < view_reasons.size(); ++index) {
    auto request          = MakeRequest(60 + index, 20 + index);
    request.intent.reason = view_reasons[index];
    ASSERT_NE(scheduler->Schedule(request), 0u);
    scheduler->WaitForSessionIdle(20 + index);
    EXPECT_FALSE(sink.last_submission.metadata.scope_update_allowed);
  }
}

TEST(EditorSessionRenderSchedulerPortTest, ScopeRefreshMarksFrameAsRequestedScopeInput) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->SetTestFrameProducer(
      [](alcedo::IFrameSink* frame_sink, const alcedo::EditorRenderRequest&) {
        if (!frame_sink) return false;
        frame_sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
        return true;
      });

  auto request                   = MakeRequest(72, 32);
  request.intent.reason          = alcedo::EditorRenderReason::ScopeRefresh;
  ASSERT_NE(scheduler->Schedule(request), 0u);
  scheduler->WaitForSessionIdle(32);

  EXPECT_TRUE(sink.last_submission.metadata.scope_update_allowed);
  EXPECT_TRUE(sink.last_submission.metadata.scope_refresh_requested);
  EXPECT_EQ(sink.last_submission.metadata.preview_generation, 0u);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, request.request_id);
}

TEST(EditorSessionRenderSchedulerPortTest, SessionDoesNotStampPreviewGenerationFromIntent) {
  auto               scheduler = std::make_shared<EditorSessionRenderSchedulerPort>();
  RecordingFrameSink sink;
  scheduler->SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });
  scheduler->SetTestFrameProducer(
      [](alcedo::IFrameSink* frame_sink, const alcedo::EditorRenderRequest&) {
        if (!frame_sink) return false;
        frame_sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
        return true;
      });

  auto request = MakeRequest(88, 41);
  ASSERT_NE(scheduler->Schedule(request), 0u);
  scheduler->WaitForSessionIdle(41);

  EXPECT_EQ(sink.last_submission.metadata.preview_generation, 0u);
  EXPECT_EQ(sink.last_submission.metadata.presentation_request_id, request.request_id);
}

TEST(EditorSessionRenderSchedulerPortTest, RequestWithoutFrameSourceIsRejected) {
  EditorSessionRenderSchedulerPort scheduler;
  const auto                       request = MakeRequest(44, 8);

  const auto                       job_id  = scheduler.Schedule(request);
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

  scheduler.BindSessionContext(/*epoch=*/7, /*element_id=*/22, /*image_id=*/11);

  const auto context = scheduler.session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 7u);
  EXPECT_EQ(context->element_id, 22u);
  EXPECT_EQ(context->image_id, 11u);
  EXPECT_EQ(context->image, nullptr);
  EXPECT_EQ(context->input, nullptr);
  EXPECT_EQ(context->pipeline_guard, nullptr);
  EXPECT_EQ(scheduler.context_payload_load_count(), 0u);
  EXPECT_EQ(pool_resolve_count.load(std::memory_order_acquire), 0);
}

TEST(EditorSessionRenderSchedulerPortTest, ClearSessionContextDropsBoundIdentity) {
  EditorSessionRenderSchedulerPort scheduler;
  scheduler.BindSessionContext(3, 22, 11);
  ASSERT_TRUE(scheduler.session_context().has_value());

  scheduler.ClearSessionContext();
  EXPECT_FALSE(scheduler.session_context().has_value());
}

TEST(EditorSessionRenderSchedulerPortTest, ImageSwitchBindReplacesPriorContextPayload) {
  EditorSessionRenderSchedulerPort scheduler;
  scheduler.InstallSessionContext(MakeReadyContext(/*epoch=*/1, /*element_id=*/22, /*image_id=*/11));
  ASSERT_TRUE(scheduler.session_context().has_value());
  ASSERT_NE(scheduler.session_context()->input, nullptr);

  scheduler.BindSessionContext(/*epoch=*/2, /*element_id=*/33, /*image_id=*/44);
  const auto context = scheduler.session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 2u);
  EXPECT_EQ(context->element_id, 33u);
  EXPECT_EQ(context->image_id, 44u);
  EXPECT_EQ(context->image, nullptr);
  EXPECT_EQ(context->input, nullptr);
  EXPECT_EQ(context->pipeline_guard, nullptr);
}

TEST(EditorSessionRenderSchedulerPortTest,
     RebindSameImageIdentityKeepsPayloadWithoutReload) {
  EditorSessionRenderSchedulerPort scheduler;
  auto                             ready = MakeReadyContext(/*epoch=*/1, /*element_id=*/22,
                                                            /*image_id=*/11);
  const auto                       input_ptr  = ready.input;
  const auto                       image_ptr  = ready.image;
  const auto                       guard_ptr  = ready.pipeline_guard;
  scheduler.InstallSessionContext(std::move(ready));
  const auto loads_before = scheduler.context_payload_load_count();

  // RouteInitialRender / undo rebind the same element+image with a new epoch.
  scheduler.BindSessionContext(/*epoch=*/9, /*element_id=*/22, /*image_id=*/11);

  const auto context = scheduler.session_context();
  ASSERT_TRUE(context.has_value());
  EXPECT_EQ(context->epoch, 9u);
  EXPECT_EQ(context->element_id, 22u);
  EXPECT_EQ(context->image_id, 11u);
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
  scheduler->SetTestFrameProducer(
      [](alcedo::IFrameSink* frame_sink, const alcedo::EditorRenderRequest&) {
        if (!frame_sink) return false;
        frame_sink->NotifyFrameReady(alcedo::FrameCompletionSubmission{});
        return true;
      });

  ASSERT_NE(scheduler->Schedule(MakeRequest(100, 9)), 0u);
  scheduler->WaitForSessionIdle(9);
  EXPECT_EQ(sink.ready_count(), 1);
  EXPECT_EQ(scheduler->context_payload_load_count(), 0u);
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

  // Production pipeline path (not test producer): context payload must be reused.
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
  scheduler.BindSessionContext(5, 22, 11);
  // Services resolve to null pool — Schedule is allowed by identity, then fails
  // asynchronously once EnsureContext cannot load payload. With no sink the
  // async failure path still clears the running job.
  RecordingFrameSink sink;
  scheduler.SetSinkResolver([&sink] { return static_cast<alcedo::IFrameSink*>(&sink); });

  // Without image_pool service and without installed payload, CanProduceFrame
  // accepts a matching bind but EnsureContext fails on dispatch.
  // When no services are set at all, matching bind alone is enough for CanProduceFrame.
  const auto job_id = scheduler.Schedule(MakeRequest(55, 5));
  ASSERT_NE(job_id, 0u);
  scheduler.WaitForSessionIdle(5);
  EXPECT_EQ(scheduler.context_payload_load_count(), 0u);
}

}  // namespace
}  // namespace alcedo::ui
