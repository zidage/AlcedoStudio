//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise input through the Develop passes of one backend (ai_denoise_integration_plan.md,
// DN5). The CUDA, OpenCL, and Metal Develop tests instantiate these checks with their device.

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "decoded_rgb_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "nn/safetensors.hpp"

namespace alcedo::gpu_dag_test {

inline const std::string kDenoiseTestModelVersion(64, 'd');
/// The camera matrix of the gain match of the test inputs (only their key reads it).
inline constexpr std::array<double, 9> kDenoiseTestXyzToCamera = {1, 0, 0, 0, 1, 0, 0, 0, 1};

/// U16 plane of @p width x @p height from @p samples (row major).
inline auto              MakeU16Plane(std::uint32_t width, std::uint32_t height,
                                      const std::vector<std::uint16_t>& samples) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = Extent2D{width, height};
  plane.stride_bytes = width * static_cast<std::uint32_t>(sizeof(std::uint16_t));
  plane.format       = HostPixelFormat::U16Cfa;
  auto storage       = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                               [](std::byte* p) { delete[] p; });
  std::copy_n(reinterpret_cast<const std::byte*>(samples.data()), plane.ByteCount(), storage.get());
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

/// F32 RGBA plane of @p width x @p height; @p value gives channel c of pixel (x, y).
template <typename Value>
auto MakeRgbaPlane(std::uint32_t width, std::uint32_t height, Value value) -> HostImagePlane {
  auto  plane  = MakeF32RgbaPlane(width, height);
  auto* pixels = const_cast<float*>(reinterpret_cast<const float*>(plane.bytes.get()));
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        pixels[(static_cast<std::size_t>(y) * width + x) * 4 + c] = value(x, y, c);
      }
    }
  }
  return plane;
}

inline auto DenoiseDocument(bool highlights_reconstruct) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  EnsureTestCameraProfile(document);
  auto& develop = document.Develop()->Params();
  develop.ApplyAiDenoiseUpdate(DevelopAiDenoise{kDenoiseTestModelVersion});
  auto payload                   = develop.Params();
  payload.highlights_reconstruct = highlights_reconstruct;
  develop.ReplaceParams(payload);
  return document;
}

/**
 * The denoised plane becomes develop.sensor_linear: white balance, the clamp of the CFA path, the
 * highlight recovery (no clipped value), and the inverse white balance give
 * clamp(v x wb, 0, r) / wb, with r = wb (highlight recovery on) or min(wb, 1) (off).
 */
template <typename Device>
void VerifyDenoisedPlaneBecomesTheSensorImageWithTheCfaClamp() {
  constexpr std::uint32_t kWidth  = 48;
  constexpr std::uint32_t kHeight = 32;
  const auto              cfa     = RawInputLoader::FromUnpackedCfa(
      MakeU16CfaPlane(kWidth, kHeight, MakeRggbPattern()), MakeRggbPattern(),
      NearBlackLinearization(), FullSensor(kWidth, kHeight), DecodeRes::FULL);
  const float wb[3] = {2.0f, 1.0f, 1.5f};  // cam_mul of NearBlackLinearization
  for (bool highlights : {false, true}) {
    SCOPED_TRACE(highlights ? "highlight recovery on" : "highlight recovery off");
    // Below black, black, mid tones, and (recovery off only) values above the white level. With
    // the recovery on, every value stays below the start of its soft ramp (0.95 x 0.987 x wb).
    const std::vector<float> samples =
        highlights ? std::vector<float>{-0.1f, 0.0f, 0.3f, 0.6f, 0.9f}
                   : std::vector<float>{-0.1f, 0.0f, 0.3f, 0.6f, 0.98f, 1.3f};
    const auto sample = [&](std::uint32_t x, std::uint32_t y, int c) {
      return samples[(x + 2 * y + static_cast<std::uint32_t>(c)) % samples.size()];
    };
    const auto input =
        RawInputLoader::FromDenoisedBayer(cfa, MakeRgbaPlane(kWidth, kHeight, sample),
                                          kDenoiseTestModelVersion, kDenoiseTestXyzToCamera);
    const auto document = DenoiseDocument(highlights);
    const auto plan     = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
    Device     device;
    const auto output = device.Execute(plan, input, document);
    device.WaitIdle();
    ASSERT_TRUE(cv::checkRange(DownloadRgb(device, output)));
    const cv::Mat sensor = DownloadRgb(device, plan.sensor_linear_output);
    ASSERT_EQ(sensor.cols, static_cast<int>(kWidth));
    ASSERT_EQ(sensor.rows, static_cast<int>(kHeight));
    for (std::uint32_t y = 0; y < kHeight; ++y) {
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        for (int c = 0; c < 3; ++c) {
          const float limit    = highlights ? wb[c] : std::min(wb[c], 1.0f);
          const float expected = std::clamp(sample(x, y, c) * wb[c], 0.0f, limit) / wb[c];
          ASSERT_NEAR(sensor.at<cv::Vec4f>(static_cast<int>(y), static_cast<int>(x))[c], expected,
                      2e-6f)
              << "x " << x << " y " << y << " c " << c;
        }
      }
    }
  }
}

