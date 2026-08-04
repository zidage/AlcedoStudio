//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "decoders/processor/cuda_tile_jobs.hpp"
#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"
#include "decoders/processor/operators/gpu/cuda_highlight_reconstruct.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_processor_internal.hpp"

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

void SetDemosaicModelDirEnv(const char* value) {
#if defined(_WIN32)
  _putenv_s("ALCEDO_DEMOASICNET_MODEL_DIR", value == nullptr ? "" : value);
#else
  if (value == nullptr) {
    unsetenv("ALCEDO_DEMOASICNET_MODEL_DIR");
  } else {
    setenv("ALCEDO_DEMOASICNET_MODEL_DIR", value, 1);
  }
#endif
}

auto MakeRawPatchData(LibRaw& raw, cv::Mat& patch, const int size) -> libraw_rawdata_t {
  cv::Mat raw_view(raw.imgdata.sizes.raw_height, raw.imgdata.sizes.raw_width, CV_16UC1,
                   raw.imgdata.rawdata.raw_image);
  patch                  = raw_view(cv::Rect(0, 0, size, size)).clone();

  libraw_rawdata_t data  = raw.imgdata.rawdata;
  data.raw_image         = patch.ptr<uint16_t>();
  data.sizes.raw_width   = static_cast<ushort>(size);
  data.sizes.raw_height  = static_cast<ushort>(size);
  data.sizes.width       = static_cast<ushort>(size);
  data.sizes.height      = static_cast<ushort>(size);
  data.sizes.iwidth      = static_cast<ushort>(size);
  data.sizes.iheight     = static_cast<ushort>(size);
  data.sizes.left_margin = 0;
  data.sizes.top_margin  = 0;
  data.sizes.raw_pitch   = static_cast<unsigned>(size * sizeof(uint16_t));
  return data;
}

void InitHighlightRawProcessor(LibRaw& raw_processor, const float red_mul = 2.2f,
                               const float green_mul = 1.0f, const float blue_mul = 1.55f) {
  raw_processor.imgdata.color.cam_mul[0] = red_mul;
  raw_processor.imgdata.color.cam_mul[1] = green_mul;
  raw_processor.imgdata.color.cam_mul[2] = blue_mul;
  raw_processor.imgdata.color.cam_mul[3] = 1.0f;
}

auto MakeRgbPlateau(int rows, int cols, const cv::Rect& plateau, bool fully_clipped) -> cv::Mat {
  cv::Mat img(rows, cols, CV_32FC3);
  for (int y = 0; y < rows; ++y) {
    cv::Vec3f* row = img.ptr<cv::Vec3f>(y);
    for (int x = 0; x < cols; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(std::max(cols - 1, 1));
      const float fy = static_cast<float>(y) / static_cast<float>(std::max(rows - 1, 1));
      cv::Vec3f   value(1.72f + 0.06f * fx, 0.79f + 0.04f * fy, 0.84f + 0.05f * fx);

      if (plateau.contains(cv::Point(x, y))) {
        if (fully_clipped) {
          value = cv::Vec3f(2.55f + 0.02f * float((x + y) & 1), 1.14f + 0.01f * float(x & 1),
                            1.72f + 0.02f * float(y & 1));
        } else {
          value = cv::Vec3f(2.45f + 0.05f * float(x & 1), 1.11f + 0.03f * float(y & 1),
                            0.54f + 0.01f * float((x + y) & 1));
        }
      }

      row[x] = value;
    }
  }
  return img;
}

auto ChannelSpread(const cv::Vec3f& pixel) -> float {
  return std::max(pixel[0], std::max(pixel[1], pixel[2])) -
         std::min(pixel[0], std::min(pixel[1], pixel[2]));
}

auto MaxDifferenceOutsideRoi(const cv::Mat& before, const cv::Mat& after, const cv::Rect& roi)
    -> float {
  float max_diff = 0.0f;
  for (int y = 0; y < before.rows; ++y) {
    const cv::Vec3f* before_row = before.ptr<cv::Vec3f>(y);
    const cv::Vec3f* after_row  = after.ptr<cv::Vec3f>(y);
    for (int x = 0; x < before.cols; ++x) {
      if (roi.contains(cv::Point(x, y))) {
        continue;
      }
      const cv::Vec3f delta = before_row[x] - after_row[x];
      max_diff              = std::max(
          max_diff, std::max(std::abs(delta[0]), std::max(std::abs(delta[1]), std::abs(delta[2]))));
    }
  }
  return max_diff;
}

auto ChannelRange(const cv::Mat& image, const cv::Rect& roi, const int channel) -> float {
  float min_value = std::numeric_limits<float>::max();
  float max_value = std::numeric_limits<float>::lowest();
  for (int y = roi.y; y < roi.y + roi.height; ++y) {
    const cv::Vec3f* row = image.ptr<cv::Vec3f>(y);
    for (int x = roi.x; x < roi.x + roi.width; ++x) {
      min_value = std::min(min_value, row[x][channel]);
      max_value = std::max(max_value, row[x][channel]);
    }
  }
  return max_value - min_value;
}

void FillConstantRgb(cv::Mat& img, const float red, const float green, const float blue) {
  for (int y = 0; y < img.rows; ++y) {
    cv::Vec3f* row = img.ptr<cv::Vec3f>(y);
    for (int x = 0; x < img.cols; ++x) {
      row[x] = cv::Vec3f(red, green, blue);
    }
  }
}

auto CountNonFinite(const cv::Mat& image) -> int {
  int count = 0;
  for (int y = 0; y < image.rows; ++y) {
    const cv::Vec3f* row = image.ptr<cv::Vec3f>(y);
    for (int x = 0; x < image.cols; ++x) {
      for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(row[x][c])) {
          ++count;
        }
      }
    }
  }
  return count;
}

}  // namespace

