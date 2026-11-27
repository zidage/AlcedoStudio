//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Vulkan highlight reconstruction against the product OpenCL functions on the same GPU (plan
// phase VK5, decision B9), on the clipped highlights of a real X-Trans RAW (RGBA form) and of a
// real Bayer RAW (planar form with the pack). The input of each stage is the OpenCL output of the
// stages before it, as in VK4. Each test runs with the validation layer. Set ALCEDO_VULKAN_DEVICE
// (for example "Iris") to select the device; the OpenCL device follows it.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../edit/runtime/vulkan_workspace_test_support.hpp"
#include "../vulkan/vulkan_opencl_comparison.hpp"
#include "decoders/processor/operators/gpu/opencl_encode.hpp"
#include "decoders/processor/operators/gpu/vulkan_raw_encode.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_raw_operators_test_support.hpp"

namespace alcedo {
namespace {

using vulkan_opencl_comparison::DifferenceRule;
using vulkan_opencl_comparison::ExpectEqualWithinTolerance;
using namespace vulkan_raw_test;

/// A Bayer RAW (Nikon NEF) with large clipped areas: 1.3 million texels change.
constexpr const char*        kClippedBayerRaw    = "raw/street/_DSC0155.NEF";
/// The texels that the reconstruction must change in each real RAW: the test inputs have
/// clipped areas, not only a few hot pixels.
constexpr std::size_t        kMinChangedTexels   = 10000;
/// Relative above 1; set from the measurement of this phase (VK5 record).
constexpr double             kHighlightTolerance = 1.0e-5;
constexpr std::array<int, 4> kFlips              = {0, 3, 5, 6};

class VulkanHighlightReconstructTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
    ASSERT_TRUE(reason.empty()) << reason;
  }

  static auto RequireRaw(const char* relative) -> std::shared_ptr<const PreparedRawInput> {
    auto input = LoadRaw(relative);
    EXPECT_NE(input, nullptr) << "test image is missing: " << relative;
    return input;
  }
};

// ---- Inputs ---------------------------------------------------------------------------------

/// The X-Trans RGBA of the Develop pass with highlight reconstruction: linear CFA without the
/// clamp, then the X-Trans demosaic (OpenCL).
auto XTransRgbaWithHighlights(const PreparedRawInput& input) -> std::vector<float> {
  const auto width  = input.host_extent.width;
  const auto height = input.host_extent.height;
  return XTransOnOpenCl(ToLinearRefOnOpenCl(input, height), input.cfa_pattern.xtrans_pattern, width,
                        height, 3)
      .rgba;
}

/// The RCD planes of the Develop pass with highlight reconstruction (linear CFA without the
/// clamp; OpenCL).
auto RcdPlanesWithHighlights(const PreparedRawInput& input) -> Planes {
  const auto width  = input.host_extent.width;
  const auto height = input.host_extent.height;
  return RcdOnOpenCl(ToLinearRefOnOpenCl(input, height), input.cfa_pattern.bayer_pattern, width,
                     height);
}

/// The clip level of each channel: the OpenCL `MakeHighlightParams`.
auto ClipLevels(std::span<const float, 4> cam_mul) -> std::array<float, 3> {
  const auto ratio = [](float value, float green) {
    return (!std::isfinite(value) || value <= 0.0F) ? 1.0F : std::clamp(value / green, 0.25F, 4.0F);
  };
  const float green = std::isfinite(cam_mul[1]) && cam_mul[1] > 0.0F ? cam_mul[1] : 1.0F;
  return {0.987F * ratio(cam_mul[0], green), 0.987F, 0.987F * ratio(cam_mul[2], green)};
}

// ---- One reconstruction on each backend ----------------------------------------------------

auto RgbaHighlightsOnOpenCl(const std::vector<float>& rgba, const float* cam_mul,
                            std::uint32_t width, std::uint32_t height) -> std::vector<float> {
  const std::size_t mask_bytes = static_cast<std::size_t>(width) * height * 6;
  OpenClTestBuffer  source(AsBytes(rgba));
  OpenClTestBuffer  destination(rgba.size() * sizeof(float));
  OpenClTestBuffer  mask(mask_bytes), dilated(mask_bytes);
  OpenClTestBuffer  sums(4 * sizeof(float)), counts(4 * sizeof(float)), clipped(sizeof(cl_int));
  auto              queue = OpenClQueue();
  OpenCL::EncodeHighlightReconstruct(queue, source.View(), destination.View(), mask.View(),
                                     dilated.View(), sums.View(), counts.View(), clipped.View(),
                                     cam_mul, width, height);
  FinishOpenCl();
  return destination.ReadFloats(rgba.size());
}

