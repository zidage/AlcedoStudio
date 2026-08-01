//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <thread>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "edit/operators/raw/raw_decode_op.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "image/image.hpp"
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

class PipelineFrameSinkTest : public ::testing::Test {
 protected:
  void SetUp() override { RegisterAllOperators(); }
};

TEST_F(PipelineFrameSinkTest, DetachFrameSinkClearsPointer) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  exec->DetachFrameSink();
  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

TEST_F(PipelineFrameSinkTest, SetExecutionStagesWithoutSinkHasNullFrameSink) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  exec->SetExecutionStages();
  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

// Re-attaching a frame sink must NOT recreate the merged GPU stage. The merged
// stage owns the LLF highlight/shadow stage's cross-frame reference cache
// (cached_reference_base_/cached_source_key_/cached_width_/...); recreating it
// every render wipes that cache so zoomed ROI/detail frames can no longer reuse
// the full-image mask (the 42ed19b CanReuseReferenceForRoi path) and recompute
// instead, flickering on every pan/zoom. The QML production path re-attaches
// the same sink per render via AttachExecutionStages -> SetExecutionStages
// (IFrameSink*), so the merged-stage identity must be stable across re-attach.
TEST_F(PipelineFrameSinkTest, ReattachingFrameSinkPreservesMergedStage) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  // First attach builds the stage graph (merged_stages_ non-null).
  exec->SetExecutionStages(&sink);
  const auto identity_after_build = exec->DebugGetMergedStageIdentity();
  ASSERT_NE(identity_after_build, std::uintptr_t{0});

  // Re-attaching the same sink must not rebuild the merged stage.
  exec->SetExecutionStages(&sink);
  EXPECT_EQ(exec->DebugGetMergedStageIdentity(), identity_after_build);

  // Swapping to a different sink also must not rebuild; only the sink pointer
  // changes (matching DetachFrameSink/AttachFrameSink's lightweight behavior).
  MockFrameSink other_sink;
  exec->SetExecutionStages(&other_sink);
  EXPECT_EQ(exec->DebugGetMergedStageIdentity(), identity_after_build);
  EXPECT_EQ(exec->GetFrameSink(), &other_sink);

  // A genuine reset (e.g. backend switch routes through ResetExecutionStages)
  // tears the graph down; the next attach rebuilds a fresh merged stage.
  exec->ResetExecutionStages();
  EXPECT_EQ(exec->DebugGetMergedStageIdentity(), std::uintptr_t{0});
  exec->SetExecutionStages(&sink);
  const auto identity_after_reset = exec->DebugGetMergedStageIdentity();
  EXPECT_NE(identity_after_reset, std::uintptr_t{0});
  EXPECT_NE(identity_after_reset, identity_after_build);
}

TEST_F(PipelineFrameSinkTest, BindFrameSubmissionIsNoOpWhenSinkIsDetached) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  EXPECT_NO_THROW(exec->BindFrameSubmission(FramePreviewMetadata{},
                                            FramePresentationMode::ViewportTransformed));
}

TEST_F(PipelineFrameSinkTest, BindFrameSubmissionForwardsToAttachedSink) {
  auto exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

  FramePreviewMetadata metadata{};
  metadata.presentation_request_id = 42;
  exec->BindFrameSubmission(metadata, FramePresentationMode::RoiFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 1);
  EXPECT_EQ(sink.last_bound_submission_.metadata.presentation_request_id, 42u);
  EXPECT_EQ(sink.last_bound_submission_.mode, FramePresentationMode::RoiFrame);
}

TEST_F(PipelineFrameSinkTest, GetViewportRenderRegionReturnsNulloptWhenSinkIsDetached) {
  auto exec = std::make_shared<CPUPipelineExecutor>();

  EXPECT_EQ(exec->GetViewportRenderRegion(), std::nullopt);
}

