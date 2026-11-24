//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the AI Denoise input preparation, gain match, and plane assembly (integration plan,
// DN6; lab docs/alcedo_integration.md).

#include "edit/input/ai_denoise_input.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ai_denoise_test_support.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/input/detail/prepared_input_finish.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "nn/safetensors.hpp"

namespace alcedo {
namespace {

using ai_denoise_test::BayerPattern;
using ai_denoise_test::MakeBayerInput;
using ai_denoise_test::TestCode;
using ai_denoise_test::TestLinearization;

auto TestAssets() -> std::optional<std::filesystem::path> {
  const char* value = std::getenv("ALCEDO_AI_DENOISE_TEST_ASSETS");
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::filesystem::path(value);
}

/// The prepared value of host position (y, x) of MakeBayerInput: (code - black) / (maximum -
/// black) with the black level of the raw color of the position.
auto ExpectedValue(const RawCfaPattern& pattern, int y, int x) -> float {
  const auto  linearization = TestLinearization();
  const int   raw_color     = RawColorAt(pattern, y, x);
  const float black         = linearization.black_level[raw_color];
  return (static_cast<float>(TestCode(y, x)) - black) / (linearization.libraw_maximum - black);
}

auto Reflect(int index, int length) -> int {
  if (index < 0) return -index;
  if (index >= length) return 2 * (length - 1) - index;
  return index;
}

TEST(AiDenoiseInputTest, PackedMosaicStartsWithRedForEachBayerPhase) {
  struct Size {
    std::uint32_t width;
    std::uint32_t height;
  };
  for (const std::string name : {"RGGB", "GBRG", "GRBG", "BGGR"}) {
    for (const Size size : {Size{30, 24}, Size{31, 25}, Size{27, 32}}) {
      SCOPED_TRACE(name + " " + std::to_string(size.width) + " x " + std::to_string(size.height));
      const auto pattern = BayerPattern(name);
      const auto input   = MakeBayerInput(size.width, size.height, pattern);
      const auto mosaic  = PackBayerForAiDenoise(input);
      const int  top     = name == "GBRG" || name == "BGGR" ? 1 : 0;
      const int  left    = name == "GRBG" || name == "BGGR" ? 1 : 0;
      EXPECT_EQ(mosaic.added_top, top);
      EXPECT_EQ(mosaic.added_left, left);
      EXPECT_EQ(mosaic.area_width, static_cast<int>(size.width));
      EXPECT_EQ(mosaic.area_height, static_cast<int>(size.height));
      EXPECT_EQ(2 * mosaic.height, static_cast<int>(size.height) + top + (size.height + top) % 2);
      EXPECT_EQ(2 * mosaic.width, static_cast<int>(size.width) + left + (size.width + left) % 2);
      const auto plane            = static_cast<std::size_t>(mosaic.width) * mosaic.height;
      int        wrong_values     = 0;
      int        wrong_colors     = 0;
      const int  channel_color[4] = {0, 1, 1, 2};
      for (int channel = 0; channel < 4; ++channel) {
        for (int py = 0; py < mosaic.height; ++py) {
          for (int px = 0; px < mosaic.width; ++px) {
            const int   y = Reflect(2 * py + channel / 2 - top, static_cast<int>(size.height));
            const int   x = Reflect(2 * px + channel % 2 - left, static_cast<int>(size.width));
            const float value =
                mosaic.planes[channel * plane + static_cast<std::size_t>(py) * mosaic.width + px];
            wrong_values += value != ExpectedValue(pattern, y, x) ? 1 : 0;
            wrong_colors +=
                RgbColorAt(pattern.bayer_pattern, y, x) != channel_color[channel] ? 1 : 0;
          }
        }
      }
      EXPECT_EQ(wrong_values, 0);
      EXPECT_EQ(wrong_colors, 0);
    }
  }
}

TEST(AiDenoiseInputTest, PreparedValuesUseTheBlackOfEachColorAndTheLibRawMaximum) {
  // An active area with an odd top margin: the area starts with G (the pattern of the plane is
  // RGGB), so one row is added at the top.
  RawSensorGeometry sensor;
  sensor.raw_width                      = 34;
  sensor.raw_height                     = 31;
  sensor.width                          = 30;
  sensor.height                         = 28;
  sensor.left_margin                    = 2;
  sensor.top_margin                     = 1;
  const auto pattern                    = BayerPattern("RGGB");
  auto       input                      = MakeBayerInput(34, 31, pattern, sensor);
  // A pattern black of 2 x 2 px.
  input.linearization.black_tile_width  = 2;
  input.linearization.black_tile_height = 2;
  const float pattern_black[4]          = {4.0f, -2.0f, 0.0f, 6.0f};
  for (int i = 0; i < 4; ++i) {
    input.linearization.pattern_black[i] = pattern_black[i];
  }

  const auto mosaic = PackBayerForAiDenoise(input);
  EXPECT_EQ(mosaic.added_top, 1);
  EXPECT_EQ(mosaic.added_left, 0);
  EXPECT_EQ(mosaic.area_width, 30);
  EXPECT_EQ(mosaic.area_height, 28);
  const auto  plane       = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  const auto& lin         = input.linearization;
  double      sums[3]     = {};
  double      counts[3]   = {};
  bool        below_black = false;
  bool        above_one   = false;
  for (int ay = 0; ay < 28; ++ay) {
    for (int ax = 0; ax < 30; ++ax) {
      const int   y         = 1 + ay;
      const int   x         = 2 + ax;
      const int   raw_color = RawColorAt(pattern, y, x);
      const float black     = lin.black_level[raw_color] + pattern_black[(y % 2) * 2 + x % 2];
      const float expected  = (static_cast<float>(TestCode(y, x)) - black) /
                             (lin.libraw_maximum - lin.black_level[raw_color]);
      const int   ey      = ay + mosaic.added_top;
      const int   channel = (ey % 2) * 2 + ax % 2;
      const float value =
          mosaic.planes[channel * plane + static_cast<std::size_t>(ey / 2) * mosaic.width + ax / 2];
      ASSERT_EQ(value, expected) << "area (" << ax << ", " << ay << ")";
      const int color = RgbColorAt(pattern.bayer_pattern, y, x);
      sums[color] += value;
      counts[color] += 1.0;
      below_black = below_black || value < 0.0f;
      above_one   = above_one || value > 1.0f;
    }
  }
  // The values are not clipped.
  EXPECT_TRUE(below_black);
  EXPECT_TRUE(above_one);
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(mosaic.input_means[c], sums[c] / counts[c], 1e-12) << "color " << c;
  }
}

TEST(AiDenoiseInputTest, PreparationRejectsAnInputThatIsNotAFullSizeBayerCfa) {
  const auto pattern                 = BayerPattern("RGGB");
  auto       input                   = MakeBayerInput(32, 32, pattern);
  input.linearization.libraw_maximum = 0.0f;  // not from LibRaw
  EXPECT_THROW((void)PackBayerForAiDenoise(input), std::runtime_error);
  auto reduced =
      RawInputLoader::FromUnpackedCfa(MakeBayerInput(64, 64, pattern).pixels, pattern,
                                      TestLinearization(), RawSensorGeometry{}, DecodeRes::HALF);
  EXPECT_THROW((void)PackBayerForAiDenoise(reduced), std::runtime_error);
}

TEST(AiDenoiseInputTest, OutputSumsAddOnlyTheOutputOfTheArea) {
  // GBRG, 31 x 25: one row at the top, one column at the right are added; the output is 32 x 26.
  const auto input  = MakeBayerInput(31, 25, BayerPattern("GBRG"));
  const auto mosaic = PackBayerForAiDenoise(input);
  ASSERT_EQ(mosaic.OutputWidth(), 32);
  ASSERT_EQ(mosaic.OutputHeight(), 26);
  std::vector<float> data(3 * 32 * 26);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<float>(i % 97);
  }
  const nn::RgbPlanesView output{data, 32, 26};
  AiDenoiseOutputSums     sums(mosaic);
  // Two kept rectangles that cover the packed image: the first five packed rows, then the rest.
  sums.AddKept(output, {0, 0, 16, 5});
  sums.AddKept(output, {0, 5, 16, 8});
  const auto means = sums.Means();
  for (int c = 0; c < 3; ++c) {
    double sum = 0.0;
    for (int y = 1; y < 26; ++y) {
      for (int x = 0; x < 31; ++x) {
        sum += data[static_cast<std::size_t>(c) * 32 * 26 + y * 32 + x];
      }
    }
    EXPECT_NEAR(means[c], sum / (25.0 * 31.0), 1e-9) << "channel " << c;
  }
}