/// Scratch buffers of one Vulkan reconstruction of a region of width x height.
struct VulkanHighlightBuffers {
  VulkanBackend::Buffer mask;
  VulkanBackend::Buffer dilated;
  VulkanBackend::Buffer statistics;

  VulkanHighlightBuffers(VulkanBackend& backend, std::uint32_t width, std::uint32_t height)
      : mask(backend.CreateBuffer(vulkan::raw::HighlightMaskBytes(width, height))),
        dilated(backend.CreateBuffer(vulkan::raw::HighlightMaskBytes(width, height))),
        statistics(backend.CreateBuffer(vulkan::raw::kHighlightStatisticsBytes)) {}

  auto Scratch() -> vulkan::raw::HighlightScratch {
    return {ViewOf(mask), ViewOf(dilated), ViewOf(statistics)};
  }
};

auto RgbaHighlightsOnVulkan(const std::vector<float>& rgba, std::span<const float, 4> cam_mul,
                            std::uint32_t width, std::uint32_t height) -> std::vector<float> {
  VulkanRenderDevice     device;
  auto&                  backend     = device.Workspace().Device();
  auto&                  context     = device.CommandContext();
  auto                   source      = UploadVulkanBuffer(backend, context, AsBytes(rgba));
  auto                   destination = backend.CreateBuffer(rgba.size() * sizeof(float));
  VulkanHighlightBuffers scratch(backend, width, height);
  vulkan::raw::EncodeHighlightReconstruct(backend, context, ViewOf(source), ViewOf(destination),
                                          scratch.Scratch(), cam_mul, width, height);
  return DownloadVulkanFloats(backend, context, destination, rgba.size());
}

auto PlanarHighlightsOnOpenCl(const Planes& planes, const float* cam_mul, RectI crop,
                              std::uint32_t width, int flip) -> std::vector<float> {
  const auto        extent     = OrientedExtent(crop, flip);
  const std::size_t mask_bytes = static_cast<std::size_t>(crop.width) * crop.height * 6;
  OpenClTestBuffer  r(AsBytes(planes.r)), g(AsBytes(planes.g)), b(AsBytes(planes.b));
  OpenClTestImage   destination(extent[0], extent[1]);
  OpenClTestBuffer  mask(mask_bytes), dilated(mask_bytes);
  OpenClTestBuffer  sums(4 * sizeof(float)), counts(4 * sizeof(float)), clipped(sizeof(cl_int));
  auto              queue = OpenClQueue();
  OpenCL::EncodeHighlightReconstructPlanarAndPack(
      queue, r.View(), g.View(), b.View(), destination.Native(), mask.View(), dilated.View(),
      sums.View(), counts.View(), clipped.View(), cam_mul, crop, width, flip);
  FinishOpenCl();
  return destination.ReadFloats();
}

auto PlanarHighlightsOnVulkan(const Planes& planes, std::span<const float, 4> cam_mul, RectI crop,
                              std::uint32_t width, std::uint32_t height, int flip)
    -> std::vector<float> {
  const auto         extent = OrientedExtent(crop, flip);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  auto               r       = UploadVulkanBuffer(backend, context, AsBytes(planes.r));
  auto               g       = UploadVulkanBuffer(backend, context, AsBytes(planes.g));
  auto               b       = UploadVulkanBuffer(backend, context, AsBytes(planes.b));
  auto destination = backend.CreateTexture2D(extent[0], extent[1], TextureFormat::Rgba32f);
  VulkanHighlightBuffers scratch(backend, static_cast<std::uint32_t>(crop.width),
                                 static_cast<std::uint32_t>(crop.height));
  vulkan::raw::EncodeHighlightReconstructPlanarAndPack(backend, context, ViewOf(r), ViewOf(g),
                                                       ViewOf(b), destination, scratch.Scratch(),
                                                       cam_mul, crop, width, height, flip);
  return DownloadVulkanTexture(backend, context, destination);
}

