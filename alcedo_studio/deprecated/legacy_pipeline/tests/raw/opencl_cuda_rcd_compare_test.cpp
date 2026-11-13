//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <libraw/libraw.h>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "decoders/processor/operators/gpu/cuda_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/cuda_white_balance.hpp"
#include "decoders/processor/operators/gpu/opencl_debayer_rcd.hpp"
#include "decoders/processor/operators/gpu/opencl_to_linear_ref.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "image/image_buffer.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

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

auto EnsureOpenClContext() -> bool {
  return OpenClContext::Instance().TryInitialize();
}

struct DiffStats {
  float max_diff = 0.0f;
  int   x        = 0;
  int   y        = 0;
  int   channel  = 0;
  float a_value  = 0.0f;
  float b_value  = 0.0f;
};

auto ComputeMaxAbsDiff(const cv::Mat& a, const cv::Mat& b) -> DiffStats {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());

  DiffStats stats;
  if (a.type() == CV_32FC4) {
    for (int y = 0; y < a.rows; ++y) {
      const float* row_a = a.ptr<float>(y);
      const float* row_b = b.ptr<float>(y);
      for (int x = 0; x < a.cols; ++x) {
        for (int c = 0; c < 4; ++c) {
          const int   index = x * 4 + c;
          const float diff  = std::abs(row_a[index] - row_b[index]);
          if (diff > stats.max_diff) {
            stats.max_diff = diff;
            stats.x        = x;
            stats.y        = y;
            stats.channel  = c;
            stats.a_value  = row_a[index];
            stats.b_value  = row_b[index];
          }
        }
      }
    }
  } else if (a.type() == CV_32FC3) {
    for (int y = 0; y < a.rows; ++y) {
      const float* row_a = a.ptr<float>(y);
      const float* row_b = b.ptr<float>(y);
      for (int x = 0; x < a.cols; ++x) {
        for (int c = 0; c < 3; ++c) {
          const int   index = x * 3 + c;
          const float diff  = std::abs(row_a[index] - row_b[index]);
          if (diff > stats.max_diff) {
            stats.max_diff = diff;
            stats.x        = x;
            stats.y        = y;
            stats.channel  = c;
            stats.a_value  = row_a[index];
            stats.b_value  = row_b[index];
          }
        }
      }
    }
  } else {
    throw std::runtime_error("Unsupported type for ComputeMaxAbsDiff");
  }
  return stats;
}

auto DescribeBayerPatternShort(const BayerPattern2x2& pattern) -> std::string {
  std::string desc(4, '?');
  for (int i = 0; i < 4; ++i) {
    switch (pattern.rgb_fc[i]) {
      case 0: desc[i] = 'R'; break;
      case 1: desc[i] = 'G'; break;
      case 2: desc[i] = 'B'; break;
      default: desc[i] = '?'; break;
    }
  }
  return desc;
}

