//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

// Minimal mock IFrameSink that tracks call counts for verification.
class MockFrameSink final : public IFrameSink {
 public:
  void EnsureSize(int width, int height) override {
    ensure_size_calls_++;
    width_  = width;
    height_ = height;
  }

  auto MapResourceForWrite(FrameMemoryDomain /*domain*/) -> FrameWriteMapping override {
    map_resource_calls_++;
    return {};
  }

  void UnmapResource() override { unmap_resource_calls_++; }

  void NotifyFrameReady(const FrameCompletionSubmission& submission) override {
    notify_frame_ready_calls_++;
    last_submission_ = submission;
  }

  void BindFrameSubmission(const FrameCompletionSubmission& submission) override {
    bind_submission_calls_++;
    last_bound_submission_ = submission;
  }

  auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> override {
    viewport_render_region_calls_++;
    return viewport_render_region_;
  }

  int                                 GetWidth() const override { return width_; }
  int                                 GetHeight() const override { return height_; }

  // --- call counters ---
  int                                 ensure_size_calls_            = 0;
  int                                 map_resource_calls_           = 0;
  int                                 unmap_resource_calls_         = 0;
  int                                 notify_frame_ready_calls_     = 0;
  mutable int                         viewport_render_region_calls_ = 0;

  int                                 bind_submission_calls_        = 0;

  FrameCompletionSubmission           last_submission_{};
  FrameCompletionSubmission           last_bound_submission_{};
  std::optional<ViewportRenderRegion> viewport_render_region_{};

 private:
  int width_  = 0;
  int height_ = 0;
};

}  // namespace

// =========================================================================
// Phase 1 Acceptance Criterion 1:
//   "Thumbnail rendering cannot call into an editor-owned IFrameSink."
// =========================================================================

class PipelineFrameSinkTest : public ::testing::Test {};

TEST_F(PipelineFrameSinkTest, DetachFrameSinkClearsPointer) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->AttachFrameSink(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  exec->DetachFrameSink();
  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

TEST_F(PipelineFrameSinkTest, NewExecutorHasNoFrameSink) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

TEST_F(PipelineFrameSinkTest, GetViewportRenderRegionReturnsNulloptWhenSinkIsDetached) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  EXPECT_EQ(exec->GetViewportRenderRegion(), std::nullopt);
}

// Request construction tests read the per-task PipelineApplyRequest that Apply receives.
// MakeApplyRequest never writes executor or stage state (G10.1).

TEST_F(PipelineFrameSinkTest, DetailRoiPreviewUsesViewportTargetPixelsAsMaxEdge) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->AttachFrameSink(&sink);

  sink.viewport_render_region_ = ViewportRenderRegion{
      .x_                = 1200,
      .y_                = 600,
      .scale_x_          = 0.25f,
      .scale_y_          = 0.2f,
      .reference_width_  = 6000,
      .reference_height_ = 4000,
      .target_width_     = 1800,
      .target_height_    = 1200,
  };

  PipelineTask task;
  task.pipeline_executor_                                       = exec;
  task.options_.render_desc_.render_type_                       = RenderType::DETAIL_ROI_PREVIEW;
  task.options_.render_desc_.use_viewport_region_               = true;
  task.options_.render_desc_.frame_metadata_.preview_generation = 7;

  const auto request                                            = task.MakeApplyRequest();
  EXPECT_EQ(request.geometry.resolution.max_edge, 1800U);
  EXPECT_FLOAT_EQ(request.geometry.view.visible_rect_in_edit_space.x, 0.2f);
  EXPECT_FLOAT_EQ(request.geometry.view.visible_rect_in_edit_space.y, 0.15f);
  EXPECT_FLOAT_EQ(request.geometry.view.visible_rect_in_edit_space.w, 0.25f);
  EXPECT_FLOAT_EQ(request.geometry.view.visible_rect_in_edit_space.h, 0.2f);
  EXPECT_EQ(request.geometry.view.viewport_extent.width, 1800U);
  EXPECT_EQ(request.geometry.view.viewport_extent.height, 1200U);
  EXPECT_EQ(request.submission.metadata.frame_role, FrameRole::DetailPatch);
}

