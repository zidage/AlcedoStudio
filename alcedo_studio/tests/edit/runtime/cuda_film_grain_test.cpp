//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the CUDA DRT/Post Film Grain (plan 'film_grain_format_and_stock_plan.md',
// phase G3). The expected values come from the lab evaluator (grain__expected.bin of
// ALCEDO_FILM_PRINT_EXPECTED_DIR) and from the host plan. No stored pixels.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_grain_plan.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

struct Rgba {
  float r;
  float g;
  float b;
  float a;
};

constexpr std::array<FilmFormat, 3>    kFormats{FilmFormat::Film16mm, FilmFormat::Film35mm,
                                             FilmFormat::Film70mm};
constexpr std::array<std::uint32_t, 2> kLongSides{2560, 6000};
constexpr FilmGrainDisplayEncoding     kSrgb{CE_TF_SRGB, 1.0f};

auto                                   HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

/**
 * A render of @p width x @p height in a 3:2 frame with the long side @p long_side. Render pixel
 * (0, 0) starts at reference pixel (@p origin_x, @p origin_y), and one render pixel covers
 * @p reference_per_render reference pixels on each axis.
 */
auto TileGeometry(std::uint32_t long_side, float reference_per_render, float origin_x,
                  float origin_y, std::uint32_t width, std::uint32_t height)
    -> ResolvedRenderGeometry {
  ResolvedRenderGeometry geometry;
  geometry.full_reference_extent    = {long_side, long_side * 2U / 3U};
  geometry.edit_extent              = geometry.full_reference_extent;
  geometry.render_extent            = {width, height};
  geometry.render_to_reference.m[0] = reference_per_render;
  geometry.render_to_reference.m[2] = origin_x;
  geometry.render_to_reference.m[4] = reference_per_render;
  geometry.render_to_reference.m[5] = origin_y;
  return geometry;
}

/// The linear display light of each channel of @p pixels, decoded with sRGB.
auto LinearChannel(const std::vector<Rgba>& pixels, int channel) -> std::vector<double> {
  std::vector<double> out(pixels.size());
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const float code = channel == 0 ? pixels[i].r : (channel == 1 ? pixels[i].g : pixels[i].b);
    out[i]           = static_cast<double>(CeSrgbDecode(code));
  }
  return out;
}

/// The density change -ln(y_out / y_in) of one channel: the relative error of the linear light.
auto DensityChange(const std::vector<Rgba>& input, const std::vector<Rgba>& output, int channel)
    -> std::vector<double> {
  const auto          in  = LinearChannel(input, channel);
  const auto          out = LinearChannel(output, channel);
  std::vector<double> change(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) change[i] = -std::log(out[i] / in[i]);
  return change;
}

auto Mean(const std::vector<double>& values) -> double {
  double sum = 0.0;
  for (const double value : values) sum += value;
  return sum / static_cast<double>(values.size());
}

auto StdDev(const std::vector<double>& values) -> double {
  const double mean = Mean(values);
  double       sum  = 0.0;
  for (const double value : values) sum += (value - mean) * (value - mean);
  return std::sqrt(sum / static_cast<double>(values.size()));
}

auto Correlation(const std::vector<double>& a, const std::vector<double>& b) -> double {
  const double ma = Mean(a);
  const double mb = Mean(b);
  double       ab = 0.0, aa = 0.0, bb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    ab += (a[i] - ma) * (b[i] - mb);
    aa += (a[i] - ma) * (a[i] - ma);
    bb += (b[i] - mb) * (b[i] - mb);
  }
  return ab / std::sqrt(aa * bb);
}

/// The horizontal lag-1 correlation of an image of @p width columns.
auto HorizontalLag1(const std::vector<double>& values, std::uint32_t width) -> double {
  std::vector<double> left;
  std::vector<double> right;
  for (std::size_t i = 0; i + 1 < values.size(); ++i) {
    if ((i + 1) % width == 0) continue;
    left.push_back(values[i]);
    right.push_back(values[i + 1]);
  }
  return Correlation(left, right);
}

