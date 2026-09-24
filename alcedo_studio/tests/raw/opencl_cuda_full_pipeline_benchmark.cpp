//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image_buffer.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

using ProfileClock = std::chrono::steady_clock;

auto ElapsedMs(ProfileClock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Unable to open RAW fixture: " + path.string());
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

struct DiffStats {
  float  max_abs_diff  = 0.0f;
  double mean_abs_diff = 0.0;
  int    x             = 0;
  int    y             = 0;
  int    channel       = 0;
  float  cuda_value    = 0.0f;
  float  opencl_value  = 0.0f;
};

auto ComputeDiffStats(const cv::Mat& cuda, const cv::Mat& opencl) -> DiffStats {
  if (cuda.size() != opencl.size() || cuda.type() != opencl.type()) {
    return {};
  }
  DiffStats stats;
  const int ch     = cuda.channels();
  double    sum_abs = 0.0;
  size_t    count   = 0;
  for (int y = 0; y < cuda.rows; ++y) {
    const float* cuda_row   = cuda.ptr<float>(y);
    const float* opencl_row = opencl.ptr<float>(y);
    for (int x = 0; x < cuda.cols; ++x) {
      for (int c = 0; c < ch; ++c) {
        const int   idx  = x * ch + c;
        const float diff = std::abs(cuda_row[idx] - opencl_row[idx]);
        sum_abs += static_cast<double>(diff);
        ++count;
        if (diff > stats.max_abs_diff) {
          stats.max_abs_diff = diff;
          stats.x            = x;
          stats.y            = y;
          stats.channel      = c;
          stats.cuda_value   = cuda_row[idx];
          stats.opencl_value = opencl_row[idx];
        }
      }
    }
  }
  stats.mean_abs_diff = (count > 0) ? sum_abs / static_cast<double>(count) : 0.0;
  return stats;
}

struct PipelineBenchResult {
  double                total_ms       = 0.0;
  cv::Mat               output;
  int                   out_width      = 0;
  int                   out_height     = 0;
  std::string           profile_log;
  std::string           backend_label;
};

auto FindRawFixture() -> std::filesystem::path {
  const std::filesystem::path raw_dir =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "canon" / "r8";
  if (!std::filesystem::exists(raw_dir)) {
    return {};
  }
  for (const auto& entry : std::filesystem::directory_iterator(raw_dir)) {
    if (entry.is_regular_file()) {
      const auto ext = entry.path().extension().string();
      if (ext == ".CR3" || ext == ".cr3" || ext == ".CR2" || ext == ".cr2") {
        return entry.path();
      }
    }
  }
  return {};
}

struct BenchmarkScenario {
  const char*               name;
  int                       max_edge;
  bool                      full_res;
  float                     max_tolerance;
  double                    mean_tolerance;
};

/// Host-output request with the scenario's output limit (0 = full resolution).
auto MakeHostRequest(std::uint32_t max_edge) -> PipelineApplyRequest {
  PipelineApplyRequest request;
  request.geometry.resolution.max_edge = max_edge;
  request.geometry.resolution.quality  = RenderQuality::Export;
  request.require_host_output          = true;
  return request;
}

auto RunPipelineWithBackend(const std::filesystem::path& raw_path,
                            AcceleratorBackendPreference pref,
                            const BenchmarkScenario& scenario) -> PipelineBenchResult {

  PipelineExecutor pipeline;
  pipeline.SetAcceleratorBackendPreference(pref);
  const auto request =
      MakeHostRequest(scenario.full_res ? 0U : static_cast<std::uint32_t>(scenario.max_edge));

  auto input = std::make_shared<ImageBuffer>(ReadFileToBuffer(raw_path));

  const auto start = ProfileClock::now();
  auto       output = pipeline.Apply(input, request);
  const double total_ms = ElapsedMs(start);

  if (output && !output->cpu_data_valid_) {
    output->SyncToCPU();
  }

  PipelineBenchResult result;
  result.total_ms    = total_ms;
  result.backend_label = AcceleratorBackendPreferenceToString(pref);

  if (output && output->cpu_data_valid_) {
    const cv::Mat& cpu = output->GetCPUData();
    result.output       = cpu.clone();
    result.out_width    = cpu.cols;
    result.out_height   = cpu.rows;
  }

  return result;
}

void PrintBenchHeader() {
  std::cout << "\n"
            << std::string(100, '=') << "\n"
            << "  OpenCL vs CUDA Full Pipeline Benchmark\n"
            << std::string(100, '=') << "\n\n";
}

void PrintBenchResult(const BenchmarkScenario& scenario,
                      const PipelineBenchResult& cuda_result,
                      const PipelineBenchResult& opencl_result,
                      const DiffStats& diff) {
  const double speedup = (opencl_result.total_ms > 0.0)
                             ? (cuda_result.total_ms / opencl_result.total_ms)
                             : 0.0;

  std::cout << std::left << std::setw(38) << scenario.name
            << " | " << std::setw(5) << cuda_result.out_width << "x"
            << std::setw(5) << cuda_result.out_height
            << " | CUDA: " << std::fixed << std::setprecision(2) << std::setw(8)
            << cuda_result.total_ms << " ms"
            << " | OpenCL: " << std::setw(8) << opencl_result.total_ms << " ms"
            << " | ratio: " << std::setprecision(2) << std::setw(5) << speedup << "x"
            << " | max_diff: " << std::scientific << std::setprecision(2) << std::setw(8)
            << diff.max_abs_diff << " mean: " << std::setw(8) << diff.mean_abs_diff
            << " | @" << diff.x << "," << diff.y << " ch=" << diff.channel << "\n";
}

}  // namespace