TEST(CudaRawOpsTest, Clamp01SupportsRgb) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  cv::Mat src(4, 5, CV_32FC3);
  for (int y = 0; y < src.rows; ++y) {
    cv::Vec3f* row = src.ptr<cv::Vec3f>(y);
    for (int x = 0; x < src.cols; ++x) {
      row[x] = cv::Vec3f(-0.35f + 0.2f * x, 0.15f * y, 0.85f + 0.18f * float(x + y));
    }
  }

  cv::cuda::GpuMat gpu(src);
  ASSERT_NO_THROW(CUDA::Clamp01(gpu));

  cv::Mat clamped;
  gpu.download(clamped);
  ASSERT_EQ(clamped.type(), CV_32FC3);

  for (int y = 0; y < src.rows; ++y) {
    const cv::Vec3f* row = clamped.ptr<cv::Vec3f>(y);
    for (int x = 0; x < src.cols; ++x) {
      EXPECT_GE(row[x][0], 0.0f);
      EXPECT_GE(row[x][1], 0.0f);
      EXPECT_GE(row[x][2], 0.0f);
      EXPECT_LE(row[x][0], 1.0f);
      EXPECT_LE(row[x][1], 1.0f);
      EXPECT_LE(row[x][2], 1.0f);
    }
  }
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructReducesSpreadOfFullyClippedRegion) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);
  const cv::Rect plateau(12, 10, 20, 18);
  const cv::Mat  input = MakeRgbPlateau(40, 44, plateau, true);

  cv::cuda::GpuMat gpu(input);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(gpu, raw_processor));

  cv::Mat output;
  gpu.download(output);
  ASSERT_EQ(output.type(), CV_32FC3);

  const cv::Vec3f before =
      input.at<cv::Vec3f>(plateau.y + plateau.height / 2, plateau.x + plateau.width / 2);
  const cv::Vec3f after =
      output.at<cv::Vec3f>(plateau.y + plateau.height / 2, plateau.x + plateau.width / 2);

  // A saturated photosite readout is a lower bound of the scene signal, so the opposed
  // algorithm may only raise channels toward the opponent estimate, never lower them.
  EXPECT_GE(after[0], before[0] - 1e-5f);
  EXPECT_GE(after[1], before[1] - 1e-5f);
  EXPECT_GE(after[2], before[2] - 1e-5f);
  // The clipped green channel is pulled up toward the brighter red/blue opponent estimate,
  // shrinking the overall spread of the blown pixel.
  EXPECT_GT(after[1], before[1] + 0.2f);
  EXPECT_LT(ChannelSpread(after), ChannelSpread(before));
  EXPECT_TRUE(std::isfinite(after[0]) && std::isfinite(after[1]) && std::isfinite(after[2]));
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructRaisesClippedChannelTowardOpponentEstimate) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);

  // Two-zone scene: background sits mid-scale on every channel; the core is brighter on
  // red/blue (still below their own clip levels) while its green channel saturates. The
  // opponent estimate for green inside the core comes from the brighter red/blue, so green
  // is raised toward it.
  const int      rows = 40;
  const int      cols = 40;
  const cv::Rect core(12, 12, 16, 16);
  cv::Mat        input(rows, cols, CV_32FC3);
  FillConstantRgb(input, 1.2f, 0.95f, 1.0f);
  for (int y = core.y; y < core.y + core.height; ++y) {
    cv::Vec3f* row = input.ptr<cv::Vec3f>(y);
    for (int x = core.x; x < core.x + core.width; ++x) {
      row[x] = cv::Vec3f(2.0f, 1.0f, 1.5f);
    }
  }

  cv::cuda::GpuMat gpu(input);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(gpu, raw_processor));

  cv::Mat output;
  gpu.download(output);

  const cv::Vec3f before = input.at<cv::Vec3f>(20, 20);
  const cv::Vec3f after  = output.at<cv::Vec3f>(20, 20);

  // Red is below its soft-transition window and blue's opponent estimate stays below the
  // measured value, so both channels keep their readout exactly.
  EXPECT_NEAR(after[0], before[0], 1e-5f);
  EXPECT_NEAR(after[2], before[2], 1e-5f);
  // Clipped green is raised toward the red/blue opponent level, closing the green-blue gap.
  EXPECT_GT(after[1], before[1] + 0.1f);
  EXPECT_LT(std::abs(after[1] - after[2]), std::abs(before[1] - before[2]));
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructRemovesCheckerNoiseInsideFullyClippedRegion) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);

  // Fully clipped plateau whose green channel carries a checkerboard pattern. Every interior
  // 3x3 window sees only censored samples, so the rebuilt green channel falls back to the
  // window mean plus the global chrominance: a spatially constant estimate that cannot
  // follow the checker.
  const int      rows = 44;
  const int      cols = 44;
  const cv::Rect plateau(10, 10, 24, 24);
  cv::Mat        input(rows, cols, CV_32FC3);
  FillConstantRgb(input, 1.5f, 0.8f, 0.9f);
  for (int y = plateau.y; y < plateau.y + plateau.height; ++y) {
    cv::Vec3f* row = input.ptr<cv::Vec3f>(y);
    for (int x = plateau.x; x < plateau.x + plateau.width; ++x) {
      row[x] = cv::Vec3f(2.5f, (x & 1) != 0 ? 1.06f : 1.0f, 1.6f);
    }
  }

  const cv::Rect interior(plateau.x + 2, plateau.y + 2, plateau.width - 4,
                          plateau.height - 4);
  const float    before_range = ChannelRange(input, interior, 1);

  cv::cuda::GpuMat gpu(input);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(gpu, raw_processor));

  cv::Mat output;
  gpu.download(output);

  const float after_range = ChannelRange(output, interior, 1);
  EXPECT_LT(after_range, before_range * 0.25f);
  EXPECT_LT(after_range, 0.025f);
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructTransitionsContinuouslyAcrossClipBoundary) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);

  // The green channel ramps slowly through its clip level inside a bright red/blue core.
  // With a hard clip decision the output would jump from the raw readout (~0.99) to the
  // opponent estimate (~1.4) at the crossing column; the soft transition must instead
  // blend smoothly, keeping every column-to-column step small.
  const int      rows = 40;
  const int      cols = 40;
  const cv::Rect core(8, 10, 24, 20);
  cv::Mat        input(rows, cols, CV_32FC3);
  FillConstantRgb(input, 1.2f, 0.95f, 1.0f);
  for (int y = core.y; y < core.y + core.height; ++y) {
    cv::Vec3f* row = input.ptr<cv::Vec3f>(y);
    for (int x = core.x; x < core.x + core.width; ++x) {
      const float green =
          0.94f + 0.06f * static_cast<float>(x - core.x) / static_cast<float>(core.width - 1);
      row[x] = cv::Vec3f(2.0f, green, 1.5f);
    }
  }

  cv::cuda::GpuMat gpu(input);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(gpu, raw_processor));

  cv::Mat output;
  gpu.download(output);

  const int sample_y = core.y + core.height / 2;
  float     max_step = 0.0f;
  float     prev     = output.at<cv::Vec3f>(sample_y, core.x + 2)[1];
  for (int x = core.x + 3; x < core.x + core.width - 2; ++x) {
    const float current = output.at<cv::Vec3f>(sample_y, x)[1];
    max_step            = std::max(max_step, std::abs(current - prev));
    prev                = current;
  }

  EXPECT_LT(max_step, 0.3f);
  // The ramp ends above the clip level, so the last sampled column must be reconstructed
  // toward the brighter red/blue opponent estimate rather than left at its readout.
  EXPECT_GT(output.at<cv::Vec3f>(sample_y, core.x + core.width - 3)[1], 1.15f);
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructKeepsOutputFiniteAroundNonFinitePixels) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);
  const cv::Rect plateau(12, 10, 20, 18);
  const cv::Mat  clean = MakeRgbPlateau(40, 44, plateau, true);

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  cv::Mat corrupted                            = clean.clone();
  corrupted.at<cv::Vec3f>(plateau.y, plateau.x) = cv::Vec3f(nan, 1.14f, 1.72f);
  corrupted.at<cv::Vec3f>(plateau.y - 2, plateau.x) = cv::Vec3f(1.75f, nan, 0.86f);
  corrupted.at<cv::Vec3f>(2, 2)                     = cv::Vec3f(1.75f, 0.8f, inf);

  cv::cuda::GpuMat clean_gpu(clean);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(clean_gpu, raw_processor));
  cv::Mat clean_output;
  clean_gpu.download(clean_output);

  cv::cuda::GpuMat corrupted_gpu(corrupted);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(corrupted_gpu, raw_processor));

  cv::Mat output;
  corrupted_gpu.download(output);

  // Non-finite samples must be dropped from the neighbourhood and chrominance statistics
  // instead of poisoning them, and the output may never contain NaN or Inf.
  EXPECT_EQ(CountNonFinite(output), 0);

  const int       cy = plateau.y + plateau.height / 2;
  const int       cx = plateau.x + plateau.width / 2;
  const cv::Vec3f clean_center     = clean_output.at<cv::Vec3f>(cy, cx);
  const cv::Vec3f corrupted_center = output.at<cv::Vec3f>(cy, cx);
  EXPECT_NEAR(corrupted_center[0], clean_center[0], 2e-2f);
  EXPECT_NEAR(corrupted_center[1], clean_center[1], 2e-2f);
  EXPECT_NEAR(corrupted_center[2], clean_center[2], 2e-2f);
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructIgnoresChrominanceBelowMinSampleCount) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);

  // A single clipped pixel in the image corner dilates to a 4x4 ring of 15 samples, below
  // the minimum trusted sample count, so the global chrominance correction must stay zero
  // and the clipped green channel is rebuilt from the local opponent estimate alone.
  const int rows = 40;
  const int cols = 40;
  cv::Mat   input(rows, cols, CV_32FC3);
  FillConstantRgb(input, 1.5f, 0.8f, 0.9f);
  input.at<cv::Vec3f>(0, 0) = cv::Vec3f(2.5f, 1.05f, 1.6f);

  cv::cuda::GpuMat gpu(input);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(gpu, raw_processor));

  cv::Mat output;
  gpu.download(output);

  // refavg at the corner sees a 2x2 window: red 1.5 / green 0.8 / blue 0.9 from the valid
  // background samples, so the green opponent estimate is
  // ((cbrt(1.5) + cbrt(0.9)) / 2)^3 ~= 1.175 and red and blue stay at their readouts.
  const cv::Vec3f result = output.at<cv::Vec3f>(0, 0);
  EXPECT_NEAR(result[0], 2.5f, 1e-5f);
  EXPECT_NEAR(result[1], 1.175f, 2e-2f);
  EXPECT_NEAR(result[2], 1.6f, 1e-5f);
#endif
}

