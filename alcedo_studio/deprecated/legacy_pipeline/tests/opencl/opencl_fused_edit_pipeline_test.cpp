//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include "edit/operators/cst/odt_op.hpp"
#include "edit/pipeline/pipeline_gpu_wrapper.hpp"
#include "edit/scope/detail/scope_opencl_shared.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "image/opencl_image.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

auto TryEnsureOpenClRuntime() -> bool {
  if (TryPrepareOpenClRuntime()) {
    return true;
  }
  return OpenClContext::Instance().IsInitialized();
}

auto MakeNoOpFusedParams() -> OperatorParams {
  OperatorParams params;
  params.to_ws_enabled_       = false;
  params.color_temp_enabled_  = false;
  params.exposure_enabled_    = false;
  params.contrast_enabled_    = false;
  params.white_enabled_       = false;
  params.black_enabled_       = false;
  params.highlights_enabled_  = false;
  params.shadows_enabled_     = false;
  params.curve_enabled_       = false;
  params.saturation_enabled_  = false;
  params.tint_enabled_        = false;
  params.vibrance_enabled_    = false;
  params.color_wheel_enabled_ = false;
  params.hls_enabled_         = false;
  params.lmt_enabled_         = false;
  params.to_output_enabled_   = false;
  params.clarity_enabled_     = false;
  params.sharpen_enabled_     = false;
  return params;
}

auto MakeAcesccTestImage() -> cv::Mat {
  cv::Mat image(2, 3, CV_32FC4);
  image.at<cv::Vec4f>(0, 0) = {0.34f, 0.38f, 0.42f, 1.0f};
  image.at<cv::Vec4f>(0, 1) = {0.48f, 0.50f, 0.53f, 1.0f};
  image.at<cv::Vec4f>(0, 2) = {0.58f, 0.62f, 0.66f, 1.0f};
  image.at<cv::Vec4f>(1, 0) = {0.28f, 0.32f, 0.36f, 1.0f};
  image.at<cv::Vec4f>(1, 1) = {0.44f, 0.47f, 0.51f, 1.0f};
  image.at<cv::Vec4f>(1, 2) = {0.70f, 0.72f, 0.74f, 1.0f};
  return image;
}

void SetGaussianWeights(OperatorParams& params, bool is_sharpen, float sigma, float /*radius*/) {
  constexpr int  kMaxTap         = OperatorParams::kDetailMaxGaussianTapCount;
  const float    safe_sigma      = std::max(sigma, 1.0e-4f);
  const uint32_t computed_radius = std::min<uint32_t>(
      static_cast<uint32_t>(std::ceil(3.0f * safe_sigma)), static_cast<uint32_t>(kMaxTap - 1));
  const uint32_t             tap_count = computed_radius + 1U;

  std::array<float, kMaxTap> weights{};
  const double               inv2sigma2  = 0.5 / (static_cast<double>(safe_sigma) * safe_sigma);
  double                     full_weight = 1.0;
  weights[0]                             = 1.0f;
  for (uint32_t tap = 1; tap <= computed_radius; ++tap) {
    const double w = std::exp(-(static_cast<double>(tap) * static_cast<double>(tap)) * inv2sigma2);
    weights[tap]   = static_cast<float>(w);
    full_weight += 2.0 * w;
  }
  if (full_weight > 0.0) {
    for (uint32_t tap = 0; tap <= computed_radius; ++tap) {
      weights[tap] = static_cast<float>(static_cast<double>(weights[tap]) / full_weight);
    }
  }

  if (is_sharpen) {
    params.sharpen_gaussian_tap_count_ = static_cast<int>(tap_count);
    for (int i = 0; i < static_cast<int>(tap_count); ++i) {
      params.sharpen_gaussian_weights_[i] = weights[i];
    }
  } else {
    params.clarity_gaussian_tap_count_ = static_cast<int>(tap_count);
    for (int i = 0; i < static_cast<int>(tap_count); ++i) {
      params.clarity_gaussian_weights_[i] = weights[i];
    }
  }
}

void SetHighlightShadowWeights(OperatorParams& params, float sigma) {
  constexpr int  kMaxTap         = OperatorParams::kDetailMaxGaussianTapCount;
  const float    safe_sigma      = std::max(sigma, 1.0e-4f);
  const uint32_t computed_radius = std::min<uint32_t>(
      static_cast<uint32_t>(std::ceil(3.0f * safe_sigma)), static_cast<uint32_t>(kMaxTap - 1));
  const uint32_t tap_count = computed_radius + 1U;

  std::array<float, kMaxTap> weights{};
  const double               inv2sigma2 = 0.5 / (static_cast<double>(safe_sigma) * safe_sigma);
  double                     full_weight = 1.0;
  weights[0] = 1.0f;
  for (uint32_t tap = 1; tap <= computed_radius; ++tap) {
    const double w = std::exp(-(static_cast<double>(tap) * static_cast<double>(tap)) * inv2sigma2);
    weights[tap] = static_cast<float>(w);
    full_weight += 2.0 * w;
  }
  if (full_weight > 0.0) {
    for (uint32_t tap = 0; tap <= computed_radius; ++tap) {
      weights[tap] = static_cast<float>(static_cast<double>(weights[tap]) / full_weight);
    }
  }

  params.hs_base_gaussian_tap_count_ = static_cast<int>(tap_count);
  for (int i = 0; i < static_cast<int>(tap_count); ++i) {
    params.hs_base_gaussian_weights_[i] = weights[i];
  }
}