/// The 2 x 2 box mean of an image of @p width x @p height.
auto BoxHalf(const std::vector<double>& values, std::uint32_t width, std::uint32_t height)
    -> std::vector<double> {
  std::vector<double> out(static_cast<std::size_t>(width / 2) * (height / 2));
  for (std::uint32_t y = 0; y < height / 2; ++y) {
    for (std::uint32_t x = 0; x < width / 2; ++x) {
      const auto at = [&](std::uint32_t xx, std::uint32_t yy) {
        return values[static_cast<std::size_t>(yy) * width + xx];
      };
      out[static_cast<std::size_t>(y) * (width / 2) + x] =
          0.25 * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                  at(2 * x + 1, 2 * y + 1));
    }
  }
  return out;
}

/// A display image of code values from a function of the reference pixel of each render pixel.
auto MakeDisplay(std::uint32_t width, std::uint32_t height, const ResolvedRenderGeometry& geometry,
                 const std::function<Rgba(float, float)>& at_reference) -> std::vector<Rgba> {
  std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
  const auto&       m = geometry.render_to_reference.m;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float rx = m[0] * (static_cast<float>(x) + 0.5f) + m[2];
      const float ry = m[4] * (static_cast<float>(y) + 0.5f) + m[5];
      pixels[static_cast<std::size_t>(y) * width + x] = at_reference(rx, ry);
    }
  }
  return pixels;
}

auto FlatSrgb(float linear) -> std::function<Rgba(float, float)> {
  const float code = CeSrgbEncode(linear);
  return [code](float, float) { return Rgba{code, code, code, 1.0f}; };
}

class CudaFilmGrainPostTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
#if defined(ALCEDO_FILM_PRINT_EXPECTED_DIR)
    const auto path = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "grain__expected.bin";
    if (fs::exists(path)) {
      expected_ =
          std::make_unique<FilmArrayFile>(FilmArrayFile::Parse(ReadFilmResourceBytes(path), path));
    }
#endif
  }
  static void TearDownTestSuite() { expected_.reset(); }

  void        SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string();
    }
  }

  void RequireLabValues() {
    if (expected_ == nullptr) GTEST_SKIP() << "No lab grain__expected.bin. This is not a pass.";
  }

  static auto Plan(const ResolvedRenderGeometry& geometry, FilmFormat format, float strength = 1.0f)
      -> FilmGrainPlan {
    return MakeFilmGrainPlan(geometry, format,
                             SharedFilmPrintTables().Bundle().Grain(kFilmGrainPostFilm),
                             FilmGrainTarget::Post, strength);
  }

  auto Render(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
              const std::vector<Rgba>& source, FilmGrainDisplayEncoding encoding = kSrgb)
      -> std::vector<Rgba> {
    const auto width  = geometry.render_extent.width;
    const auto height = geometry.render_extent.height;
    device_.BeginRender();
    auto& workspace = device_.Workspace();
    auto  src       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
    auto  dst       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(
        src.Texture(),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(source.data()),
                                   source.size() * sizeof(Rgba)),
        device_.CommandContext());
    RecordCudaFilmGrainPost(device_, plan, geometry, encoding, src.Texture(), dst.Texture(), width,
                            height);
    std::vector<Rgba> output(source.size());
    workspace.Device().DownloadTexture2D(
        dst.Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(output.data()),
                             output.size() * sizeof(Rgba)),
        device_.CommandContext());
    device_.EndRender();
    device_.WaitIdle();
    return output;
  }

  CudaRenderDevice                             device_;
  static inline std::unique_ptr<FilmArrayFile> expected_;
};