TEST(CudaRawOpsTest, HighlightReconstructDoesNotBleedOutsideClippedRegion) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  LibRaw raw_processor;
  InitHighlightRawProcessor(raw_processor);
  const cv::Rect   plateau(14, 13, 16, 15);
  const cv::Mat    input = MakeRgbPlateau(44, 47, plateau, false);

  cv::cuda::GpuMat gpu(input);
  ASSERT_NO_THROW(CUDA::HighlightReconstruct(gpu, raw_processor));

  cv::Mat output;
  gpu.download(output);

  EXPECT_LT(MaxDifferenceOutsideRoi(input, output, plateau), 1e-6f);
#endif
}

TEST(CudaRawOpsTest, NeuralEngineBayerTrimsOddTrailingEdgesAndReusesCachedWeights) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  cv::Mat cfa(65, 67, CV_32FC1);
  cv::randu(cfa, 0.0F, 1.0F);
  cv::cuda::GpuMat gpu_cfa(cfa);
  cv::cuda::GpuMat first_rgb;
  cv::cuda::GpuMat second_rgb;
  RawCfaPattern    pattern;
  pattern.kind = RawCfaKind::Bayer2x2;

  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;

  const auto first = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, first_rgb, nullptr, options);
  ASSERT_TRUE(first.succeeded) << first.error;
  // Odd 65×67 → even 64×66; student natural shrink H-34 → 30×32 (border 17).
  ASSERT_EQ(first.source_border, BayerDemosaicNet::kNaturalSpatialLoss / 2);
  ASSERT_EQ(first_rgb.type(), CV_32FC3);
  EXPECT_EQ(first_rgb.size(), cv::Size(32, 30));
  const float* weight_ptr = cache.Bayer().PackWeightDevicePtr();
  ASSERT_NE(weight_ptr, nullptr);

  const auto second =
      CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, second_rgb, nullptr, options);
  ASSERT_TRUE(second.succeeded) << second.error;
  EXPECT_EQ(cache.Bayer().PackWeightDevicePtr(), weight_ptr);
#endif
}

TEST(CudaRawOpsTest, NeuralEngineLoadFailureLeavesOutputUntouchedForLegacyFallback) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  cv::Mat          cfa(64, 64, CV_32FC1, cv::Scalar(0.5F));
  cv::cuda::GpuMat gpu_cfa(cfa);
  cv::cuda::GpuMat rgb(3, 5, CV_32FC3);
  RawCfaPattern    pattern;
  pattern.kind = RawCfaKind::Bayer2x2;

  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache            = &cache;
  options.load_options.model_dir = std::filesystem::path("definitely_missing_demosaicnet_models");

  const auto result = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
  EXPECT_FALSE(result.succeeded);
  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(rgb.size(), cv::Size(5, 3));
  EXPECT_FALSE(cache.IsLoaded(DemosaicNetVariant::Bayer));
#endif
}

TEST(CudaRawOpsTest, NeuralEngineXTransProducesExpectedValidConvolutionShape) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  cv::Mat cfa(48, 50, CV_32FC1);
  cv::randu(cfa, 0.0F, 1.0F);
  cv::cuda::GpuMat gpu_cfa(cfa);
  cv::cuda::GpuMat rgb;
  RawCfaPattern    pattern;
  pattern.kind = RawCfaKind::XTrans6x6;
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 6; ++x) {
      const int index                      = XTransCellIndex(y, x);
      pattern.xtrans_pattern.rgb_fc[index] = (x + 2 * y) % 3;
    }
  }

  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;
  const auto result   = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);

  ASSERT_TRUE(result.succeeded) << result.error;
  // 48×50 even → natural X-Trans shrink H-18 → 30×32 (border 9).
  EXPECT_EQ(result.source_border, XTransDemosaicNet::kNaturalSpatialLoss / 2);
  EXPECT_EQ(rgb.type(), CV_32FC3);
  EXPECT_EQ(rgb.size(), cv::Size(32, 30));
#endif
}

// Purpose: load a real Bayer camera RAW, take a 64×64 CFA patch, and verify Neural
// Engine demosaics it to CV_32FC3 with the student natural valid-convolution size.
TEST(CudaRawOpsTest, NeuralEngineDemosaicsRealBayerRawPatchToValidRgb) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_NE(raw->imgdata.rawdata.raw_image, nullptr);

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  ASSERT_EQ(pattern.kind, RawCfaKind::Bayer2x2);
  cv::Mat raw_view(raw->imgdata.sizes.raw_height, raw->imgdata.sizes.raw_width, CV_16UC1,
                   raw->imgdata.rawdata.raw_image);
  cv::Mat patch;
  raw_view(cv::Rect(0, 0, 64, 64)).convertTo(patch, CV_32FC1, 1.0 / 65535.0);

  cv::cuda::GpuMat            gpu_patch(patch);
  cv::cuda::GpuMat            rgb;
  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;
  const auto result   = CUDA::DemosaicWithNeuralEngine(gpu_patch, pattern, rgb, nullptr, options);

  ASSERT_TRUE(result.succeeded) << result.error;
  EXPECT_EQ(rgb.type(), CV_32FC3);
  // Student natural: 64 - 34 = 30 on both axes.
  EXPECT_EQ(rgb.size(), cv::Size(30, 30));
  raw->recycle();
#endif
}

TEST(CudaRawOpsTest, RawProcessorDefaultXTransLoadsNeuralEngineOnRealRawPatch) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_NE(raw->imgdata.rawdata.raw_image, nullptr);
  ASSERT_EQ(raw->imgdata.idata.filters, 9U);

  constexpr int    kPatch = 64;
  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, kPatch);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::CUDA;
  params.demosaic_method_        = RawDemosaicMethod::Default;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  const auto          shift   = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());
  const int aligned_h = (kPatch - shift->sy) - ((kPatch - shift->sy) % 6);
  const int aligned_w = (kPatch - shift->sx) - ((kPatch - shift->sx) % 6);
  // Product student tiling restores same-size aligned RGB; sensor crop maps once.
  const detail::NeuralOutputGeometry geometry = detail::MakeStudentTiledNeuralOutputGeometry(
      shift->sx, shift->sy, cv::Size(aligned_w, aligned_h));
  const cv::Rect expected_crop = detail::BuildNeuralEngineDecodeCropRect(
      patch_data.sizes, no_crop, cv::Size(kPatch, kPatch), DecodeRes::FULL, geometry);

  auto& cache = DemosaicNetModelCache::Instance();
  cache.Unload(DemosaicNetVariant::XTrans);
  ASSERT_FALSE(cache.IsLoaded(DemosaicNetVariant::XTrans));

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(cache.IsLoaded(DemosaicNetVariant::XTrans));
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::CUDA);
  EXPECT_EQ(output.GetCUDAImage().type(), CV_32FC4);
  EXPECT_EQ(output.GetCUDAImage().size(), expected_crop.size());
  raw->recycle();
#endif
}

TEST(CudaRawOpsTest, RawProcessorNeuralLoadFailureFallsBackToLegacyAndKeepsCacheCold) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::CUDA;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  auto&                  cache      = DemosaicNetModelCache::Instance();
  cache.Unload(DemosaicNetVariant::Bayer);
  SetDemosaicModelDirEnv("definitely_missing_demosaicnet_models");
  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();
  SetDemosaicModelDirEnv(nullptr);

  EXPECT_FALSE(cache.IsLoaded(DemosaicNetVariant::Bayer));
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::CUDA);
  EXPECT_EQ(output.GetCUDAImage().type(), CV_32FC4);
  EXPECT_EQ(output.GetCUDAImage().size(), cv::Size(56, 56));
  raw->recycle();
#endif
}

// --- Phase 6b: CFA phase-align + gamma sandwich (product Neural path) ---