TEST_F(PipelineFrameSinkTest, RenderRegionCropsEvenWhenScaleIsFullRes) {
  cv::Mat image(100, 200, CV_32FC3);
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      image.at<cv::Vec3f>(y, x) = cv::Vec3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
    }
  }

  nlohmann::json params;
  params["resize"] = {{"enable_scale", false},
                      {"maximum_edge", 4096},
                      {"enable_roi", true},
                      {"downsample_algorithm", "inter_area"},
                      {"roi",
                       {{"x", 50},
                        {"y", 20},
                        {"resize_factor_x", 0.5f},
                        {"resize_factor_y", 0.5f},
                        {"resize_factor", 0.5f},
                        {"reference_width", 200},
                        {"reference_height", 100}}}};

  PipelineStage stage(PipelineStageName::Geometry_Adjustment,
                      /*enable_cache=*/true,
                      /*is_streamable=*/false);
  stage.SetOperator(OperatorType::RESIZE, params);
  stage.SetInputImage(std::make_shared<ImageBuffer>(std::move(image)));

  OperatorParams global_params;
  auto           result = stage.ApplyStage(global_params);
  ASSERT_TRUE(result);
  const auto& output = result->GetCPUData();

  ASSERT_EQ(output.cols, 100);
  ASSERT_EQ(output.rows, 50);
  EXPECT_FLOAT_EQ(output.at<cv::Vec3f>(0, 0)[0], 50.0f);
  EXPECT_FLOAT_EQ(output.at<cv::Vec3f>(0, 0)[1], 20.0f);
}

TEST_F(PipelineFrameSinkTest, RenderRegionDoesNotUpscaleWhenViewportTargetExceedsSourceRoi) {
  cv::Mat image(100, 200, CV_32FC3);
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      image.at<cv::Vec3f>(y, x) = cv::Vec3f(static_cast<float>(x), static_cast<float>(y), 0.0f);
    }
  }

  nlohmann::json params;
  params["resize"] = {{"enable_scale", true},
                      {"maximum_edge", 220},
                      {"enable_roi", true},
                      {"downsample_algorithm", "inter_area"},
                      {"roi",
                       {{"x", 0},
                        {"y", 0},
                        {"resize_factor_x", 0.5f},
                        {"resize_factor_y", 0.5f},
                        {"resize_factor", 0.5f},
                        {"reference_width", 200},
                        {"reference_height", 100}}}};

  PipelineStage stage(PipelineStageName::Geometry_Adjustment,
                      /*enable_cache=*/true,
                      /*is_streamable=*/false);
  stage.SetOperator(OperatorType::RESIZE, params);
  stage.SetInputImage(std::make_shared<ImageBuffer>(std::move(image)));

  OperatorParams global_params;
  auto           result = stage.ApplyStage(global_params);
  ASSERT_TRUE(result);
  const auto& output = result->GetCPUData();

  EXPECT_EQ(output.cols, 100);
  EXPECT_EQ(output.rows, 50);
}

TEST_F(PipelineFrameSinkTest, DetailRoiPreviewUsesViewportTargetPixelsAsMaxEdge) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

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

  task.SetExecutorRenderParams();

  const auto resize_entry =
      exec->GetStage(PipelineStageName::Geometry_Adjustment).GetOperator(OperatorType::RESIZE);
  ASSERT_TRUE(resize_entry.has_value());
  ASSERT_NE(resize_entry.value(), nullptr);
  ASSERT_NE(resize_entry.value()->op_, nullptr);

  const auto params = resize_entry.value()->op_->GetParams();
  ASSERT_TRUE(params.contains("resize"));
  const auto& resize = params["resize"];
  EXPECT_TRUE(resize.value("enable_scale", false));
  EXPECT_EQ(resize.value("maximum_edge", 0), 1800);
  EXPECT_TRUE(resize.value("enable_roi", false));
  ASSERT_TRUE(resize.contains("roi"));
  const auto& roi = resize["roi"];
  EXPECT_EQ(roi.value("x", 0), 1200);
  EXPECT_EQ(roi.value("y", 0), 600);
  EXPECT_FLOAT_EQ(roi.value("resize_factor_x", 0.0f), 0.25f);
  EXPECT_FLOAT_EQ(roi.value("resize_factor_y", 0.0f), 0.2f);
  EXPECT_EQ(roi.value("reference_width", 0), 6000);
  EXPECT_EQ(roi.value("reference_height", 0), 4000);
}

