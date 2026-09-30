//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_diffusion_filter_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

auto operator+(Rgb lhs, Rgb rhs) -> Rgb { return {lhs.r + rhs.r, lhs.g + rhs.g, lhs.b + rhs.b}; }
auto operator*(Rgb lhs, double s) -> Rgb { return {lhs.r * s, lhs.g * s, lhs.b * s}; }

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto AcesccEncode(double value) -> float {
  constexpr double kA = 9.72;
  constexpr double kB = 17.52;
  if (value < 0.000030517578125) {
    return static_cast<float>((std::log2(0.0000152587890625 + value * 0.5) + kA) / kB);
  }
  return static_cast<float>((std::log2(value) + kA) / kB);
}

auto AcesccDecode(float value) -> double {
  constexpr double kA         = 9.72;
  constexpr double kB         = 17.52;
  constexpr double kFloor     = (-16.0 + kA) / kB;
  constexpr double kThreshold = (-15.0 + kA) / kB;
  if (value < kFloor) return value - kFloor;
  if (value <= kThreshold) return (std::exp2(value * kB - kA) - 0.0000152587890625) * 2.0;
  return std::exp2(value * kB - kA);
}

/// Double-precision reference of the CUDA scatter kernels on one linear image.
class ScatterReference {
 public:
  ScatterReference(std::vector<Rgb> linear, std::uint32_t width, std::uint32_t height,
                   const DiffusionFilterLayout& layout)
      : linear_(std::move(linear)), width_(width), height_(height), layout_(layout) {}

  auto Mix() const -> std::vector<Rgb> {
    const auto scatter = Scatter();
    const auto base    = layout_.extents[0];
    const auto inverse = 1.0 / static_cast<double>(1U << layout_.base_level);
    std::vector<Rgb> out(linear_.size());
    for (std::uint32_t y = 0; y < height_; ++y) {
      for (std::uint32_t x = 0; x < width_; ++x) {
        const auto glow   = SampleBSpline(scatter, base, (x + 0.5) * inverse, (y + 0.5) * inverse);
        const auto direct = linear_[static_cast<std::size_t>(y) * width_ + x];
        out[static_cast<std::size_t>(y) * width_ + x] =
            (direct * (1.0 - layout_.scatter_fraction) + glow * layout_.scatter_fraction) *
            layout_.transmission;
      }
    }
    return out;
  }

  auto Boost(Rgb value) const -> Rgb {
    value        = {std::max(value.r, 0.0), std::max(value.g, 0.0), std::max(value.b, 0.0)};
    const double peak = std::max(value.r, std::max(value.g, value.b));
    const double knee = layout_.highlight_knee;
    const double t    = std::clamp((peak - knee) / (1.0 - knee), 0.0, 1.0);
    return value * (1.0 + layout_.highlight_gain * t * t * (3.0 - 2.0 * t));
  }

 private:
  using Level = std::vector<Rgb>;