auto ChangedTexels(const std::vector<float>& before, const std::vector<float>& after)
    -> std::size_t {
  std::size_t changed = 0;
  for (std::size_t i = 0; i + 3 < before.size(); i += 4) {
    changed +=
        (before[i] != after[i] || before[i + 1] != after[i + 1] || before[i + 2] != after[i + 2])
            ? 1
            : 0;
  }
  return changed;
}

// ---- Tests ----------------------------------------------------------------------------------

TEST_F(VulkanHighlightReconstructTest, RgbaHighlightsOfAnXTransRawEqualOpenCl) {
  const auto input = RequireRaw(kXTransRaw);
  ASSERT_NE(input, nullptr);
  const auto width   = input->host_extent.width;
  const auto height  = input->host_extent.height;
  const auto rgba    = XTransRgbaWithHighlights(*input);
  const auto opencl  = RgbaHighlightsOnOpenCl(rgba, input->linearization.cam_mul, width, height);
  const auto vulkan  = RgbaHighlightsOnVulkan(rgba, input->linearization.cam_mul, width, height);
  const auto changed = ChangedTexels(rgba, opencl);
  std::printf("[RGBA highlights] OpenCL changed %zu of %zu texels\n", changed, rgba.size() / 4);
  EXPECT_GE(changed, kMinChangedTexels);
  ExpectEqualWithinTolerance("Highlight RGBA", opencl, vulkan, kHighlightTolerance,
                             DifferenceRule::ScaledAboveOne);
}

TEST_F(VulkanHighlightReconstructTest, PlanarHighlightsOfABayerRawEqualOpenClForEachOrientation) {
  const auto input = RequireRaw(kClippedBayerRaw);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->cfa_pattern.kind, RawCfaKind::Bayer2x2);
  const auto  width  = input->host_extent.width;
  const auto  height = input->host_extent.height;
  const auto  planes = RcdPlanesWithHighlights(*input);
  const RectI crop   = CropOrFull(*input);
  const auto  cam    = std::span<const float, 4>(input->linearization.cam_mul);
  for (const int flip : kFlips) {
    const auto opencl = PlanarHighlightsOnOpenCl(planes, cam.data(), crop, width, flip);
    const auto vulkan = PlanarHighlightsOnVulkan(planes, cam, crop, width, height, flip);
    if (flip == 0) {
      // The pack without the reconstruction gives the unchanged texels.
      OpenClTestBuffer r(AsBytes(planes.r)), g(AsBytes(planes.g)), b(AsBytes(planes.b));
      OpenClTestImage  packed(static_cast<std::uint32_t>(crop.width),
                              static_cast<std::uint32_t>(crop.height));
      auto             queue = OpenClQueue();
      OpenCL::EncodePackPlanesCropInverseOrient(queue, r.View(), g.View(), b.View(),
                                                packed.Native(), crop, width, cam.data(), 0);
      FinishOpenCl();
      const auto changed = ChangedTexels(packed.ReadFloats(), opencl);
      std::printf("[Planar highlights] OpenCL changed %zu of %zu texels\n", changed,
                  opencl.size() / 4);
      EXPECT_GE(changed, kMinChangedTexels);
    }
    ExpectEqualWithinTolerance("Highlight planar flip=" + std::to_string(flip), opencl, vulkan,
                               kHighlightTolerance, DifferenceRule::ScaledAboveOne);
  }
}