TEST(AiDenoiseInputTest, DenoisedPlaneIsTheGainMatchedOutputInTheWhiteLevelDomain) {
  const auto         input  = MakeBayerInput(31, 25, BayerPattern("BGGR"));
  const auto         mosaic = PackBayerForAiDenoise(input);
  const int          width  = mosaic.OutputWidth();
  const int          height = mosaic.OutputHeight();
  std::vector<float> data(static_cast<std::size_t>(3) * width * height);
  const auto         value = [&](int c, int y, int x) { return 1000.0f * c + 10.0f * y + x; };
  for (int c = 0; c < 3; ++c) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        data[(static_cast<std::size_t>(c) * height + y) * width + x] = value(c, y, x);
      }
    }
  }
  constexpr double kGain = 2.5e-4;
  const auto       plane =
      AssembleDenoisedPlane({data, width, height}, mosaic, kGain, input.linearization);
  ASSERT_EQ(plane.format, HostPixelFormat::F32Rgba);
  ASSERT_EQ(plane.extent.width, 31U);
  ASSERT_EQ(plane.extent.height, 25U);
  ASSERT_EQ(plane.stride_bytes, 31U * 16U);
  const auto* pixels = reinterpret_cast<const float*>(plane.bytes.get());
  const auto& lin    = input.linearization;
  for (int y = 0; y < 25; ++y) {
    for (int x = 0; x < 31; ++x) {
      for (int c = 0; c < 3; ++c) {
        const double scale = (lin.libraw_maximum - lin.black_level[c]) /
                             (static_cast<double>(lin.white_level[c]) - lin.black_level[c]);
        const double expected =
            kGain * value(c, y + mosaic.added_top, x + mosaic.added_left) * scale;
        ASSERT_NEAR(pixels[(static_cast<std::size_t>(y) * 31 + x) * 4 + c], expected,
                    1e-6 * std::abs(expected) + 1e-9)
            << "x " << x << " y " << y << " c " << c;
      }
      ASSERT_EQ(pixels[(static_cast<std::size_t>(y) * 31 + x) * 4 + 3], 1.0f);
    }
  }
  // The result is the DN5 input of the same decode area.
  const auto denoised = RawInputLoader::FromDenoisedBayer(input, plane, std::string(64, 'a'),
                                                          ai_denoise_test::kTestXyzToCamera);
  EXPECT_EQ(denoised.input_kind, RawInputKind::DenoisedRgb);
}