TEST_F(PipelineFrameSinkTest, DetailRoiPreviewUsesFrozenRequestRegionInsteadOfChangedSinkRegion) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->AttachFrameSink(&sink);

  // Simulate the view moving after the session request was accepted but before
  // the blocking pipeline task acquired the executor render lock.
  sink.viewport_render_region_ = ViewportRenderRegion{
      .x_                = 404,
      .y_                = 428,
      .scale_x_          = 0.17f,
      .scale_y_          = 0.17f,
      .reference_width_  = 903,
      .reference_height_ = 1351,
      .target_width_     = 903,
      .target_height_    = 1351,
  };
  const ViewportRenderRegion requested_region{
      .x_                = 316,
      .y_                = 428,
      .scale_x_          = 0.491694f,
      .scale_y_          = 0.170244f,
      .reference_width_  = 903,
      .reference_height_ = 1351,
      .target_width_     = 3008,
      .target_height_    = 1558,
  };

  PipelineTask task;
  task.pipeline_executor_                         = exec;
  task.options_.render_desc_.render_type_         = RenderType::DETAIL_ROI_PREVIEW;
  task.options_.render_desc_.use_viewport_region_ = true;
  task.options_.render_desc_.viewport_region_     = requested_region;
  const auto request                              = task.MakeApplyRequest();

  EXPECT_EQ(sink.viewport_render_region_calls_, 0);
  const auto& metadata = request.submission.metadata;
  EXPECT_EQ(metadata.frame_role, FrameRole::DetailPatch);
  EXPECT_NEAR(metadata.source_roi_norm.x, 316.0f / 903.0f, 1.0e-5f);
  EXPECT_NEAR(metadata.source_roi_norm.y, 428.0f / 1351.0f, 1.0e-5f);
  EXPECT_NEAR(metadata.source_roi_norm.width, 0.491694f, 1.0e-5f);
  EXPECT_NEAR(metadata.source_roi_norm.height, 0.170244f, 1.0e-5f);
  EXPECT_EQ(request.geometry.resolution.max_edge, 3008U);
  EXPECT_NEAR(request.geometry.view.visible_rect_in_edit_space.x, 316.0f / 903.0f, 1.0e-5f);
  EXPECT_EQ(request.geometry.view.viewport_extent.width, 3008U);
}

TEST_F(PipelineFrameSinkTest, QualityBaseRequestDoesNotCarryViewportGeometry) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->AttachFrameSink(&sink);
  sink.viewport_render_region_ = ViewportRenderRegion{.x_                = 700,
                                                      .y_                = 300,
                                                      .scale_x_          = 0.2f,
                                                      .scale_y_          = 0.3f,
                                                      .reference_width_  = 5000,
                                                      .reference_height_ = 3000,
                                                      .target_width_     = 1800,
                                                      .target_height_    = 1200};

  PipelineTask roi;
  roi.pipeline_executor_                         = exec;
  roi.options_.render_desc_.render_type_         = RenderType::DETAIL_ROI_PREVIEW;
  roi.options_.render_desc_.use_viewport_region_ = true;
  EXPECT_FALSE(roi.MakeApplyRequest().geometry.view.visible_rect_in_edit_space.IsFullFrame());

  PipelineTask quality_base;
  quality_base.pipeline_executor_                 = exec;
  quality_base.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  const auto request                              = quality_base.MakeApplyRequest();

  EXPECT_TRUE(request.geometry.view.visible_rect_in_edit_space.IsFullFrame());
  EXPECT_EQ(request.geometry.view.viewport_extent.width, 0U);
  EXPECT_EQ(request.submission.mode, FramePresentationMode::ViewportTransformed);
  EXPECT_FLOAT_EQ(request.submission.metadata.source_roi_norm.x, 0.0f);
  EXPECT_FLOAT_EQ(request.submission.metadata.source_roi_norm.y, 0.0f);
  EXPECT_FLOAT_EQ(request.submission.metadata.source_roi_norm.width, 1.0f);
  EXPECT_FLOAT_EQ(request.submission.metadata.source_roi_norm.height, 1.0f);
}

