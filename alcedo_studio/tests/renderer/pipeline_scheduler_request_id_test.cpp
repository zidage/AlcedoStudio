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
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
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

TEST(PipelineSchedulerRequestIdTest, StaleSchedulerTaskDoesNotReachSink) {

  auto               exec = std::make_shared<CPUPipelineExecutor>();
  RecordingFrameSink sink;
  exec->AttachFrameSink(&sink);

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
    task.configure_under_render_lock_ = [&](PipelineTask& locked_task) {
      locked_task.pipeline_executor_->AttachFrameSink(&sink);
      locked_task.options_.render_desc_.frame_metadata_.presentation_request_id = request_id;
      return true;
    };
    auto future = task.result_->get_future();
    scheduler.ScheduleTask(std::move(task));
    return future;
  };

  // Plan §5.5.1: request 2 reaches MarkSinkApplyStarted first. Apply fails without a bound
  // document; stale tracking must still reject request 1.
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

TEST(PipelineSchedulerRequestIdTest, CompletionRunsAfterLivePipelineRenderLockIsReleased) {
  auto               exec = std::make_shared<CPUPipelineExecutor>();
  RecordingFrameSink sink;
  exec->AttachFrameSink(&sink);

  PipelineScheduler scheduler(1);
  PipelineTask      task;
  task.input_                             = MakeSolidImage(8, 8);
  task.pipeline_executor_                 = exec;
  task.request_id_                        = 17;
  task.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
  auto lock_released = std::make_shared<std::promise<bool>>();
  auto completed     = lock_released->get_future();
  task.on_complete_  = [exec, lock_released](bool, std::string) {
    const bool acquired = exec->GetRenderLock().try_lock();
    if (acquired) {
      exec->GetRenderLock().unlock();
    }
    lock_released->set_value(acquired);
  };

  scheduler.ScheduleTask(std::move(task));

  ASSERT_EQ(completed.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  EXPECT_TRUE(completed.get());
}

TEST(PipelineSchedulerRequestIdTest, MissingDocumentRequestsReportFailureOnEveryRender) {
  auto exec = std::make_shared<CPUPipelineExecutor>();
  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);

  auto input = MakeSolidImage(64, 48);
  PipelineScheduler scheduler(1);
  auto run_interactive = [&](std::uint64_t request_id) {
    PipelineTask task;
    task.input_                             = input;
    task.pipeline_executor_                 = exec;
    task.request_id_                        = request_id;
    task.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
    task.options_.is_blocking_              = true;
    task.result_ = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
    auto future = task.result_->get_future();
    scheduler.ScheduleTask(std::move(task));
    EXPECT_EQ(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    return future.get();
  };

  EXPECT_THROW((void)run_interactive(101), std::runtime_error);
  EXPECT_THROW((void)run_interactive(102), std::runtime_error);
  EXPECT_FALSE(exec->HasGpuDagDocument());
}

// G10.1 removed the stage CROP_ROTATE read from FAST_PREVIEW. Before that change the read was
// false for every document edit (defect D1: the stage never received the crop_rotate key), so a
// rotated crop used the ROI path. The expected values below are that pre-change request.
TEST(PipelineSchedulerRequestIdTest, FastPreviewRequestIsUnchangedForRotatedCrop) {
  auto exec     = std::make_shared<CPUPipelineExecutor>();
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  document->Geometry().SetCropRect({0.2f, 0.1f, 0.5f, 0.6f});
  document->Geometry().SetRotationDegrees(7.0f);
  exec->SetPipelineDocument(document);
  const auto document_before = document->ToJson();

  auto       make_task       = [&](const ViewportRenderRegion& region) {
    PipelineTask task;
    task.pipeline_executor_                         = exec;
    task.options_.render_desc_.render_type_         = RenderType::FAST_PREVIEW;
    task.options_.render_desc_.use_viewport_region_ = true;
    task.options_.render_desc_.viewport_region_     = region;
    return task;
  };

  const ViewportRenderRegion roi{.x_                = 600,
                                 .y_                = 400,
                                 .scale_x_          = 0.3f,
                                 .scale_y_          = 0.25f,
                                 .reference_width_  = 5000,
                                 .reference_height_ = 3000,
                                 .target_width_     = 1600,
                                 .target_height_    = 1200};
  const auto                 roi_request = make_task(roi).MakeApplyRequest();
  EXPECT_EQ(roi_request.submission.mode, FramePresentationMode::RoiFrame);
  EXPECT_EQ(roi_request.submission.metadata.frame_role, FrameRole::InteractivePrimary);
  EXPECT_FLOAT_EQ(roi_request.geometry.view.visible_rect_in_edit_space.x, 0.12f);
  EXPECT_FLOAT_EQ(roi_request.geometry.view.visible_rect_in_edit_space.y, 400.0f / 3000.0f);
  EXPECT_FLOAT_EQ(roi_request.geometry.view.visible_rect_in_edit_space.w, 0.3f);
  EXPECT_FLOAT_EQ(roi_request.geometry.view.visible_rect_in_edit_space.h, 0.25f);
  EXPECT_EQ(roi_request.geometry.view.viewport_extent.width, 1600U);
  EXPECT_EQ(roi_request.geometry.view.viewport_extent.height, 1200U);
  EXPECT_NEAR(roi_request.submission.metadata.source_roi_norm.x, 0.12f, 1.0e-6f);
  EXPECT_NEAR(roi_request.submission.metadata.source_roi_norm.width, 0.3f, 1.0e-6f);
  EXPECT_EQ(roi_request.geometry.resolution.max_edge, 2560U);
  EXPECT_EQ(roi_request.geometry.resolution.quality, RenderQuality::Preview);
  EXPECT_EQ(roi_request.decode_res, DecodeRes::FULL);
  EXPECT_EQ(roi_request.cache_policy, RenderCachePolicy::UseSessionCache);
  EXPECT_FALSE(roi_request.require_host_output);

  const ViewportRenderRegion full{.x_                = 0,
                                  .y_                = 0,
                                  .scale_x_          = 1.0f,
                                  .scale_y_          = 1.0f,
                                  .reference_width_  = 5000,
                                  .reference_height_ = 3000,
                                  .target_width_     = 1600,
                                  .target_height_    = 960};
  const auto                 full_request = make_task(full).MakeApplyRequest();
  EXPECT_EQ(full_request.submission.mode, FramePresentationMode::ViewportTransformed);
  EXPECT_TRUE(full_request.geometry.view.visible_rect_in_edit_space.IsFullFrame());
  EXPECT_EQ(full_request.geometry.view.viewport_extent.width, 1600U);
  EXPECT_EQ(full_request.geometry.view.viewport_extent.height, 960U);
  EXPECT_FLOAT_EQ(full_request.submission.metadata.source_roi_norm.width, 1.0f);
  EXPECT_EQ(full_request.geometry.resolution.max_edge, 2560U);
  EXPECT_EQ(full_request.decode_res, DecodeRes::FULL);

  EXPECT_EQ(document->ToJson(), document_before);
}

TEST(DirectPresentQueueRequestIdTest, ConsumeNewestReadyPrefersHigherRequestId) {
  using editor_rhi::DirectPresentQueue;
  using editor_rhi::EditorBackend;
  using editor_rhi::LeaseNativeHandleKind;
  using editor_rhi::LeaseWritableResourceKind;

  DirectPresentQueue queue(EditorBackend::Cuda);
  queue.SetConsumerAvailable(true);
  queue.InvalidateSessionEpoch(1, 10);

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

TEST(DirectPresentQueueRequestIdTest, ThirdInteractivePresentReusesFirstDisplayedSlot) {
  using editor_rhi::DirectPresentQueue;
  using editor_rhi::EditorBackend;
  using editor_rhi::LeaseNativeHandleKind;
  using editor_rhi::LeaseWritableResourceKind;

  DirectPresentQueue queue(EditorBackend::OpenCl);
  queue.SetConsumerAvailable(true);
  queue.InvalidateSessionEpoch(1, 42);

  const auto present_interactive = [&](std::uintptr_t handle) {
    constexpr int width    = 1600;
    constexpr int height   = 900;
    const auto    prepared = queue.PrepareWrite(width, height, 1, 42);
    EXPECT_TRUE(prepared.ok);
    if (prepared.need_create) {
      DirectPresentQueue::SlotNative native{};
      native.backend           = EditorBackend::OpenCl;
      native.handle_kind       = LeaseNativeHandleKind::OpenGLTexture2D;
      native.writable_kind     = LeaseWritableResourceKind::OpenClImage;
      native.native_handle     = handle;
      native.writable_resource = handle + 100;
      EXPECT_TRUE(queue.PublishCreatedSlot(prepared.slot_index, width, height, native, 1, 42));
    }
    EXPECT_TRUE(queue.BeginWrite(prepared.slot_index).has_value());
    queue.EndWrite(prepared.slot_index);
    FramePreviewMetadata metadata{};
    metadata.frame_role = FrameRole::InteractivePrimary;
    queue.NotifyReady(prepared.slot_index, FramePresentationMode::FullFrame, metadata);
    return prepared.slot_index;
  };

  const int  first_slot      = present_interactive(1);
  const auto first_displayed = queue.ConsumeNewestReady(FrameRole::InteractivePrimary, 1, 42);
  ASSERT_TRUE(first_displayed.has_value());
  EXPECT_EQ(first_displayed->slot.index, first_slot);

  const int second_slot = present_interactive(2);
  EXPECT_NE(second_slot, first_slot);
  queue.CompleteRendererRead(first_slot);
  const auto second_displayed = queue.ConsumeNewestReady(FrameRole::InteractivePrimary, 1, 42);
  ASSERT_TRUE(second_displayed.has_value());
  EXPECT_EQ(second_displayed->slot.index, second_slot);

  const int third_slot = present_interactive(1);
  EXPECT_EQ(third_slot, first_slot);
  queue.CompleteRendererRead(second_slot);
}

TEST(PipelineSchedulerRequestIdTest, EditorRenderFailureForwardsExceptionMessageInsteadOfEmptyResult) {
  auto exec = std::make_shared<CPUPipelineExecutor>();
  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);

  PipelineScheduler scheduler(1);
  PipelineTask      task;
  task.input_                             = MakeSolidImage(8, 8);
  task.pipeline_executor_                 = exec;
  task.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
  auto done                               = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto future                             = done->get_future();
  task.configure_under_render_lock_       = [](PipelineTask&) -> bool {
    throw std::runtime_error("Neural Engine unavailable: missing weights");
  };
  task.on_complete_ = [done](bool success, std::string message) {
    done->set_value({success, std::move(message)});
  };

  scheduler.ScheduleTask(std::move(task));
  ASSERT_EQ(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  const auto result = future.get();
  EXPECT_FALSE(result.first);
  EXPECT_EQ(result.second, "Neural Engine unavailable: missing weights");
}

}  // namespace
}  // namespace alcedo