TEST_F(PipelineFrameSinkTest, ActiveCudaHighlightShadowKeepsDetailRoiPreviewAsPatch) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

  try {
    exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA backend unavailable: " << e.what();
  }

  auto& basic = exec->GetStage(PipelineStageName::Basic_Adjustment);
  basic.SetOperator(OperatorType::SHADOWS, {{"shadows", 40.0f}}, exec->GetGlobalParams());

  sink.viewport_render_region_ = ViewportRenderRegion{
      .x_                = 900,
      .y_                = 450,
      .scale_x_          = 0.2f,
      .scale_y_          = 0.2f,
      .reference_width_  = 6000,
      .reference_height_ = 4000,
      .target_width_     = 2200,
      .target_height_    = 1500,
  };

  PipelineTask task;
  task.pipeline_executor_                                       = exec;
  task.options_.render_desc_.render_type_                       = RenderType::DETAIL_ROI_PREVIEW;
  task.options_.render_desc_.use_viewport_region_               = true;
  task.options_.render_desc_.frame_metadata_.preview_generation = 8;

  task.SetExecutorRenderParams();

  EXPECT_GT(sink.viewport_render_region_calls_, 0);
  EXPECT_EQ(sink.last_bound_submission_.metadata.frame_role, FrameRole::DetailPatch);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.x, 0.15f, 1.0e-5f);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.y, 0.1125f, 1.0e-5f);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.width, 0.2f, 1.0e-5f);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.height, 0.2f, 1.0e-5f);

  const auto resize_entry =
      exec->GetStage(PipelineStageName::Geometry_Adjustment).GetOperator(OperatorType::RESIZE);
  ASSERT_TRUE(resize_entry.has_value());
  ASSERT_NE(resize_entry.value(), nullptr);
  ASSERT_NE(resize_entry.value()->op_, nullptr);

  const auto params = resize_entry.value()->op_->GetParams();
  ASSERT_TRUE(params.contains("resize"));
  const auto& resize = params["resize"];
  EXPECT_TRUE(resize.value("enable_roi", false));
  EXPECT_TRUE(resize.value("enable_scale", false));
  EXPECT_EQ(resize.value("maximum_edge", 0), 2200);
}

TEST_F(PipelineFrameSinkTest, ActiveCudaHighlightShadowKeepsFastPreviewAsRoiFrame) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

  try {
    exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA backend unavailable: " << e.what();
  }

  auto& basic = exec->GetStage(PipelineStageName::Basic_Adjustment);
  basic.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", -35.0f}}, exec->GetGlobalParams());

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

  task.SetExecutorRenderParams();

  EXPECT_GT(sink.viewport_render_region_calls_, 0);
  EXPECT_EQ(sink.last_bound_submission_.mode, FramePresentationMode::RoiFrame);
  EXPECT_EQ(sink.last_bound_submission_.metadata.frame_role, FrameRole::InteractivePrimary);
  EXPECT_FALSE(sink.last_bound_submission_.metadata.scope_update_allowed);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.x, 0.12f, 1.0e-5f);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.y, 0.13333334f, 1.0e-5f);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.width, 0.3f, 1.0e-5f);
  EXPECT_NEAR(sink.last_bound_submission_.metadata.source_roi_norm.height, 0.25f, 1.0e-5f);

  const auto resize_entry =
      exec->GetStage(PipelineStageName::Geometry_Adjustment).GetOperator(OperatorType::RESIZE);
  ASSERT_TRUE(resize_entry.has_value());
  ASSERT_NE(resize_entry.value(), nullptr);
  ASSERT_NE(resize_entry.value()->op_, nullptr);

  const auto params = resize_entry.value()->op_->GetParams();
  ASSERT_TRUE(params.contains("resize"));
  const auto& resize = params["resize"];
  EXPECT_TRUE(resize.value("enable_roi", false));
  EXPECT_TRUE(resize.value("enable_scale", false));
  EXPECT_EQ(resize.value("maximum_edge", 0), 2560);
  ASSERT_TRUE(resize.contains("roi"));
  const auto& roi = resize["roi"];
  EXPECT_EQ(roi.value("x", 0), 600);
  EXPECT_EQ(roi.value("y", 0), 400);
  EXPECT_FLOAT_EQ(roi.value("resize_factor_x", 0.0f), 0.3f);
  EXPECT_FLOAT_EQ(roi.value("resize_factor_y", 0.0f), 0.25f);
  EXPECT_EQ(roi.value("reference_width", 0), 5000);
  EXPECT_EQ(roi.value("reference_height", 0), 3000);
}

TEST_F(PipelineFrameSinkTest, ScopeRefreshFastPreviewAllowsCurrentRoiAsScopeInput) {
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

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

  task.SetExecutorRenderParams();

  EXPECT_EQ(sink.last_bound_submission_.mode, FramePresentationMode::RoiFrame);
  EXPECT_TRUE(sink.last_bound_submission_.metadata.scope_update_allowed);
  EXPECT_TRUE(sink.last_bound_submission_.metadata.scope_refresh_requested);
}