TEST_F(PipelineFrameSinkTest, FastPreviewSubRegionUsesRoiFrameWithSinkRegion) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->AttachFrameSink(&sink);
  sink.viewport_render_region_ = ViewportRenderRegion{
      .x_                = 600,
      .y_                = 400,
      .scale_x_          = 0.3f,
      .scale_y_          = 0.25f,
      .reference_width_  = 5000,
      .reference_height_ = 3000,
      .target_width_     = 1600,
      .target_height_    = 1200,
  };

  PipelineTask task;
  task.pipeline_executor_                                       = exec;
  task.options_.render_desc_.render_type_                       = RenderType::FAST_PREVIEW;
  task.options_.render_desc_.use_viewport_region_               = true;
  task.options_.render_desc_.frame_metadata_.preview_generation = 9;
  const auto request                                            = task.MakeApplyRequest();

  EXPECT_GT(sink.viewport_render_region_calls_, 0);
  const auto& metadata = request.submission.metadata;
  EXPECT_EQ(request.submission.mode, FramePresentationMode::RoiFrame);
  EXPECT_EQ(metadata.frame_role, FrameRole::InteractivePrimary);
  EXPECT_FALSE(metadata.scope_update_allowed);
  EXPECT_NEAR(metadata.source_roi_norm.x, 0.12f, 1.0e-5f);
  EXPECT_NEAR(metadata.source_roi_norm.y, 0.13333334f, 1.0e-5f);
  EXPECT_NEAR(metadata.source_roi_norm.width, 0.3f, 1.0e-5f);
  EXPECT_NEAR(metadata.source_roi_norm.height, 0.25f, 1.0e-5f);
  EXPECT_EQ(request.geometry.resolution.max_edge, 2560U);
  EXPECT_EQ(request.geometry.view.viewport_extent.width, 1600U);
  EXPECT_EQ(request.geometry.view.viewport_extent.height, 1200U);
}

TEST_F(PipelineFrameSinkTest, ScopeRefreshFastPreviewAllowsCurrentRoiAsScopeInput) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->AttachFrameSink(&sink);

  sink.viewport_render_region_ = ViewportRenderRegion{
      .x_                = 600,
      .y_                = 400,
      .scale_x_          = 0.3f,
      .scale_y_          = 0.25f,
      .reference_width_  = 5000,
      .reference_height_ = 3000,
      .target_width_     = 1600,
      .target_height_    = 1200,
  };

  PipelineTask task;
  task.pipeline_executor_                                            = exec;
  task.options_.render_desc_.render_type_                            = RenderType::FAST_PREVIEW;
  task.options_.render_desc_.use_viewport_region_                    = true;
  task.options_.render_desc_.frame_metadata_.scope_update_allowed    = true;
  task.options_.render_desc_.frame_metadata_.scope_refresh_requested = true;
  const auto request                                                 = task.MakeApplyRequest();

  EXPECT_EQ(request.submission.mode, FramePresentationMode::RoiFrame);
  EXPECT_TRUE(request.submission.metadata.scope_update_allowed);
  EXPECT_TRUE(request.submission.metadata.scope_refresh_requested);
}

TEST_F(PipelineFrameSinkTest, FullResExportRequestsExportQualityAtFullResolution) {
  auto         exec = std::make_shared<CPUPipelineExecutor>();

  PipelineTask export_task;
  export_task.pipeline_executor_                 = exec;
  export_task.options_.render_desc_.render_type_ = RenderType::FULL_RES_EXPORT;
  const auto export_request                      = export_task.MakeApplyRequest();
  EXPECT_EQ(export_request.geometry.resolution.quality, RenderQuality::Export);
  EXPECT_EQ(export_request.geometry.resolution.max_edge, 0U);

  PipelineTask preview_task;
  preview_task.pipeline_executor_                 = exec;
  preview_task.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  EXPECT_EQ(preview_task.MakeApplyRequest().geometry.resolution.quality, RenderQuality::Preview);
}

