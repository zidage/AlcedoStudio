//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

#include <libraw/libraw.h>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "decoders/processor/operators/gpu/cuda_white_balance.hpp"
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

auto ComputeMaxAbsDiff(const cv::Mat& a, const cv::Mat& b) -> float {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());
  CV_Assert(a.type() == CV_32FC1);

  float max_diff = 0.0f;
  for (int y = 0; y < a.rows; ++y) {
    const float* row_a = a.ptr<float>(y);
    const float* row_b = b.ptr<float>(y);
    for (int x = 0; x < a.cols; ++x) {
      max_diff = std::max(max_diff, std::abs(row_a[x] - row_b[x]));
    }
  }
  return max_diff;
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

void RunToLinearRefComparison(const std::filesystem::path& test_img,
                              const char*                  image_name) {
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
  // CUDA path
  // ------------------------------------------------------------------------
  cv::Mat cuda_result;
  double  cuda_ms = 0.0;
  {
    cv::cuda::GpuMat gpu_img(raw_cpu);
    const auto       start = std::chrono::steady_clock::now();
    CUDA::ToLinearRef(gpu_img, *raw_processor, cfa_pattern, nullptr);
    const auto end = std::chrono::steady_clock::now();
    cuda_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    gpu_img.download(cuda_result);
    ASSERT_EQ(cuda_result.type(), CV_32FC1);
  }

  // ------------------------------------------------------------------------
  // OpenCL path
  // ------------------------------------------------------------------------
  cv::Mat opencl_result;
  double  opencl_ms = 0.0;
  {
    opencl::OpenClImage cl_img;
    cl_img.Upload(raw_cpu);

    const auto start = std::chrono::steady_clock::now();
    OpenCL::ToLinearRef(cl_img, *raw_processor, cfa_pattern);
    const auto end = std::chrono::steady_clock::now();
    opencl_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();

    cl_img.Download(opencl_result);
    ASSERT_EQ(opencl_result.type(), CV_32FC1);
  }

  // ------------------------------------------------------------------------
  // Compare
  // ------------------------------------------------------------------------
  const float max_diff = ComputeMaxAbsDiff(cuda_result, opencl_result);

  std::cout << "[" << image_name << " Compare] CUDA: " << cuda_ms
            << " ms | OpenCL: " << opencl_ms << " ms | max_abs_diff: " << max_diff << "\n";

  EXPECT_LT(max_diff, 1.0e-4f)
      << "CUDA and OpenCL to_linear_ref outputs differ beyond tolerance for " << image_name
      << ".";

  raw_processor->recycle();
}

}  // namespace

TEST(OpenClCudaCompare, ToLinearRefProducesIdenticalResults) {
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

  const std::filesystem::path canon_r5 =
      std::string(TEST_IMG_PATH) + "/raw/camera/canon/r5/Canon-eos-r5-raw-00016.cr3";

  const std::filesystem::path leica_sl2 =
      std::string(TEST_IMG_PATH) + "/raw/camera/leica/sl2/L1010172.DNG";

  const std::filesystem::path fuji_xt5 =
      std::string(TEST_IMG_PATH) + "/raw/camera/fuji/xt5/DSCF2074.RAF";

  RunToLinearRefComparison(canon_r5, "Canon-R5");
  RunToLinearRefComparison(leica_sl2, "Leica-SL2");
  RunToLinearRefComparison(fuji_xt5, "Fuji-XT5");
}

}  // namespace alcedo