  static auto Fetch(const Level& level, ImageExtent extent, int x, int y) -> Rgb {
    x = std::clamp(x, 0, static_cast<int>(extent.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(extent.height) - 1);
    return level[static_cast<std::size_t>(y) * extent.width + x];
  }

  static auto Bilinear(const Level& level, ImageExtent extent, double px, double py) -> Rgb {
    const double fx = px - 0.5;
    const double fy = py - 0.5;
    const double x0 = std::floor(fx);
    const double y0 = std::floor(fy);
    const double ax = fx - x0;
    const double ay = fy - y0;
    const int    ix = static_cast<int>(x0);
    const int    iy = static_cast<int>(y0);
    const auto top =
        Fetch(level, extent, ix, iy) * (1.0 - ax) + Fetch(level, extent, ix + 1, iy) * ax;
    const auto bottom =
        Fetch(level, extent, ix, iy + 1) * (1.0 - ax) + Fetch(level, extent, ix + 1, iy + 1) * ax;
    return top * (1.0 - ay) + bottom * ay;
  }

  static auto SampleBSpline(const Level& level, ImageExtent extent, double px, double py) -> Rgb {
    const double fx = px - 0.5;
    const double fy = py - 0.5;
    const double x0 = std::floor(fx);
    const double y0 = std::floor(fy);
    auto weights = [](double t, double w[4]) {
      const double u = 1.0 - t;
      w[0]           = u * u * u / 6.0;
      w[1]           = (3.0 * t * t * t - 6.0 * t * t + 4.0) / 6.0;
      w[2]           = (-3.0 * t * t * t + 3.0 * t * t + 3.0 * t + 1.0) / 6.0;
      w[3]           = t * t * t / 6.0;
    };
    double wx[4];
    double wy[4];
    weights(fx - x0, wx);
    weights(fy - y0, wy);
    Rgb sum;
    for (int j = 0; j < 4; ++j) {
      for (int i = 0; i < 4; ++i) {
        sum = sum + Fetch(level, extent, static_cast<int>(x0) - 1 + i,
                          static_cast<int>(y0) - 1 + j) *
                        (wx[i] * wy[j]);
      }
    }
    return sum;
  }

  auto Scatter() const -> Level {
    const auto count = layout_.level_count;
    std::vector<Level> levels(count);
    const auto block = 1U << layout_.base_level;
    const auto base  = layout_.extents[0];
    levels[0].resize(static_cast<std::size_t>(base.width) * base.height);
    for (std::uint32_t y = 0; y < base.height; ++y) {
      for (std::uint32_t x = 0; x < base.width; ++x) {
        Rgb           sum;
        std::uint32_t samples = 0;
        for (std::uint32_t sy = y * block; sy < std::min((y + 1) * block, height_); ++sy) {
          for (std::uint32_t sx = x * block; sx < std::min((x + 1) * block, width_); ++sx) {
            sum = sum + Boost(linear_[static_cast<std::size_t>(sy) * width_ + sx]);
            ++samples;
          }
        }
        levels[0][static_cast<std::size_t>(y) * base.width + x] = sum * (1.0 / samples);
      }
    }
    for (std::uint32_t index = 1; index < count; ++index) {
      const auto from = layout_.extents[index - 1];
      const auto to   = layout_.extents[index];
      levels[index].resize(static_cast<std::size_t>(to.width) * to.height);
      for (std::uint32_t y = 0; y < to.height; ++y) {
        for (std::uint32_t x = 0; x < to.width; ++x) {
          const double cx = 2.0 * (x + 0.5);
          const double cy = 2.0 * (y + 0.5);
          auto s = [&](double ox, double oy) {
            return Bilinear(levels[index - 1], from, cx + ox, cy + oy);
          };
          const auto corners = s(-2, -2) + s(2, -2) + s(-2, 2) + s(2, 2);
          const auto edges   = s(0, -2) + s(-2, 0) + s(2, 0) + s(0, 2);
          const auto inner   = s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1);
          levels[index][static_cast<std::size_t>(y) * to.width + x] =
              s(0, 0) * 0.125 + corners * 0.03125 + edges * 0.0625 + inner * 0.125;
        }
      }
    }
    Level  coarse        = levels[count - 1];
    double coarse_weight = layout_.weights[count - 1];
    for (std::uint32_t index = count - 1; index-- > 0;) {
      const auto coarse_extent = layout_.extents[index + 1];
      const auto extent        = layout_.extents[index];
      Level      accumulated(static_cast<std::size_t>(extent.width) * extent.height);
      for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
          const double cx = (x + 0.5) * 0.5;
          const double cy = (y + 0.5) * 0.5;
          auto s = [&](double ox, double oy) {
            return Bilinear(coarse, coarse_extent, cx + ox, cy + oy);
          };
          const auto edges   = s(-1, 0) + s(1, 0) + s(0, -1) + s(0, 1);
          const auto corners = s(-1, -1) + s(1, -1) + s(-1, 1) + s(1, 1);
          const auto tent    = (s(0, 0) * 4.0 + edges * 2.0 + corners) * (1.0 / 16.0);
          const auto i       = static_cast<std::size_t>(y) * extent.width + x;
          accumulated[i]     = levels[index][i] * layout_.weights[index] + tent * coarse_weight;
        }
      }
      coarse        = std::move(accumulated);
      coarse_weight = 1.0;
    }
    return coarse;
  }

  std::vector<Rgb>      linear_;
  std::uint32_t         width_;
  std::uint32_t         height_;
  DiffusionFilterLayout layout_;
};

class CudaDiffusionFilterFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  }

  /// Run the pass on @p linear (encoded to ACEScc) and return the linear output.
  auto Run(const std::vector<Rgb>& linear, std::uint32_t width, std::uint32_t height,
           float strength) -> std::vector<Rgba> {
    auto document = CreateDefaultPipelineDocument();
    document.Drt()->Params().ApplyDiffusionStrength(strength);
    SetExtent(width, height);

    std::vector<Rgba> encoded(linear.size());
    for (std::size_t i = 0; i < linear.size(); ++i) {
      encoded[i] = {AcesccEncode(linear[i].r), AcesccEncode(linear[i].g),
                    AcesccEncode(linear[i].b), 0.75f};
    }
    device_.BeginRender();
    auto&             workspace = device_.Workspace();
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(
        lease.Texture(),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(encoded.data()),
                                   encoded.size() * sizeof(Rgba)),
        device_.CommandContext());
    workspace.EnsureSceneWorkImages({width, height});
    const auto output = ExecuteCudaDiffusionFilter(device_, plan_, document,
                                                   FrameSceneBinding::CachedImage(scene_id));
    EXPECT_TRUE(output.IsWorkImage());
    auto&             texture = CudaSceneTexture(device_, output);
    std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
    workspace.Device().DownloadTexture2D(
        texture,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_.CommandContext());
    device_.CancelRender();
    return pixels;
  }

  void SetExtent(std::uint32_t width, std::uint32_t height) {
    plan_.geometry.full_reference_extent = Extent2D{width, height};
    plan_.geometry.render_extent         = Extent2D{width, height};
  }

  auto Layout(std::uint32_t width, std::uint32_t height, float strength) -> DiffusionFilterLayout {
    SetExtent(width, height);
    return MakeDiffusionFilterLayout({width, height}, DiffusionShortSideRenderPixels(plan_.geometry),
                                     ResolveDiffusionFilterShape(strength));
  }

  ExecutionPlan    plan_;
  CudaRenderDevice device_;
};

auto PointLightField(std::uint32_t width, std::uint32_t height, std::uint32_t light_x,
                     std::uint32_t light_y) -> std::vector<Rgb> {
  std::vector<Rgb> field(static_cast<std::size_t>(width) * height, Rgb{0.02, 0.018, 0.015});
  field[static_cast<std::size_t>(light_y) * width + light_x] = Rgb{40.0, 32.0, 24.0};
  return field;
}

TEST_F(CudaDiffusionFilterFixture, ZeroStrengthDecodesAcesccToLinearAp1) {
  constexpr std::uint32_t kWidth  = 37;
  constexpr std::uint32_t kHeight = 23;
  std::vector<Rgb>        linear(static_cast<std::size_t>(kWidth) * kHeight);
  for (std::size_t i = 0; i < linear.size(); ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(linear.size());
    linear[i]      = {1.0e-6 + 8.0 * t * t, 0.18 * t, 2.0e-5 * (1.0 - t)};
  }
  const auto pixels = Run(linear, kWidth, kHeight, 0.0f);
  ASSERT_EQ(pixels.size(), linear.size());
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Rgba encoded{AcesccEncode(linear[i].r), AcesccEncode(linear[i].g),
                       AcesccEncode(linear[i].b), 0.75f};
    EXPECT_NEAR(pixels[i].r, AcesccDecode(encoded.r), 1.0e-5 * (1.0 + AcesccDecode(encoded.r)));
    EXPECT_NEAR(pixels[i].g, AcesccDecode(encoded.g), 1.0e-5 * (1.0 + AcesccDecode(encoded.g)));
    EXPECT_NEAR(pixels[i].b, AcesccDecode(encoded.b), 1.0e-5 * (1.0 + AcesccDecode(encoded.b)));
    EXPECT_EQ(pixels[i].a, 0.75f);
  }
}