TEST(AiDenoiseInputTest, InputCurveIsTheGainUpToTheKneeAndKeepsTheWhiteLevel) {
  EXPECT_TRUE(AiDenoiseInputCurveOfGain(1.0).IsIdentity());
  EXPECT_TRUE(AiDenoiseInputCurveOfGain(0.5).IsIdentity());
  EXPECT_THROW((void)AiDenoiseInputCurveOfGain(std::nan("")), std::runtime_error);
  for (const double gain : {1.001, 1.5, 4.0, kAiDenoiseMaxInputGain}) {
    SCOPED_TRACE("gain " + std::to_string(gain));
    const auto curve = AiDenoiseInputCurveOfGain(gain);
    ASSERT_FALSE(curve.IsIdentity());
    const double knee_x = kAiDenoiseCurveKnee / gain;
    // The gain alone up to the knee, also below 0.
    for (const double x : {-0.01, 0.0, 0.3 * knee_x, knee_x}) {
      EXPECT_DOUBLE_EQ(curve.Apply(x), gain * x) << x;
    }
    // The white level stays at 1; the highlights stay at or below it.
    EXPECT_NEAR(curve.Apply(1.0), 1.0, 1e-12);
    // A continuous slope at the knee and at the white level.
    const double h           = 1e-7;
    const auto   slope_below = [&](double x) { return (curve.Apply(x) - curve.Apply(x - h)) / h; };
    const auto   slope_above = [&](double x) { return (curve.Apply(x + h) - curve.Apply(x)) / h; };
    EXPECT_NEAR(slope_above(knee_x) / gain, 1.0, 1e-4);
    EXPECT_NEAR(slope_below(1.0) / curve.tail_slope, 1.0, 1e-4);
    EXPECT_NEAR(slope_above(1.0) / curve.tail_slope, 1.0, 1e-6);
    // Rising everywhere, and the inverse returns each value.
    double previous = curve.Apply(-0.1);
    for (int i = -99; i <= 2000; ++i) {
      const double x = i / 1000.0;
      const double y = curve.Apply(x);
      ASSERT_GT(y, previous) << x;
      if (x <= 1.0) {
        ASSERT_LE(y, 1.0 + 1e-12) << x;
      }
      ASSERT_NEAR(curve.Invert(y), x, 1e-12 * std::max(1.0, std::abs(x))) << x;
      previous = y;
    }
  }
}

