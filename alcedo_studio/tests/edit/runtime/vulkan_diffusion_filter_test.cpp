//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DiffusionFilter pass on the Vulkan backend (#1036, phase VK12): the shared checks of the
// OpenCL test against the CPU reference (diffusion_filter_reference.hpp), the comparison with the
// same pass of the reference backend on the same GPU (decision B9: OpenCL on Windows, Metal on
// macOS), the published scatter image, and the recording rule.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "diffusion_filter_reference.hpp"
#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_diffusion_filter_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using namespace diffusion_filter_test;
using namespace vulkan_reference;

/// The largest relative difference of the DiffusionFilter output (linear AP1) from the reference
/// backend.
constexpr double kDiffusionTolerance = 1.0e-5;

/// Where the pass reads its ACEScc input: a cached image (the Develop or Camera Color output) or
/// a scene-work member (the output of a Grade).
enum class SceneInput { CachedImage, WorkMember };

/// The view of a @p render part of a @p reference frame; @p render_to_reference places it.
auto MakeView(Extent2D reference, Extent2D render, const Matrix3x3& render_to_reference)
    -> ExecutionPlan {
  ExecutionPlan plan;
  auto&         geometry         = plan.geometry;
  geometry.full_reference_extent = reference;
  geometry.edit_extent           = reference;
  geometry.render_extent         = render;
  geometry.reference_to_edit     = Matrix3x3::Identity();
  geometry.render_to_reference   = render_to_reference;
  geometry.reference_to_render   = InvertAffine(render_to_reference);
  geometry.edit_to_render        = geometry.reference_to_render;
  return plan;
}

auto FullFrame(std::uint32_t width, std::uint32_t height) -> ExecutionPlan {
  return MakeView({width, height}, {width, height}, Matrix3x3::Identity());
}

auto DocumentWithStrength(float strength) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  document.Drt()->Params().ApplyDiffusionStrength(strength);
  return document;
}

auto Encode(const std::vector<Rgb>& linear) -> std::vector<Rgba> {
  std::vector<Rgba> encoded(linear.size());
  for (std::size_t i = 0; i < linear.size(); ++i) {
    encoded[i] = {AcesccEncode(linear[i].r), AcesccEncode(linear[i].g), AcesccEncode(linear[i].b),
                  0.75F};
  }
  return encoded;
}

auto AsRgba(const std::vector<float>& values) -> std::vector<Rgba> {
  std::vector<Rgba> pixels(values.size() / 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = {values[4 * i], values[4 * i + 1], values[4 * i + 2], values[4 * i + 3]};
  }
  return pixels;
}

/**
 * The DiffusionFilter pass of @p device on @p linear (encoded to ACEScc) with the view of @p plan;
 * the RGBA32F values of its output. @p publish ends the render and publishes its results; else
 * the render is cancelled.
 */
template <class Device>
auto RenderDiffusion(Device& device, const ExecutionPlan& plan, const PipelineDocument& document,
                     const std::vector<Rgb>& linear, bool publish,
                     SceneInput input = SceneInput::CachedImage) -> std::vector<float> {
  const auto       width   = plan.geometry.render_extent.width;
  const auto       height  = plan.geometry.render_extent.height;
  const auto       encoded = Encode(linear);
  const auto       bytes   = std::as_bytes(std::span<const Rgba>(encoded));
  PreparedRawInput prepared;
  device.ResetPassStats();
  device.BeginRender();
  auto& workspace = device.Workspace();
  workspace.PrepareResultValidity(plan, document, prepared);
  workspace.EnsureSceneWorkImages({width, height});
  FrameSceneBinding scene{};
  if (input == SceneInput::CachedImage) {
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), bytes, device.CommandContext());
    scene = FrameSceneBinding::CachedImage(scene_id);
  } else {
    auto& member = workspace.SceneWork().Member(SceneWorkMember::Member1);
    if constexpr (requires { member.Storage(); }) {
      workspace.Device().UploadBufferRange(member.Storage(), 0, bytes, device.CommandContext());
    } else {
      workspace.Device().UploadTexture2D(member, bytes, device.CommandContext());
    }
    scene = FrameSceneBinding::WorkImage(SceneWorkMember::Member1);
  }
  FrameSceneBinding output{};
  if constexpr (std::is_same_v<Device, VulkanRenderDevice>) {
    output = PassEncoder<VulkanBackend, GpuPassKind::DiffusionFilter>::Encode(
        device, plan, prepared, document, scene);
  } else {
    output = ExecuteReferenceDiffusionFilter(device, plan, document, scene);
  }
  EXPECT_TRUE(output.IsWorkImage());
  if (input == SceneInput::WorkMember) {
    EXPECT_NE(output.member, SceneWorkMember::Member1) << "the output must be the free member";
  }
  device.EndRender();
  device.WaitIdle();
  auto values = DownloadScene(device, output);
  if (publish) {
    device.PublishResults();
  }
  return values;
}

class VulkanDiffusionFilterTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    device_ = std::make_unique<VulkanRenderDevice>();
  }

  void TearDown() override {
    device_.reset();
    VulkanWorkspaceFixture::TearDown();
  }

  auto Run(const std::vector<Rgb>& linear, std::uint32_t width, std::uint32_t height,
           float strength) -> std::vector<Rgba> {
    plan_ = FullFrame(width, height);
    return AsRgba(RenderDiffusion(*device_, plan_, DocumentWithStrength(strength), linear, false));
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

  auto Stats() const -> const GpuNodePassStats& { return device_->PassStats(); }

  std::unique_ptr<VulkanRenderDevice> device_;
  ExecutionPlan                       plan_;
};

/// The input of the comparisons: a point light, a bright patch, and a texture of every seventh
/// texel on a dark field.
auto ComparisonField(std::uint32_t width, std::uint32_t height) -> std::vector<Rgb> {
  auto linear = PointLightField(width, height, width / 3, height / 2);
  for (std::uint32_t y = height / 5; y < height / 5 + 6 && y < height; ++y) {
    for (std::uint32_t x = 2 * width / 3; x < 2 * width / 3 + 9 && x < width; ++x) {
      linear[static_cast<std::size_t>(y) * width + x] = Rgb{3.0, 2.0, 1.0};
    }
  }
  for (std::size_t i = 0; i < linear.size(); i += 7) {
    linear[i] = linear[i] + Rgb{0.05, 0.1, 0.02};
  }
  return linear;
}

// ---- The shared checks of the OpenCL test (opencl_diffusion_filter_test.cpp) ----------------

TEST_F(VulkanDiffusionFilterTest, ZeroStrengthDecodesAcesccToLinearAp1) {
  constexpr std::uint32_t kWidth  = 37;
  constexpr std::uint32_t kHeight = 23;
  std::vector<Rgb>        linear(static_cast<std::size_t>(kWidth) * kHeight);
  for (std::size_t i = 0; i < linear.size(); ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(linear.size());
    linear[i]      = {1.0e-6 + 8.0 * t * t, 0.18 * t, 2.0e-5 * (1.0 - t)};
  }
  const auto pixels = Run(linear, kWidth, kHeight, 0.0F);
  ASSERT_EQ(pixels.size(), linear.size());
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Rgba encoded{AcesccEncode(linear[i].r), AcesccEncode(linear[i].g),
                       AcesccEncode(linear[i].b), 0.75F};
    EXPECT_NEAR(pixels[i].r, AcesccDecode(encoded.r), 1.0e-5 * (1.0 + AcesccDecode(encoded.r)));
    EXPECT_NEAR(pixels[i].g, AcesccDecode(encoded.g), 1.0e-5 * (1.0 + AcesccDecode(encoded.g)));
    EXPECT_NEAR(pixels[i].b, AcesccDecode(encoded.b), 1.0e-5 * (1.0 + AcesccDecode(encoded.b)));
    EXPECT_EQ(pixels[i].a, 0.75F);
  }
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 0U);
  EXPECT_EQ(Stats().diffusion_scatter_sample, 0U);
}

