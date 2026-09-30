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
#include "edit/input/prepared_raw_input.hpp"
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

auto Apply(const Matrix3x3& matrix, double x, double y) -> std::pair<double, double> {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

/// Double-precision reference of the CUDA scatter kernels on one linear render.
class ScatterReference {
 public:
  ScatterReference(std::vector<Rgb> linear, std::uint32_t width, std::uint32_t height,
                   const DiffusionFilterLayout& layout, const DiffusionScatterMapping& mapping)
      : linear_(std::move(linear)),
        width_(width),
        height_(height),
        layout_(layout),
        mapping_(mapping) {}

  auto Mix() const -> std::vector<Rgb> {
    const auto       scatter = Scatter();
    const auto       base    = layout_.extents[0];
    std::vector<Rgb> out(linear_.size());
    for (std::uint32_t y = 0; y < height_; ++y) {
      for (std::uint32_t x = 0; x < width_; ++x) {
        const auto [bx, by] = Apply(mapping_.render_to_base, x + 0.5, y + 0.5);
        const auto glow     = SampleBSpline(scatter, base, bx, by);
        const auto direct   = linear_[static_cast<std::size_t>(y) * width_ + x];
        out[static_cast<std::size_t>(y) * width_ + x] =
            (direct * (1.0 - layout_.scatter_fraction) + glow * layout_.scatter_fraction) *
            layout_.transmission;
      }
    }
    return out;
  }

  auto Boost(Rgb value) const -> Rgb {
    value             = {std::max(value.r, 0.0), std::max(value.g, 0.0), std::max(value.b, 0.0)};
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
    const auto   top =
        Fetch(level, extent, ix, iy) * (1.0 - ax) + Fetch(level, extent, ix + 1, iy) * ax;
    const auto bottom =
        Fetch(level, extent, ix, iy + 1) * (1.0 - ax) + Fetch(level, extent, ix + 1, iy + 1) * ax;
    return top * (1.0 - ay) + bottom * ay;
  }

  static auto SampleBSpline(const Level& level, ImageExtent extent, double px, double py) -> Rgb {
    const double fx      = px - 0.5;
    const double fy      = py - 0.5;
    const double x0      = std::floor(fx);
    const double y0      = std::floor(fy);
    auto         weights = [](double t, double w[4]) {
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
    const auto         count = layout_.level_count;
    std::vector<Level> levels(count);
    const auto         base    = layout_.extents[0];
    const ImageExtent  render  = {width_, height_};
    const auto         samples = mapping_.reduce_samples;
    const double       step    = 1.0 / samples;
    levels[0].resize(static_cast<std::size_t>(base.width) * base.height);
    for (std::uint32_t y = 0; y < base.height; ++y) {
      for (std::uint32_t x = 0; x < base.width; ++x) {
        Rgb sum;
        for (std::uint32_t j = 0; j < samples; ++j) {
          for (std::uint32_t i = 0; i < samples; ++i) {
            const auto [rx, ry] =
                Apply(mapping_.base_to_render, x + (i + 0.5) * step, y + (j + 0.5) * step);
            sum = sum + Boost(Bilinear(linear_, render, rx, ry));
          }
        }
        levels[0][static_cast<std::size_t>(y) * base.width + x] = sum * (step * step);
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
          auto         s  = [&](double ox, double oy) {
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
          auto         s  = [&](double ox, double oy) {
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

  std::vector<Rgb>        linear_;
  std::uint32_t           width_;
  std::uint32_t           height_;
  DiffusionFilterLayout   layout_;
  DiffusionScatterMapping mapping_;
};

class CudaDiffusionFilterFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  }

  /// Render the full reference frame at its own resolution.
  void SetFullFrame(std::uint32_t width, std::uint32_t height) {
    SetView({width, height}, {width, height}, Matrix3x3::Identity());
  }

  /**
   * @brief Render @p render pixels of a @p reference frame. @p render_to_reference places the
   *        render in the frame: a scale is a preview resolution, a translation is an ROI.
   */
  void SetView(Extent2D reference, Extent2D render, const Matrix3x3& render_to_reference) {
    auto& geometry                 = plan_.geometry;
    geometry.full_reference_extent = reference;
    geometry.edit_extent           = reference;
    geometry.render_extent         = render;
    geometry.reference_to_edit     = Matrix3x3::Identity();
    geometry.render_to_reference   = render_to_reference;
    geometry.reference_to_render   = InvertAffine(render_to_reference);
    geometry.edit_to_render        = geometry.reference_to_render;
  }

  /**
   * @brief Run the pass on @p linear (encoded to ACEScc) with the current view.
   *
   * @p publish ends the render and publishes its results; otherwise the render is cancelled.
   */
  auto Render(const PipelineDocument& document, const std::vector<Rgb>& linear, bool publish)
      -> std::vector<Rgba> {
    const auto width  = plan_.geometry.render_extent.width;
    const auto height = plan_.geometry.render_extent.height;
    std::vector<Rgba> encoded(linear.size());
    for (std::size_t i = 0; i < linear.size(); ++i) {
      encoded[i] = {AcesccEncode(linear[i].r), AcesccEncode(linear[i].g),
                    AcesccEncode(linear[i].b), 0.75f};
    }
    device_.ResetPassStats();
    device_.BeginRender();
    auto& workspace = device_.Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
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
    if (publish) {
      device_.EndRender();
      device_.PublishResults();
    } else {
      device_.CancelRender();
    }
    return pixels;
  }

  /// Run a full-frame render of @p linear with a new document at @p strength.
  auto Run(const std::vector<Rgb>& linear, std::uint32_t width, std::uint32_t height,
           float strength) -> std::vector<Rgba> {
    auto document = CreateDefaultPipelineDocument();
    document.Drt()->Params().ApplyDiffusionStrength(strength);
    SetFullFrame(width, height);
    return Render(document, linear, false);
  }

  auto Layout(float strength) const -> DiffusionFilterLayout {
    return MakeDiffusionFilterLayout(DiffusionCanvasExtent(plan_.geometry.full_reference_extent),
                                     ResolveDiffusionFilterShape(strength));
  }

  auto Reference(const std::vector<Rgb>& linear, float strength) const -> ScatterReference {
    const auto layout = Layout(strength);
    return ScatterReference(linear, plan_.geometry.render_extent.width,
                            plan_.geometry.render_extent.height, layout,
                            MakeDiffusionScatterMapping(plan_.geometry, layout));
  }

  ExecutionPlan    plan_;
  PreparedRawInput input_;
  CudaRenderDevice device_;
};

auto PointLightField(std::uint32_t width, std::uint32_t height, std::uint32_t light_x,
                     std::uint32_t light_y) -> std::vector<Rgb> {
  std::vector<Rgb> field(static_cast<std::size_t>(width) * height, Rgb{0.02, 0.018, 0.015});
  field[static_cast<std::size_t>(light_y) * width + light_x] = Rgb{40.0, 32.0, 24.0};
  return field;
}

/// Dark field with a bright @p size x @p size patch whose top-left corner is (@p x, @p y).
auto PatchField(std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t y,
                std::uint32_t size) -> std::vector<Rgb> {
  std::vector<Rgb> field(static_cast<std::size_t>(width) * height, Rgb{0.02, 0.018, 0.015});
  for (std::uint32_t row = y; row < y + size; ++row) {
    for (std::uint32_t col = x; col < x + size; ++col) {
      field[static_cast<std::size_t>(row) * width + col] = Rgb{40.0, 32.0, 24.0};
    }
  }
  return field;
}

void ExpectMatchesReference(const std::vector<Rgba>& pixels, const std::vector<Rgb>& expected) {
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
  EXPECT_EQ(device_.PassStats().diffusion_scatter_rebuild, 0U);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_sample, 0U);
}

TEST_F(CudaDiffusionFilterFixture, ActiveFilterMatchesCpuReferenceWithinTolerance) {
  constexpr std::uint32_t kWidth    = 173;
  constexpr std::uint32_t kHeight   = 97;
  constexpr float         kStrength = 0.6f;
  auto                    linear    = PointLightField(kWidth, kHeight, 60, 40);
  linear[static_cast<std::size_t>(80) * kWidth + 150] = Rgb{0.9, 1.4, 0.7};
  for (std::size_t i = 0; i < linear.size(); i += 7) {
    linear[i] = linear[i] + Rgb{0.05, 0.1, 0.02};
  }
  SetFullFrame(kWidth, kHeight);
  ASSERT_GT(Layout(kStrength).level_count, 2U);
  const auto expected = Reference(linear, kStrength).Mix();
  ExpectMatchesReference(Run(linear, kWidth, kHeight, kStrength), expected);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_rebuild, 1U);
}

TEST_F(CudaDiffusionFilterFixture, CappedCanvasMatchesCpuReferenceWithinTolerance) {
  // The long edge exceeds the canvas limit, so one base texel averages several render pixels.
  constexpr std::uint32_t kWidth    = 2200;
  constexpr std::uint32_t kHeight   = 240;
  constexpr float         kStrength = 0.8f;
  auto                    linear    = PatchField(kWidth, kHeight, 1203, 117, 5);
  linear[static_cast<std::size_t>(40) * kWidth + 2100] = Rgb{3.0, 2.0, 1.0};
  SetFullFrame(kWidth, kHeight);
  const auto layout = Layout(kStrength);
  EXPECT_LT(layout.canvas_extent.width, kWidth);
  ASSERT_GT(MakeDiffusionScatterMapping(plan_.geometry, layout).reduce_samples, 1U);
  const auto expected = Reference(linear, kStrength).Mix();
  ExpectMatchesReference(Run(linear, kWidth, kHeight, kStrength), expected);
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
  SetFullFrame(kWidth, kHeight);
  const auto   layout         = Layout(kStrength);
  const auto   reference      = Reference(linear, kStrength);
  double       direct_energy  = 0.0;
  double       boosted_energy = 0.0;
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

TEST_F(CudaDiffusionFilterFixture, RoiRenderSamplesThePublishedFullFrameScatter) {
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  constexpr std::uint32_t kRoiX    = 96;
  constexpr std::uint32_t kRoiY    = 40;
  constexpr std::uint32_t kRoiSize = 128;
  // The bright patch sits left of the ROI; its glow must still reach into the ROI.
  const auto linear   = PatchField(kWidth, kHeight, 76, 90, 8);
  auto       document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(0.8f);

  SetFullFrame(kWidth, kHeight);
  const auto full = Render(document, linear, true);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_rebuild, 1U);

  std::vector<Rgb> roi(static_cast<std::size_t>(kRoiSize) * kRoiSize);
  for (std::uint32_t y = 0; y < kRoiSize; ++y) {
    for (std::uint32_t x = 0; x < kRoiSize; ++x) {
      roi[static_cast<std::size_t>(y) * kRoiSize + x] =
          linear[static_cast<std::size_t>(y + kRoiY) * kWidth + x + kRoiX];
    }
  }
  SetView({kWidth, kHeight}, {kRoiSize, kRoiSize},
          Matrix3x3::Translate(static_cast<float>(kRoiX), static_cast<float>(kRoiY)));
  const auto detail = Render(document, roi, true);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_sample, 1U);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_rebuild, 0U);

  for (std::uint32_t y = 0; y < kRoiSize; ++y) {
    for (std::uint32_t x = 0; x < kRoiSize; ++x) {
      const auto& got  = detail[static_cast<std::size_t>(y) * kRoiSize + x];
      const auto& want = full[static_cast<std::size_t>(y + kRoiY) * kWidth + x + kRoiX];
      ASSERT_NEAR(got.r, want.r, 1.0e-5f * (1.0f + want.r)) << x << ", " << y;
      ASSERT_NEAR(got.g, want.g, 1.0e-5f * (1.0f + want.g)) << x << ", " << y;
      ASSERT_NEAR(got.b, want.b, 1.0e-5f * (1.0f + want.b)) << x << ", " << y;
    }
  }
  // Light from outside the ROI scatters in at the ROI's left edge.
  const auto background = linear[static_cast<std::size_t>(kRoiY) * kWidth + kRoiX].r;
  EXPECT_GT(detail[static_cast<std::size_t>(94 - kRoiY) * kRoiSize].r, background * 1.05);
}

TEST_F(CudaDiffusionFilterFixture, RoiRenderWithoutPublishedScatterDoesNotPublishOne) {
  constexpr std::uint32_t kWidth  = 128;
  constexpr std::uint32_t kHeight = 96;
  auto                    document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(0.5f);
  const auto linear = PatchField(64, 64, 20, 20, 4);
  SetView({kWidth, kHeight}, {64, 64}, Matrix3x3::Translate(32.0f, 16.0f));
  (void)Render(document, linear, true);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_rebuild, 1U);
  (void)Render(document, linear, true);
  EXPECT_EQ(device_.PassStats().diffusion_scatter_rebuild, 1U)
      << "an ROI-built scatter image must not become the full-frame image";
  EXPECT_EQ(device_.PassStats().diffusion_scatter_sample, 0U);
}