TEST_F(PipelineFrameSinkTest, RenderSourceCacheKeyUsesStableImageIdentityBeforeBufferPointer) {
  auto image =
      std::make_shared<Image>(42, std::filesystem::path(L"D:/photos/source.dng"), ImageType::DNG);

  PipelineTask first;
  first.pipeline_executor_                 = std::make_shared<CPUPipelineExecutor>();
  first.input_desc_                        = image;
  first.input_                             = std::make_shared<ImageBuffer>(cv::Mat(4, 4, CV_32FC3));
  first.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  first.SetExecutorRenderParams();
  const auto   first_key = first.pipeline_executor_->GetGlobalParams().render_source_cache_key_;

  PipelineTask second;
  second.pipeline_executor_ = std::make_shared<CPUPipelineExecutor>();
  second.input_desc_        = image;
  second.input_             = std::make_shared<ImageBuffer>(cv::Mat(4, 4, CV_32FC3));
  second.options_.render_desc_.render_type_ = RenderType::DETAIL_ROI_PREVIEW;
  second.SetExecutorRenderParams();
  const auto second_key = second.pipeline_executor_->GetGlobalParams().render_source_cache_key_;

  EXPECT_EQ(first_key, second_key);
}

TEST_F(PipelineFrameSinkTest, FullResExportPreservesHighlightShadowSourceDetail) {
  auto         exec = std::make_shared<CPUPipelineExecutor>();

  PipelineTask export_task;
  export_task.pipeline_executor_                 = exec;
  export_task.options_.render_desc_.render_type_ = RenderType::FULL_RES_EXPORT;
  export_task.SetExecutorRenderParams();

  EXPECT_TRUE(exec->GetGlobalParams().render_hs_preserve_source_detail_);

  const auto resize_entry =
      exec->GetStage(PipelineStageName::Geometry_Adjustment).GetOperator(OperatorType::RESIZE);
  ASSERT_TRUE(resize_entry.has_value());
  ASSERT_NE(resize_entry.value(), nullptr);
  ASSERT_NE(resize_entry.value()->op_, nullptr);

  const auto params = resize_entry.value()->op_->GetParams();
  ASSERT_TRUE(params.contains("resize"));
  EXPECT_FALSE(params["resize"].value("enable_scale", true));

  PipelineTask preview_task;
  preview_task.pipeline_executor_                 = exec;
  preview_task.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  preview_task.SetExecutorRenderParams();

  EXPECT_FALSE(exec->GetGlobalParams().render_hs_preserve_source_detail_);
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

  exec->SetExecutionStages(&sink);
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
    exec->SetExecutionStages(&sink1);
  }
  EXPECT_EQ(exec->GetFrameSink(), &sink1);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->DetachFrameSink();
  }
  EXPECT_EQ(exec->GetFrameSink(), nullptr);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->SetExecutionStages(&sink2);
  }
  EXPECT_EQ(exec->GetFrameSink(), &sink2);
}

TEST_F(PipelineFrameSinkTest, AttachFrameSinkSetsPointerWithoutRebuildingStages) {
  // AttachFrameSink should set the sink on both the executor and the tail
  // execution stage without tearing down and rebuilding the stage vector.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages();
  EXPECT_EQ(exec->GetFrameSink(), nullptr);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink);
  }
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  // Verify the sink delegates work — the tail stage should forward
  // presentation metadata to the attached sink.
  exec->BindFrameSubmission({}, FramePresentationMode::RoiFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 1);
  EXPECT_EQ(sink.last_bound_submission_.mode, FramePresentationMode::RoiFrame);
}

TEST_F(PipelineFrameSinkTest, AttachDetachRoundTripWithoutStageRebuild) {
  // Verify that AttachFrameSink / DetachFrameSink form a lightweight
  // round-trip that does not require SetExecutionStages (which is expensive).
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  // Round-trip: detach → re-attach → detach again.
  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->DetachFrameSink();
    EXPECT_EQ(exec->GetFrameSink(), nullptr);
    exec->AttachFrameSink(&sink);
    EXPECT_EQ(exec->GetFrameSink(), &sink);
    exec->DetachFrameSink();
    EXPECT_EQ(exec->GetFrameSink(), nullptr);
  }

  // Re-attach after round-trip still works.
  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink);
  }
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  exec->BindFrameSubmission(FramePreviewMetadata{}, FramePresentationMode::FullFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 1);
}