TEST(CudaRawOpsTest, FindCfaAlignShift_MatchesGrbgOriginForCommonBayerPhases) {
  // All four classic Bayer phases must map to a unique cyclic shift yielding GRBG at (0,0).
  const struct {
    const char* name;
    int         rgb[4];
  } phases[] = {
      {"RGGB", {0, 1, 1, 2}},
      {"GRBG", {1, 0, 2, 1}},
      {"GBRG", {1, 2, 0, 1}},
      {"BGGR", {2, 1, 1, 0}},
  };

  for (const auto& phase : phases) {
    RawCfaPattern pattern;
    pattern.kind = RawCfaKind::Bayer2x2;
    for (int i = 0; i < 4; ++i) {
      pattern.bayer_pattern.rgb_fc[i] = phase.rgb[i];
      pattern.bayer_pattern.raw_fc[i] = phase.rgb[i] == 1 ? 1 : phase.rgb[i];
    }
    const auto shift = FindCfaAlignShift(pattern);
    ASSERT_TRUE(shift.has_value()) << phase.name;
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        const int camera = RgbColorAt(pattern, i + shift->sy, j + shift->sx);
        const int target = kDemosaicNetBayerTargetRgb[BayerCellIndex(i, j)];
        EXPECT_EQ(camera, target) << phase.name << " at (" << i << "," << j << ")";
      }
    }
  }
}

TEST(CudaRawOpsTest, FindCfaAlignShift_MatchesXTransTrainingOrigin) {
  RawCfaPattern pattern;
  pattern.kind           = RawCfaKind::XTrans6x6;
  pattern.xtrans_pattern = DemosaicNetTrainingXTransPattern();
  // Shift the training pattern by a known cyclic offset and recover it.
  constexpr int kSy      = 2;
  constexpr int kSx      = 3;
  RawCfaPattern shifted;
  shifted.kind = RawCfaKind::XTrans6x6;
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 6; ++x) {
      const int dst = XTransCellIndex(y, x);
      // Camera sees target at (y+sy, x+sx) when cropped by (sy,sx).
      shifted.xtrans_pattern.rgb_fc[dst] =
          RgbColorAt(pattern, WrapPatternCoord(y - kSy, 6), WrapPatternCoord(x - kSx, 6));
      shifted.xtrans_pattern.raw_fc[dst] = shifted.xtrans_pattern.rgb_fc[dst];
    }
  }

  const auto shift = FindCfaAlignShift(shifted);
  ASSERT_TRUE(shift.has_value());
  EXPECT_EQ(shift->sy, kSy);
  EXPECT_EQ(shift->sx, kSx);
}

TEST(CudaRawOpsTest, FindCfaAlignShift_UnsupportedRotationFailsSoft) {
  // Mirror of GRBG is not a cyclic shift of GRBG → nullopt (product falls back to Legacy).
  RawCfaPattern pattern;
  pattern.kind                    = RawCfaKind::Bayer2x2;
  // Horizontal mirror of GRBG (G R / B G) → (R G / G B) is actually RGGB which *is* a shift.
  // Use a non-Bayer permutation: R R / B B (no greens) — not cyclic-equivalent to GRBG.
  pattern.bayer_pattern.rgb_fc[0] = 0;
  pattern.bayer_pattern.rgb_fc[1] = 0;
  pattern.bayer_pattern.rgb_fc[2] = 2;
  pattern.bayer_pattern.rgb_fc[3] = 2;
  for (int i = 0; i < 4; ++i) {
    pattern.bayer_pattern.raw_fc[i] = pattern.bayer_pattern.rgb_fc[i];
  }
  EXPECT_FALSE(FindCfaAlignShift(pattern).has_value());
}

TEST(CudaRawOpsTest, NeuralEngineGammaEncodeDecode_RoundTripsLinearRgb) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  cv::Mat host(8, 8, CV_32FC3);
  for (int y = 0; y < host.rows; ++y) {
    for (int x = 0; x < host.cols; ++x) {
      host.at<cv::Vec3f>(y, x) = cv::Vec3f(0.02F + 0.01F * static_cast<float>(x), 0.5F,
                                           1.25F + 0.05F * static_cast<float>(y));
    }
  }
  cv::cuda::GpuMat gpu(host);
  GammaEncodeGpuMat(gpu);
  GammaDecodeGpuMat(gpu);
  cv::Mat roundtrip;
  gpu.download(roundtrip);

  double max_abs = 0.0;
  for (int y = 0; y < host.rows; ++y) {
    for (int x = 0; x < host.cols; ++x) {
      const cv::Vec3f a = host.at<cv::Vec3f>(y, x);
      const cv::Vec3f b = roundtrip.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        max_abs = std::max(max_abs, static_cast<double>(std::abs(a[c] - b[c])));
      }
    }
  }
  EXPECT_LT(max_abs, 2e-5);
#endif
}

TEST(CudaRawOpsTest, NeuralEngineGammaEncodeDecode_PreservesOverRangeHighlights) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const float values[] = {1.5F, 2.0F, 4.0F};
  for (const float v : values) {
    cv::Mat          host(4, 4, CV_32FC1, cv::Scalar(v));
    cv::cuda::GpuMat gpu(host);
    GammaEncodeGpuMat(gpu);
    GammaDecodeGpuMat(gpu);
    cv::Mat out;
    gpu.download(out);
    const float recovered = out.at<float>(0, 0);
    EXPECT_GT(recovered, 1.0F) << "value=" << v;
    EXPECT_NEAR(recovered, v, 1e-4F) << "value=" << v;
  }
#endif
}

TEST(CudaRawOpsTest, NeuralEnginePath_SkipsClamp01WhenHighlightsReconstruct) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  // Over-range linear CFA must survive preprocess when HLR is on (product path skips Clamp01).
  cv::Mat          cfa(64, 64, CV_32FC1, cv::Scalar(1.75F));
  cv::cuda::GpuMat gpu_cfa(cfa);
  RawCfaPattern    pattern;
  pattern.kind          = RawCfaKind::Bayer2x2;
  pattern.bayer_pattern = DemosaicNetTrainingBayerPattern();

  // Simulate product order for HLR-on: no Clamp01, then prepare (encode).
  cv::cuda::GpuMat aligned;
  const auto       prep = PrepareNeuralEngineCfa(gpu_cfa, pattern, aligned, nullptr);
  ASSERT_TRUE(prep.succeeded) << prep.error;

  cv::Mat encoded;
  aligned.download(encoded);
  const float sample = encoded.at<float>(0, 0);
  // 1.75^(1/2.2) ≈ 1.28 > 1
  EXPECT_GT(sample, 1.0F);
#endif
}

TEST(CudaRawOpsTest, NeuralEnginePath_AppliesClamp01WhenHighlightsReconstructOff) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  cv::Mat          cfa(64, 64, CV_32FC1, cv::Scalar(1.75F));
  cv::cuda::GpuMat gpu_cfa(cfa);
  // Product order for HLR-off: Clamp01 then prepare.
  CUDA::Clamp01(gpu_cfa);
  RawCfaPattern pattern;
  pattern.kind          = RawCfaKind::Bayer2x2;
  pattern.bayer_pattern = DemosaicNetTrainingBayerPattern();

  cv::cuda::GpuMat aligned;
  const auto       prep = PrepareNeuralEngineCfa(gpu_cfa, pattern, aligned, nullptr);
  ASSERT_TRUE(prep.succeeded) << prep.error;

  cv::Mat encoded;
  aligned.download(encoded);
  // After Clamp01 to 1.0, encode stays <= 1.
  EXPECT_LE(encoded.at<float>(0, 0), 1.0F + 1e-6F);
#endif
}

TEST(CudaRawOpsTest, NeuralEnginePhaseAlignAndGamma_OnRealBayerRawPatch) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_NE(raw->imgdata.rawdata.raw_image, nullptr);

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  ASSERT_EQ(pattern.kind, RawCfaKind::Bayer2x2);
  const auto shift = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());

  cv::Mat raw_view(raw->imgdata.sizes.raw_height, raw->imgdata.sizes.raw_width, CV_16UC1,
                   raw->imgdata.rawdata.raw_image);
  cv::Mat patch;
  raw_view(cv::Rect(0, 0, 128, 128)).convertTo(patch, CV_32FC1, 1.0 / 65535.0);

  cv::cuda::GpuMat gpu_patch(patch);
  cv::cuda::GpuMat aligned;
  const auto       prep = PrepareNeuralEngineCfa(gpu_patch, pattern, aligned, nullptr);
  ASSERT_TRUE(prep.succeeded) << prep.error;
  EXPECT_EQ(prep.aligned_pattern.kind, RawCfaKind::Bayer2x2);
  EXPECT_EQ(DescribeBayerPattern(prep.aligned_pattern.bayer_pattern), "GRBG");

  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;
  cv::cuda::GpuMat rgb;
  const auto       result =
      CUDA::DemosaicWithNeuralEngine(aligned, prep.aligned_pattern, rgb, nullptr, options);
  ASSERT_TRUE(result.succeeded) << result.error;
  FinishNeuralEngineRgb(rgb, nullptr);

  ASSERT_EQ(rgb.type(), CV_32FC3);
  EXPECT_GT(rgb.rows, 0);
  EXPECT_GT(rgb.cols, 0);
  cv::Mat host;
  rgb.download(host);
  for (int y = 0; y < host.rows; ++y) {
    for (int x = 0; x < host.cols; ++x) {
      const cv::Vec3f p = host.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(p[c])) << "y=" << y << " x=" << x << " c=" << c;
      }
    }
  }
  raw->recycle();
