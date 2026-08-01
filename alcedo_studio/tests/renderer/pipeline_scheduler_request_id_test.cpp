//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"

namespace alcedo {
namespace {

class RecordingFrameSink final : public IFrameSink {
 public:
  void EnsureSize(int width, int height) override {
    width_  = width;
    height_ = height;
  }

  auto MapResourceForWrite(FrameMemoryDomain /*preferred_domain*/) -> FrameWriteMapping override {
    return {};
  }

  void UnmapResource() override {}

  void NotifyFrameReady(const FrameCompletionSubmission& submission) override {
    if (submission.metadata.presentation_request_id != 0 &&
        submission.metadata.presentation_request_id < latest_accepted_request_id_) {
      return;
    }
    if (submission.metadata.presentation_request_id != 0) {
      latest_accepted_request_id_ =
          std::max(latest_accepted_request_id_, submission.metadata.presentation_request_id);
    }
    std::lock_guard lock(mutex_);
    submissions_.push_back(submission);
    ++notify_count_;
    wake_.notify_all();
  }

  void BindFrameSubmission(const FrameCompletionSubmission& submission) override {
    std::lock_guard lock(mutex_);
    bound_submission_ = submission;
  }

  [[nodiscard]] auto GetWidth() const -> int override { return width_; }
  [[nodiscard]] auto GetHeight() const -> int override { return height_; }

  [[nodiscard]] auto submissions() const -> std::vector<FrameCompletionSubmission> {
    std::lock_guard lock(mutex_);
    return submissions_;
  }

  [[nodiscard]] auto notify_count() const -> int {
    std::lock_guard lock(mutex_);
    return notify_count_;
  }