auto RunOpenClFusedPipeline(GPUPipelineWrapper& pipeline, const cv::Mat& input,
                            OperatorParams& params) -> cv::Mat {
  auto input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto output_buffer = std::make_shared<ImageBuffer>();

  pipeline.SetInputImage(input_buffer);
  pipeline.SetParams(params);
  pipeline.Execute(output_buffer);

  output_buffer->SyncToCPU();
  return output_buffer->GetCPUData().clone();
}

auto RunOpenClFusedPipeline(const cv::Mat& input, OperatorParams params) -> cv::Mat {
  GPUPipelineWrapper pipeline(GpuBackendKind::OpenCL);
  return RunOpenClFusedPipeline(pipeline, input, params);
}

auto RunFusedPipeline(GpuBackendKind backend, const cv::Mat& input, OperatorParams params,
                      double* elapsed_ms = nullptr) -> cv::Mat {
  GPUPipelineWrapper pipeline(backend);
  auto               input_buffer  = std::make_shared<ImageBuffer>(input.clone());
  auto               output_buffer = std::make_shared<ImageBuffer>();

  pipeline.SetInputImage(input_buffer);
  pipeline.SetParams(params);
  const auto start = std::chrono::steady_clock::now();
  pipeline.Execute(output_buffer);
  output_buffer->SyncToCPU();
  const auto end = std::chrono::steady_clock::now();
  if (elapsed_ms != nullptr) {
    *elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  }
  return output_buffer->GetCPUData().clone();
}

auto MaxRgbDiff(const cv::Mat& a, const cv::Mat& b) -> float {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());
  float max_diff = 0.0f;
  for (int y = 0; y < a.rows; ++y) {
    for (int x = 0; x < a.cols; ++x) {
      const cv::Vec4f lhs = a.at<cv::Vec4f>(y, x);
      const cv::Vec4f rhs = b.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 3; ++c) {
        max_diff = std::max(max_diff, std::abs(lhs[c] - rhs[c]));
      }
    }
  }
  return max_diff;
}

struct RgbDiffStats {
  float max_diff = 0.0f;
  int   x        = 0;
  int   y        = 0;
  int   channel  = 0;
  float lhs      = 0.0f;
  float rhs      = 0.0f;
};

auto ComputeRgbDiffStats(const cv::Mat& a, const cv::Mat& b) -> RgbDiffStats {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());
  RgbDiffStats stats;
  for (int y = 0; y < a.rows; ++y) {
    for (int x = 0; x < a.cols; ++x) {
      const cv::Vec4f lhs = a.at<cv::Vec4f>(y, x);
      const cv::Vec4f rhs = b.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 3; ++c) {
        const float diff = std::abs(lhs[c] - rhs[c]);
        if (diff > stats.max_diff) {
          stats.max_diff = diff;
          stats.x        = x;
          stats.y        = y;
          stats.channel  = c;
          stats.lhs      = lhs[c];
          stats.rhs      = rhs[c];
        }
      }
    }
  }
  return stats;
}

auto MaxRgbDiffInsideBorder(const cv::Mat& a, const cv::Mat& b, int border) -> float {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());
  if (border <= 0 || a.cols <= 2 * border || a.rows <= 2 * border) {
    return MaxRgbDiff(a, b);
  }

  float max_diff = 0.0f;
  for (int y = border; y < a.rows - border; ++y) {
    for (int x = border; x < a.cols - border; ++x) {
      const cv::Vec4f lhs = a.at<cv::Vec4f>(y, x);
      const cv::Vec4f rhs = b.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 3; ++c) {
        max_diff = std::max(max_diff, std::abs(lhs[c] - rhs[c]));
      }
    }
  }
  return max_diff;
}

auto MaxRgbDiffOnBorder(const cv::Mat& a, const cv::Mat& b, int border) -> float {
  CV_Assert(a.size() == b.size());
  CV_Assert(a.type() == b.type());
  if (border <= 0 || a.cols <= 2 * border || a.rows <= 2 * border) {
    return MaxRgbDiff(a, b);
  }

  float max_diff = 0.0f;
  for (int y = 0; y < a.rows; ++y) {
    for (int x = 0; x < a.cols; ++x) {
      if (x >= border && x < a.cols - border && y >= border && y < a.rows - border) {
        continue;
      }
      const cv::Vec4f lhs = a.at<cv::Vec4f>(y, x);
      const cv::Vec4f rhs = b.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 3; ++c) {
        max_diff = std::max(max_diff, std::abs(lhs[c] - rhs[c]));
      }
    }
  }
  return max_diff;
}

void ExpectFiniteRgba32f(const cv::Mat& image);