TEST_F(VulkanHighlightReconstructTest, UnclippedTexelsKeepTheirValues) {
  // A texel with every channel below 95 % of its clip level has no reconstruction weight: the
  // result is its value (alpha copied); a clipped texel gets at least its value.
  const auto input = RequireRaw(kXTransRaw);
  ASSERT_NE(input, nullptr);
  const auto  width     = input->host_extent.width;
  const auto  height    = input->host_extent.height;
  const auto  rgba      = XTransRgbaWithHighlights(*input);
  const auto  cam       = std::span<const float, 4>(input->linearization.cam_mul);
  const auto  result    = RgbaHighlightsOnVulkan(rgba, cam, width, height);
  const auto  clip      = ClipLevels(cam);

  std::size_t unclipped = 0, unclipped_changed = 0, clipped = 0, clipped_darker = 0;
  for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
    bool finite_positive = true;
    bool below           = true;
    for (int c = 0; c < 3; ++c) {
      finite_positive = finite_positive && std::isfinite(rgba[i + c]) && rgba[i + c] >= 0.0F;
      below           = below && rgba[i + c] < 0.95F * clip[c];
    }
    if (!finite_positive) {
      continue;
    }
    if (below) {
      ++unclipped;
      unclipped_changed += (result[i] != rgba[i] || result[i + 1] != rgba[i + 1] ||
                            result[i + 2] != rgba[i + 2] || result[i + 3] != rgba[i + 3])
                               ? 1
                               : 0;
      continue;
    }
    ++clipped;
    for (int c = 0; c < 3; ++c) {
      clipped_darker += result[i + c] < rgba[i + c] ? 1 : 0;
    }
  }
  EXPECT_GT(unclipped, rgba.size() / 8);
  EXPECT_EQ(unclipped_changed, 0U);
  EXPECT_GE(clipped, kMinChangedTexels);
  EXPECT_EQ(clipped_darker, 0U);
}

TEST_F(VulkanHighlightReconstructTest, HighlightStagesRecordIntoOneRecordingWithoutASubmission) {
  constexpr std::uint32_t kWidth = 96, kHeight = 64;
  const std::size_t       texels = std::size_t{kWidth} * kHeight;
  // A bright disc above the clip level on a mid grey.
  std::vector<float>      rgba(texels * 4);
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const float dx    = static_cast<float>(x) - 48.0F;
      const float dy    = static_cast<float>(y) - 32.0F;
      const float value = dx * dx + dy * dy < 225.0F ? 1.2F : 0.4F;
      float*      texel = rgba.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4;
      texel[0]          = value;
      texel[1]          = value * 0.9F;
      texel[2]          = value * 0.8F;
      texel[3]          = 1.0F;
    }
  }
  const float        cam_mul[4] = {2.0F, 1.0F, 1.5F, 1.0F};
  const Planes       planes = {std::vector<float>(texels, 0.5F), std::vector<float>(texels, 1.1F),
                               std::vector<float>(texels, 0.7F)};

  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  auto               source  = UploadVulkanBuffer(backend, context, AsBytes(rgba));
  auto               result  = backend.CreateBuffer(texels * 16);
  auto               r       = UploadVulkanBuffer(backend, context, AsBytes(planes.r));
  auto               g       = UploadVulkanBuffer(backend, context, AsBytes(planes.g));
  auto               b       = UploadVulkanBuffer(backend, context, AsBytes(planes.b));
  auto packed = backend.CreateTexture2D(kHeight - 8, kWidth - 8, TextureFormat::Rgba32f);
  VulkanHighlightBuffers rgba_scratch(backend, kWidth, kHeight);
  VulkanHighlightBuffers planar_scratch(backend, kWidth - 8, kHeight - 8);
  backend.Submit(context);
  backend.Wait(context);

  backend.ResetCounters();
  const auto before = vulkan::ReadVulkanApiCounters();
  vulkan::raw::EncodeHighlightReconstruct(backend, context, ViewOf(source), ViewOf(result),
                                          rgba_scratch.Scratch(), cam_mul, kWidth, kHeight);
  vulkan::raw::EncodeHighlightReconstructPlanarAndPack(
      backend, context, ViewOf(r), ViewOf(g), ViewOf(b), packed, planar_scratch.Scratch(), cam_mul,
      RectI{4, 4, kWidth - 8, kHeight - 8}, kWidth, kHeight, 6);
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 8U);
  EXPECT_EQ(delta.submits, 0U);
  EXPECT_EQ(delta.waits, 0U);
  EXPECT_EQ(backend.SubmitCount(), 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
  EXPECT_TRUE(context.IsRecording());

  const auto reconstructed = DownloadVulkanFloats(backend, context, result, texels * 4);
  EXPECT_FALSE(context.IsRecording());
  EXPECT_EQ(backend.SubmitCount(), 1U);
  // The clipped green of the disc keeps at least its value; the grey keeps its value.
  const std::size_t center = (std::size_t{32} * kWidth + 48) * 4;
  EXPECT_GE(reconstructed[center + 1], 1.2F * 0.9F);
  EXPECT_EQ(reconstructed[1], 0.4F * 0.9F);
  // The planar result: every texel written (alpha 1), the green above the clip level reconstructed.
  const auto pixels = DownloadVulkanTexture(backend, context, packed);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    ASSERT_EQ(pixels[i + 3], 1.0F) << "texel " << i / 4;
    ASSERT_GE(pixels[i + 1], 1.1F) << "texel " << i / 4;
  }
}

