//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

using ProfileClock                = std::chrono::steady_clock;

constexpr int kWarmupRuns         = 1;
constexpr int kMeasuredRuns       = 5;
constexpr int kFastPreviewMaxEdge = 2560;

struct MetalSegmentStats {
  double input_prepare_ms = 0.0;
  double fused_encode_ms  = 0.0;
  double hs_encode_ms     = 0.0;
  double hs_source_ms     = 0.0;
  double hs_remap_ms      = 0.0;
  double hs_select_ms     = 0.0;
  double hs_collapse_ms   = 0.0;
  double hs_apply_ms      = 0.0;
  double neighbor_ms      = 0.0;
  double gpu_wait_ms      = 0.0;
  double download_ms      = 0.0;
  double submit_ms        = 0.0;
  double output_ms        = 0.0;
};

struct RunResult {
  double            total_ms = 0.0;
  int               width    = 0;
  int               height   = 0;
  MetalSegmentStats segments;
};

class CoutCapture {
 public:
  CoutCapture() : old_(std::cout.rdbuf(buffer_.rdbuf())) {}
  ~CoutCapture() { std::cout.rdbuf(old_); }

  auto Text() const -> std::string { return buffer_.str(); }

 private:
  std::ostringstream buffer_;
  std::streambuf*    old_ = nullptr;
};

auto ElapsedMs(ProfileClock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Unable to open RAW profile input: " + path.string());
  }
  stream.seekg(0, std::ios::end);
  const auto size = stream.tellg();
  stream.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buffer(static_cast<size_t>(size));
  if (!buffer.empty()) {
    stream.read(reinterpret_cast<char*>(buffer.data()), size);
  }
  return buffer;
}

auto SplitRawList(const std::string& value) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;
  size_t                             start = 0;
  while (start <= value.size()) {
    const size_t end = value.find(':', start);
    const auto   token =
        value.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!token.empty()) {
      paths.emplace_back(token);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return paths;
}

auto ProfileRawInputs() -> std::vector<std::filesystem::path> {
  if (const char* env = std::getenv("ALCEDO_METAL_PROFILE_RAWS");
      env != nullptr && std::string(env).size() > 0) {
    return SplitRawList(env);
  }

  const std::vector<std::filesystem::path> defaults = {
      "/Users/zidage/Photos/_DSC2043.ARW",
      "/Users/zidage/Photos/_DSC0689.ARW",
      "/Users/zidage/Photos/香港2026-5-12-晚/P2625440.RW2",
      "/Users/zidage/Photos/香港/P2625336.RW2",
      "/Users/zidage/Photos/DSC_1811.NEF",
  };

  std::vector<std::filesystem::path> existing;
  for (const auto& path : defaults) {
    if (std::filesystem::exists(path)) {
      existing.push_back(path);
    }
  }
  return existing;
}

auto Median(std::vector<double> values) -> double {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

auto Percentile90(std::vector<double> values) -> double {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t index =
      std::min(values.size() - 1, static_cast<size_t>(std::ceil(values.size() * 0.90)) - 1);
  return values[index];
}

auto ExtractMetric(const std::string& line, const std::string& key) -> double {
  const size_t key_pos = line.find(key);
  if (key_pos == std::string::npos) {
    return 0.0;
  }
  const size_t value_pos = key_pos + key.size();
  return std::strtod(line.c_str() + value_pos, nullptr);
}

auto ParseMetalSegments(const std::string& text) -> MetalSegmentStats {
  MetalSegmentStats stats;
  const size_t      pos = text.rfind("Metal preview:");
  if (pos == std::string::npos) {
    return stats;
  }

  const std::string line = text.substr(pos);
  stats.input_prepare_ms = ExtractMetric(line, "in:");
  stats.fused_encode_ms  = ExtractMetric(line, "fe:");
  stats.hs_encode_ms     = ExtractMetric(line, "lt:");
  stats.hs_source_ms     = ExtractMetric(line, "hs_src:");
  stats.hs_remap_ms      = ExtractMetric(line, "hs_remap:");
  stats.hs_select_ms     = ExtractMetric(line, "hs_sel:");
  stats.hs_collapse_ms   = ExtractMetric(line, "hs_col:");
  stats.hs_apply_ms      = ExtractMetric(line, "hs_app:");
  stats.neighbor_ms      = ExtractMetric(line, "ne:");
  stats.gpu_wait_ms      = ExtractMetric(line, "gw:");
  stats.download_ms      = ExtractMetric(line, "hd:");
  stats.submit_ms        = ExtractMetric(line, "sub:");
  stats.output_ms        = ExtractMetric(line, "ow:");
  return stats;
}

auto RunFastPreviewOnce(const std::vector<std::uint8_t>& raw_bytes, int iteration, bool enable_hs)
    -> RunResult {
#ifdef _WIN32
  _putenv_s("ALCEDO_METAL_PROFILE_VERBOSE", "1");
#else
  setenv("ALCEDO_METAL_PROFILE_VERBOSE", "1", 1);
#endif

  PipelineExecutor pipeline(false);
  pipeline.SetAcceleratorBackendPreference(AcceleratorBackendPreference::Metal);
  pipeline.SetForceCPUOutput(true);
  pipeline.SetRenderRes(false, kFastPreviewMaxEdge);
  pipeline.SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Bilinear);

  if (enable_hs) {
    auto& basic = pipeline.GetStage(PipelineStageName::Basic_Adjustment);
    basic.SetOperator(OperatorType::SHADOWS, {{"shadows", 40.0f}}, pipeline.GetGlobalParams());
    basic.SetOperator(OperatorType::HIGHLIGHTS, {{"highlights", 35.0f}},
                      pipeline.GetGlobalParams());
  }
  pipeline.SetExecutionStages();

  auto        input = std::make_shared<ImageBuffer>(std::vector<std::uint8_t>(raw_bytes));

  CoutCapture capture;
  const auto  start  = ProfileClock::now();
  auto        output = pipeline.Apply(input);
  const auto  total  = ElapsedMs(start);

  if (output && !output->cpu_data_valid_) {
    output->SyncToCPU();
  }

  RunResult result;
  result.total_ms = total;
  result.segments = ParseMetalSegments(capture.Text());
  if (output && output->cpu_data_valid_) {
    const cv::Mat& cpu = output->GetCPUData();
    result.width       = cpu.cols;
    result.height      = cpu.rows;
  }

  if (enable_hs && iteration == 0 && result.segments.hs_encode_ms == 0.0) {
    std::cout << "[MetalFastPreviewProfile] warning: Metal preview reporter did not emit "
                 "segment stats for this run.\n";
  }
  return result;
}