TEST_F(PipelineFrameSinkTest, ThumbnailAndExportApplyRequestsBypassSessionCache) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  PipelineTask preview;
  preview.pipeline_executor_                 = exec;
  preview.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
  const auto preview_request                 = preview.MakeApplyRequest();
  EXPECT_EQ(preview_request.cache_policy, RenderCachePolicy::UseSessionCache);

  PipelineTask thumbnail;
  thumbnail.pipeline_executor_                 = exec;
  thumbnail.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  thumbnail.options_.render_desc_.max_edge_    = 256;
  const auto thumbnail_request                 = thumbnail.MakeApplyRequest();
  EXPECT_EQ(thumbnail_request.cache_policy, RenderCachePolicy::BypassSessionCache);

  PipelineTask export_task;
  export_task.pipeline_executor_                 = exec;
  export_task.options_.render_desc_.render_type_ = RenderType::FULL_RES_EXPORT;
  const auto export_request                      = export_task.MakeApplyRequest();
  EXPECT_EQ(export_request.cache_policy, RenderCachePolicy::BypassSessionCache);
}

TEST_F(PipelineFrameSinkTest, QualityBasePreviewUsesSessionCacheAndSensorDevelopPersistence) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  PipelineTask quality;
  quality.pipeline_executor_                 = exec;
  quality.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  const auto request                         = quality.MakeApplyRequest();
  EXPECT_EQ(request.submission.metadata.frame_role, FrameRole::QualityBase);
  EXPECT_EQ(request.cache_policy, RenderCachePolicy::UseSessionCache);
  EXPECT_EQ(request.geometry.resolution.max_edge, 4096U);
  EXPECT_EQ(ResultPersistenceScopeForRole(request.submission.metadata.frame_role),
            ResultPersistenceScope::SensorDevelopOnly);
}

TEST_F(PipelineFrameSinkTest, ThumbnailAndExportRequestsRequireHostOutput) {
  auto         exec = std::make_shared<CPUPipelineExecutor>();

  PipelineTask thumbnail;
  thumbnail.pipeline_executor_                 = exec;
  thumbnail.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  thumbnail.options_.render_desc_.max_edge_    = 256;
  const auto thumbnail_request                 = thumbnail.MakeApplyRequest();
  EXPECT_TRUE(thumbnail_request.require_host_output);
  EXPECT_EQ(thumbnail_request.sink, nullptr);

  PipelineTask preview;
  preview.pipeline_executor_                 = exec;
  preview.options_.render_desc_.render_type_ = RenderType::FAST_PREVIEW;
  EXPECT_FALSE(preview.MakeApplyRequest().require_host_output);

  PipelineTask export_task;
  export_task.pipeline_executor_                 = exec;
  export_task.options_.render_desc_.render_type_ = RenderType::FULL_RES_EXPORT;
  const auto export_request                      = export_task.MakeApplyRequest();
  EXPECT_TRUE(export_request.require_host_output);
  EXPECT_EQ(export_request.sink, nullptr);
}

// =========================================================================
// Phase 1 Acceptance Criterion 2:
//   "Closing the editor while preview work is in flight cannot leave a
//    dangling sink pointer."
// =========================================================================

TEST_F(PipelineFrameSinkTest, DetachUnderLockIsSafeDuringConcurrentAccess) {
  // Simulates the pattern used by EditorFrameManager::~EditorFrameManager():
  // acquire render_lock_ → DetachFrameSink() → release.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->AttachFrameSink(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->DetachFrameSink();
  }

  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

TEST_F(PipelineFrameSinkTest, ReattachAfterDetachIsSafe) {
  // After detach, re-attaching a new sink should work without stale state.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink1;
  MockFrameSink sink2;

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink1);
  }
  EXPECT_EQ(exec->GetFrameSink(), &sink1);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->DetachFrameSink();
  }
  EXPECT_EQ(exec->GetFrameSink(), nullptr);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink2);
  }
  EXPECT_EQ(exec->GetFrameSink(), &sink2);
}