void ExpectCudaOpenClDetailClose(const char* label, const cv::Mat& cuda_output,
                                 const cv::Mat& opencl_output, double cuda_ms, double opencl_ms) {
  ExpectFiniteRgba32f(cuda_output);
  ExpectFiniteRgba32f(opencl_output);

  constexpr int   kBorder       = 10;
  constexpr float kDiffTol      = 2.0e-4f;
  const auto      stats         = ComputeRgbDiffStats(cuda_output, opencl_output);
  const float     interior_diff = MaxRgbDiffInsideBorder(cuda_output, opencl_output, kBorder);
  const float     border_diff   = MaxRgbDiffOnBorder(cuda_output, opencl_output, kBorder);

  std::cout << label << " CUDA: " << cuda_ms << " ms | OpenCL: " << opencl_ms
            << " ms | max_abs_diff=" << stats.max_diff << " max_interior_diff=" << interior_diff
            << " max_border_diff=" << border_diff << " at (" << stats.x << "," << stats.y
            << ") channel=" << stats.channel << " cuda=" << stats.lhs << " opencl=" << stats.rhs
            << "\n";

  EXPECT_LE(interior_diff, kDiffTol)
      << label << " differs away from image borders, so this is not a border handling issue.";
  EXPECT_LE(stats.max_diff, kDiffTol) << label << " differs beyond tolerance including borders.";
  EXPECT_GT(cuda_ms, 0.0);
  EXPECT_GT(opencl_ms, 0.0);
}

auto MakeRgbOffsetImage(const cv::Mat& input, const cv::Vec3f& offset) -> cv::Mat {
  cv::Mat expected = input.clone();
  for (int y = 0; y < expected.rows; ++y) {
    for (int x = 0; x < expected.cols; ++x) {
      cv::Vec4f& px = expected.at<cv::Vec4f>(y, x);
      px[0] += offset[0];
      px[1] += offset[1];
      px[2] += offset[2];
    }
  }
  return expected;
}

void ExpectFiniteRgba32f(const cv::Mat& image) {
  ASSERT_EQ(image.type(), CV_32FC4);
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      const cv::Vec4f px = image.at<cv::Vec4f>(y, x);
      for (int c = 0; c < 4; ++c) {
        EXPECT_TRUE(std::isfinite(px[c])) << "at (" << x << ", " << y << ") channel " << c;
      }
    }
  }
}

auto MakeOutputTransformParams(std::string method) -> OperatorParams {
  auto   params = MakeNoOpFusedParams();
  ODT_Op odt({{"odt",
               {{"method", method},
                {"encoding_space", "rec709"},
                {"encoding_eotf", "gamma_2_2"},
                {"peak_luminance", 100.0f}}}});
  odt.SetGlobalParams(params);
  params.to_output_enabled_ = true;
  return params;
}

auto MakeDefaultOutputTransformParams() -> OperatorParams {
  return MakeOutputTransformParams("open_drt");
}

auto MakeHighlightShadowComparisonInput() -> cv::Mat {
  constexpr int kW = 96;
  constexpr int kH = 64;
  cv::Mat       input(kH, kW, CV_32FC4);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(kW - 1);
      const float fy = static_cast<float>(y) / static_cast<float>(kH - 1);
      const float soft_edge = 0.5f + 0.5f * std::tanh((fx - 0.47f) * 12.0f);
      const float texture =
          0.010f * std::sin(static_cast<float>(x) * 0.55f) *
          std::cos(static_cast<float>(y) * 0.37f);
      const float shadow_band = 0.25f + 0.10f * fy + texture;
      const float sky_band = 0.54f + 0.26f * (1.0f - fy) + 0.035f * fx + texture;
      const float code = shadow_band + (sky_band - shadow_band) * soft_edge;
      input.at<cv::Vec4f>(y, x) = {
          code + 0.010f * (1.0f - fy),
          code,
          code - 0.012f * fx,
          1.0f,
      };
    }
  }
  return input;
}

auto MakeDetailComparisonInput(int width = 128, int height = 64) -> cv::Mat {
  cv::Mat input(height, width, CV_32FC4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(width - 1);
      const float fy = static_cast<float>(y) / static_cast<float>(height - 1);
      const float edge = (x >= width / 2) ? 0.7f : 0.3f;
      const float spot = (std::abs(x - width * 2 / 3) < 8 && std::abs(y - height / 2) < 6)
                             ? 0.55f
                             : 0.0f;
      const float base = 0.2f + 0.3f * fx + 0.1f * edge + spot;
      input.at<cv::Vec4f>(y, x) = {base, base + 0.05f * fy,
                                   std::max(0.0f, base - 0.03f * (1.0f - fy)), 1.0f};
    }
  }
  return input;
}

auto MakeHighlightShadowComparisonParams() -> OperatorParams {
  auto params = MakeNoOpFusedParams();
  params.shadows_enabled_ = true;
  params.shadows_offset_ = 1.0f;
  params.highlights_enabled_ = true;
  params.highlights_offset_ = -1.0f;
  params.hs_local_tone_enabled_ = true;
  params.hs_base_radius_ = 18.0f;
  SetHighlightShadowWeights(params, params.hs_base_radius_);
  return params;
}

}  // namespace