TEST_F(CudaFilmGrainPostTest, PixelSigmaAtGreyMatchesTheEvaluatorForEachFormatAndSize) {
  RequireLabValues();
  const auto&             file          = *expected_;
  const auto              sides         = file.I32("long_sides", {2});
  const auto              pixel_factor  = file.F64("pixel_factor", {3, 2, 3});
  const auto              post_pixel    = file.F64("post_pixel_sigma_linear", {3, 2, 3});
  const auto              post_value    = file.F64("post_display_value_linear", {3});
  const auto              post_sigma_48 = file.F64("post_display_sigma_48_linear", {3});
  const auto              lag1_kernel   = file.F64("lag1_kernel", {3, 2, 3});
  constexpr std::uint32_t kSize         = 512;
  for (std::size_t f = 0; f < kFormats.size(); ++f) {
    for (std::size_t s = 0; s < kLongSides.size(); ++s) {
      ASSERT_EQ(static_cast<std::uint32_t>(sides[s]), kLongSides[s]);
      const auto geometry = TileGeometry(kLongSides[s], 1.0f, 600.0f, 400.0f, kSize, kSize);
      const auto plan     = Plan(geometry, kFormats[f]);
      const auto input =
          MakeDisplay(kSize, kSize, geometry, FlatSrgb(static_cast<float>(plan.post_grey)));
      const auto output = Render(plan, geometry, input);
      for (int channel = 0; channel < 3; ++channel) {
        SCOPED_TRACE(std::string(FilmFormatId(kFormats[f])) + " " + std::to_string(kLongSides[s]) +
                     " channel " + std::to_string(channel));
        const auto   index = (f * 2 + s) * 3 + static_cast<std::size_t>(channel);
        // Green is the evaluator value of the post path. Red and blue have the #660 ratio of the
        // scans at the 48 um aperture, at the pixel factor of the evaluator for their layer.
        const double expected =
            channel == 1 ? post_pixel[index] / post_value[1]
                         : kFilmGrainPostChannelRatio[static_cast<std::size_t>(channel)] *
                               post_sigma_48[1] / post_value[1] * pixel_factor[index];
        const auto change = DensityChange(input, output, channel);
        EXPECT_NEAR(StdDev(change) / expected, 1.0, 0.05);
        // The lag-1 of the mixed field: sum over k of L_jk^2 times the lag-1 of layer k.
        double lag1 = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
          const double l = plan.mix[static_cast<std::size_t>(channel) * 3 + k];
          lag1 += l * l * lag1_kernel[(f * 2 + s) * 3 + k];
        }
        EXPECT_NEAR(HorizontalLag1(change, kSize), lag1, 0.03);
      }
    }
  }
}

TEST_F(CudaFilmGrainPostTest, HalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled) {
  // Issue 674: the fit view showed about 4 times the grain of the full image at the same size.
  constexpr std::uint32_t kFull = 1024;
  for (const auto format : {FilmFormat::Film16mm, FilmFormat::Film35mm}) {
    const auto full_geometry = TileGeometry(6000, 1.0f, 512.0f, 512.0f, kFull, kFull);
    const auto half_geometry = TileGeometry(6000, 2.0f, 512.0f, 512.0f, kFull / 2, kFull / 2);
    const auto full_plan     = Plan(full_geometry, format);
    const auto half_plan     = Plan(half_geometry, format);
    ASSERT_EQ(half_plan.aggregate, 2U);
    const auto grey        = FlatSrgb(static_cast<float>(full_plan.post_grey));
    const auto full_input  = MakeDisplay(kFull, kFull, full_geometry, grey);
    const auto half_input  = MakeDisplay(kFull / 2, kFull / 2, half_geometry, grey);
    const auto full_output = Render(full_plan, full_geometry, full_input);
    const auto half_output = Render(half_plan, half_geometry, half_input);
    for (int channel = 0; channel < 3; ++channel) {
      SCOPED_TRACE(std::string(FilmFormatId(format)) + " channel " + std::to_string(channel));
      const auto boxed = BoxHalf(DensityChange(full_input, full_output, channel), kFull, kFull);
      const auto half  = DensityChange(half_input, half_output, channel);
      EXPECT_NEAR(StdDev(half) / StdDev(boxed), 1.0, 0.05);
      EXPECT_GT(Correlation(half, boxed), 0.9);
    }
  }
}