TEST_F(PipelineFrameSinkTest, AttachDetachRoundTripKeepsSinkQueries) {
  // The editor attaches the sink under the render lock before each render and detaches it on
  // close; the round-trip must leave the viewport query routed to the attached sink only.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  sink.viewport_render_region_ = ViewportRenderRegion{.x_ = 10, .reference_width_ = 100};

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink);
    EXPECT_EQ(exec->GetFrameSink(), &sink);
    exec->DetachFrameSink();
    EXPECT_EQ(exec->GetFrameSink(), nullptr);
    EXPECT_EQ(exec->GetViewportRenderRegion(), std::nullopt);
  }
  EXPECT_EQ(sink.viewport_render_region_calls_, 0);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink);
  }
  const auto region = exec->GetViewportRenderRegion();
  ASSERT_TRUE(region.has_value());
  EXPECT_EQ(region->x_, 10);
  EXPECT_EQ(sink.viewport_render_region_calls_, 1);
}

// =========================================================================
// Phase 1 Acceptance Criterion 4:
//   "A cached pipeline can be reused for thumbnail/export/editor without
//    carrying stale UI output state."
// =========================================================================

TEST_F(PipelineFrameSinkTest, ClearAllIntermediateBuffersDoesNotClearFrameSink) {
  // ClearAllIntermediateBuffers() is an intermediate cleanup, not a full
  // reset; it should preserve the frame sink binding.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->AttachFrameSink(&sink);
  exec->ClearAllIntermediateBuffers();

  EXPECT_EQ(exec->GetFrameSink(), &sink);
}

TEST_F(PipelineFrameSinkTest, SetAcceleratorBackendPreservesFrameSink) {
  // Changing the accelerator backend preference should preserve an attached
  // frame sink so editor preview is not disrupted by a preference change.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->AttachFrameSink(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);
  EXPECT_EQ(exec->GetFrameSink(), &sink);
}

// =========================================================================
// Thread-safety tests
// =========================================================================