TEST(OpenClFusedEditPipelineTest, AppliesPointOperatorsWhenOutputTransformDisabled) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  cv::Mat input(1, 2, CV_32FC4);
  input.at<cv::Vec4f>(0, 0) = {0.10f, 0.20f, 0.30f, 1.0f};
  input.at<cv::Vec4f>(0, 1) = {0.40f, 0.50f, 0.60f, 0.75f};

  auto params               = MakeNoOpFusedParams();
  params.exposure_enabled_  = true;
  params.exposure_offset_   = 0.25f;

  const cv::Mat output      = RunOpenClFusedPipeline(input, params);

  ASSERT_EQ(output.rows, input.rows);
  ASSERT_EQ(output.cols, input.cols);
  ExpectFiniteRgba32f(output);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 0)[0], 0.35f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 0)[1], 0.45f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 0)[2], 0.55f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 0)[3], 1.0f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 1)[0], 0.65f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 1)[1], 0.75f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 1)[2], 0.85f, 1.0e-6f);
  EXPECT_NEAR(output.at<cv::Vec4f>(0, 1)[3], 0.75f, 1.0e-6f);
}

TEST(OpenClFusedEditPipelineTest, OutputTransformAppliesWhenEnabled) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input                       = MakeAcesccTestImage();

  auto          output_disabled_params      = MakeNoOpFusedParams();
  output_disabled_params.to_output_enabled_ = false;
  const cv::Mat output_disabled             = RunOpenClFusedPipeline(input, output_disabled_params);

  const cv::Mat output_enabled = RunOpenClFusedPipeline(input, MakeDefaultOutputTransformParams());

  ExpectFiniteRgba32f(output_disabled);
  ExpectFiniteRgba32f(output_enabled);
  EXPECT_GT(MaxRgbDiff(output_enabled, output_disabled), 1.0e-4f)
      << "Enabled OpenCL CST/ToOutput should change ACEScc input into display output.";
}

#ifdef HAVE_CUDA
TEST(OpenClFusedEditPipelineTest, OutputTransformMatchesCudaForOpenDrtAndAces) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  cv::Mat input(64, 128, CV_32FC4);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      const float fx            = static_cast<float>(x) / static_cast<float>(input.cols - 1);
      const float fy            = static_cast<float>(y) / static_cast<float>(input.rows - 1);
      const float base          = 0.01f + 0.24f * fx;
      input.at<cv::Vec4f>(y, x) = {base, base + 0.012f * fy,
                                   fmax(0.0f, base - 0.008f * (1.0f - fy)), 1.0f};
    }
  }

  for (const std::string method : {"open_drt", "aces_2_0"}) {
    double  cuda_ms   = 0.0;
    double  opencl_ms = 0.0;
    cv::Mat cuda_output;
    cv::Mat opencl_output;
    try {
      auto cuda_params             = MakeOutputTransformParams(method);
      auto opencl_params           = MakeOutputTransformParams(method);
      cuda_params.to_ws_enabled_   = true;
      opencl_params.to_ws_enabled_ = true;
      cuda_output   = RunFusedPipeline(GpuBackendKind::CUDA, input, cuda_params, &cuda_ms);
      opencl_output = RunFusedPipeline(GpuBackendKind::OpenCL, input, opencl_params, &opencl_ms);
    } catch (const std::exception& e) {
      GTEST_SKIP() << "CUDA/OpenCL output transform comparison unavailable: " << e.what();
    }

    ExpectFiniteRgba32f(cuda_output);
    ExpectFiniteRgba32f(opencl_output);
    const RgbDiffStats stats = ComputeRgbDiffStats(cuda_output, opencl_output);
    const float        diff  = stats.max_diff;
    std::cout << "[OpenCL fused " << method << "] CUDA: " << cuda_ms
              << " ms | OpenCL: " << opencl_ms << " ms | max_abs_diff=" << diff << " at ("
              << stats.x << "," << stats.y << ") channel=" << stats.channel << " cuda=" << stats.lhs
              << " opencl=" << stats.rhs << "\n";

    EXPECT_LE(diff, method == "aces_2_0" ? 8.0e-3f : 5.0e-3f) << method;
    EXPECT_GT(cuda_ms, 0.0);
    EXPECT_GT(opencl_ms, 0.0);
    EXPECT_LT(opencl_ms, cuda_ms * 100.0 + 50.0) << method;
  }
}
#endif

TEST(OpenClFusedEditPipelineTest, LmtPassthroughWhenDisabled) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input  = MakeAcesccTestImage();

  auto          params = MakeNoOpFusedParams();
  params.lmt_enabled_  = false;

  const cv::Mat output = RunOpenClFusedPipeline(input, params);

  ExpectFiniteRgba32f(output);
  ASSERT_EQ(output.rows, input.rows);
  ASSERT_EQ(output.cols, input.cols);
  EXPECT_LE(MaxRgbDiff(output, input), 1.0e-6f)
      << "When LMT is disabled, output should match input for NoOp params.";
}