TEST_F(CudaFilmGrainPostTest, ViewportRegionEqualsTheSameRegionOfTheFullRender) {
  // Issue 687: a viewport region hashed its render coordinates, so it had another grain pattern.
  constexpr std::uint32_t kFull   = 1024;
  constexpr std::uint32_t kRegion = 512;
  // Not a multiple of the 16 x 16 tile, so the tiles of the two renders do not line up and the
  // halo of a tile edge must read the noise of the neighbour tile.
  constexpr std::uint32_t kLeft   = 250;
  constexpr std::uint32_t kTop    = 317;
  const auto              ramp    = [](float rx, float ry) {
    const float value = 0.15f + 0.6f * rx / 1024.0f;
    return Rgba{value, value * 0.9f + 0.05f * ry / 1024.0f, value * 0.8f, 1.0f};
  };
  const auto full_geometry   = TileGeometry(6000, 1.0f, 0.0f, 0.0f, kFull, kFull);
  const auto region_geometry = TileGeometry(6000, 1.0f, static_cast<float>(kLeft),
                                            static_cast<float>(kTop), kRegion, kRegion);
  const auto full_output     = Render(Plan(full_geometry, FilmFormat::Film16mm), full_geometry,
                                      MakeDisplay(kFull, kFull, full_geometry, ramp));
  const auto region_output   = Render(Plan(region_geometry, FilmFormat::Film16mm), region_geometry,
                                      MakeDisplay(kRegion, kRegion, region_geometry, ramp));
  float      largest         = 0.0f;
  for (std::uint32_t y = 0; y < kRegion; ++y) {
    for (std::uint32_t x = 0; x < kRegion; ++x) {
      const auto& a = region_output[static_cast<std::size_t>(y) * kRegion + x];
      const auto& b = full_output[static_cast<std::size_t>(y + kTop) * kFull + x + kLeft];
      largest = std::max({largest, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
    }
  }
  EXPECT_LE(largest, 1.0e-5f);
}

TEST_F(CudaFilmGrainPostTest, ChannelCorrelationAtGreyIsTheScanCorrelationOf660) {
  constexpr std::uint32_t kSize    = 512;
  const auto              geometry = TileGeometry(6000, 1.0f, 300.0f, 300.0f, kSize, kSize);
  const auto              plan     = Plan(geometry, FilmFormat::Film35mm);
  const auto              input =
      MakeDisplay(kSize, kSize, geometry, FlatSrgb(static_cast<float>(plan.post_grey)));
  const auto output = Render(plan, geometry, input);
  const auto r      = DensityChange(input, output, 0);
  const auto g      = DensityChange(input, output, 1);
  const auto b      = DensityChange(input, output, 2);
  EXPECT_NEAR(Correlation(r, g), kFilmGrainPostScanCorrelation[0], 0.03);
  EXPECT_NEAR(Correlation(g, b), kFilmGrainPostScanCorrelation[1], 0.03);
  EXPECT_NEAR(Correlation(r, b), kFilmGrainPostScanCorrelation[2], 0.03);
  // The green grain is the quietest, as in the scans.
  EXPECT_LT(StdDev(g), StdDev(r));
  EXPECT_LT(StdDev(g), StdDev(b));
}

TEST_F(CudaFilmGrainPostTest, St2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput) {
  constexpr std::uint32_t kSize    = 256;
  const auto              geometry = TileGeometry(6000, 1.0f, 300.0f, 300.0f, kSize, kSize);
  const auto              plan     = Plan(geometry, FilmFormat::Film35mm);
  const float             grey     = static_cast<float>(plan.post_grey);
  const auto              srgb_in  = MakeDisplay(kSize, kSize, geometry, FlatSrgb(grey));
  // The same light in ST 2084: 1.0 relative is the reference white of 100 nits.
  const float             pq_code  = CePqEncode(grey * 100.0f / CE_PQ_PEAK_NITS);
  const auto              pq_in    = MakeDisplay(kSize, kSize, geometry, [pq_code](float, float) {
    return Rgba{pq_code, pq_code, pq_code, 1.0f};
  });
  const FilmGrainDisplayEncoding pq{CE_TF_ST2084, CE_PQ_PEAK_NITS / 100.0f};
  const auto                     srgb_out = Render(plan, geometry, srgb_in);
  const auto                     pq_out   = Render(plan, geometry, pq_in, pq);
  for (int channel = 0; channel < 3; ++channel) {
    const auto srgb_change = DensityChange(srgb_in, srgb_out, channel);
    double     largest     = 0.0;
    for (std::size_t i = 0; i < pq_out.size(); ++i) {
      const float  code = channel == 0 ? pq_out[i].r : (channel == 1 ? pq_out[i].g : pq_out[i].b);
      const double pq_change = -std::log(static_cast<double>(CePqDecode(code)) /
                                         static_cast<double>(CePqDecode(pq_code)));
      largest                = std::max(largest, std::abs(pq_change - srgb_change[i]));
    }
    EXPECT_LT(largest, 2.0e-3) << "channel " << channel;
  }
}

TEST_F(CudaFilmGrainPostTest, KernelEqualsTheHostFormOfItsStepsAtTheTileEdges) {
  // 16mm at 6000 px, rendered at half size. The render of 37 x 29 has partial tiles at the right
  // and the bottom, and a fit-view aggregate of 2.
  constexpr std::uint32_t kWidth   = 37;
  constexpr std::uint32_t kHeight  = 29;
  const auto              geometry = TileGeometry(6000, 2.0f, 101.0f, 57.0f, kWidth, kHeight);
  const auto              plan     = Plan(geometry, FilmFormat::Film16mm, 1.6f);
  const auto              field    = MakeFilmGrainFieldParams(plan, geometry);
  const auto              post     = MakeFilmGrainPostParams(plan, kSrgb.transfer, 1.0f);
  const auto              ramp     = [](float rx, float ry) {
    const float value = 0.1f + 0.7f * (rx - 101.0f) / 74.0f;
    return Rgba{value, value * 0.85f + 0.002f * (ry - 57.0f), value * 0.7f, 1.0f};
  };
  const auto input  = MakeDisplay(kWidth, kHeight, geometry, ramp);
  const auto output = Render(plan, geometry, input);
  const int  halo   = field.halo;
  ASSERT_GE(halo, 1);
  // The steps on the host: noise, horizontal blur, vertical blur, unit field, mix, apply.
  const auto noise      = [&](int x, int y) { return FilmGrainAggregateNoise(field.noise, x, y); };
  const auto channel_of = [](const FilmGrainNoise3& value, int channel) {
    return channel == 0 ? value.x : (channel == 1 ? value.y : value.z);
  };
  float largest = 0.0f;
  for (int y = 0; y < static_cast<int>(kHeight); ++y) {
    for (int x = 0; x < static_cast<int>(kWidth); ++x) {
      std::array<float, 3> unit{};
      for (int channel = 0; channel < 3; ++channel) {
        float sum = 0.0f;
        for (int dy = -halo; dy <= halo; ++dy) {
          float row = 0.0f;
          for (int dx = -halo; dx <= halo; ++dx) {
            row +=
                channel_of(noise(x + dx, y + dy), channel) * field.weights[channel][std::abs(dx)];
          }
          sum += row * field.weights[channel][std::abs(dy)];
        }
        unit[static_cast<std::size_t>(channel)] = sum * field.inverse_weight_square_sum[channel];
      }
      const auto  grain    = FilmGrainMix(field.mix, FilmGrainNoise3{unit[0], unit[1], unit[2]});
      const auto& source   = input[static_cast<std::size_t>(y) * kWidth + x];
      const auto  expected = FilmGrainPostPixel(post, {source.r, source.g, source.b}, grain);
      const auto& actual   = output[static_cast<std::size_t>(y) * kWidth + x];
      largest = std::max({largest, std::abs(actual.r - expected.r), std::abs(actual.g - expected.g),
                          std::abs(actual.b - expected.b)});
      ASSERT_EQ(actual.a, source.a);
    }
  }
  EXPECT_LE(largest, 2.0e-5f);
}

TEST_F(CudaFilmGrainPostTest, DisabledPlanIsRejectedBeforeAnyKernel) {
  const auto geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, 64, 64);
  const auto plan     = Plan(geometry, FilmFormat::Film35mm, 0.0f);
  device_.BeginRender();
  auto& workspace = device_.Workspace();
  auto  src       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  auto  dst       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  EXPECT_THROW(
      RecordCudaFilmGrainPost(device_, plan, geometry, kSrgb, src.Texture(), dst.Texture(), 64, 64),
      std::invalid_argument);
  device_.EndRender();
}

}  // namespace
}  // namespace alcedo