TEST_F(PipelineFrameSinkTest, HistoryQueuesBehindRenderOwnershipOfLivePipeline) {
  // render_lock_ is sole live-pipeline ownership for the full frame. History
  // must wait until render releases it — not race under a second occupancy bit.
  auto                         exec = std::make_shared<CPUPipelineExecutor>();
  std::unique_lock<std::mutex> worker_lock(exec->GetRenderLock());
  EXPECT_TRUE(worker_lock.owns_lock());

  std::atomic<bool> owner_acquired{false};
  std::thread       owner([&] {
    std::unique_lock<std::mutex> owner_lock(exec->GetRenderLock());
    owner_acquired.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(owner_acquired.load());

  worker_lock.unlock();
  for (int i = 0; i < 50 && !owner_acquired.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(owner_acquired.load());
  owner.join();
}

TEST_F(PipelineFrameSinkTest, ConcurrentDetachAndRenderLockIsDeadlockFree) {
  // Multiple threads repeatedly acquiring render_lock_ for detach/render
  // operations must not deadlock.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->AttachFrameSink(&sink);

  std::atomic<bool> stop{false};
  std::atomic<int>  ops{0};

  const auto        detach_work = [&]() {
    while (!stop.load()) {
      std::unique_lock<std::mutex> lock(exec->GetRenderLock());
      exec->DetachFrameSink();
      exec->AttachFrameSink(&sink);
      ops.fetch_add(1);
      std::this_thread::yield();
    }
  };

  const auto render_work = [&]() {
    while (!stop.load()) {
      {
        std::unique_lock<std::mutex> lock(exec->GetRenderLock());
        // Simulate the render path's use of frame sink methods.
        (void)exec->GetFrameSink();
        (void)exec->GetViewportRenderRegion();
      }
      ops.fetch_add(1);
      std::this_thread::yield();
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(6);
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back(detach_work);
    threads.emplace_back(render_work);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true);

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_GT(ops.load(), 0);
  // The real assertion: we reached here without deadlock.
  SUCCEED();
}

// =========================================================================
// Exception-safety tests
// =========================================================================

TEST_F(PipelineFrameSinkTest, SinkIsRestoredAfterExceptionDuringRender) {
  // Simulates the scenario from Phase 1 Review Finding 1:
  // if an exception is thrown after detaching the editor frame sink,
  // the RAII guard must restore the sink before the exception propagates.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->AttachFrameSink(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  IFrameSink* saved_sink = nullptr;
  bool        caught     = false;

  try {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());

    saved_sink = exec->GetFrameSink();
    ASSERT_NE(saved_sink, nullptr);
    exec->DetachFrameSink();
    EXPECT_EQ(exec->GetFrameSink(), nullptr);

    const auto restore_sink = [&]() {
      if (saved_sink && exec) {
        exec->AttachFrameSink(saved_sink);
      }
    };
    auto sink_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1), [&restore_sink](void*) { restore_sink(); });

    // Simulate render work that throws.
    throw std::runtime_error("simulated render failure");
  } catch (const std::runtime_error&) {
    caught = true;
  }

  EXPECT_TRUE(caught);
  // After exception, the RAII guard must have restored the sink.
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  // And the sink still answers the viewport query.
  (void)exec->GetViewportRenderRegion();
  EXPECT_EQ(sink.viewport_render_region_calls_, 1);
}

TEST_F(PipelineFrameSinkTest, SinkIsRestoredAfterExceptionBeforeRender) {
  // If an exception is thrown between detach and Apply() (e.g., in
  // MakeApplyRequest), the RAII guard must still restore the sink.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->AttachFrameSink(&sink);

  IFrameSink* saved_sink = nullptr;
  bool        caught     = false;

  try {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());

    saved_sink = exec->GetFrameSink();
    exec->DetachFrameSink();

    const auto restore_sink = [&]() {
      if (saved_sink && exec) {
        exec->AttachFrameSink(saved_sink);
      }
    };
    auto sink_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1), [&restore_sink](void*) { restore_sink(); });

    // Throw before MakeApplyRequest / Apply.
    throw std::logic_error("pre-render failure");
  } catch (const std::logic_error&) {
    caught = true;
  }

  EXPECT_TRUE(caught);
  EXPECT_EQ(exec->GetFrameSink(), &sink);
}

TEST_F(PipelineFrameSinkTest, SinkIsNotRestoredIfNeverDetached) {
  // If no sink was attached when entering the render path, the RAII guard
  // must be a no-op (no spurious attach of nullptr).
  auto exec = std::make_shared<CPUPipelineExecutor>();  // no sink attached

  bool caught = false;
  try {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());

    IFrameSink*                  saved_sink   = exec->GetFrameSink();  // nullptr
    // No detach — saved_sink is nullptr.

    const auto                   restore_sink = [&]() {
      if (saved_sink && exec) {
        exec->AttachFrameSink(saved_sink);
      }
    };
    auto sink_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1), [&restore_sink](void*) { restore_sink(); });

    throw std::runtime_error("no-sink render failure");
  } catch (const std::runtime_error&) {
    caught = true;
  }

  EXPECT_TRUE(caught);
  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

// The accelerator backend is a runtime property of the process (the user's backend setting). It
// is resolved on the executor and is never part of the persisted document.
TEST_F(PipelineFrameSinkTest, AcceleratorPreferenceResolvesRuntimeBackend) {
  auto exec = std::make_shared<CPUPipelineExecutor>();
  EXPECT_EQ(exec->GetAcceleratorBackendPreference(), AcceleratorBackendPreference::Auto);
  EXPECT_EQ(exec->GetResolvedAcceleratorBackend(),
            ResolveAcceleratorBackend(AcceleratorBackendPreference::Auto));

  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);
  EXPECT_EQ(exec->GetAcceleratorBackendPreference(), AcceleratorBackendPreference::CPU);
  EXPECT_EQ(exec->GetResolvedAcceleratorBackend(), GpuBackendKind::None);
}

}  // namespace alcedo