TEST(OpenClFusedEditPipelineTest, LmtIdentityLutPreservesValues) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  // Generate a minimal identity 3D LUT (edge=2).
  // Cube file data order: blue slowest, green middle, red fastest.
  const std::string cube_content = R"(LUT_3D_SIZE 2
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

  const auto        tmp_dir      = std::filesystem::temp_directory_path();
  const auto        lut_path     = tmp_dir / "opencl_test_identity_lut.cube";
  {
    std::ofstream ofs(lut_path, std::ios::binary);
    ofs << cube_content;
  }

  const cv::Mat input    = MakeAcesccTestImage();

  auto          params   = MakeNoOpFusedParams();
  params.lmt_enabled_    = true;
  params.lmt_lut_path_   = lut_path;

  const cv::Mat   output = RunOpenClFusedPipeline(input, params);

  std::error_code ec;
  std::filesystem::remove(lut_path, ec);

  ExpectFiniteRgba32f(output);

  // An identity 2-edge LUT preserves values in [0, 1] exactly (trilinear
  // interpolation of corner values gives back the input coordinate).
  // Slight fp differences from the lookup math are expected; we allow a
  // generous tolerance because the LUT resolution is extremely low (edge=2)
  // and ACEScc values near 0/1 fall into the clamped region.
  EXPECT_LE(MaxRgbDiff(output, input), 1.0e-5f);
}

TEST(OpenClFusedEditPipelineTest, LmtAppliesCubeLut) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  // This LUT encodes a simple affine offset: rgb -> rgb + (0.1, 0.2, 0.3).
  const std::string cube_content = R"(LUT_3D_SIZE 2
0.1 0.2 0.3
1.1 0.2 0.3
0.1 1.2 0.3
1.1 1.2 0.3
0.1 0.2 1.3
1.1 0.2 1.3
0.1 1.2 1.3
1.1 1.2 1.3
)";

  const auto        tmp_dir      = std::filesystem::temp_directory_path();
  const auto        lut_path     = tmp_dir / "opencl_test_offset_lut.cube";
  {
    std::ofstream ofs(lut_path, std::ios::binary);
    ofs << cube_content;
  }

  const cv::Mat input    = MakeAcesccTestImage();

  auto          params   = MakeNoOpFusedParams();
  params.lmt_enabled_    = true;
  params.lmt_lut_path_   = lut_path;

  const cv::Mat   output = RunOpenClFusedPipeline(input, params);

  std::error_code ec;
  std::filesystem::remove(lut_path, ec);

  ExpectFiniteRgba32f(output);
  const cv::Mat expected = MakeRgbOffsetImage(input, {0.1f, 0.2f, 0.3f});
  EXPECT_LE(MaxRgbDiff(output, expected), 1.0e-5f);
}

TEST(OpenClFusedEditPipelineTest, LmtReloadsWhenPathChangesWithoutDirtyFlag) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const std::string first_cube  = R"(LUT_3D_SIZE 2
0.1 0.0 0.0
1.1 0.0 0.0
0.1 1.0 0.0
1.1 1.0 0.0
0.1 0.0 1.0
1.1 0.0 1.0
0.1 1.0 1.0
1.1 1.0 1.0
)";
  const std::string second_cube = R"(LUT_3D_SIZE 2
0.2 0.0 0.0
1.2 0.0 0.0
0.2 1.0 0.0
1.2 1.0 0.0
0.2 0.0 1.0
1.2 0.0 1.0
0.2 1.0 1.0
1.2 1.0 1.0
)";

  const auto        tmp_dir     = std::filesystem::temp_directory_path();
  const auto        first_path  = tmp_dir / "opencl_test_first_lut.cube";
  const auto        second_path = tmp_dir / "opencl_test_second_lut.cube";
  {
    std::ofstream first(first_path, std::ios::binary);
    first << first_cube;
    std::ofstream second(second_path, std::ios::binary);
    second << second_cube;
  }

  const cv::Mat input  = MakeAcesccTestImage();
  auto          params = MakeNoOpFusedParams();
  params.lmt_enabled_  = true;
  params.lmt_lut_path_ = first_path;

  GPUPipelineWrapper pipeline(GpuBackendKind::OpenCL);
  (void)RunOpenClFusedPipeline(pipeline, input, params);

  params.lmt_lut_path_ = second_path;
  ASSERT_FALSE(params.to_lmt_dirty_);

  const cv::Mat   output = RunOpenClFusedPipeline(pipeline, input, params);

  std::error_code ec;
  std::filesystem::remove(first_path, ec);
  std::filesystem::remove(second_path, ec);

  ExpectFiniteRgba32f(output);
  const cv::Mat expected = MakeRgbOffsetImage(input, {0.2f, 0.0f, 0.0f});
  EXPECT_LE(MaxRgbDiff(output, expected), 1.0e-5f);
}

TEST(OpenClFusedEditPipelineTest, LmtEnabledWithEmptyPathThrows) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input  = MakeAcesccTestImage();

  auto          params = MakeNoOpFusedParams();
  params.lmt_enabled_  = true;
  params.lmt_lut_path_ = std::filesystem::path();  // empty path

  EXPECT_THROW(RunOpenClFusedPipeline(input, params), std::runtime_error);
}

TEST(OpenClFusedEditPipelineTest, DetailPassPassthroughWhenDisabled) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input     = MakeAcesccTestImage();

  auto          params    = MakeNoOpFusedParams();
  params.sharpen_enabled_ = false;
  params.clarity_enabled_ = false;

  const cv::Mat output    = RunOpenClFusedPipeline(input, params);

  ExpectFiniteRgba32f(output);
  ASSERT_EQ(output.rows, input.rows);
  ASSERT_EQ(output.cols, input.cols);
  EXPECT_LE(MaxRgbDiff(output, input), 1.0e-6f)
      << "When detail passes are disabled, output should match input for NoOp params.";
}