 private:
  mutable std::mutex                     mutex_;
  std::condition_variable                wake_;
  int                                    width_                       = 0;
  int                                    height_                      = 0;
  int                                    notify_count_                = 0;
  std::uint64_t                          latest_accepted_request_id_  = 0;
  FrameCompletionSubmission              bound_submission_{};
  std::vector<FrameCompletionSubmission> submissions_;
};

auto MakeSolidImage(int width, int height) -> std::shared_ptr<ImageBuffer> {
  return std::make_shared<ImageBuffer>(
      cv::Mat(height, width, CV_32FC3, cv::Scalar(0.25f, 0.5f, 0.75f)));
}

void ConfigureMinimalPipeline(const std::shared_ptr<CPUPipelineExecutor>& exec) {
  auto&          output_stage = exec->GetStage(PipelineStageName::Output_Transform);
  nlohmann::json output_params;
  output_params["ocio"] = {{"src", "ACEScct"}, {"dst", "Camera Rec.709"}, {"limit", true}};
  output_stage.SetOperator(OperatorType::CST, output_params);
}

TEST(PipelineSchedulerRequestIdTest, OlderRequestIdIsRejectedAtSink) {
  RecordingFrameSink sink;

  FrameCompletionSubmission newer{};
  newer.metadata.presentation_request_id = 2;
  sink.NotifyFrameReady(newer);
  EXPECT_EQ(sink.notify_count(), 1);

  FrameCompletionSubmission older{};
  older.metadata.presentation_request_id = 1;
  sink.NotifyFrameReady(older);
  EXPECT_EQ(sink.notify_count(), 1);
  EXPECT_EQ(sink.submissions().back().metadata.presentation_request_id, 2u);
}

TEST(PipelineSchedulerRequestIdTest, BindFrameSubmissionTagsRequestBeforeNotify) {
  RegisterAllOperators();
  auto               exec = std::make_shared<CPUPipelineExecutor>();
  RecordingFrameSink sink;
  ConfigureMinimalPipeline(exec);
  exec->SetExecutionStages(&sink);

  FramePreviewMetadata metadata{};
  metadata.presentation_request_id = 9;
  exec->BindFrameSubmission(metadata, FramePresentationMode::FullFrame);

  FrameCompletionSubmission submission = exec->BoundFrameSubmission();
  EXPECT_EQ(submission.metadata.presentation_request_id, 9u);
  sink.NotifyFrameReady(submission);
  ASSERT_EQ(sink.submissions().size(), 1u);
  EXPECT_EQ(sink.submissions().front().metadata.presentation_request_id, 9u);
}

TEST(PipelineSchedulerRequestIdTest, StaleSchedulerTaskDoesNotReachSink) {
  RegisterAllOperators();

  auto               exec = std::make_shared<CPUPipelineExecutor>();
  RecordingFrameSink sink;
  ConfigureMinimalPipeline(exec);
  exec->SetExecutionStages(&sink);

  PipelineScheduler scheduler(1);

  auto run_blocking = [&](std::uint64_t request_id) {
    PipelineTask task;
    task.input_                                                    = MakeSolidImage(8, 8);
    task.pipeline_executor_                                        = exec;
    task.request_id_                                               = request_id;
    task.options_.render_desc_.render_type_                        = RenderType::FAST_PREVIEW;
    task.options_.render_desc_.frame_metadata_.presentation_request_id = request_id;
    task.options_.is_blocking_                                     = true;
    task.result_ = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
    task.prepare_with_render_lock_ = [&](PipelineTask& locked_task) {
      locked_task.pipeline_executor_->AttachFrameSink(&sink);
      locked_task.options_.render_desc_.frame_metadata_.presentation_request_id = request_id;
      return true;
    };
    auto future = task.result_->get_future();
    scheduler.ScheduleTask(std::move(task));
    return future;
  };

  // Plan §5.5.1: request 2 reaches MarkSinkApplyStarted first. Apply may fail
  // without a full operator graph; stale tracking must still reject request 1.
  auto newer = run_blocking(2);
  ASSERT_TRUE(newer.wait_for(std::chrono::seconds(30)) == std::future_status::ready)
      << "newer request timed out";
  try {
    (void)newer.get();
  } catch (...) {
  }

  const auto notifies_after_newer = sink.notify_count();

  auto older = run_blocking(1);
  ASSERT_TRUE(older.wait_for(std::chrono::seconds(30)) == std::future_status::ready)
      << "older request stale abort timed out";
  try {
    EXPECT_EQ(older.get(), nullptr);
  } catch (const std::exception& ex) {
    FAIL() << "older request should abort as nullptr, not throw: " << ex.what();
  }
  EXPECT_EQ(sink.notify_count(), notifies_after_newer);
}

TEST(DirectPresentQueueRequestIdTest, ConsumeNewestReadyPrefersHigherRequestId) {
  using editor_rhi::DirectPresentQueue;
  using editor_rhi::EditorBackend;
  using editor_rhi::LeaseNativeHandleKind;
  using editor_rhi::LeaseWritableResourceKind;

  DirectPresentQueue queue(EditorBackend::Cuda);
  queue.SetConsumerAvailable(true);
  queue.InvalidateImageGeneration(1, 10);

  const auto publish_ready = [&](std::uintptr_t handle, std::uint64_t request_id) {
    constexpr int width    = 64;
    constexpr int height   = 48;
    const auto    prepared = queue.PrepareWrite(width, height, 1, 10);
    ASSERT_TRUE(prepared.ok);
    DirectPresentQueue::SlotNative native{};
    native.backend           = EditorBackend::Cuda;
    native.handle_kind       = LeaseNativeHandleKind::D3D11Texture2D;
    native.writable_kind     = LeaseWritableResourceKind::CudaArray;
    native.native_handle     = handle;
    native.writable_resource = handle + 100;
    ASSERT_TRUE(queue.PublishCreatedSlot(prepared.slot_index, width, height, native, 1, 10));
    ASSERT_TRUE(queue.BeginWrite(prepared.slot_index).has_value());
    queue.EndWrite(prepared.slot_index);
    FramePreviewMetadata meta{};
    meta.frame_role              = FrameRole::InteractivePrimary;
    meta.presentation_request_id = request_id;
    queue.NotifyReady(prepared.slot_index, FramePresentationMode::FullFrame, meta);
  };

  publish_ready(0x100, 1);
  publish_ready(0x101, 2);

  const auto frame = queue.ConsumeNewestReady(FrameRole::InteractivePrimary, 1, 10);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->slot.preview_metadata.presentation_request_id, 2u);
}

}  // namespace
}  // namespace alcedo