#endif
}

TEST(CudaRawOpsTest, NeuralEnginePhaseAlignAndGamma_OnRealXTransRawPatch) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_NE(raw->imgdata.rawdata.raw_image, nullptr);
  ASSERT_EQ(raw->imgdata.idata.filters, 9U);

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  ASSERT_EQ(pattern.kind, RawCfaKind::XTrans6x6);
  ASSERT_TRUE(FindCfaAlignShift(pattern).has_value());

  cv::Mat raw_view(raw->imgdata.sizes.raw_height, raw->imgdata.sizes.raw_width, CV_16UC1,
                   raw->imgdata.rawdata.raw_image);
  cv::Mat patch;
  raw_view(cv::Rect(0, 0, 96, 96)).convertTo(patch, CV_32FC1, 1.0 / 65535.0);

  cv::cuda::GpuMat gpu_patch(patch);
  cv::cuda::GpuMat aligned;
  const auto       prep = PrepareNeuralEngineCfa(gpu_patch, pattern, aligned, nullptr);
  ASSERT_TRUE(prep.succeeded) << prep.error;
  EXPECT_EQ(prep.aligned_pattern.kind, RawCfaKind::XTrans6x6);

  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;
  cv::cuda::GpuMat rgb;
  const auto       result =
      CUDA::DemosaicWithNeuralEngine(aligned, prep.aligned_pattern, rgb, nullptr, options);
  ASSERT_TRUE(result.succeeded) << result.error;
  FinishNeuralEngineRgb(rgb, nullptr);

  ASSERT_EQ(rgb.type(), CV_32FC3);
  cv::Mat host;
  rgb.download(host);
  for (int y = 0; y < host.rows; ++y) {
    for (int x = 0; x < host.cols; ++x) {
      const cv::Vec3f p = host.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(p[c]));
      }
    }
  }
  raw->recycle();
#endif
}

// Purpose: Bayer student planner + product tile entry preserve pad32 phase (mod-2
// origins, GRBG training pattern) and first model-output origin at -1.
TEST(CudaRawOpsTest, ProcessCudaTiled_BayerStudentPad32PreservesGrbgOrigin) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kAligned = 2048;  // two step-1024 tiles on each axis
  cv::Mat       host(kAligned, kAligned, CV_32FC1);
  cv::randu(host, 0.05F, 0.95F);
  cv::cuda::GpuMat            gpu_cfa(host);
  const RawCfaPattern         pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  const detail::CudaTilePolicy policy = detail::MakeBayerStudentTilePolicy();
  ASSERT_EQ(policy.virtual_pad.x, 32);
  ASSERT_EQ(policy.output_border.x, 31);
  ASSERT_EQ(policy.step.width, 1024);

  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, kAligned, kAligned), cv::Size(kAligned, kAligned),
                            policy);
  ASSERT_FALSE(jobs.empty());
  for (const auto& job : jobs) {
    EXPECT_EQ(job.input_origin.x % 2, 0);
    EXPECT_EQ(job.input_origin.y % 2, 0);
  }
  // First grid origin: input = -pad; model output origin in assembled coords = -1
  // (clipped so destination starts at 0 with one discarded leading column/row).
  EXPECT_EQ(jobs.front().input_origin, cv::Point(-32, -32));
  EXPECT_EQ(jobs.front().destination_roi.tl(), cv::Point(0, 0));
  EXPECT_EQ(jobs.front().model_output_roi.tl(), cv::Point(1, 1));

  DemosaicNetModelCache         cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;
  cv::cuda::GpuMat tile_rgb;
  const auto       result = CUDA::DemosaicStudentTileWithNeuralEngine(
      gpu_cfa, jobs.front().input_origin, pattern, tile_rgb, nullptr, options);
  ASSERT_TRUE(result.succeeded) << result.error;
  EXPECT_EQ(result.source_border, 31);
  EXPECT_EQ(tile_rgb.size(), cv::Size(1024, 1024));
  EXPECT_EQ(pattern.bayer_pattern.rgb_fc[0], 1);  // GRBG green
  EXPECT_EQ(pattern.bayer_pattern.rgb_fc[1], 0);  // red
#endif
}

// Purpose: two Bayer tiles across the step-1024 boundary assemble without holes or
// double-writes; interior strip is finite.
TEST(CudaRawOpsTest, ProcessCudaTiled_BayerStudentMatchesReferenceAcross1024Boundary) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kW = 2048;
  constexpr int kH = 1024;
  cv::Mat       host(kH, kW, CV_32FC1);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 31 + x * 17) % 251) / 251.0F;
    }
  }
  cv::cuda::GpuMat             gpu_cfa(host);
  const RawCfaPattern          pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  const detail::CudaTilePolicy policy  = detail::MakeBayerStudentTilePolicy();
  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, kW, kH), cv::Size(kW, kH), policy);

  cv::cuda::GpuMat assembled(kH, kW, CV_32FC3);
  assembled.setTo(cv::Scalar(0, 0, 0));
  cv::cuda::GpuMat              tile_rgb;
  DemosaicNetModelCache         cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;

  std::vector<std::uint8_t> coverage(static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH),
                                     0);
  for (const auto& job : jobs) {
    const auto result = CUDA::DemosaicStudentTileWithNeuralEngine(
        gpu_cfa, job.input_origin, pattern, tile_rgb, nullptr, options);
    ASSERT_TRUE(result.succeeded) << result.error;
    tile_rgb(job.model_output_roi).copyTo(assembled(job.destination_roi));
    for (int y = job.destination_roi.y; y < job.destination_roi.y + job.destination_roi.height;
         ++y) {
      for (int x = job.destination_roi.x; x < job.destination_roi.x + job.destination_roi.width;
           ++x) {
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(kW) + static_cast<std::size_t>(x);
        ASSERT_EQ(coverage[idx], 0) << "double write at " << x << "," << y;
        coverage[idx] = 1;
      }
    }
  }
  for (const auto v : coverage) {
    ASSERT_EQ(v, 1);
  }

  cv::Mat host_rgb;
  assembled.download(host_rgb);
  // Seam strip around x=1024 (Bayer has no inter-tile overlap; abutting edges).
  for (int y = 0; y < kH; ++y) {
    for (int x = 1020; x < 1028 && x < kW; ++x) {
      const cv::Vec3f p = host_rgb.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(p[c])) << "seam " << x << "," << y << " c=" << c;
      }
    }
  }
#endif
}