TEST(AiDenoiseInputTest, InputCurveRaisesAPhotoBelowTheTrainingLevelToIt) {
  const auto curve_of_level = [](double level) {
    // The level is the mean of the 3 means.
    return AiDenoiseInputCurveFor({0.5 * level, level, 1.5 * level});
  };
  EXPECT_TRUE(curve_of_level(kAiDenoiseTargetInputLevel).IsIdentity());
  EXPECT_TRUE(curve_of_level(0.2).IsIdentity());
  EXPECT_NEAR(curve_of_level(kAiDenoiseTargetInputLevel / 4.0).gain, 4.0, 1e-12);
  EXPECT_NEAR(curve_of_level(kAiDenoiseTargetInputLevel / 11.0).gain, 11.0, 1e-12);
  // The largest gain for a very dark photo, a black frame, and a level below the black level.
  EXPECT_EQ(curve_of_level(kAiDenoiseTargetInputLevel / 100.0).gain, kAiDenoiseMaxInputGain);
  EXPECT_EQ(curve_of_level(0.0).gain, kAiDenoiseMaxInputGain);
  EXPECT_EQ(curve_of_level(-1e-4).gain, kAiDenoiseMaxInputGain);
}

TEST(AiDenoiseInputTest, InputCurveChangesThePlanesAndTheMeansOfTheArea) {
  // GBRG, 31 x 25: a row at the top and a column at the right are added by reflection; their
  // samples are not part of the means.
  const auto input    = MakeBayerInput(31, 25, BayerPattern("GBRG"));
  const auto original = PackBayerForAiDenoise(input);
  auto       mosaic   = original;
  const auto curve    = AiDenoiseInputCurveOfGain(6.0);
  ApplyAiDenoiseInputCurve(mosaic, curve);
  EXPECT_EQ(mosaic.curve.gain, 6.0);
  int wrong = 0;
  for (std::size_t i = 0; i < original.planes.size(); ++i) {
    wrong += mosaic.planes[i] != static_cast<float>(curve.Apply(original.planes[i])) ? 1 : 0;
  }
  EXPECT_EQ(wrong, 0);

  const auto plane     = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  double     sums[3]   = {};
  double     counts[3] = {};
  for (int ay = 0; ay < 25; ++ay) {
    for (int ax = 0; ax < 31; ++ax) {
      const int ey      = ay + mosaic.added_top;
      const int ex      = ax + mosaic.added_left;
      const int channel = (ey % 2) * 2 + ex % 2;
      const int color   = channel == 0 ? 0 : (channel == 3 ? 2 : 1);
      sums[color] +=
          mosaic.planes[channel * plane + static_cast<std::size_t>(ey / 2) * mosaic.width + ex / 2];
      counts[color] += 1.0;
    }
  }
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(mosaic.input_means[c], sums[c] / counts[c], 1e-12) << "color " << c;
  }
  // A mosaic takes one curve.
  EXPECT_THROW(ApplyAiDenoiseInputCurve(mosaic, curve), std::runtime_error);
  auto identity = original;
  ApplyAiDenoiseInputCurve(identity, AiDenoiseInputCurve{});
  EXPECT_EQ(identity.planes, original.planes);
  EXPECT_EQ(identity.input_means, original.input_means);
}