/// With the switch on, a full-size CFA input stops the render; a reduced CFA input renders.
template <typename Device>
void VerifyFullSizeRenderWithTheSwitchOnAndACfaInputReportsTheError() {
  const auto document = DenoiseDocument(true);
  for (const auto decode_res : {DecodeRes::FULL, DecodeRes::HALF}) {
    const auto input = RawInputLoader::FromUnpackedCfa(MakeU16CfaPlane(64, 48, MakeRggbPattern()),
                                                       MakeRggbPattern(), NearBlackLinearization(),
                                                       FullSensor(64, 48), decode_res);
    const auto plan  = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
    Device     device;
    if (decode_res == DecodeRes::FULL) {
      try {
        (void)device.Execute(plan, input, document);
        ADD_FAILURE() << "the full-size CFA input rendered without the model";
      } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("AI Denoise is on"), std::string::npos)
            << error.what();
      }
      continue;
    }
    const auto output = device.Execute(plan, input, document);
    device.WaitIdle();
    const auto display = DownloadRgb(device, output);
    EXPECT_TRUE(cv::checkRange(display));
    EXPECT_GT(cv::norm(display, cv::NORM_L1), 1.0);
  }
}

inline auto DenoiseTestAssets() -> std::optional<std::filesystem::path> {
  const char* value = std::getenv("ALCEDO_AI_DENOISE_TEST_ASSETS");
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::filesystem::path(value);
}

/// Mean absolute 4-neighbour Laplacian of channel 1 inside @p area of an RGBA image.
inline auto MeanAbsoluteLaplacian(const cv::Mat& rgba, cv::Rect area) -> double {
  double sum   = 0.0;
  int    count = 0;
  for (int y = area.y + 1; y < area.y + area.height - 1; ++y) {
    for (int x = area.x + 1; x < area.x + area.width - 1; ++x) {
      const auto g = [&](int px, int py) { return double{rgba.at<cv::Vec4f>(py, px)[1]}; };
      sum += std::abs(4.0 * g(x, y) - g(x - 1, y) - g(x + 1, y) - g(x, y - 1) - g(x, y + 1));
      ++count;
    }
  }
  return sum / count;
}

/**
 * rawnind_crop_512 of the DN0 expected outputs: the Develop render of its input mosaic (CFA path)
 * and of its output after the gain match of the lab (DenoisedRgb path) have the same channel means
 * in develop.sensor_linear, and the denoised render has at most half of the high-frequency noise.
 */