// Purpose: X-Trans step-1020 first-writer ownership covers every pixel once across the
// four-pixel overlap band.
TEST(CudaRawOpsTest, ProcessCudaTiled_XTransStudentOverlapHasNoUnwrittenOrDoubleWrittenPixels) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  // Two tiles on X: step 1020, output 1024 → cover width 2040 (period-6).
  constexpr int kW = 2040;
  constexpr int kH = 1020;
  cv::Mat       host(kH, kW, CV_32FC1);
  cv::randu(host, 0.05F, 0.95F);
  cv::cuda::GpuMat             gpu_cfa(host);
  const RawCfaPattern          pattern = DemosaicNetTrainingPattern(RawCfaKind::XTrans6x6);
  const detail::CudaTilePolicy policy  = detail::MakeXTransStudentTilePolicy();
  ASSERT_EQ(policy.step.width, 1020);
  ASSERT_EQ(policy.virtual_pad.x, 12);
  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, kW, kH), cv::Size(kW, kH), policy);

  // Second tile on X must discard four leading columns.
  bool found_overlap_discard = false;
  for (const auto& job : jobs) {
    if (job.destination_roi.x > 0) {
      EXPECT_EQ(job.model_output_roi.x, 4);
      found_overlap_discard = true;
    }
  }
  EXPECT_TRUE(found_overlap_discard);

  cv::cuda::GpuMat assembled(kH, kW, CV_32FC3);
  assembled.setTo(cv::Scalar(-1, -1, -1));
  cv::cuda::GpuMat              tile_rgb;
  DemosaicNetModelCache         cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;

  std::vector<std::uint8_t> coverage(static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH),
                                     0);
  for (const auto& job : jobs) {
    EXPECT_EQ(job.input_origin.x % 6, 0);
    EXPECT_EQ(job.input_origin.y % 6, 0);
    const auto result = CUDA::DemosaicStudentTileWithNeuralEngine(
        gpu_cfa, job.input_origin, pattern, tile_rgb, nullptr, options);
    ASSERT_TRUE(result.succeeded) << result.error;
    EXPECT_EQ(result.source_border, 12);
    tile_rgb(job.model_output_roi).copyTo(assembled(job.destination_roi));
    for (int y = job.destination_roi.y; y < job.destination_roi.y + job.destination_roi.height;
         ++y) {
      for (int x = job.destination_roi.x; x < job.destination_roi.x + job.destination_roi.width;
           ++x) {
        const std::size_t idx =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(kW) + static_cast<std::size_t>(x);
        ASSERT_EQ(coverage[idx], 0) << "double write at " << x << "," << y;
        coverage[idx] = 1;
      }
    }
  }
  for (const auto v : coverage) {
    ASSERT_EQ(v, 1);
  }

  cv::Mat host_rgb;
  assembled.download(host_rgb);
  // Overlap band around x=1020..1023 is written exactly once (first writer). Network
  // values may be slightly negative; the unwritten sentinel is exactly -1.
  for (int y = 0; y < kH; ++y) {
    for (int x = 1016; x < 1028 && x < kW; ++x) {
      const cv::Vec3f p = host_rgb.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(p[c]));
        EXPECT_NE(p[c], -1.0F) << "unwritten sentinel at " << x << "," << y;
      }
    }
  }
#endif
}

// Purpose: X-Trans student tiles across the 1020 boundary produce a full assembled frame
// of aligned size with finite values (product geometry contract).
TEST(CudaRawOpsTest, ProcessCudaTiled_XTransStudentMatchesReferenceAcross1020Boundary) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  // One row of tiles across the step-1020 boundary (two tiles) is enough for the seam.
  constexpr int kW = 2040;
  constexpr int kH = 1020;
  cv::Mat       host(kH, kW, CV_32FC1);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 19 + x * 23) % 241) / 241.0F;
    }
  }
  cv::cuda::GpuMat             gpu_cfa(host);
  const RawCfaPattern          pattern = DemosaicNetTrainingPattern(RawCfaKind::XTrans6x6);
  const detail::CudaTilePolicy policy  = detail::MakeXTransStudentTilePolicy();
  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, kW, kH), cv::Size(kW, kH), policy);

  cv::cuda::GpuMat assembled(kH, kW, CV_32FC3);
  assembled.setTo(cv::Scalar(0, 0, 0));
  cv::cuda::GpuMat              tile_rgb;
  DemosaicNetModelCache         cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;

  for (const auto& job : jobs) {
    const auto result = CUDA::DemosaicStudentTileWithNeuralEngine(
        gpu_cfa, job.input_origin, pattern, tile_rgb, nullptr, options);
    ASSERT_TRUE(result.succeeded) << result.error;
    tile_rgb(job.model_output_roi).copyTo(assembled(job.destination_roi));
  }

  cv::Mat host_rgb;
  assembled.download(host_rgb);
  ASSERT_EQ(host_rgb.size(), cv::Size(kW, kH));
  for (int y = 0; y < kH; ++y) {
    for (int x = 1016; x < 1028; ++x) {
      const cv::Vec3f p = host_rgb.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(p[c]));
      }
    }
  }
#endif
}

// Purpose: after the first (largest) tile reserves NeuralDemosaicWorkspace capacity,
// subsequent same-shape forwards must not bump allocation_generation (Phase 8.1 contract).
TEST(CudaRawOpsTest, NeuralEngineWorkspaceAllocationGenerationStableAfterWarmup) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kInput  = 1086;  // 1024 + Bayer border 31*2
  constexpr int kOutput = 1024;
  cv::Mat       cfa(kInput, kInput, CV_32FC1);
  cv::randu(cfa, 0.0F, 1.0F);
  cv::cuda::GpuMat            gpu_cfa(cfa);
  cv::cuda::GpuMat            rgb;
  RawCfaPattern               pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache       cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;

  const auto first = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
  ASSERT_TRUE(first.succeeded) << first.error;
  ASSERT_EQ(rgb.size(), cv::Size(kOutput, kOutput));
  const std::uint64_t gen_after_warmup = workspace.allocation_generation();
  ASSERT_GT(gen_after_warmup, 0u);

  for (int i = 0; i < 3; ++i) {
    const auto again = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
    ASSERT_TRUE(again.succeeded) << again.error;
    EXPECT_EQ(workspace.allocation_generation(), gen_after_warmup) << "iteration " << i;
  }
  EXPECT_GT(workspace.OwnedDeviceBytes(), 0u);
#endif
}

namespace {

void ExpectRgbMatsNear(const cv::Mat& a, const cv::Mat& b, const float abs_tol = 1e-5F) {
  ASSERT_EQ(a.type(), CV_32FC3);
  ASSERT_EQ(b.type(), CV_32FC3);
  ASSERT_EQ(a.size(), b.size());
  for (int y = 0; y < a.rows; ++y) {
    const auto* row_a = a.ptr<cv::Vec3f>(y);
    const auto* row_b = b.ptr<cv::Vec3f>(y);
    for (int x = 0; x < a.cols; ++x) {
      for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(row_a[x][c], row_b[x][c], abs_tol) << "pixel " << x << "," << y << " c=" << c;
      }
    }
  }
}

}  // namespace

// Purpose: Phase 8E — Enqueue + one host wait matches the synchronous Bayer student forward
// bit-for-bit within FP32 noise (same weights, same workspace shape).
TEST(CudaRawOpsTest, NeuralEngineAsyncBayerStudentMatchesSynchronousForward) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kInput = BayerDemosaicNet::kTileInput;
  cv::Mat       host(kInput, kInput, CV_32FC1);
  for (int y = 0; y < kInput; ++y) {
    for (int x = 0; x < kInput; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 13 + x * 29) % 251) / 251.0F;
    }
  }
  cv::cuda::GpuMat        gpu_cfa(host);
  const RawCfaPattern     pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache   cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;

  CUDA::NeuralDemosaicWorkspace sync_ws;
  options.workspace = &sync_ws;
  cv::cuda::GpuMat sync_rgb;
  const auto       sync_result =
      CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, sync_rgb, nullptr, options);
  ASSERT_TRUE(sync_result.succeeded) << sync_result.error;

  CUDA::NeuralDemosaicWorkspace async_ws;
  options.workspace = &async_ws;
  cv::cuda::Stream stream;
  cv::cuda::GpuMat async_rgb;
  CUDA::ResetNeuralEngineHostSyncCountForTest();
  const auto async_result =
      CUDA::EnqueueDemosaicWithNeuralEngine(gpu_cfa, pattern, async_rgb, &stream, options);
  ASSERT_TRUE(async_result.succeeded) << async_result.error;
  EXPECT_EQ(CUDA::NeuralEngineHostSyncCountForTest(), 0u)
      << "Enqueue must not host-synchronize";
  stream.waitForCompletion();

  cv::Mat host_sync;
  cv::Mat host_async;
  sync_rgb.download(host_sync);
  async_rgb.download(host_async);
  ASSERT_EQ(host_sync.size(), cv::Size(BayerDemosaicNet::kTileOutput, BayerDemosaicNet::kTileOutput));
  ExpectRgbMatsNear(host_sync, host_async);
#endif
}