TEST_F(PipelineFrameSinkTest, DetachThenAttachPreservesSinkCalls) {
  // After detach+attach, the re-attached sink should receive subsequent
  // frame presentation calls normally.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  exec->BindFrameSubmission({}, FramePresentationMode::FullFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 1);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->DetachFrameSink();
  }

  exec->BindFrameSubmission({}, FramePresentationMode::RoiFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 1);

  {
    std::unique_lock<std::mutex> lock(exec->GetRenderLock());
    exec->AttachFrameSink(&sink);
  }

  exec->BindFrameSubmission({}, FramePresentationMode::RoiFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 2);
  EXPECT_EQ(sink.last_bound_submission_.mode, FramePresentationMode::RoiFrame);
}

// =========================================================================
// Phase 1 Acceptance Criterion 4:
//   "A cached pipeline can be reused for thumbnail/export/editor without
//    carrying stale UI output state."
// =========================================================================

TEST_F(PipelineFrameSinkTest, ResetExecutionStagesClearsFrameSink) {
  // PipelineMgmtService::SavePipeline() calls ResetExecutionStages() which
  // must clear frame_sink_ so the cached pipeline carries no stale sink.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  exec->ResetExecutionStages();
  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

TEST_F(PipelineFrameSinkTest, ClearAllIntermediateBuffersDoesNotClearFrameSink) {
  // ClearAllIntermediateBuffers() is an intermediate cleanup, not a full
  // reset; it should preserve the frame sink binding.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  exec->ClearAllIntermediateBuffers();

  EXPECT_EQ(exec->GetFrameSink(), &sink);
}

// =========================================================================
// Phase 1 Acceptance Criterion 3 (partial):
//   Importing history cannot mutate execution stages concurrently with render.
// =========================================================================

TEST_F(PipelineFrameSinkTest, ImportPipelineParamsResetsFrameSink) {
  // ImportPipelineParams() internally calls ResetExecutionStages() and must
  // clear the frame sink so that importing history doesn't leave a stale
  // editor sink attached.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  nlohmann::json params = exec->ExportPipelineParams();
  exec->ImportPipelineParams(params);

  EXPECT_EQ(exec->GetFrameSink(), nullptr);
}

TEST_F(PipelineFrameSinkTest, SetAcceleratorBackendPreservesFrameSink) {
  // Changing the accelerator backend preference should preserve an attached
  // frame sink so editor preview is not disrupted by a preference change.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);
  EXPECT_EQ(exec->GetFrameSink(), &sink);

  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);
  EXPECT_EQ(exec->GetFrameSink(), &sink);
}

// =========================================================================
// Thread-safety tests
// =========================================================================