auto MedianGpuWait(const std::vector<RunResult>& runs) -> double {
  std::vector<double> gpu_wait_ms;
  gpu_wait_ms.reserve(runs.size());
  for (const auto& run : runs) {
    gpu_wait_ms.push_back(run.segments.gpu_wait_ms);
  }
  return Median(std::move(gpu_wait_ms));
}

void PrintSummary(const std::filesystem::path& raw_path, const std::vector<RunResult>& runs,
                  const std::vector<RunResult>& baseline_runs) {
  std::vector<double> total_ms;
  std::vector<double> fused_ms;
  std::vector<double> hs_ms;
  std::vector<double> hs_source_ms;
  std::vector<double> hs_remap_ms;
  std::vector<double> hs_select_ms;
  std::vector<double> hs_collapse_ms;
  std::vector<double> hs_apply_ms;
  std::vector<double> gpu_wait_ms;
  for (const auto& run : runs) {
    total_ms.push_back(run.total_ms);
    fused_ms.push_back(run.segments.fused_encode_ms);
    hs_ms.push_back(run.segments.hs_encode_ms);
    hs_source_ms.push_back(run.segments.hs_source_ms);
    hs_remap_ms.push_back(run.segments.hs_remap_ms);
    hs_select_ms.push_back(run.segments.hs_select_ms);
    hs_collapse_ms.push_back(run.segments.hs_collapse_ms);
    hs_apply_ms.push_back(run.segments.hs_apply_ms);
    gpu_wait_ms.push_back(run.segments.gpu_wait_ms);
  }

  const double baseline_gpu_wait = MedianGpuWait(baseline_runs);
  const double hs_gpu_wait       = Median(gpu_wait_ms);
  const auto&  first             = runs.front();
  std::cout << std::left << std::setw(42) << raw_path.filename().string() << " out=" << first.width
            << "x" << first.height << " total median/p90=" << std::fixed << std::setprecision(2)
            << Median(total_ms) << "/" << Percentile90(total_ms) << " fused=" << Median(fused_ms)
            << " hs=" << Median(hs_ms) << " gpu_wait=" << hs_gpu_wait
            << " gpu_wait_base=" << baseline_gpu_wait
            << " gpu_wait_delta=" << (hs_gpu_wait - baseline_gpu_wait)
            << " src/remap/select/collapse/apply=" << Median(hs_source_ms) << "/"
            << Median(hs_remap_ms) << "/" << Median(hs_select_ms) << "/" << Median(hs_collapse_ms)
            << "/" << Median(hs_apply_ms) << " ms\n";
}

}  // namespace

TEST(MetalFastPreviewProfile, LocalPhotosFastPreview) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RegisterAllOperators();

  const auto raw_paths = ProfileRawInputs();
  if (raw_paths.empty()) {
    GTEST_SKIP() << "No RAW profile inputs found. Set ALCEDO_METAL_PROFILE_RAWS.";
  }

  std::cout << "\nMetal Fast Preview profile (" << kWarmupRuns << " warmup + " << kMeasuredRuns
            << " measured runs per RAW)\n";
  for (const auto& raw_path : raw_paths) {
    if (!std::filesystem::exists(raw_path)) {
      std::cout << "[MetalFastPreviewProfile] skip missing: " << raw_path.string() << "\n";
      continue;
    }

    const auto raw_bytes = ReadFileToBuffer(raw_path);
    ASSERT_FALSE(raw_bytes.empty()) << raw_path.string();

    std::vector<RunResult> baseline_measured;
    for (int i = 0; i < kWarmupRuns + kMeasuredRuns; ++i) {
      auto result = RunFastPreviewOnce(raw_bytes, i, false);
      ASSERT_GT(result.width, 0) << raw_path.string();
      ASSERT_GT(result.height, 0) << raw_path.string();
      if (i >= kWarmupRuns) {
        baseline_measured.push_back(result);
      }
    }
    ASSERT_FALSE(baseline_measured.empty());

    std::vector<RunResult> measured;
    for (int i = 0; i < kWarmupRuns + kMeasuredRuns; ++i) {
      auto result = RunFastPreviewOnce(raw_bytes, i, true);
      ASSERT_GT(result.width, 0) << raw_path.string();
      ASSERT_GT(result.height, 0) << raw_path.string();
      if (i >= kWarmupRuns) {
        measured.push_back(result);
      }
    }
    ASSERT_FALSE(measured.empty());
    PrintSummary(raw_path, measured, baseline_measured);
  }
#endif
}

}  // namespace alcedo