// Purpose: Phase 8E — Enqueue + one host wait matches the synchronous X-Trans student forward.
TEST(CudaRawOpsTest, NeuralEngineAsyncXTransStudentMatchesSynchronousForward) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kInput = XTransDemosaicNet::kTileInput;
  cv::Mat       host(kInput, kInput, CV_32FC1);
  for (int y = 0; y < kInput; ++y) {
    for (int x = 0; x < kInput; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 17 + x * 23) % 241) / 241.0F;
    }
  }
  cv::cuda::GpuMat        gpu_cfa(host);
  const RawCfaPattern     pattern = DemosaicNetTrainingPattern(RawCfaKind::XTrans6x6);
  DemosaicNetModelCache   cache;
  CUDA::NeuralDemosaicOptions options;
  options.model_cache = &cache;

  CUDA::NeuralDemosaicWorkspace sync_ws;
  options.workspace = &sync_ws;
  cv::cuda::GpuMat sync_rgb;
  const auto       sync_result =
      CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, sync_rgb, nullptr, options);
  ASSERT_TRUE(sync_result.succeeded) << sync_result.error;

  CUDA::NeuralDemosaicWorkspace async_ws;
  options.workspace = &async_ws;
  cv::cuda::Stream stream;
  cv::cuda::GpuMat async_rgb;
  CUDA::ResetNeuralEngineHostSyncCountForTest();
  const auto async_result =
      CUDA::EnqueueDemosaicWithNeuralEngine(gpu_cfa, pattern, async_rgb, &stream, options);
  ASSERT_TRUE(async_result.succeeded) << async_result.error;
  EXPECT_EQ(CUDA::NeuralEngineHostSyncCountForTest(), 0u);
  stream.waitForCompletion();

  cv::Mat host_sync;
  cv::Mat host_async;
  sync_rgb.download(host_sync);
  async_rgb.download(host_async);
  ASSERT_EQ(host_sync.size(),
            cv::Size(XTransDemosaicNet::kTileOutput, XTransDemosaicNet::kTileOutput));
  ExpectRgbMatsNear(host_sync, host_async);
#endif
}

// Purpose: Phase 8E product loop — one stream, one fixed workspace, enqueue pack/forward/unpack
// then owned ROI copy for every job; single wait after all tiles; no mid-loop buffer growth.
TEST(CudaRawOpsTest, ProcessCudaTiled_StudentSingleStreamReusesBuffersAfterQueuedRoiCopy) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kW = 2048;
  constexpr int kH = 1024;
  cv::Mat       host(kH, kW, CV_32FC1);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 31 + x * 17) % 251) / 251.0F;
    }
  }
  cv::cuda::GpuMat             gpu_cfa(host);
  const RawCfaPattern          pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  const detail::CudaTilePolicy policy  = detail::MakeBayerStudentTilePolicy();
  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, kW, kH), cv::Size(kW, kH), policy);
  ASSERT_GE(jobs.size(), 2u);

  cv::cuda::Stream              stream;
  cv::cuda::GpuMat              assembled(kH, kW, CV_32FC3);
  assembled.setTo(cv::Scalar(0, 0, 0), stream);
  cv::cuda::GpuMat              tile_rgb;
  DemosaicNetModelCache         cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;

  // Warm fixed student shape before the first tile enqueue (product contract).
  workspace.EnsureCapacity(
      DemosaicNetVariant::Bayer, BayerDemosaicNet::kTileInput, BayerDemosaicNet::kTileInput,
      static_cast<std::size_t>(3) * BayerDemosaicNet::kTileInput * BayerDemosaicNet::kTileInput);
  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, {})) << cache.LastError();
  const std::uint64_t gen_after_warm = workspace.allocation_generation();
  ASSERT_GT(gen_after_warm, 0u);

  CUDA::ResetNeuralEngineHostSyncCountForTest();
  for (const auto& job : jobs) {
    const auto result = CUDA::EnqueueDemosaicStudentTileWithNeuralEngine(
        gpu_cfa, job.input_origin, pattern, tile_rgb, &stream, options);
    ASSERT_TRUE(result.succeeded) << result.error;
    EXPECT_EQ(workspace.allocation_generation(), gen_after_warm);
    tile_rgb(job.model_output_roi).copyTo(assembled(job.destination_roi), stream);
  }
  EXPECT_EQ(CUDA::NeuralEngineHostSyncCountForTest(), 0u)
      << "student tile Enqueue path must not host-synchronize mid-loop";
  // One final product-boundary wait (mirrors ProcessCudaTiled).
  stream.waitForCompletion();

  cv::Mat host_rgb;
  assembled.download(host_rgb);
  ASSERT_EQ(host_rgb.size(), cv::Size(kW, kH));
  for (int y = 0; y < kH; y += 64) {
    for (int x = 0; x < kW; x += 64) {
      const cv::Vec3f p = host_rgb.at<cv::Vec3f>(y, x);
      for (int c = 0; c < 3; ++c) {
        EXPECT_TRUE(std::isfinite(p[c]));
      }
    }
  }
#endif
}

// Purpose: Phase 8E — after EnsureCapacity for the fixed student tile shape, repeated
// Enqueue student tiles must not bump allocation_generation.
TEST(CudaRawOpsTest, NeuralEngineStudentWorkspaceGenerationStableAfterWarmup) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kAligned = 2048;
  cv::Mat       host(kAligned, kAligned, CV_32FC1);
  cv::randu(host, 0.05F, 0.95F);
  cv::cuda::GpuMat             gpu_cfa(host);
  const RawCfaPattern          pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  const detail::CudaTilePolicy policy  = detail::MakeBayerStudentTilePolicy();
  const auto jobs =
      detail::BuildTileJobs(cv::Rect(0, 0, kAligned, kAligned), cv::Size(kAligned, kAligned),
                            policy);
  ASSERT_FALSE(jobs.empty());

  DemosaicNetModelCache         cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;
  workspace.EnsureCapacity(
      DemosaicNetVariant::Bayer, BayerDemosaicNet::kTileInput, BayerDemosaicNet::kTileInput,
      static_cast<std::size_t>(3) * BayerDemosaicNet::kTileInput * BayerDemosaicNet::kTileInput);
  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, {})) << cache.LastError();
  const std::uint64_t gen = workspace.allocation_generation();
  ASSERT_GT(gen, 0u);

  cv::cuda::Stream stream;
  cv::cuda::GpuMat tile_rgb;
  for (std::size_t i = 0; i < std::min<std::size_t>(jobs.size(), 4); ++i) {
    const auto result = CUDA::EnqueueDemosaicStudentTileWithNeuralEngine(
        gpu_cfa, jobs[i].input_origin, pattern, tile_rgb, &stream, options);
    ASSERT_TRUE(result.succeeded) << result.error;
    EXPECT_EQ(workspace.allocation_generation(), gen) << "tile " << i;
  }
  stream.waitForCompletion();
#endif
}

// Purpose: Phase 8E — synchronous wrappers perform exactly one host wait per call;
// Enqueue performs zero. A multi-tile timed-style pass uses only Enqueue (no wrapper waits).
TEST(CudaRawOpsTest, NeuralEngineStudentTimedPassHasOneFinalSynchronization) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kInput = BayerDemosaicNet::kTileInput;
  cv::Mat       host(kInput, kInput, CV_32FC1);
  cv::randu(host, 0.0F, 1.0F);
  cv::cuda::GpuMat      gpu_cfa(host);
  const RawCfaPattern   pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache cache;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache = &cache;
  options.workspace   = &workspace;

  // Warm once outside the measured-style pass.
  {
    cv::cuda::GpuMat warm_rgb;
    ASSERT_TRUE(
        CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, warm_rgb, nullptr, options).succeeded);
  }
  const std::uint64_t gen = workspace.allocation_generation();

  CUDA::ResetNeuralEngineHostSyncCountForTest();
  cv::cuda::GpuMat sync_rgb;
  ASSERT_TRUE(
      CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, sync_rgb, nullptr, options).succeeded);
  EXPECT_EQ(CUDA::NeuralEngineHostSyncCountForTest(), 1u);
  EXPECT_EQ(workspace.allocation_generation(), gen);

  CUDA::ResetNeuralEngineHostSyncCountForTest();
  cv::cuda::Stream stream;
  cv::cuda::GpuMat async_rgb;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(CUDA::EnqueueDemosaicWithNeuralEngine(gpu_cfa, pattern, async_rgb, &stream, options)
                    .succeeded);
  }
  EXPECT_EQ(CUDA::NeuralEngineHostSyncCountForTest(), 0u)
      << "Enqueue path must not host-synchronize during a multi-tile timed pass";
  EXPECT_EQ(workspace.allocation_generation(), gen);
  // Single final product-boundary synchronization.
  stream.waitForCompletion();
  EXPECT_EQ(CUDA::NeuralEngineHostSyncCountForTest(), 0u)
      << "product wait is outside the Neural Engine wrappers";
