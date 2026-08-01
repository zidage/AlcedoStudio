//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>

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

TEST(EditorSessionRenderSchedulerPortTest,
     TestProducerSubmitsFrameAndAcknowledgementUsesImageIdentity) {
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
  EXPECT_EQ(scheduler->pending_present_request_id(), 33u);

  scheduler->NotifyPresentationAcknowledged(33, 7, 99);
  EXPECT_EQ(scheduler->pending_present_request_id(), 33u);
  scheduler->NotifyPresentationAcknowledged(33, 7, 11);
  EXPECT_EQ(scheduler->pending_present_request_id(), 0u);
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

TEST(EditorSessionRenderSchedulerPortTest, CancelledSyntheticRequestLeavesSessionIdle) {
  EditorSessionRenderSchedulerPort scheduler;
  const auto                       request = MakeRequest(44, 8);

  const auto                       job_id  = scheduler.Schedule(request);
  ASSERT_NE(job_id, 0u);
  ASSERT_EQ(scheduler.last_scheduled().size(), 1u);

  scheduler.Cancel(job_id);
  scheduler.WaitForSessionIdle(8);
  EXPECT_EQ(scheduler.pending_present_request_id(), 0u);
}

}  // namespace
}  // namespace alcedo::ui