TEST(AiDenoiseInputTest, DenoisedPlaneInvertsTheInputCurve) {
  const auto input  = MakeBayerInput(31, 25, BayerPattern("BGGR"));
  auto       mosaic = PackBayerForAiDenoise(input);
  ApplyAiDenoiseInputCurve(mosaic, AiDenoiseInputCurveOfGain(4.0));
  const int          width  = mosaic.OutputWidth();
  const int          height = mosaic.OutputHeight();
  std::vector<float> data(static_cast<std::size_t>(3) * width * height);
  const auto         value = [&](int c, int y, int x) { return 1000.0f * c + 10.0f * y + x; };
  for (int c = 0; c < 3; ++c) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        data[(static_cast<std::size_t>(c) * height + y) * width + x] = value(c, y, x);
      }
    }
  }
  // The gain-matched output reaches 1.15: the line, the shoulder, and the line above 1.
  constexpr double kGain = 5e-4;
  const auto       plane =
      AssembleDenoisedPlane({data, width, height}, mosaic, kGain, input.linearization);
  const auto* pixels = reinterpret_cast<const float*>(plane.bytes.get());
  const auto& lin    = input.linearization;
  for (int y = 0; y < 25; ++y) {
    for (int x = 0; x < 31; ++x) {
      for (int c = 0; c < 3; ++c) {
        const double scale = (lin.libraw_maximum - lin.black_level[c]) /
                             (static_cast<double>(lin.white_level[c]) - lin.black_level[c]);
        const double expected =
            mosaic.curve.Invert(kGain * value(c, y + mosaic.added_top, x + mosaic.added_left)) *
            scale;
        ASSERT_NEAR(pixels[(static_cast<std::size_t>(y) * 31 + x) * 4 + c], expected,
                    1e-6 * std::abs(expected) + 1e-9)
            << "x " << x << " y " << y << " c " << c;
      }
    }
  }
}