TEST_F(VulkanHighlightReconstructTest, InvalidRequestIsRejectedBeforeACommand) {
  VulkanRenderDevice      device;
  auto&                   backend = device.Workspace().Device();
  auto&                   context = device.CommandContext();
  constexpr std::uint32_t kWidth = 64, kHeight = 32;
  const std::size_t       texels = std::size_t{kWidth} * kHeight;
  auto                    rgba   = backend.CreateBuffer(texels * 16 + 64);
  auto                    out    = backend.CreateBuffer(texels * 16);
  auto                    plane  = backend.CreateBuffer(texels * 4);
  auto mask       = backend.CreateBuffer(vulkan::raw::HighlightMaskBytes(kWidth, kHeight));
  auto small_mask = backend.CreateBuffer(vulkan::raw::HighlightMaskBytes(kWidth, kHeight) - 4);
  auto statistics = backend.CreateBuffer(vulkan::raw::kHighlightStatisticsBytes + 8);
  auto texture    = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  const float                         cam_mul[4] = {2.0F, 1.0F, 1.5F, 1.0F};
  const vulkan::raw::HighlightScratch scratch{ViewOf(mask), ViewOf(mask), ViewOf(statistics)};
  const RectI                         full_crop{0, 0, kWidth, kHeight};

  struct Rejected {
    const char*           name;
    std::function<void()> encode;
  };
  const auto rgba_request = [&](VulkanBufferView source, vulkan::raw::HighlightScratch views) {
    vulkan::raw::EncodeHighlightReconstruct(backend, context, source, ViewOf(out), views, cam_mul,
                                            kWidth, kHeight);
  };
  const std::vector<Rejected> requests = {
      {"RGBA source at 4 bytes", [&] { rgba_request(ViewOf(rgba, 4), scratch); }},
      {"mask smaller than the region",
       [&] { rgba_request(ViewOf(rgba), {ViewOf(small_mask), ViewOf(mask), ViewOf(statistics)}); }},
      {"dilated mask without a buffer",
       [&] { rgba_request(ViewOf(rgba), {ViewOf(mask), VulkanBufferView{}, ViewOf(statistics)}); }},
      {"statistics past the end of the buffer",
       [&] { rgba_request(ViewOf(rgba), {ViewOf(mask), ViewOf(mask), ViewOf(statistics, 12)}); }},
      {"statistics at 2 bytes",
       [&] { rgba_request(ViewOf(rgba), {ViewOf(mask), ViewOf(mask), ViewOf(statistics, 2)}); }},
      {"planar crop outside the planes",
       [&] {
         vulkan::raw::EncodeHighlightReconstructPlanarAndPack(
             backend, context, ViewOf(plane), ViewOf(plane), ViewOf(plane), texture, scratch,
             cam_mul, RectI{0, 1, kWidth, kHeight}, kWidth, kHeight, 0);
       }},
      {"planar destination of the unturned size for flip 5",
       [&] {
         vulkan::raw::EncodeHighlightReconstructPlanarAndPack(
             backend, context, ViewOf(plane), ViewOf(plane), ViewOf(plane), texture, scratch,
             cam_mul, full_crop, kWidth, kHeight, 5);
       }},
      {"planar masks smaller than the crop",
       [&] {
         vulkan::raw::EncodeHighlightReconstructPlanarAndPack(
             backend, context, ViewOf(plane), ViewOf(plane), ViewOf(plane), texture,
             {ViewOf(small_mask), ViewOf(mask), ViewOf(statistics)}, cam_mul, full_crop, kWidth,
             kHeight, 0);
       }},
  };

  backend.ResetCounters();
  const auto before = vulkan::ReadVulkanApiCounters();
  for (const auto& request : requests) {
    EXPECT_THROW(request.encode(), std::invalid_argument) << request.name;
  }
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(delta.pipeline_creations + delta.pipeline_cache_hits, 0U);
  EXPECT_FALSE(context.IsRecording());
}

}  // namespace
}  // namespace alcedo