void RunRcdComparison(const std::filesystem::path& test_img, const char* image_name) {
  ASSERT_TRUE(std::filesystem::exists(test_img))
      << "Test image not found: " << test_img.string();

  auto raw_processor = std::make_unique<LibRaw>();
  ASSERT_EQ(raw_processor->open_file(test_img.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw_processor->unpack(), LIBRAW_SUCCESS);

  const libraw_rawdata_t& rawdata = raw_processor->imgdata.rawdata;
  const int               raw_w   = static_cast<int>(rawdata.sizes.raw_width);
  const int               raw_h   = static_cast<int>(rawdata.sizes.raw_height);

  cv::Mat raw_view(raw_h, raw_w, CV_16UC1, rawdata.raw_image);
  cv::Mat raw_cpu = raw_view.clone();

  const RawCfaPattern cfa_pattern = ReadLibRawCfaPattern(*raw_processor);

  std::cout << "[" << image_name << "] dimensions=" << raw_w << "x" << raw_h;
  if (cfa_pattern.kind == RawCfaKind::Bayer2x2) {
    std::cout << " bayer=" << DescribeBayerPatternShort(cfa_pattern.bayer_pattern);
  } else {
    std::cout << " bayer=X-Trans";
  }
  std::cout << "\n";

  // ------------------------------------------------------------------------
  // CUDA path: to_linear_ref + Bayer2x2ToRGB_RCD
  // ------------------------------------------------------------------------
  cv::Mat cuda_result;
  cv::Mat cuda_after_linear;
  double  cuda_ms = 0.0;
  {
    cv::cuda::GpuMat gpu_img(raw_cpu);
    cv::cuda::Stream stream;

    CUDA::ToLinearRef(gpu_img, *raw_processor, cfa_pattern, &stream);
    stream.waitForCompletion();
    gpu_img.download(cuda_after_linear);

    const auto start = std::chrono::steady_clock::now();
    CUDA::Bayer2x2ToRGB_RCD(gpu_img, cfa_pattern.bayer_pattern, nullptr, &stream);
    stream.waitForCompletion();
    const auto end = std::chrono::steady_clock::now();
    cuda_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    // CUDA outputs RGB (CV_32FC3); convert to RGBA for fair comparison.
    cv::Mat cuda_rgb;
    gpu_img.download(cuda_rgb);
    cv::cvtColor(cuda_rgb, cuda_result, cv::COLOR_RGB2RGBA);
    ASSERT_EQ(cuda_result.type(), CV_32FC4);
  }

  // ------------------------------------------------------------------------
  // OpenCL path: to_linear_ref + Bayer2x2ToRGB_RCD
  // ------------------------------------------------------------------------
  cv::Mat opencl_result;
  cv::Mat opencl_after_linear;
  double  opencl_ms = 0.0;
  {
    opencl::OpenClImage cl_img;
    cl_img.Upload(raw_cpu);

    OpenCL::ToLinearRef(cl_img, *raw_processor, cfa_pattern);
    cl_img.Download(opencl_after_linear);

    const auto start = std::chrono::steady_clock::now();
    OpenCL::Bayer2x2ToRGB_RCD(cl_img, cfa_pattern.bayer_pattern);
    const auto end = std::chrono::steady_clock::now();
    opencl_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    cl_img.Download(opencl_result);
    ASSERT_EQ(opencl_result.type(), CV_32FC4);
  }

  // ------------------------------------------------------------------------
  // Compare
  // ------------------------------------------------------------------------
  // Both CUDA and OpenCL RCD crop the 4-pixel invalid border band.
  const int overlap_w = std::min(cuda_result.cols, opencl_result.cols);
  const int overlap_h = std::min(cuda_result.rows, opencl_result.rows);

  ASSERT_GT(overlap_w, 0) << "No overlapping region between CUDA and OpenCL outputs.";
  ASSERT_GT(overlap_h, 0) << "No overlapping region between CUDA and OpenCL outputs.";

  const cv::Rect cuda_roi(0, 0, overlap_w, overlap_h);
  const cv::Rect opencl_roi(0, 0, overlap_w, overlap_h);

  const DiffStats diff_stats = ComputeMaxAbsDiff(cuda_result(cuda_roi), opencl_result(opencl_roi));

  // Compare ToLinearRef intermediate outputs
  float linear_max_diff = 0.0f;
  if (!cuda_after_linear.empty() && !opencl_after_linear.empty() &&
      cuda_after_linear.type() == opencl_after_linear.type() &&
      cuda_after_linear.size() == opencl_after_linear.size()) {
    cv::Mat diff;
    cv::absdiff(cuda_after_linear, opencl_after_linear, diff);
    cv::Mat flat = diff.reshape(1, static_cast<int>(diff.total() * diff.channels()));
    double min_val, max_val;
    cv::minMaxLoc(flat, &min_val, &max_val);
    linear_max_diff = static_cast<float>(max_val);
  }
  std::cout << "[" << image_name << " LinearRef] max_diff=" << linear_max_diff << "\n";

  std::cout << "[" << image_name
            << " Compare] CUDA: " << cuda_ms << " ms (" << cuda_result.cols << "x"
            << cuda_result.rows << ") | OpenCL: " << opencl_ms << " ms (" << opencl_result.cols
            << "x" << opencl_result.rows
            << ") | overlap_max_abs_diff: " << diff_stats.max_diff
            << " at overlap=(" << diff_stats.x << "," << diff_stats.y << ")"
            << " raw=(" << (diff_stats.x + 4) << "," << (diff_stats.y + 4) << ")"
            << " channel=" << diff_stats.channel << " cuda=" << diff_stats.a_value
            << " opencl=" << diff_stats.b_value
            << " fc="
            << static_cast<int>(cfa_pattern.bayer_pattern.rgb_fc[(((diff_stats.y + 4) & 1) << 1) |
                                                                 ((diff_stats.x + 4) & 1)])
            << "\n";
  // RCD's direction-selection branches can diverge at near ties across GPU compilers.
  // This verifies baseline parity while tolerating isolated branch flips.
  EXPECT_LT(diff_stats.max_diff, 8.0e-2f)
      << "CUDA and OpenCL RCD outputs differ beyond tolerance for " << image_name << ".";

  raw_processor->recycle();
}

}  // namespace

TEST(OpenClCudaCompare, RcdProducesConsistentResults) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#endif
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#endif

  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }
  if (!EnsureOpenClContext()) {
    GTEST_SKIP() << "OpenCL context initialization failed: "
                 << OpenClContext::Instance().LastInitializationError();
  }

  PrepareOpenClRuntime();
  (void)OpenClProgramLibrary::Instance().GetProgram("raw_processor_core");
  (void)OpenClProgramLibrary::Instance().GetProgram("raw_processor_debayer_rcd");

  const std::filesystem::path canon_r5 =
      std::string(TEST_IMG_PATH) + "/raw/camera/canon/r5/Canon-eos-r5-raw-00016.cr3";

  const std::filesystem::path leica_sl2 =
      std::string(TEST_IMG_PATH) + "/raw/camera/leica/sl2/L1010172.DNG";

  RunRcdComparison(canon_r5, "Canon-R5");
  RunRcdComparison(leica_sl2, "Leica-SL2");
}

}  // namespace alcedo