TEST_F(PipelineFrameSinkTest, ConcurrentDetachAndRenderLockIsDeadlockFree) {
  // Multiple threads repeatedly acquiring render_lock_ for detach/render
  // operations must not deadlock.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

  std::atomic<bool> stop{false};
  std::atomic<int>  ops{0};

  const auto        detach_work = [&]() {
    while (!stop.load()) {
      std::unique_lock<std::mutex> lock(exec->GetRenderLock());
      exec->DetachFrameSink();
      exec->SetExecutionStages(&sink);
      ops.fetch_add(1);
      std::this_thread::yield();
    }
  };

  const auto render_work = [&]() {
    while (!stop.load()) {
      {
        std::unique_lock<std::mutex> lock(exec->GetRenderLock());
        // Simulate the render path's use of frame sink methods.
        exec->BindFrameSubmission(FramePreviewMetadata{}, FramePresentationMode::ViewportTransformed);
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

TEST_F(PipelineFrameSinkTest, ConcurrentImportPipelineParamsAndRenderIsDeadlockFree) {
  // Simulates the scenario described in Acceptance Criterion 3:
  // reopening the editor or importing history concurrently with render.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;
  exec->SetExecutionStages(&sink);

  const nlohmann::json params = exec->ExportPipelineParams();

  std::atomic<bool>    stop{false};
  std::atomic<int>     ops{0};

  const auto           import_work = [&]() {
    while (!stop.load()) {
      {
        std::unique_lock<std::mutex> lock(exec->GetRenderLock());
        exec->ImportPipelineParams(params);
        exec->SetExecutionStages(&sink);
      }
      ops.fetch_add(1);
      std::this_thread::yield();
    }
  };

  const auto render_work = [&]() {
    while (!stop.load()) {
      {
        std::unique_lock<std::mutex> lock(exec->GetRenderLock());
        exec->BindFrameSubmission({}, FramePresentationMode::ViewportTransformed);
      }
      ops.fetch_add(1);
      std::this_thread::yield();
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int i = 0; i < 2; ++i) {
    threads.emplace_back(import_work);
    threads.emplace_back(render_work);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop.store(true);

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_GT(ops.load(), 0);
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

  exec->SetExecutionStages(&sink);
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

  // And the sink is still functional.
  exec->BindFrameSubmission({}, FramePresentationMode::FullFrame);
  EXPECT_EQ(sink.bind_submission_calls_, 1);
}

TEST_F(PipelineFrameSinkTest, SinkIsRestoredAfterExceptionBeforeRender) {
  // If an exception is thrown between detach and Apply() (e.g., in
  // SetExecutorRenderParams), the RAII guard must still restore the sink.
  auto          exec = std::make_shared<CPUPipelineExecutor>();
  MockFrameSink sink;

  exec->SetExecutionStages(&sink);

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

    // Throw before SetExecutorRenderParams / Apply.
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
  auto exec = std::make_shared<CPUPipelineExecutor>();
  exec->SetExecutionStages();  // no sink attached

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

// The accelerator backend is a runtime property of the process (the user's
// backend setting), never part of the persisted edit state. Stored params that
// carry an old backend (e.g. "cuda" saved when the state was created) must not
// drive the decode: an OpenCL session must decode with OpenCL even when the
// imported state was saved under CUDA.
auto RawDecodeBackendOf(CPUPipelineExecutor& exec) -> RawGpuBackend {
  auto entry =
      exec.GetStage(PipelineStageName::Image_Loading).GetOperator(OperatorType::RAW_DECODE);
  if (!entry.has_value() || !entry.value() || !entry.value()->op_) {
    return RawGpuBackend::CPU;
  }
  auto* raw_op = dynamic_cast<RawDecodeOp*>(entry.value()->op_.get());
  return raw_op ? raw_op->params_.gpu_backend_ : RawGpuBackend::CPU;
}

TEST_F(PipelineFrameSinkTest, ImportedRawBackendCannotOverrideRuntimePreference) {
  auto exec = std::make_shared<CPUPipelineExecutor>();
  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);
  EXPECT_EQ(RawDecodeBackendOf(*exec), RawGpuBackend::CPU);

  // Params never carry the backend: exported state has no backend key.
  // Exported stage state is nested as stage name -> {script_name -> {…}}.
  const nlohmann::json exported = exec->ExportPipelineParams();
  const nlohmann::json raw_params =
      exported.value("Image_Loading", nlohmann::json::object())
          .value("Image_Loading", nlohmann::json::object())
          .value("raw_decode", nlohmann::json::object())
          .value("params", nlohmann::json::object())
          .value("raw", nlohmann::json::object());
  EXPECT_FALSE(raw_params.contains("gpu_backend"));

  // A state saved under a different backend (CUDA) must not change the decode.
  nlohmann::json stored = exported;
  stored["Image_Loading"]["Image_Loading"]["raw_decode"]["params"]["raw"]["gpu_backend"] =
      "cuda";
  exec->ImportPipelineParams(stored);

  EXPECT_EQ(RawDecodeBackendOf(*exec), RawGpuBackend::CPU);
}

TEST_F(PipelineFrameSinkTest, RawBackendParamsAreInertAndRuntimePreferenceWins) {
  auto exec = std::make_shared<CPUPipelineExecutor>();
  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);

  // Direct param writes with backend keys must not move the decode: the keys
  // are ignored by SetParams.
  auto&          raw_stage = exec->GetStage(PipelineStageName::Image_Loading);
  nlohmann::json stale_params;
  stale_params["raw"] = {{"gpu_backend", "cuda"}};
  raw_stage.SetOperator(OperatorType::RAW_DECODE, stale_params);
  EXPECT_EQ(RawDecodeBackendOf(*exec), RawGpuBackend::CPU);

  // The runtime preference drives the decode; switching it moves the op.
  try {
    exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA backend unavailable: " << e.what();
  }
  EXPECT_EQ(RawDecodeBackendOf(*exec), RawGpuBackend::CUDA);
}

}  // namespace alcedo