TEST(OpenClCudaFullPipelineBenchmark, RawToDisplayEndToEnd) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#endif
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#endif

  const auto raw_path = FindRawFixture();
  if (raw_path.empty()) {
    GTEST_SKIP() << "No RAW fixture found under TEST_IMG_PATH/raw/camera/canon/r8/";
  }

  // Verify both backends are available
  if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
    GTEST_SKIP() << "CUDA device is unavailable.";
  }
  cv::cuda::setDevice(0);
  if (!TryPrepareOpenClRuntime() && !OpenClContext::Instance().IsInitialized()) {
    GTEST_SKIP() << "OpenCL runtime is unavailable: "
                 << OpenClContext::Instance().LastInitializationError();
  }

  PrintBenchHeader();
  std::cout << "  RAW fixture: " << raw_path.filename().string() << "\n\n";

  const std::vector<BenchmarkScenario> scenarios = {
      {"FastPreview (2560px)", 2560, false, 1.5e-1f, 4.0e-3},
      {"DetailPreview (4096px)", 4096, false, 1.5e-1f, 4.0e-3},
      {"FullRes", 0, true, 1.5e-1f, 4.0e-3},
  };

  // Header
  std::cout << std::left << std::setw(38) << "Scenario"
            << " | " << std::setw(11) << "Resolution"
            << " | " << std::setw(21) << "CUDA Time"
            << " | " << std::setw(21) << "OpenCL Time"
            << " | " << std::setw(8) << "Ratio"
            << " | Diff (max/mean)"
            << "\n"
            << std::string(160, '-') << "\n";

  for (const auto& scenario : scenarios) {
    std::cout << "Running: " << scenario.name << "..." << std::flush;

    const auto cuda_result   = RunPipelineWithBackend(raw_path, AcceleratorBackendPreference::CUDA,
                                                      scenario);
    const auto opencl_result = RunPipelineWithBackend(raw_path,
                                                       AcceleratorBackendPreference::OpenCL, scenario);

    if (cuda_result.output.empty() || opencl_result.output.empty()) {
      std::cout << " SKIP (no output)\n";
      continue;
    }

    if (cuda_result.out_width != opencl_result.out_width ||
        cuda_result.out_height != opencl_result.out_height) {
      std::cout << " SIZE MISMATCH (CUDA: " << cuda_result.out_width << "x"
                << cuda_result.out_height << " vs OpenCL: " << opencl_result.out_width << "x"
                << opencl_result.out_height << ")\n";
      continue;
    }

    const DiffStats diff = ComputeDiffStats(cuda_result.output, opencl_result.output);
    std::cout << " done.\n";
    PrintBenchResult(scenario, cuda_result, opencl_result, diff);

    EXPECT_LE(diff.max_abs_diff, scenario.max_tolerance)
        << scenario.name << ": max pixel difference exceeds tolerance";
    EXPECT_LE(diff.mean_abs_diff, scenario.mean_tolerance)
        << scenario.name << ": mean pixel difference exceeds tolerance";
  }

  std::cout << "\n" << std::string(100, '=') << "\n";
}