#ifdef HAVE_CUDA
TEST(OpenClFusedEditPipelineTest, DetailSharpenMatchesCuda) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input = MakeDetailComparisonInput();

  // Verify sharpen modifies the image (non-identity).
  auto noop_params           = MakeNoOpFusedParams();
  noop_params.to_ws_enabled_ = true;
  const cv::Mat noop_output  = RunOpenClFusedPipeline(input, noop_params);
  ExpectFiniteRgba32f(noop_output);

  auto sharpen_params               = MakeNoOpFusedParams();
  sharpen_params.to_ws_enabled_     = true;
  sharpen_params.sharpen_enabled_   = true;
  sharpen_params.sharpen_offset_    = 0.5f;
  sharpen_params.sharpen_radius_    = 3.0f;
  sharpen_params.sharpen_threshold_ = 0.0f;
  SetGaussianWeights(sharpen_params, true, sharpen_params.sharpen_radius_,
                     sharpen_params.sharpen_radius_);
  const cv::Mat sharpen_output = RunOpenClFusedPipeline(input, sharpen_params);
  ExpectFiniteRgba32f(sharpen_output);

  const float diff = MaxRgbDiff(sharpen_output, noop_output);
  std::cout << "[OpenCL sharpen standalone] max_diff_vs_noop=" << diff << "\n";
  EXPECT_GT(diff, 1.0e-4f) << "OpenCL sharpen with offset=0.5 should modify the image vs no-op.";

  double  cuda_ms   = 0.0;
  double  opencl_ms = 0.0;
  cv::Mat cuda_output;
  cv::Mat opencl_output;
  try {
    auto cuda_params   = sharpen_params;
    auto opencl_params = sharpen_params;
    cuda_output        = RunFusedPipeline(GpuBackendKind::CUDA, input, cuda_params, &cuda_ms);
    opencl_output      = RunFusedPipeline(GpuBackendKind::OpenCL, input, opencl_params, &opencl_ms);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA/OpenCL sharpen comparison unavailable: " << e.what();
  }

  ExpectCudaOpenClDetailClose("[OpenCL fused sharpen]", cuda_output, opencl_output, cuda_ms,
                              opencl_ms);
}

TEST(OpenClFusedEditPipelineTest, DetailClarityMatchesCuda) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input = MakeDetailComparisonInput();

  // Verify clarity modifies the image (non-identity).
  auto          noop_params       = MakeNoOpFusedParams();
  const cv::Mat noop_output       = RunOpenClFusedPipeline(input, noop_params);

  auto          clarity_params    = MakeNoOpFusedParams();
  clarity_params.clarity_enabled_ = true;
  clarity_params.clarity_offset_  = 0.3f;
  clarity_params.clarity_radius_  = 5.0f;
  clarity_params.tone_mapping_.clarity_enabled_ = true;
  clarity_params.tone_mapping_.clarity_amount_  = clarity_params.clarity_offset_;
  SetGaussianWeights(clarity_params, false, clarity_params.clarity_radius_,
                     clarity_params.clarity_radius_);
  const cv::Mat clarity_output = RunOpenClFusedPipeline(input, clarity_params);
  ExpectFiniteRgba32f(clarity_output);

  const float diff = MaxRgbDiff(clarity_output, noop_output);
  std::cout << "[OpenCL clarity standalone] max_diff_vs_noop=" << diff << "\n";
  EXPECT_GT(diff, 1.0e-4f) << "OpenCL clarity with offset=0.3 should modify the image vs no-op.";

  double  cuda_ms   = 0.0;
  double  opencl_ms = 0.0;
  cv::Mat cuda_output;
  cv::Mat opencl_output;
  try {
    auto cuda_params   = clarity_params;
    auto opencl_params = clarity_params;
    cuda_output        = RunFusedPipeline(GpuBackendKind::CUDA, input, cuda_params, &cuda_ms);
    opencl_output      = RunFusedPipeline(GpuBackendKind::OpenCL, input, opencl_params, &opencl_ms);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA/OpenCL clarity comparison unavailable: " << e.what();
  }

  ExpectCudaOpenClDetailClose("[OpenCL fused clarity]", cuda_output, opencl_output, cuda_ms,
                              opencl_ms);
}