TEST_F(CudaDiffusionFilterFixture, ActiveFilterMatchesCpuReferenceWithinTolerance) {
  constexpr std::uint32_t kWidth  = 173;
  constexpr std::uint32_t kHeight = 97;
  constexpr float         kStrength = 0.6f;
  auto                    linear  = PointLightField(kWidth, kHeight, 60, 40);
  linear[static_cast<std::size_t>(80) * kWidth + 150] = Rgb{0.9, 1.4, 0.7};
  for (std::size_t i = 0; i < linear.size(); i += 7) {
    linear[i] = linear[i] + Rgb{0.05, 0.1, 0.02};
  }
  const auto layout = Layout(kWidth, kHeight, kStrength);
  ASSERT_GT(layout.level_count, 2U);
  const auto expected = ScatterReference(linear, kWidth, kHeight, layout).Mix();
  const auto pixels   = Run(linear, kWidth, kHeight, kStrength);
  ASSERT_EQ(pixels.size(), expected.size());
  // The GPU reads float32 ACEScc and sums in float32.
  auto tolerance = [](double value) { return 2.0e-4 * (1.0 + std::fabs(value)); };
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    ASSERT_NEAR(pixels[i].r, expected[i].r, tolerance(expected[i].r)) << "pixel " << i;
    ASSERT_NEAR(pixels[i].g, expected[i].g, tolerance(expected[i].g)) << "pixel " << i;
    ASSERT_NEAR(pixels[i].b, expected[i].b, tolerance(expected[i].b)) << "pixel " << i;
    ASSERT_EQ(pixels[i].a, 0.75f);
  }
}

TEST_F(CudaDiffusionFilterFixture, ActiveFilterSpreadsPointLightIntoItsSurroundings) {
  constexpr std::uint32_t kWidth  = 256;
  constexpr std::uint32_t kHeight = 192;
  const auto              linear  = PointLightField(kWidth, kHeight, 128, 96);
  const auto              plain   = Run(linear, kWidth, kHeight, 0.0f);
  const auto              misted  = Run(linear, kWidth, kHeight, 1.0f);
  auto at = [&](const std::vector<Rgba>& image, std::uint32_t x, std::uint32_t y) {
    return image[static_cast<std::size_t>(y) * kWidth + x];
  };
  // The light stays the brightest pixel; the boosted glow spreads into the dark field.
  EXPECT_GT(at(misted, 128, 96).r, at(misted, 129, 96).r * 1.5f);
  for (const std::uint32_t distance : {8U, 12U}) {
    EXPECT_GT(at(misted, 128 + distance, 96).r, at(plain, 128 + distance, 96).r * 1.05f)
        << "distance " << distance;
  }
  // The glow falls off with distance.
  EXPECT_GT(at(misted, 132, 96).r, at(misted, 158, 96).r);
}

TEST_F(CudaDiffusionFilterFixture, ActiveFilterKeepsEnergyOfBoostedLight) {
  constexpr std::uint32_t kWidth    = 256;
  constexpr std::uint32_t kHeight   = 192;
  constexpr float         kStrength = 0.75f;
  const auto              linear    = PointLightField(kWidth, kHeight, 128, 96);
  const auto              layout    = Layout(kWidth, kHeight, kStrength);
  const ScatterReference  reference(linear, kWidth, kHeight, layout);
  double                  direct_energy  = 0.0;
  double                  boosted_energy = 0.0;
  for (const auto& value : linear) {
    direct_energy += value.r;
    boosted_energy += reference.Boost(value).r;
  }
  const double expected = layout.transmission * ((1.0 - layout.scatter_fraction) * direct_energy +
                                                 layout.scatter_fraction * boosted_energy);
  const auto   pixels   = Run(linear, kWidth, kHeight, kStrength);
  double       energy   = 0.0;
  for (const auto& pixel : pixels) {
    energy += pixel.r;
  }
  EXPECT_NEAR(energy, expected, expected * 0.01);
}

}  // namespace
}  // namespace alcedo