TEST(AiDenoiseInputTest, GainMatchFactorEqualsTheLabValueOnTheExpectedCrop) {
  const auto folder = TestAssets();
  if (!folder) {
    GTEST_SKIP()
        << "Set ALCEDO_AI_DENOISE_TEST_ASSETS to the folder with the DN0 expected outputs.";
  }
  const auto  tensors = nn::LoadSafetensors(*folder / "rawnind_crop_512.safetensors");
  const auto& packed  = tensors.at("input");
  const auto& output  = tensors.at("output");
  const auto& matrix  = tensors.at("rgb_xyz_matrix");
  ASSERT_TRUE(packed.ShapeEquals({4, 512, 512}));
  ASSERT_TRUE(output.ShapeEquals({3, 1024, 1024}));
  ASSERT_TRUE(matrix.ShapeEquals({3, 3}));

  // The crop has no added row or column: the area is the whole output.
  AiDenoisePackedMosaic mosaic;
  mosaic.planes.assign(packed.data.begin(), packed.data.end());
  mosaic.width              = 512;
  mosaic.height             = 512;
  mosaic.area_width         = 1024;
  mosaic.area_height        = 1024;
  const std::size_t plane   = 512 * 512;
  double            sums[4] = {};
  for (int channel = 0; channel < 4; ++channel) {
    for (std::size_t i = 0; i < plane; ++i) {
      sums[channel] += mosaic.planes[channel * plane + i];
    }
  }
  mosaic.input_means = {sums[0] / plane, (sums[1] + sums[2]) / (2.0 * plane), sums[3] / plane};

  std::array<double, 9> xyz_to_camera{};
  for (int i = 0; i < 9; ++i) {
    xyz_to_camera[static_cast<std::size_t>(i)] = matrix.data[static_cast<std::size_t>(i)];
  }
  const auto   weights        = AiDenoiseGainWeights(xyz_to_camera);
  // Lab results/export/expected_outputs_manifest.json, gain_match_rawnind_crop_512.
  const double lab_weights[3] = {2.4918828617609377, 1.268977618475007, 0.797147114361554};
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(weights[c] / lab_weights[c], 1.0, 1e-5) << "weight " << c;
  }
  std::vector<float>      output_data(output.data.begin(), output.data.end());
  const nn::RgbPlanesView view{output_data, 1024, 1024};
  AiDenoiseOutputSums     output_sums(mosaic);
  output_sums.AddKept(view, {0, 0, 256, 512});
  output_sums.AddKept(view, {256, 0, 256, 512});
  const double factor = AiDenoiseGainFactor(weights, mosaic.input_means, output_sums.Means());
  EXPECT_NEAR(factor / 9.693665298685682e-06, 1.0, 1e-6) << factor;
}