TEST(OpenClFusedEditPipelineTest, DetailSharpenWithThresholdMatchesCuda) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input = MakeDetailComparisonInput();

  // Verify sharpen with threshold produces different result than sharpen without.
  auto sharpen_no_thresh               = MakeNoOpFusedParams();
  sharpen_no_thresh.sharpen_enabled_   = true;
  sharpen_no_thresh.sharpen_offset_    = 0.8f;
  sharpen_no_thresh.sharpen_radius_    = 2.0f;
  sharpen_no_thresh.sharpen_threshold_ = 0.0f;
  SetGaussianWeights(sharpen_no_thresh, true, sharpen_no_thresh.sharpen_radius_,
                     sharpen_no_thresh.sharpen_radius_);
  const cv::Mat output_no_thresh = RunOpenClFusedPipeline(input, sharpen_no_thresh);
  ExpectFiniteRgba32f(output_no_thresh);

  auto sharpen_with_thresh               = MakeNoOpFusedParams();
  sharpen_with_thresh.sharpen_enabled_   = true;
  sharpen_with_thresh.sharpen_offset_    = 0.8f;
  sharpen_with_thresh.sharpen_radius_    = 2.0f;
  sharpen_with_thresh.sharpen_threshold_ = 0.05f;
  SetGaussianWeights(sharpen_with_thresh, true, sharpen_with_thresh.sharpen_radius_,
                     sharpen_with_thresh.sharpen_radius_);
  const cv::Mat output_with_thresh = RunOpenClFusedPipeline(input, sharpen_with_thresh);
  ExpectFiniteRgba32f(output_with_thresh);

  const float diff_thresh = MaxRgbDiff(output_with_thresh, output_no_thresh);
  std::cout << "[OpenCL sharpen threshold standalone] max_diff_with_vs_without_threshold="
            << diff_thresh << "\n";
  EXPECT_GT(diff_thresh, 1.0e-6f) << "Sharpen with threshold=0.05 should differ from threshold=0.";

  double  cuda_ms   = 0.0;
  double  opencl_ms = 0.0;
  cv::Mat cuda_output;
  cv::Mat opencl_output;
  try {
    auto cuda_params   = sharpen_with_thresh;
    auto opencl_params = sharpen_with_thresh;
    cuda_output        = RunFusedPipeline(GpuBackendKind::CUDA, input, cuda_params, &cuda_ms);
    opencl_output      = RunFusedPipeline(GpuBackendKind::OpenCL, input, opencl_params, &opencl_ms);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA/OpenCL sharpen threshold comparison unavailable: " << e.what();
  }

  ExpectCudaOpenClDetailClose("[OpenCL fused sharpen+threshold]", cuda_output, opencl_output,
                              cuda_ms, opencl_ms);
}

TEST(OpenClFusedEditPipelineTest, DetailHalationMatchesCuda) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input = MakeDetailComparisonInput();

  auto noop_params = MakeNoOpFusedParams();
  const cv::Mat noop_output = RunOpenClFusedPipeline(input, noop_params);

  auto halation_params = MakeNoOpFusedParams();
  halation_params.halation_.enabled_ = true;
  halation_params.halation_.strength_ = 0.75f;
  halation_params.halation_.sigma_ = 5.0f;
  halation_params.halation_.redshift_[0] = 1.0f;
  halation_params.halation_.redshift_[1] = 0.05f;
  halation_params.halation_.redshift_[2] = 0.02f;

  const cv::Mat halation_output = RunOpenClFusedPipeline(input, halation_params);
  ExpectFiniteRgba32f(halation_output);

  const float diff = MaxRgbDiff(halation_output, noop_output);
  std::cout << "[OpenCL halation standalone] max_diff_vs_noop=" << diff << "\n";
  EXPECT_GT(diff, 1.0e-4f) << "OpenCL halation should modify the image vs no-op.";

  double  cuda_ms   = 0.0;
  double  opencl_ms = 0.0;
  cv::Mat cuda_output;
  cv::Mat opencl_output;
  try {
    auto cuda_params   = halation_params;
    auto opencl_params = halation_params;
    cuda_output        = RunFusedPipeline(GpuBackendKind::CUDA, input, cuda_params, &cuda_ms);
    opencl_output      = RunFusedPipeline(GpuBackendKind::OpenCL, input, opencl_params, &opencl_ms);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA/OpenCL halation comparison unavailable: " << e.what();
  }

  ExpectCudaOpenClDetailClose("[OpenCL fused halation]", cuda_output, opencl_output, cuda_ms,
                              opencl_ms);
}

TEST(OpenClFusedEditPipelineTest, DetailFilmGrainMatchesCuda) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input = MakeDetailComparisonInput();

  auto noop_params = MakeNoOpFusedParams();
  const cv::Mat noop_output = RunOpenClFusedPipeline(input, noop_params);

  auto grain_params = MakeNoOpFusedParams();
  grain_params.film_grain_.enabled_ = true;
  grain_params.film_grain_.strength_ = 0.35f;
  grain_params.film_grain_.seed_ = 0x123456789abcdef0ULL;

  const cv::Mat grain_output = RunOpenClFusedPipeline(input, grain_params);
  ExpectFiniteRgba32f(grain_output);

  const float diff = MaxRgbDiff(grain_output, noop_output);
  std::cout << "[OpenCL film grain standalone] max_diff_vs_noop=" << diff << "\n";
  EXPECT_GT(diff, 1.0e-4f) << "OpenCL film grain should modify the image vs no-op.";

  double  cuda_ms   = 0.0;
  double  opencl_ms = 0.0;
  cv::Mat cuda_output;
  cv::Mat opencl_output;
  try {
    auto cuda_params   = grain_params;
    auto opencl_params = grain_params;
    cuda_output        = RunFusedPipeline(GpuBackendKind::CUDA, input, cuda_params, &cuda_ms);
    opencl_output      = RunFusedPipeline(GpuBackendKind::OpenCL, input, opencl_params, &opencl_ms);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA/OpenCL film grain comparison unavailable: " << e.what();
  }

  ExpectCudaOpenClDetailClose("[OpenCL fused film grain]", cuda_output, opencl_output, cuda_ms,
                              opencl_ms);
}