TEST_F(CudaDiffusionFilterFixture, PreviewAndFullResolutionReadTheSameGlow) {
  constexpr std::uint32_t kWidth  = 256;
  constexpr std::uint32_t kHeight = 192;
  // The patch is aligned to the 2 x 2 preview blocks, so the preview input is exact.
  const auto linear   = PatchField(kWidth, kHeight, 124, 92, 8);
  auto       document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(1.0f);

  SetFullFrame(kWidth, kHeight);
  const auto full = Render(document, linear, false);

  std::vector<Rgb> half(static_cast<std::size_t>(kWidth / 2) * (kHeight / 2));
  for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
    for (std::uint32_t x = 0; x < kWidth / 2; ++x) {
      Rgb sum;
      for (std::uint32_t j = 0; j < 2; ++j) {
        for (std::uint32_t i = 0; i < 2; ++i) {
          sum = sum + linear[static_cast<std::size_t>(2 * y + j) * kWidth + 2 * x + i];
        }
      }
      half[static_cast<std::size_t>(y) * (kWidth / 2) + x] = sum * 0.25;
    }
  }
  SetView({kWidth, kHeight}, {kWidth / 2, kHeight / 2}, Matrix3x3::Scale(2.0f, 2.0f));
  const auto preview = Render(document, half, false);

  // Outside the patch the glow at a preview pixel equals the mean glow of its 2 x 2 block.
  for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
    for (std::uint32_t x = 0; x < kWidth / 2; ++x) {
      if (x >= 58 && x < 70 && y >= 42 && y < 54) {
        continue;
      }
      float mean = 0.0f;
      for (std::uint32_t j = 0; j < 2; ++j) {
        for (std::uint32_t i = 0; i < 2; ++i) {
          mean += full[static_cast<std::size_t>(2 * y + j) * kWidth + 2 * x + i].r;
        }
      }
      mean *= 0.25f;
      ASSERT_NEAR(preview[static_cast<std::size_t>(y) * (kWidth / 2) + x].r, mean, mean * 0.03f)
          << x << ", " << y;
    }
  }
}

}  // namespace
}  // namespace alcedo