TEST(OpenClCudaFullPipelineBenchmark, RepeatedFrameTimingStability) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#endif
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#endif

  const auto raw_path = FindRawFixture();
  if (raw_path.empty()) {
    GTEST_SKIP() << "No RAW fixture found.";
  }
  if (cv::cuda::getCudaEnabledDeviceCount() <= 0) {
    GTEST_SKIP() << "CUDA device is unavailable.";
  }
  cv::cuda::setDevice(0);
  if (!TryPrepareOpenClRuntime() && !OpenClContext::Instance().IsInitialized()) {
    GTEST_SKIP() << "OpenCL runtime is unavailable.";
  }

  constexpr int                   kWarmupFrames = 3;
  constexpr int                   kTimedFrames  = 10;
  constexpr int                   kMaxEdge      = 2560;

  std::cout << "\n--- Repeated Frame Timing Stability ("
            << kTimedFrames << " frames, " << kMaxEdge << "px max edge) ---\n";

  struct TimingStats {
    double min_ms   = 1e9;
    double max_ms   = 0.0;
    double sum_ms   = 0.0;
    double avg_ms   = 0.0;
    int    frames   = 0;
  };

  auto measure_backend = [&](AcceleratorBackendPreference pref,
                             const char* label) -> TimingStats {
    PipelineExecutor pipeline;
    pipeline.SetAcceleratorBackendPreference(pref);
    const auto request = MakeHostRequest(static_cast<std::uint32_t>(kMaxEdge));

    // Warmup
    for (int i = 0; i < kWarmupFrames; ++i) {
      auto raw_bytes = ReadFileToBuffer(raw_path);
      auto input  = std::make_shared<ImageBuffer>(std::move(raw_bytes));
      auto output = pipeline.Apply(input, request);
      (void)output;
    }

    TimingStats stats;
    for (int i = 0; i < kTimedFrames; ++i) {
      auto raw_bytes = ReadFileToBuffer(raw_path);
      auto input = std::make_shared<ImageBuffer>(std::move(raw_bytes));
      const auto start = ProfileClock::now();
      auto       output = pipeline.Apply(input, request);
      const double ms   = ElapsedMs(start);
      (void)output;

      stats.min_ms = std::min(stats.min_ms, ms);
      stats.max_ms = std::max(stats.max_ms, ms);
      stats.sum_ms += ms;
      ++stats.frames;
    }
    stats.avg_ms = stats.sum_ms / static_cast<double>(stats.frames);

    std::cout << "  " << label << ": avg=" << std::fixed << std::setprecision(2)
              << stats.avg_ms << " ms | min=" << stats.min_ms << " ms | max="
              << stats.max_ms << " ms | frames=" << stats.frames << "\n";
    return stats;
  };

  const auto cuda_stats   = measure_backend(AcceleratorBackendPreference::CUDA, "CUDA  ");
  const auto opencl_stats = measure_backend(AcceleratorBackendPreference::OpenCL, "OpenCL");

  const double ratio = (opencl_stats.avg_ms > 0.0)
                           ? (cuda_stats.avg_ms / opencl_stats.avg_ms)
                           : 0.0;
  std::cout << "  Speed ratio (CUDA/OpenCL): " << std::fixed << std::setprecision(2)
            << ratio << "x\n";

  // Verify both produce reasonable frame times
  EXPECT_GT(cuda_stats.avg_ms, 0.0);
  EXPECT_GT(opencl_stats.avg_ms, 0.0);
  EXPECT_LT(cuda_stats.avg_ms, 60000.0) << "CUDA frame time unreasonably high";
  EXPECT_LT(opencl_stats.avg_ms, 120000.0) << "OpenCL frame time unreasonably high";
}

}  // namespace alcedo
