//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>

#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"

namespace alcedo {
namespace {

auto EnsureCudaDevice() -> bool {
  const int device_count = cv::cuda::getCudaEnabledDeviceCount();
  if (device_count <= 0) {
    return false;
  }
  cv::cuda::setDevice(0);
  return true;
}

auto MakeLinearInput(int width, int height) -> cv::Mat {
  cv::Mat input(height, width, CV_32FC4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(std::max(1, width - 1));
      const float fy = static_cast<float>(y) / static_cast<float>(std::max(1, height - 1));
      input.at<cv::Vec4f>(y, x) = cv::Vec4f(fx, fy, 0.5f * (fx + fy), 1.0f);
    }
  }
  return input;
}

auto ExpectedScratchBytes(int width, int height) -> size_t {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(float4);
}

auto ExpectedFastPreviewSize(int width, int height) -> cv::Size {
  const float scale = std::min(1.0f, 2560.0f / static_cast<float>(std::max(width, height)));
  return cv::Size(std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale))),
                  std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale))));
}

auto MakeConfiguredExecutor() -> std::shared_ptr<CPUPipelineExecutor> {
  auto executor = std::make_shared<CPUPipelineExecutor>();
  auto& loading = executor->GetStage(PipelineStageName::Image_Loading);
  auto& params  = executor->GetGlobalParams();
  loading.EnableOperator(OperatorType::RAW_DECODE, false, params);
  loading.EnableOperator(OperatorType::LENS_CALIBRATION, false, params);
  return executor;
}

auto SubmitBlockingRender(PipelineScheduler& scheduler,
                          const std::shared_ptr<CPUPipelineExecutor>& pipeline,
                          const std::shared_ptr<ImageBuffer>& input,
                          RenderType render_type) -> std::shared_ptr<ImageBuffer> {
  PipelineTask task;
  task.pipeline_executor_                 = pipeline;
  task.input_                             = input;
  task.options_.render_desc_.render_type_ = render_type;
  task.options_.is_blocking_              = true;
  task.options_.is_callback_              = false;
  task.options_.is_seq_callback_          = false;
  task.options_.task_priority_            = 0;

  auto promise = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
  auto future  = promise->get_future();
  task.result_ = promise;

  scheduler.ScheduleTask(std::move(task));
  EXPECT_EQ(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  auto result = future.get();

  // The promise may be fulfilled before the worker finishes its post-render
  // baseline reset. Taking the render lock forces that transition to complete.
  auto& render_lock = pipeline->GetRenderLock();
  std::lock_guard<std::mutex> lock(render_lock);
  return result;
}

TEST(CudaPreviewVramReclamationTest, ExecutorScratchReleaseDropsHighWaterMark) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  RegisterAllOperators();

  auto pipeline    = MakeConfiguredExecutor();
  auto large_input = std::make_shared<ImageBuffer>(MakeLinearInput(2048, 1536));
  auto small_input = std::make_shared<ImageBuffer>(MakeLinearInput(2048, 1536));

  pipeline->SetRenderRes(true);
  auto large_result = pipeline->Apply(large_input);
  ASSERT_NE(large_result, nullptr);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), ExpectedScratchBytes(2048, 1536));

  pipeline->SetRenderRes(false, 640);
  auto small_result_without_release = pipeline->Apply(small_input);
  ASSERT_NE(small_result_without_release, nullptr);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), ExpectedScratchBytes(2048, 1536));

  pipeline->ReleasePreviewGpuScratch();
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), 0u);

  auto small_result = pipeline->Apply(small_input);
  ASSERT_NE(small_result, nullptr);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), ExpectedScratchBytes(640, 480));
}

TEST(CudaPreviewVramReclamationTest, FullResPreviewTransitionReleasesScratchBeforeFastPreview) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  RegisterAllOperators();

  auto pipeline = MakeConfiguredExecutor();
  auto input    = std::make_shared<ImageBuffer>(MakeLinearInput(3072, 2048));
  PipelineScheduler scheduler(1);

  auto full_res_result = SubmitBlockingRender(scheduler, pipeline, input, RenderType::FULL_RES_PREVIEW);
  ASSERT_NE(full_res_result, nullptr);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), 0u);

  auto fast_result = SubmitBlockingRender(scheduler, pipeline, input, RenderType::FAST_PREVIEW);
  ASSERT_NE(fast_result, nullptr);
  ASSERT_TRUE(fast_result->gpu_data_valid_);
  const auto expected_size = ExpectedFastPreviewSize(3072, 2048);
  EXPECT_EQ(fast_result->GetGPUWidth(), expected_size.width);
  EXPECT_EQ(fast_result->GetGPUHeight(), expected_size.height);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(),
            ExpectedScratchBytes(expected_size.width, expected_size.height));
}

TEST(CudaPreviewVramReclamationTest, QualityBasePreviewTransitionReleasesScratchBeforeFastPreview) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  RegisterAllOperators();

  auto pipeline = MakeConfiguredExecutor();
  auto input    = std::make_shared<ImageBuffer>(MakeLinearInput(3072, 2048));
  PipelineScheduler scheduler(1);

  auto quality_base_result =
      SubmitBlockingRender(scheduler, pipeline, input, RenderType::QUALITY_BASE_PREVIEW);
  ASSERT_NE(quality_base_result, nullptr);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), 0u);

  auto fast_result = SubmitBlockingRender(scheduler, pipeline, input, RenderType::FAST_PREVIEW);
  ASSERT_NE(fast_result, nullptr);
  ASSERT_TRUE(fast_result->gpu_data_valid_);
  const auto expected_size = ExpectedFastPreviewSize(3072, 2048);
  EXPECT_EQ(fast_result->GetGPUWidth(), expected_size.width);
  EXPECT_EQ(fast_result->GetGPUHeight(), expected_size.height);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(),
            ExpectedScratchBytes(expected_size.width, expected_size.height));
}

TEST(CudaPreviewVramReclamationTest, FullResExportTransitionReleasesScratchBeforeFastPreview) {
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }

  RegisterAllOperators();

  auto pipeline = MakeConfiguredExecutor();
  auto input    = std::make_shared<ImageBuffer>(MakeLinearInput(3072, 2048));
  PipelineScheduler scheduler(1);

  auto export_result = SubmitBlockingRender(scheduler, pipeline, input, RenderType::FULL_RES_EXPORT);
  ASSERT_NE(export_result, nullptr);
  ASSERT_TRUE(export_result->cpu_data_valid_);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(), 0u);

  auto fast_result = SubmitBlockingRender(scheduler, pipeline, input, RenderType::FAST_PREVIEW);
  ASSERT_NE(fast_result, nullptr);
  ASSERT_TRUE(fast_result->gpu_data_valid_);
  const auto expected_size = ExpectedFastPreviewSize(3072, 2048);
  EXPECT_EQ(fast_result->GetGPUWidth(), expected_size.width);
  EXPECT_EQ(fast_result->GetGPUHeight(), expected_size.height);
  EXPECT_EQ(pipeline->DebugGetMergedStageScratchBytes(),
            ExpectedScratchBytes(expected_size.width, expected_size.height));
}

}  // namespace
}  // namespace alcedo