TEST(OpenClFusedEditPipelineTest, HighlightShadowLocalToneMatchesCuda) {
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << OpenClContext::Instance().LastInitializationError();
  }

  const cv::Mat input = MakeHighlightShadowComparisonInput();
  const auto    params = MakeHighlightShadowComparisonParams();

  double  cuda_ms = 0.0;
  double  opencl_ms = 0.0;
  cv::Mat cuda_output;
  cv::Mat opencl_output;
  try {
    cuda_output = RunFusedPipeline(GpuBackendKind::CUDA, input, params, &cuda_ms);
    opencl_output = RunFusedPipeline(GpuBackendKind::OpenCL, input, params, &opencl_ms);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "CUDA/OpenCL H/S comparison unavailable: " << e.what();
  }

  ExpectFiniteRgba32f(cuda_output);
  ExpectFiniteRgba32f(opencl_output);
  const RgbDiffStats stats = ComputeRgbDiffStats(cuda_output, opencl_output);
  std::cout << "[OpenCL fused H/S] CUDA: " << cuda_ms << " ms | OpenCL: " << opencl_ms
            << " ms | max_abs_diff=" << stats.max_diff << " at (" << stats.x << ","
            << stats.y << ") channel=" << stats.channel << " cuda=" << stats.lhs
            << " opencl=" << stats.rhs << "\n";

  EXPECT_LE(stats.max_diff, 8.0e-4f);
  EXPECT_GT(cuda_ms, 0.0);
  EXPECT_GT(opencl_ms, 0.0);
}

TEST(OpenClFusedEditPipelineTest, ScopeAnalyzerProcessesOpenClFrame) {
  auto& context = OpenClContext::Instance();
  if (!TryEnsureOpenClRuntime()) {
    GTEST_SKIP() << context.LastInitializationError();
  }

  cv::Mat input(8, 8, CV_32FC4);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      input.at<cv::Vec4f>(y, x) = {
          static_cast<float>(x) / static_cast<float>(input.cols - 1),
          static_cast<float>(y) / static_cast<float>(input.rows - 1),
          static_cast<float>(x + y) / static_cast<float>(input.cols + input.rows - 2), 1.0f};
    }
  }

  opencl::OpenClImage gpu_image;
  gpu_image.Upload(input);
  ASSERT_NE(gpu_image.Buffer(), nullptr);
  ASSERT_EQ(clRetainMemObject(gpu_image.Buffer()), CL_SUCCESS);

  auto resource           = std::make_shared<scope::opencl_detail::OpenClLinearImageResource>();
  resource->buffer        = gpu_image.Buffer();
  resource->row_bytes     = gpu_image.RowBytes();
  resource->width         = gpu_image.Width();
  resource->height        = gpu_image.Height();
  resource->format        = FramePixelFormat::RGBA32F;
  resource->owns_memory   = true;
  resource->native_object = reinterpret_cast<std::uintptr_t>(gpu_image.Buffer());

  ScopeRequest request;
  request.enabled_mask =
      static_cast<uint32_t>(ScopeType::Histogram) | static_cast<uint32_t>(ScopeType::Waveform);
  request.histogram_bins      = 16;
  request.waveform_width      = 16;
  request.waveform_height     = 16;
  request.analysis_downsample = 1;
  request.target_fps          = 0;

  const auto analyzer         = CreateOpenClScopeAnalyzer();
  const auto display_frame    = FinalDisplayFrameView{
      SharedGpuImageHandle{GpuBackend::OpenCL, std::shared_ptr<void>(resource, resource.get()),
                           gpu_image.Width(), gpu_image.Height(), gpu_image.RowBytes(),
                           FramePixelFormat::RGBA32F},
      gpu_image.Width(),
      gpu_image.Height(),
      FramePixelFormat::RGBA32F,
      ViewerDisplayConfig{},
      AnalysisDomain::DisplayEncoded,
      {},
      1};
  const auto staged = analyzer->StageFrame(display_frame, request);
  ASSERT_TRUE(static_cast<bool>(staged));
  analyzer->SubmitFrame(staged, request);
  (void)clFinish(OpenClContext::Instance().ScopeQueue());

  const ScopeOutputSet output = analyzer->GetLatestOutput();
  EXPECT_TRUE(output.histogram_valid);
  EXPECT_TRUE(output.waveform_valid);
  EXPECT_EQ(output.histogram_bins, request.histogram_bins);
  EXPECT_EQ(output.waveform_width, request.waveform_width);
  EXPECT_EQ(output.waveform_height, request.waveform_height);

  const ScopeRenderSnapshot snapshot = ReadScopeRenderSnapshot(output);
  EXPECT_TRUE(snapshot.histogram.valid);
  EXPECT_TRUE(snapshot.waveform.valid);
  EXPECT_EQ(snapshot.histogram.bins, request.histogram_bins);
  EXPECT_EQ(snapshot.waveform.width, request.waveform_width);
  EXPECT_EQ(snapshot.waveform.height, request.waveform_height);
}
#endif

}  // namespace alcedo

#endif