TEST_F(VulkanDiffusionFilterTest, ActiveFilterMatchesCpuReferenceWithinTolerance) {
  constexpr std::uint32_t kWidth                      = 173;
  constexpr std::uint32_t kHeight                     = 97;
  constexpr float         kStrength                   = 0.6F;
  auto                    linear                      = PointLightField(kWidth, kHeight, 60, 40);
  linear[static_cast<std::size_t>(80) * kWidth + 150] = Rgb{0.9, 1.4, 0.7};
  for (std::size_t i = 0; i < linear.size(); i += 7) {
    linear[i] = linear[i] + Rgb{0.05, 0.1, 0.02};
  }
  plan_ = FullFrame(kWidth, kHeight);
  ASSERT_GT(Layout(kStrength).level_count, 2U);
  const auto expected = Reference(linear, kStrength).Mix();
  ExpectMatchesReference(Run(linear, kWidth, kHeight, kStrength), expected);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U);
}

TEST_F(VulkanDiffusionFilterTest, CappedCanvasMatchesCpuReferenceWithinTolerance) {
  // The long edge exceeds the canvas limit, so one base texel averages several render pixels.
  constexpr std::uint32_t kWidth                       = 2200;
  constexpr std::uint32_t kHeight                      = 240;
  constexpr float         kStrength                    = 0.8F;
  auto                    linear                       = PatchField(kWidth, kHeight, 1203, 117, 5);
  linear[static_cast<std::size_t>(40) * kWidth + 2100] = Rgb{3.0, 2.0, 1.0};
  plan_                                                = FullFrame(kWidth, kHeight);
  const auto layout                                    = Layout(kStrength);
  EXPECT_LT(layout.canvas_extent.width, kWidth);
  ASSERT_GT(MakeDiffusionScatterMapping(plan_.geometry, layout).reduce_samples, 1U);
  const auto expected = Reference(linear, kStrength).Mix();
  ExpectMatchesReference(Run(linear, kWidth, kHeight, kStrength), expected);
}

TEST_F(VulkanDiffusionFilterTest, ActiveFilterSpreadsPointLightIntoItsSurroundings) {
  constexpr std::uint32_t kWidth  = 256;
  constexpr std::uint32_t kHeight = 192;
  const auto              linear  = PointLightField(kWidth, kHeight, 128, 96);
  const auto              plain   = Run(linear, kWidth, kHeight, 0.0F);
  const auto              misted  = Run(linear, kWidth, kHeight, 1.0F);
  auto at = [&](const std::vector<Rgba>& image, std::uint32_t x, std::uint32_t y) {
    return image[static_cast<std::size_t>(y) * kWidth + x];
  };
  EXPECT_GT(at(misted, 128, 96).r, at(misted, 129, 96).r * 1.5F);
  for (const std::uint32_t distance : {8U, 12U}) {
    EXPECT_GT(at(misted, 128 + distance, 96).r, at(plain, 128 + distance, 96).r * 1.05F)
        << "distance " << distance;
  }
  EXPECT_GT(at(misted, 132, 96).r, at(misted, 158, 96).r);
}