template <typename Device>
void VerifyDenoisedExpectedCropKeepsTheLevelsAndLowersTheNoiseOfTheCfaRender() {
  const auto folder = DenoiseTestAssets();
  if (!folder) {
    GTEST_SKIP()
        << "Set ALCEDO_AI_DENOISE_TEST_ASSETS to the folder with the DN0 expected outputs.";
  }
  const auto  tensors = nn::LoadSafetensors(*folder / "rawnind_crop_512.safetensors");
  const auto& packed  = tensors.at("input");   // 4 x 512 x 512: R, G1, G2, B
  const auto& output  = tensors.at("output");  // 3 x 1024 x 1024, before the gain match
  const auto& matrix  = tensors.at("rgb_xyz_matrix");
  ASSERT_TRUE(packed.ShapeEquals({4, 512, 512}));
  ASSERT_TRUE(output.ShapeEquals({3, 1024, 1024}));
  ASSERT_TRUE(matrix.ShapeEquals({3, 3}));
  constexpr int          kPacked = 512;
  constexpr int          kSize   = 2 * kPacked;
  // The value domain of the lab: (raw - black) / (maximum - black); Sony ILCE-7C levels.
  constexpr float        kBlack  = 512.0f;
  constexpr float        kWhite  = 16383.0f;

  RawLinearizationParams linearization;
  for (int c = 0; c < 4; ++c) {
    linearization.black_level[c] = kBlack;
    linearization.white_level[c] = kWhite;
  }
  linearization.cam_mul[0] = 2.4f;
  linearization.cam_mul[1] = 1.0f;
  linearization.cam_mul[2] = 1.6f;
  linearization.cam_mul[3] = 1.0f;

  // The mosaic of the packed input and the means of the mosaic values of each color.
  std::vector<std::uint16_t> mosaic(static_cast<std::size_t>(kSize) * kSize);
  double                     mean_in[3]          = {};
  const int                  color_of_channel[4] = {0, 1, 1, 2};
  for (int channel = 0; channel < 4; ++channel) {
    const int dy = channel / 2;
    const int dx = channel % 2;
    for (int y = 0; y < kPacked; ++y) {
      for (int x = 0; x < kPacked; ++x) {
        const float value =
            packed.data[(static_cast<std::size_t>(channel) * kPacked + y) * kPacked + x];
        mean_in[color_of_channel[channel]] += value;
        const float code =
            std::clamp(std::round(kBlack + value * (kWhite - kBlack)), 0.0f, 65535.0f);
        mosaic[static_cast<std::size_t>(2 * y + dy) * kSize + 2 * x + dx] =
            static_cast<std::uint16_t>(code);
      }
    }
  }
  const double samples = double{kPacked} * kPacked;
  mean_in[0] /= samples;
  mean_in[1] /= 2.0 * samples;
  mean_in[2] /= samples;

  // Gain match of the lab: w_c is the sum of column c of camera RGB to linear Rec.2020.
  cv::Matx33d rgb_xyz;
  for (int i = 0; i < 9; ++i) rgb_xyz.val[i] = matrix.data[static_cast<std::size_t>(i)];
  const cv::Matx33d xyz_to_rec2020(1.7166512, -0.3556708, -0.2533663, -0.6666844, 1.6164812,
                                   0.0157685, 0.0176399, -0.0427706, 0.9421031);
  const cv::Matx33d camera_to_rec2020 = xyz_to_rec2020 * rgb_xyz.inv();
  double            mean_out[3]       = {};
  const std::size_t pixels            = static_cast<std::size_t>(kSize) * kSize;
  for (int c = 0; c < 3; ++c) {
    for (std::size_t i = 0; i < pixels; ++i) mean_out[c] += output.data[c * pixels + i];
    mean_out[c] /= static_cast<double>(pixels);
  }
  double numerator   = 0.0;
  double denominator = 0.0;
  for (int c = 0; c < 3; ++c) {
    const double weight =
        camera_to_rec2020(0, c) + camera_to_rec2020(1, c) + camera_to_rec2020(2, c);
    numerator += weight * mean_in[c];
    denominator += weight * mean_out[c];
  }
  const auto gain = static_cast<float>(numerator / denominator);
  // The plane domain uses the white level of the linearization, which here is the maximum.
  const auto denoised_plane =
      MakeRgbaPlane(kSize, kSize, [&](std::uint32_t x, std::uint32_t y, int c) {
        return gain * output.data[c * pixels + static_cast<std::size_t>(y) * kSize + x];
      });

  const auto cfa =
      RawInputLoader::FromUnpackedCfa(MakeU16Plane(kSize, kSize, mosaic), MakeRggbPattern(),
                                      linearization, FullSensor(kSize, kSize), DecodeRes::FULL);
  const auto denoised = RawInputLoader::FromDenoisedBayer(
      cfa, denoised_plane, kDenoiseTestModelVersion, kDenoiseTestXyzToCamera);

  const auto render = [](const PreparedRawInput& input, const PipelineDocument& document) {
    const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
    Device     device;
    const auto output_id = device.Execute(plan, input, document);
    device.WaitIdle();
    EXPECT_TRUE(cv::checkRange(DownloadRgb(device, output_id)));
    return DownloadRgb(device, plan.sensor_linear_output);
  };
  auto cfa_document = CreateDefaultPipelineDocument();
  EnsureTestCameraProfile(cfa_document);
  const cv::Mat cfa_sensor      = render(cfa, cfa_document);
  const cv::Mat denoised_sensor = render(denoised, DenoiseDocument(true));
  ASSERT_EQ(denoised_sensor.cols, kSize);
  ASSERT_EQ(denoised_sensor.rows, kSize);
  // The RCD border: the CFA render is the denoised area without 4 px at each side.
  const int border = (kSize - cfa_sensor.cols) / 2;
  ASSERT_EQ(cfa_sensor.cols, kSize - 2 * border);
  ASSERT_EQ(cfa_sensor.rows, kSize - 2 * border);
  const cv::Rect   common(border, border, cfa_sensor.cols, cfa_sensor.rows);
  const cv::Scalar cfa_mean      = cv::mean(cfa_sensor);
  const cv::Scalar denoised_mean = cv::mean(denoised_sensor(common));
  for (int c = 0; c < 3; ++c) {
    SCOPED_TRACE(c);
    EXPECT_GT(cfa_mean[c], 0.0);
    // The gain match of the lab leaves the channel means at 0.997, 1.000, and 1.015 of the mosaic
    // means; the CFA demosaic of each backend adds its own difference.
    EXPECT_NEAR(denoised_mean[c] / cfa_mean[c], 1.0, 0.03)
        << "CFA " << cfa_mean[c] << ", denoised " << denoised_mean[c];
    std::printf("channel %d: CFA mean %.6f, denoised mean %.6f, ratio %.4f\n", c, cfa_mean[c],
                denoised_mean[c], denoised_mean[c] / cfa_mean[c]);
  }
  const double cfa_noise =
      MeanAbsoluteLaplacian(cfa_sensor, cv::Rect(0, 0, cfa_sensor.cols, cfa_sensor.rows));
  const double denoised_noise = MeanAbsoluteLaplacian(denoised_sensor, common);
  std::printf("mean absolute Laplacian of G: CFA %.6f, denoised %.6f\n", cfa_noise, denoised_noise);
  EXPECT_LE(denoised_noise, 0.5 * cfa_noise)
      << "CFA " << cfa_noise << ", denoised " << denoised_noise;
}

}  // namespace alcedo::gpu_dag_test