TEST(AiDenoiseInputTest, PackedMosaicEqualsTheLabPreparationOfARawFile) {
  const auto                  folder = TestAssets();
  const std::filesystem::path raw_path(ALCEDO_AI_DENOISE_TEST_RAW);
  if (!folder || !std::filesystem::exists(*folder / "_DSC0726.packed.f32") ||
      !std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "Set ALCEDO_AI_DENOISE_TEST_ASSETS to a folder with the lab preparation of "
                    "_DSC0726.ARW (_DSC0726.packed.f32 and .txt).";
  }
  std::ifstream description(*folder / "_DSC0726.packed.txt");
  int           channels = 0, height = 0, width = 0, top = 0, left = 0, area_h = 0, area_w = 0;
  description >> channels >> height >> width >> top >> left >> area_h >> area_w;
  double lab_means[3]   = {};
  double lab_weights[3] = {};
  description >> lab_means[0] >> lab_means[1] >> lab_means[2] >> lab_weights[0] >> lab_weights[1] >>
      lab_weights[2];
  ASSERT_TRUE(description) << "the description of the lab preparation cannot be read";
  std::vector<float> lab(static_cast<std::size_t>(channels) * height * width);
  std::ifstream      packed_file(*folder / "_DSC0726.packed.f32", std::ios::binary);
  packed_file.read(reinterpret_cast<char*>(lab.data()),
                   static_cast<std::streamsize>(lab.size() * sizeof(float)));
  ASSERT_TRUE(packed_file);

  std::ifstream     raw_file(raw_path, std::ios::binary);
  std::vector<char> encoded((std::istreambuf_iterator<char>(raw_file)),
                            std::istreambuf_iterator<char>());
  const auto        input = RawInputLoader::LoadEncoded(
      std::as_bytes(std::span(encoded.data(), encoded.size())), DecodeRes::FULL);
  const auto  mosaic   = PackBayerForAiDenoise(input);
  // The lab packs the visible area of LibRaw; Alcedo packs its decode area, here the default crop
  // of the file inside the visible area (the area that the Develop render shows).
  const RectI decode   = input_detail::FullSizeDecodeArea(input.sensor, input.host_extent);
  const int   offset_x = decode.x - input.sensor.left_margin;
  const int   offset_y = decode.y - input.sensor.top_margin;
  std::printf("lab area %d x %d, Alcedo decode area %d x %d at (%d, %d) of the visible area\n",
              area_w, area_h, decode.width, decode.height, offset_x, offset_y);
  ASSERT_EQ(top, 0);
  ASSERT_EQ(left, 0);
  ASSERT_GE(offset_x, 0);
  ASSERT_GE(offset_y, 0);
  ASSERT_LE(offset_x + decode.width, area_w);
  ASSERT_LE(offset_y + decode.height, area_h);
  EXPECT_EQ(mosaic.area_width, decode.width);
  EXPECT_EQ(mosaic.area_height, decode.height);
  const auto alcedo_plane = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  const auto lab_plane    = static_cast<std::size_t>(width) * height;
  const auto value_of = [](const float* planes, std::size_t plane, int packed_width, int y, int x) {
    const int channel = (y % 2) * 2 + x % 2;
    return planes[channel * plane + static_cast<std::size_t>(y / 2) * packed_width + x / 2];
  };
  double largest_difference = 0.0;
  double sums[3]            = {};
  double counts[3]          = {};
  for (int ay = 0; ay < decode.height; ++ay) {
    for (int ax = 0; ax < decode.width; ++ax) {
      const float alcedo = value_of(mosaic.planes.data(), alcedo_plane, mosaic.width,
                                    ay + mosaic.added_top, ax + mosaic.added_left);
      const int   ly     = ay + offset_y;
      const int   lx     = ax + offset_x;
      const float value  = value_of(lab.data(), lab_plane, width, ly, lx);
      largest_difference =
          std::max(largest_difference, std::abs(static_cast<double>(alcedo) - value));
      // The lab mosaic starts with R: its color follows from the position.
      const int color = ly % 2 == 0 ? (lx % 2 == 0 ? 0 : 1) : (lx % 2 == 0 ? 1 : 2);
      sums[color] += value;
      counts[color] += 1.0;
    }
  }
  // The lab divides in float64 and rounds to float32; Alcedo divides in float32.
  EXPECT_LE(largest_difference, 1e-7) << largest_difference;
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(mosaic.input_means[c] / (sums[c] / counts[c]), 1.0, 1e-6) << "mean " << c;
    // The means of the whole visible area (the lab record) differ only by the border.
    EXPECT_NEAR(mosaic.input_means[c] / lab_means[c], 1.0, 0.02) << "visible area mean " << c;
  }
  // The lab weights come from LibRaw cam_xyz (rawpy rgb_xyz_matrix). Alcedo uses the D65 matrix of
  // the render (#1055): for the ILCE-7RM3 the ColorMatrix2 of camera_matrices.hpp, which is not
  // the matrix of the LibRaw table, so the weights differ from the lab by design. The weights are
  // those of the matrix of the render.
  const auto camera  = ai_denoise_test::ImportedXyzToCamera(raw_path);
  const auto weights = AiDenoiseGainWeights(camera);
  ASSERT_FALSE(ai_denoise_test::LibRawCamXyzIsSingular(raw_path));
  for (int c = 0; c < 3; ++c) {
    std::printf("weight %d: Alcedo %.6f, lab (LibRaw cam_xyz) %.6f\n", c, weights[c],
                lab_weights[c]);
    EXPECT_TRUE(std::isfinite(weights[c])) << "weight " << c;
  }
  EXPECT_GT(weights[0] + weights[1] + weights[2], 0.0);
}

}  // namespace
}  // namespace alcedo