TEST_F(VulkanDiffusionFilterTest, ActiveFilterKeepsEnergyOfBoostedLight) {
  constexpr std::uint32_t kWidth    = 256;
  constexpr std::uint32_t kHeight   = 192;
  constexpr float         kStrength = 0.75F;
  const auto              linear    = PointLightField(kWidth, kHeight, 128, 96);
  plan_                             = FullFrame(kWidth, kHeight);
  const auto layout                 = Layout(kStrength);
  const auto reference              = Reference(linear, kStrength);
  double     direct_energy          = 0.0;
  double     boosted_energy         = 0.0;
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

TEST_F(VulkanDiffusionFilterTest, RoiRenderSamplesThePublishedFullFrameScatter) {
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  constexpr std::uint32_t kRoiX    = 96;
  constexpr std::uint32_t kRoiY    = 40;
  constexpr std::uint32_t kRoiSize = 128;
  // The bright patch sits left of the ROI; its glow must still reach into the ROI.
  const auto              linear   = PatchField(kWidth, kHeight, 76, 90, 8);
  const auto              document = DocumentWithStrength(0.8F);

  plan_                            = FullFrame(kWidth, kHeight);
  const auto full = AsRgba(RenderDiffusion(*device_, plan_, document, linear, true));
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U);
  EXPECT_NE(device_->Workspace().Images().Find(DiffusionScatterId(plan_.drt.node_id)), nullptr);

  std::vector<Rgb> roi(static_cast<std::size_t>(kRoiSize) * kRoiSize);
  for (std::uint32_t y = 0; y < kRoiSize; ++y) {
    for (std::uint32_t x = 0; x < kRoiSize; ++x) {
      roi[static_cast<std::size_t>(y) * kRoiSize + x] =
          linear[static_cast<std::size_t>(y + kRoiY) * kWidth + x + kRoiX];
    }
  }
  plan_             = MakeView({kWidth, kHeight}, {kRoiSize, kRoiSize},
                               Matrix3x3::Translate(static_cast<float>(kRoiX), static_cast<float>(kRoiY)));
  const auto detail = AsRgba(RenderDiffusion(*device_, plan_, document, roi, true));
  EXPECT_EQ(Stats().diffusion_scatter_sample, 1U);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 0U);

  for (std::uint32_t y = 0; y < kRoiSize; ++y) {
    for (std::uint32_t x = 0; x < kRoiSize; ++x) {
      const auto& got  = detail[static_cast<std::size_t>(y) * kRoiSize + x];
      const auto& want = full[static_cast<std::size_t>(y + kRoiY) * kWidth + x + kRoiX];
      ASSERT_NEAR(got.r, want.r, 1.0e-5F * (1.0F + want.r)) << x << ", " << y;
      ASSERT_NEAR(got.g, want.g, 1.0e-5F * (1.0F + want.g)) << x << ", " << y;
      ASSERT_NEAR(got.b, want.b, 1.0e-5F * (1.0F + want.b)) << x << ", " << y;
    }
  }
  const auto background = linear[static_cast<std::size_t>(kRoiY) * kWidth + kRoiX].r;
  EXPECT_GT(detail[static_cast<std::size_t>(94 - kRoiY) * kRoiSize].r, background * 1.05);
}

TEST_F(VulkanDiffusionFilterTest, RoiRenderWithoutPublishedScatterDoesNotPublishOne) {
  constexpr std::uint32_t kWidth   = 128;
  constexpr std::uint32_t kHeight  = 96;
  const auto              document = DocumentWithStrength(0.5F);
  const auto              linear   = PatchField(64, 64, 20, 20, 4);
  plan_ = MakeView({kWidth, kHeight}, {64, 64}, Matrix3x3::Translate(32.0F, 16.0F));
  (void)RenderDiffusion(*device_, plan_, document, linear, true);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U);
  (void)RenderDiffusion(*device_, plan_, document, linear, true);
  EXPECT_EQ(Stats().diffusion_scatter_rebuild, 1U)
      << "an ROI-built scatter image must not become the full-frame image";
  EXPECT_EQ(Stats().diffusion_scatter_sample, 0U);
  EXPECT_EQ(device_->Workspace().Images().Find(DiffusionScatterId(plan_.drt.node_id)), nullptr);
}