#endif
}

TEST(CudaRawOpsTest, ProcessCudaTiled_NeuralEngineBayerAssemblesActiveAreaFromRealRaw) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::CUDA;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};
  const RawCfaPattern    pattern    = ReadLibRawCfaPattern(*raw);
  const auto             shift      = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());
  const int aligned_h =
      raw->imgdata.sizes.raw_height - shift->sy - ((raw->imgdata.sizes.raw_height - shift->sy) % 2);
  const int aligned_w =
      raw->imgdata.sizes.raw_width - shift->sx - ((raw->imgdata.sizes.raw_width - shift->sx) % 2);
  // Student virtual-pad tiling restores same-size aligned RGB; sensor crop maps once.
  const detail::NeuralOutputGeometry geometry = detail::MakeStudentTiledNeuralOutputGeometry(
      shift->sx, shift->sy, cv::Size(aligned_w, aligned_h));
  const cv::Rect expected_crop = detail::BuildNeuralEngineDecodeCropRect(
      raw->imgdata.sizes, no_crop,
      cv::Size(raw->imgdata.sizes.raw_width, raw->imgdata.sizes.raw_height), DecodeRes::FULL,
      geometry);

  RawProcessor processor(params, raw->imgdata.rawdata, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::CUDA);
  EXPECT_EQ(output.GetCUDAImage().type(), CV_32FC4);
  EXPECT_EQ(output.GetCUDAImage().size(), expected_crop.size());
  raw->recycle();
#endif
}

TEST(CudaRawOpsTest, ProcessCudaTiled_NeuralEngineXTransRealRawLoadsAndAssembles) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  const std::filesystem::path raw_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::CUDA;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};
  const RawCfaPattern    pattern    = ReadLibRawCfaPattern(*raw);
  ASSERT_EQ(pattern.kind, RawCfaKind::XTrans6x6);
  const auto shift = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());
  const int aligned_h =
      raw->imgdata.sizes.raw_height - shift->sy - ((raw->imgdata.sizes.raw_height - shift->sy) % 6);
  const int aligned_w =
      raw->imgdata.sizes.raw_width - shift->sx - ((raw->imgdata.sizes.raw_width - shift->sx) % 6);
  // Fuji full RAW is student-tiled with pad12/step1020; assembled RGB matches aligned size.
  const detail::NeuralOutputGeometry geometry = detail::MakeStudentTiledNeuralOutputGeometry(
      shift->sx, shift->sy, cv::Size(aligned_w, aligned_h));
  const cv::Rect expected_crop = detail::BuildNeuralEngineDecodeCropRect(
      raw->imgdata.sizes, no_crop,
      cv::Size(raw->imgdata.sizes.raw_width, raw->imgdata.sizes.raw_height), DecodeRes::FULL,
      geometry);

  auto& cache = DemosaicNetModelCache::Instance();
  cache.Unload(DemosaicNetVariant::XTrans);
  RawProcessor processor(params, raw->imgdata.rawdata, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();
  ASSERT_TRUE(cache.IsLoaded(DemosaicNetVariant::XTrans));
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::CUDA);
  EXPECT_EQ(output.GetCUDAImage().type(), CV_32FC4);
  EXPECT_EQ(output.GetCUDAImage().size(), expected_crop.size());
  raw->recycle();
#endif
}

// Phase 8B: fused reflect/pack must match “pack full sparse mosaic, reflect, slice”.
TEST(CudaRawOpsTest, PackReflectPaddedCfaTile_BayerMatchesPackThenReflectReference) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kH = 64;
  constexpr int kW = 64;
  constexpr int kTile = 48;
  cv::Mat       host(kH, kW, CV_32FC1);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 17 + x * 13) % 97) / 97.0F;
    }
  }
  cv::cuda::GpuMat gpu_cfa(host);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);

  // Reference: pack full mosaic to NCHW host, then reflect-sample into tile.
  std::vector<float> full_mosaic(static_cast<std::size_t>(3) * kH * kW, 0.0F);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int color = RgbColorAt(pattern, y, x);
      const int idx   = color * kH * kW + y * kW + x;
      full_mosaic[static_cast<std::size_t>(idx)] = host.at<float>(y, x);
    }
  }
  auto reflect101 = [](int c, int limit) {
    if (limit <= 1) {
      return 0;
    }
    while (c < 0 || c >= limit) {
      c = c < 0 ? -c : 2 * limit - c - 2;
    }
    return c;
  };

  const cv::Point origin(-8, -12);  // period-aligned for Bayer (even)
  std::vector<float> expected(static_cast<std::size_t>(3) * kTile * kTile, 0.0F);
  for (int y = 0; y < kTile; ++y) {
    for (int x = 0; x < kTile; ++x) {
      const int sx = reflect101(origin.x + x, kW);
      const int sy = reflect101(origin.y + y, kH);
      for (int c = 0; c < 3; ++c) {
        expected[static_cast<std::size_t>(c * kTile * kTile + y * kTile + x)] =
            full_mosaic[static_cast<std::size_t>(c * kH * kW + sy * kW + sx)];
      }
    }
  }

  cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(3) * kTile * kTile);
  auto                      tensor = d_out.AsTensor({1, 3, kTile, kTile});
  CUDA::PackReflectPaddedCfaTile(gpu_cfa, origin, pattern, tensor, kTile, kTile, nullptr);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  const auto actual = d_out.Download();
  ASSERT_EQ(actual.size(), expected.size());
  float max_abs = 0.0F;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    max_abs = std::max(max_abs, std::fabs(actual[i] - expected[i]));
  }
  EXPECT_LE(max_abs, 0.0F);
#endif
}

TEST(CudaRawOpsTest, PackReflectPaddedCfaTile_XTransMatchesPackThenReflectReferenceAtAllEdges) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "CUDA is not enabled in this build.";
#else
  if (!EnsureCudaDevice()) {
    GTEST_SKIP() << "CUDA device is unavailable in this environment.";
  }

  constexpr int kH = 36;
  constexpr int kW = 36;
  constexpr int kTile = 24;
  cv::Mat       host(kH, kW, CV_32FC1);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      host.at<float>(y, x) = static_cast<float>((y * 11 + x * 7) % 53) / 53.0F;
    }
  }
  cv::cuda::GpuMat    gpu_cfa(host);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::XTrans6x6);

  std::vector<float> full_mosaic(static_cast<std::size_t>(3) * kH * kW, 0.0F);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const int color = RgbColorAt(pattern, y, x);
      full_mosaic[static_cast<std::size_t>(color * kH * kW + y * kW + x)] = host.at<float>(y, x);
    }
  }
  auto reflect101 = [](int c, int limit) {
    if (limit <= 1) {
      return 0;
    }
    while (c < 0 || c >= limit) {
      c = c < 0 ? -c : 2 * limit - c - 2;
    }
    return c;
  };

  // Probe all four exterior edges with period-aligned origins (mod 6).
  const std::array<cv::Point, 4> origins = {
      cv::Point(-12, -12),
      cv::Point(kW - 6, -12),
      cv::Point(-12, kH - 6),
      cv::Point(kW - 6, kH - 6),
  };

  for (const cv::Point origin : origins) {
    std::vector<float> expected(static_cast<std::size_t>(3) * kTile * kTile, 0.0F);
    for (int y = 0; y < kTile; ++y) {
      for (int x = 0; x < kTile; ++x) {
        const int sx = reflect101(origin.x + x, kW);
        const int sy = reflect101(origin.y + y, kH);
        for (int c = 0; c < 3; ++c) {
          expected[static_cast<std::size_t>(c * kTile * kTile + y * kTile + x)] =
              full_mosaic[static_cast<std::size_t>(c * kH * kW + sy * kW + sx)];
        }
      }
    }

    cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(3) * kTile * kTile);
    auto                      tensor = d_out.AsTensor({1, 3, kTile, kTile});
    CUDA::PackReflectPaddedCfaTile(gpu_cfa, origin, pattern, tensor, kTile, kTile, nullptr);
    ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
    const auto actual = d_out.Download();
    float      max_abs = 0.0F;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      max_abs = std::max(max_abs, std::fabs(actual[i] - expected[i]));
    }
    EXPECT_LE(max_abs, 0.0F) << "origin=" << origin;
  }
#endif
}

}  // namespace alcedo
