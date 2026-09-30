//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "diffusion_filter_reference.hpp"
#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_diffusion_filter_pass.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

using namespace diffusion_filter_test;

class OpenClDiffusionFilterFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime() || !OpenClContext::Instance().Capabilities().image_support) {
      GTEST_SKIP() << "No OpenCL image device available.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
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
    const auto        width  = plan_.geometry.render_extent.width;
    const auto        height = plan_.geometry.render_extent.height;
    std::vector<Rgba> encoded(linear.size());
    for (std::size_t i = 0; i < linear.size(); ++i) {
      encoded[i] = {AcesccEncode(linear[i].r), AcesccEncode(linear[i].g), AcesccEncode(linear[i].b),
                    0.75f};
    }
    auto& device = *device_;
    device.ResetPassStats();
    device.BeginRender();
    auto& workspace = device.Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(
        lease.Texture(),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(encoded.data()),
                                   encoded.size() * sizeof(Rgba)),
        device.CommandContext());
    workspace.EnsureSceneWorkImages({width, height});
    const auto output = ExecuteOpenClDiffusionFilter(device, plan_, document,
                                                     FrameSceneBinding::CachedImage(scene_id));
    EXPECT_TRUE(output.IsWorkImage());
    auto&             image = workspace.SceneWork().Member(output.member);
    std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
    workspace.Device().DownloadBufferRange(
        image.Storage(), 0,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device.CommandContext());
    if (publish) {
      device.EndRender();
      device.WaitIdle();
      device.PublishResults();
    } else {
      device.CancelRender();
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

  auto             Stats() const -> const GpuNodePassStats& { return device_->PassStats(); }

  ExecutionPlan    plan_;
  PreparedRawInput input_;
  std::unique_ptr<OpenClRenderDevice> device_;
};

TEST_F(OpenClDiffusionFilterFixture, ZeroStrengthDecodesAcesccToLinearAp1) {
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
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 0U);
  EXPECT_EQ(Stats().diffusion_scatter_sample, 0U);
}

TEST_F(OpenClDiffusionFilterFixture, ActiveFilterMatchesCpuReferenceWithinTolerance) {
  constexpr std::uint32_t kWidth                      = 173;
  constexpr std::uint32_t kHeight                     = 97;
  constexpr float         kStrength                   = 0.6f;
  auto                    linear                      = PointLightField(kWidth, kHeight, 60, 40);
  linear[static_cast<std::size_t>(80) * kWidth + 150] = Rgb{0.9, 1.4, 0.7};
  for (std::size_t i = 0; i < linear.size(); i += 7) {
    linear[i] = linear[i] + Rgb{0.05, 0.1, 0.02};
  }
  SetFullFrame(kWidth, kHeight);
  ASSERT_GT(Layout(kStrength).level_count, 2U);
  const auto expected = Reference(linear, kStrength).Mix();
  ExpectMatchesReference(Run(linear, kWidth, kHeight, kStrength), expected);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U);
}

TEST_F(OpenClDiffusionFilterFixture, CappedCanvasMatchesCpuReferenceWithinTolerance) {
  // The long edge exceeds the canvas limit, so one base texel averages several render pixels.
  constexpr std::uint32_t kWidth                       = 2200;
  constexpr std::uint32_t kHeight                      = 240;
  constexpr float         kStrength                    = 0.8f;
  auto                    linear                       = PatchField(kWidth, kHeight, 1203, 117, 5);
  linear[static_cast<std::size_t>(40) * kWidth + 2100] = Rgb{3.0, 2.0, 1.0};
  SetFullFrame(kWidth, kHeight);
  const auto layout = Layout(kStrength);
  EXPECT_LT(layout.canvas_extent.width, kWidth);
  ASSERT_GT(MakeDiffusionScatterMapping(plan_.geometry, layout).reduce_samples, 1U);
  const auto expected = Reference(linear, kStrength).Mix();
  ExpectMatchesReference(Run(linear, kWidth, kHeight, kStrength), expected);
}

TEST_F(OpenClDiffusionFilterFixture, ActiveFilterSpreadsPointLightIntoItsSurroundings) {
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

TEST_F(OpenClDiffusionFilterFixture, ActiveFilterKeepsEnergyOfBoostedLight) {
  constexpr std::uint32_t kWidth    = 256;
  constexpr std::uint32_t kHeight   = 192;
  constexpr float         kStrength = 0.75f;
  const auto              linear    = PointLightField(kWidth, kHeight, 128, 96);
  SetFullFrame(kWidth, kHeight);
  const auto layout         = Layout(kStrength);
  const auto reference      = Reference(linear, kStrength);
  double     direct_energy  = 0.0;
  double     boosted_energy = 0.0;
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

TEST_F(OpenClDiffusionFilterFixture, RoiRenderSamplesThePublishedFullFrameScatter) {
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  constexpr std::uint32_t kRoiX    = 96;
  constexpr std::uint32_t kRoiY    = 40;
  constexpr std::uint32_t kRoiSize = 128;
  // The bright patch sits left of the ROI; its glow must still reach into the ROI.
  const auto              linear   = PatchField(kWidth, kHeight, 76, 90, 8);
  auto                    document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(0.8f);

  SetFullFrame(kWidth, kHeight);
  const auto full = Render(document, linear, true);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U);

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
  EXPECT_EQ(Stats().diffusion_scatter_sample, 1U);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 0U);

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

TEST_F(OpenClDiffusionFilterFixture, RoiRenderWithoutPublishedScatterDoesNotPublishOne) {
  constexpr std::uint32_t kWidth   = 128;
  constexpr std::uint32_t kHeight  = 96;
  auto                    document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(0.5f);
  const auto linear = PatchField(64, 64, 20, 20, 4);
  SetView({kWidth, kHeight}, {64, 64}, Matrix3x3::Translate(32.0f, 16.0f));
  (void)Render(document, linear, true);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U);
  (void)Render(document, linear, true);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U)
      << "an ROI-built scatter image must not become the full-frame image";
  EXPECT_EQ(Stats().diffusion_scatter_sample, 0U);
}

TEST_F(OpenClDiffusionFilterFixture, PreviewAndFullResolutionReadTheSameGlow) {
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  // The patch is aligned to the 2 x 2 preview blocks, so the preview input is exact.
  const auto              linear   = PatchField(kWidth, kHeight, 124, 92, 8);
  auto                    document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(1.0f);

  SetFullFrame(kWidth, kHeight);
  const auto       full = Render(document, linear, false);

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