TEST_F(VulkanDiffusionFilterTest, PreviewAndFullResolutionReadTheSameGlow) {
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  // The patch is aligned to the 2 x 2 preview blocks, so the preview input is exact.
  const auto              linear   = PatchField(kWidth, kHeight, 124, 92, 8);
  const auto              document = DocumentWithStrength(1.0F);

  plan_                            = FullFrame(kWidth, kHeight);
  const auto       full = AsRgba(RenderDiffusion(*device_, plan_, document, linear, false));

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
  plan_ = MakeView({kWidth, kHeight}, {kWidth / 2, kHeight / 2}, Matrix3x3::Scale(2.0F, 2.0F));
  const auto preview = AsRgba(RenderDiffusion(*device_, plan_, document, half, false));

  for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
    for (std::uint32_t x = 0; x < kWidth / 2; ++x) {
      if (x >= 58 && x < 70 && y >= 42 && y < 54) {
        continue;
      }
      float mean = 0.0F;
      for (std::uint32_t j = 0; j < 2; ++j) {
        for (std::uint32_t i = 0; i < 2; ++i) {
          mean += full[static_cast<std::size_t>(2 * y + j) * kWidth + 2 * x + i].r;
        }
      }
      mean *= 0.25F;
      ASSERT_NEAR(preview[static_cast<std::size_t>(y) * (kWidth / 2) + x].r, mean, mean * 0.03F)
          << x << ", " << y;
    }
  }
}

// ---- Against the reference backend on the same GPU (B9) ---------------------------------------

