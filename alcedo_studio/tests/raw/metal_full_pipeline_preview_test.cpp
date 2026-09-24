//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;

auto MetalAvailable() -> bool {
#ifdef HAVE_METAL
  auto* device = MTL::CreateSystemDefaultDevice();
  if (device == nullptr) {
    return false;
  }
  device->release();
  return true;
#else
  return false;
#endif
}

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }

  file.seekg(0, std::ios::end);
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size <= 0) {
    return {};
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return {};
  }
  return buffer;
}

auto CiRawFixturePath() -> std::filesystem::path {
  const auto root = std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles";
  if (!std::filesystem::exists(root)) {
    return {};
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_regular_file()) {
      const auto ext = entry.path().extension().string();
      if (ext == ".ARW" || ext == ".arw" || ext == ".DNG" || ext == ".dng" || ext == ".NEF" ||
          ext == ".nef") {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths.empty() ? std::filesystem::path{} : paths.front();
}

auto RenderBlocking(RenderType render_type, std::vector<uint8_t> raw_bytes)
    -> std::shared_ptr<ImageBuffer> {
  auto         pipeline = std::make_shared<PipelineExecutor>();

  PipelineTask task;
  task.pipeline_executor_                 = pipeline;
  task.input_                             = std::make_shared<ImageBuffer>(std::move(raw_bytes));
  task.options_.render_desc_.render_type_ = render_type;
  task.options_.is_blocking_              = true;
  task.options_.is_callback_              = false;
  task.options_.is_seq_callback_          = false;
  task.options_.task_priority_            = 0;

  auto promise = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
  auto future  = promise->get_future();
  task.result_ = promise;

  PipelineScheduler scheduler(1);
  scheduler.ScheduleTask(std::move(task));

  EXPECT_EQ(future.wait_for(60s), std::future_status::ready);
  return future.get();
}

}  // namespace

TEST(MetalFullPipelinePreview, DecodeGeometryAndMergedStageStillLife) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  const auto raw_path = CiRawFixturePath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();

  auto raw_bytes = ReadFileToBuffer(raw_path);
  ASSERT_FALSE(raw_bytes.empty());

  RegisterAllOperators();

  PipelineExecutor pipeline;
  pipeline.SetForceCPUOutput(true);

  auto input  = std::make_shared<ImageBuffer>(std::move(raw_bytes));
  auto output = pipeline.Apply(input);

  ASSERT_NE(output, nullptr);
  if (!output->cpu_data_valid_) {
    ASSERT_NO_THROW(output->SyncToCPU());
  }

  const cv::Mat& full_cpu = output->GetCPUData();
  ASSERT_FALSE(full_cpu.empty());
  ASSERT_EQ(full_cpu.type(), CV_32FC4);
  ASSERT_EQ(full_cpu.channels(), 4);
#endif
}

TEST(MetalFullPipelinePreview, FastPreviewSchedulerStillProducesImage) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  const auto raw_path = CiRawFixturePath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();

  RegisterAllOperators();

  auto output = RenderBlocking(RenderType::FAST_PREVIEW, ReadFileToBuffer(raw_path));
  ASSERT_NE(output, nullptr);

  if (!output->cpu_data_valid_) {
    ASSERT_NO_THROW(output->SyncToCPU());
  }

  const cv::Mat& cpu = output->GetCPUData();
  ASSERT_FALSE(cpu.empty());
  EXPECT_EQ(cpu.type(), CV_32FC4);
  EXPECT_LE(std::max(cpu.cols, cpu.rows), 2560);
#endif
}

TEST(MetalFullPipelinePreview, ThumbnailSchedulerStillProducesImage) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  if (!MetalAvailable()) {
    GTEST_SKIP() << "Metal device is unavailable in this environment.";
  }
  const auto raw_path = CiRawFixturePath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles";
  }
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();

  RegisterAllOperators();

  auto output = RenderBlocking(RenderType::THUMBNAIL, ReadFileToBuffer(raw_path));
  ASSERT_NE(output, nullptr);
  ASSERT_TRUE(output->cpu_data_valid_);

  const cv::Mat& cpu = output->GetCPUData();
  ASSERT_FALSE(cpu.empty());
  EXPECT_EQ(cpu.type(), CV_32FC4);
  EXPECT_LE(std::max(cpu.cols, cpu.rows), 1024);
#endif
}

}  // namespace alcedo