class VulkanDiffusionFilterComparisonTest : public VulkanDiffusionFilterTest {
 protected:
  void SetUp() override {
    VulkanDiffusionFilterTest::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

TEST_F(VulkanDiffusionFilterComparisonTest, FilterEqualsTheReferenceBackend) {
  struct Case {
    const char*   name;
    std::uint32_t width;
    std::uint32_t height;
    float         strength;
    SceneInput    input;
  };
  const std::vector<Case> cases = {
      {"strength 0 (decode), cached image", 203, 157, 0.0F, SceneInput::CachedImage},
      {"strength 0 (decode), scene-work member", 203, 157, 0.0F, SceneInput::WorkMember},
      {"strength 0.6, cached image", 173, 97, 0.6F, SceneInput::CachedImage},
      {"strength 1, scene-work member", 256, 192, 1.0F, SceneInput::WorkMember},
      {"strength 0.8, capped canvas 2200 x 240", 2200, 240, 0.8F, SceneInput::CachedImage},
  };
  for (const auto& c : cases) {
    const auto            linear   = ComparisonField(c.width, c.height);
    const auto            document = DocumentWithStrength(c.strength);
    const auto            plan     = FullFrame(c.width, c.height);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto vulkan = RenderDiffusion(vulkan_device, plan, document, linear, false, c.input);
    const auto reference =
        RenderDiffusion(reference_device, plan, document, linear, false, c.input);
    ExpectEqualToReference(c.name, reference, vulkan, c.width, Uniform(kDiffusionTolerance));
  }
}

TEST_F(VulkanDiffusionFilterComparisonTest, RegionAfterAFullFrameEqualsTheReferenceBackend) {
  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  constexpr std::uint32_t kRoiX    = 96;
  constexpr std::uint32_t kRoiY    = 40;
  constexpr std::uint32_t kRoiSize = 128;
  const auto              linear   = ComparisonField(kWidth, kHeight);
  const auto              document = DocumentWithStrength(0.8F);
  std::vector<Rgb>        roi(static_cast<std::size_t>(kRoiSize) * kRoiSize);
  for (std::uint32_t y = 0; y < kRoiSize; ++y) {
    for (std::uint32_t x = 0; x < kRoiSize; ++x) {
      roi[static_cast<std::size_t>(y) * kRoiSize + x] =
          linear[static_cast<std::size_t>(y + kRoiY) * kWidth + x + kRoiX];
    }
  }
  const auto full_plan = FullFrame(kWidth, kHeight);
  const auto region_plan =
      MakeView({kWidth, kHeight}, {kRoiSize, kRoiSize},
               Matrix3x3::Translate(static_cast<float>(kRoiX), static_cast<float>(kRoiY)));
  VulkanRenderDevice    vulkan_device;
  ReferenceRenderDevice reference_device;
  (void)RenderDiffusion(vulkan_device, full_plan, document, linear, true);
  (void)RenderDiffusion(reference_device, full_plan, document, linear, true);
  const auto vulkan = RenderDiffusion(vulkan_device, region_plan, document, roi, true);
  EXPECT_EQ(vulkan_device.PassStats().diffusion_scatter_sample, 1U);
  const auto reference = RenderDiffusion(reference_device, region_plan, document, roi, true);
  ExpectEqualToReference("region after a full frame", reference, vulkan, kRoiSize,
                         Uniform(kDiffusionTolerance));
}

// ---- Recording and failure rules --------------------------------------------------------------

TEST_F(VulkanDiffusionFilterTest, PassRecordsIntoTheOpenRecordingWithoutASubmission) {
  constexpr std::uint32_t kWidth   = 173;
  constexpr std::uint32_t kHeight  = 97;
  const auto              linear   = ComparisonField(kWidth, kHeight);
  const auto              document = DocumentWithStrength(0.6F);
  plan_                            = FullFrame(kWidth, kHeight);
  const auto layout                = Layout(0.6F);
  ASSERT_GT(layout.level_count, 2U);
  // The first render creates the pipelines.
  (void)RenderDiffusion(*device_, plan_, document, linear, false);

  auto&            backend = device_->Workspace().Device();
  PreparedRawInput prepared;
  device_->BeginRender();
  device_->Workspace().PrepareResultValidity(plan_, document, prepared);
  device_->Workspace().EnsureSceneWorkImages({kWidth, kHeight});
  const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
  (void)device_->Workspace().AcquireImageForWrite(scene_id,
                                                  {kWidth, kHeight, TextureFormat::Rgba32f});
  const auto submits = backend.SubmitCount();
  const auto waits   = backend.WaitCount();
  const auto before  = vulkan::ReadVulkanApiCounters();
  (void)ExecuteVulkanDiffusionFilter(*device_, plan_, document,
                                     FrameSceneBinding::CachedImage(scene_id));
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_TRUE(device_->CommandContext().IsRecording());
  EXPECT_EQ(backend.SubmitCount(), submits);
  EXPECT_EQ(backend.WaitCount(), waits);
  EXPECT_EQ(delta.submits, 0U);
  // The reduction, a downsample and an upsample for each further level, and the mix.
  EXPECT_EQ(delta.dispatches, 2U * layout.level_count);
  EXPECT_EQ(delta.pipeline_creations, 0U);
  EXPECT_EQ(delta.module_loads, 0U);
  device_->EndRender();
  device_->WaitIdle();
  device_->PublishResults();
}

TEST_F(VulkanDiffusionFilterTest, InvalidRequestsFailAndRecordNothing) {
  constexpr std::uint32_t kWidth   = 64;
  constexpr std::uint32_t kHeight  = 48;
  const auto              document = DocumentWithStrength(0.6F);
  plan_                            = FullFrame(kWidth, kHeight);
  const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
  const auto         scene = FrameSceneBinding::CachedImage(scene_id);

  // Outside BeginRender.
  EXPECT_THROW((void)ExecuteVulkanDiffusionFilter(*device_, plan_, document, scene),
               std::runtime_error);

  PreparedRawInput prepared;
  device_->BeginRender();
  auto& workspace = device_->Workspace();
  workspace.PrepareResultValidity(plan_, document, prepared);
  (void)workspace.AcquireImageForWrite(scene_id, {kWidth, kHeight, TextureFormat::Rgba32f});
  const auto       before = vulkan::ReadVulkanApiCounters();
  // No DRT endpoint.
  PipelineDocument empty;
  EXPECT_THROW((void)ExecuteVulkanDiffusionFilter(*device_, plan_, empty, scene),
               std::runtime_error);
  // Scene-work members of another extent.
  workspace.EnsureSceneWorkImages({kWidth / 2, kHeight});
  EXPECT_THROW((void)ExecuteVulkanDiffusionFilter(*device_, plan_, document, scene),
               std::runtime_error);
  // A missing scene image.
  workspace.EnsureSceneWorkImages({kWidth, kHeight});
  EXPECT_THROW((void)ExecuteVulkanDiffusionFilter(*device_, plan_, document,
                                                  FrameSceneBinding::CachedImage(GraphValueId{
                                                      NodeId{"missing"}, PortId{"image"}})),
               std::runtime_error);
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  device_->CancelRender();
}

}  // namespace
}  // namespace alcedo
