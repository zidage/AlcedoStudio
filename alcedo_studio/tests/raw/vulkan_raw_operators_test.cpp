//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Vulkan RAW encode functions against the product OpenCL encode functions on the same GPU,
// one stage at a time, on a real Bayer RAW and a real X-Trans RAW (plan phase VK4, decision B9).
// Each stage gets the same host input on both backends; the input of a stage is the OpenCL output
// of the stage before. Each test runs with the validation layer. Set ALCEDO_VULKAN_DEVICE (for
// example "Iris") to select the device; the OpenCL device follows it.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

/// The tolerance of OpenClDevelopLinearizeMatchesCudaReferenceWithinTolerance.
constexpr double             kLinearizeTolerance = 1.0e-5;
/// X-Trans: the linearize tolerance; the kernel has no selection that amplifies one ulp.
constexpr double             kXTransTolerance    = 1.0e-5;
/// RCD, relative above 1: each multiply and each sum keeps the OpenCL rounding, but the OpenCL
/// division of the NVIDIA driver is not correctly rounded either, and the direction selections
/// amplify one ulp of a quotient (measured 8.5e-6 on the RTX 3080 Laptop GPU; VK4 record).
constexpr double             kRcdTolerance       = 1.0e-4;
/// The orientations of LibRaw (`flip`): none, 180 degrees, and the two 90 degree turns.
constexpr std::array<int, 4> kFlips              = {0, 3, 5, 6};

class VulkanRawOperatorsTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
    ASSERT_TRUE(reason.empty()) << reason;
  }

  /// The decoded RAW; a missing test image fails the test.
  static auto RequireRaw(const char* relative) -> std::shared_ptr<const PreparedRawInput> {
    auto input = LoadRaw(relative);
    EXPECT_NE(input, nullptr) << "test image is missing: " << relative;
    return input;
  }
};

auto TexelCount(const PreparedRawInput& input) -> std::size_t {
  return static_cast<std::size_t>(input.host_extent.width) * input.host_extent.height;
}

// ---- One stage on each backend --------------------------------------------------------------

auto ToLinearRefOnVulkan(const PreparedRawInput& input, std::uint32_t rows,
                         std::uint32_t cfa_offset = 0) -> std::vector<float> {
  const std::uint32_t width = input.host_extent.width;
  const std::size_t   count = static_cast<std::size_t>(width) * rows;
  VulkanRenderDevice  device;
  auto&               backend = device.Workspace().Device();
  auto&               context = device.CommandContext();
  auto                source  = UploadVulkanBuffer(
      backend, context, input.pixels.Span().first(count * sizeof(std::uint16_t)), cfa_offset);
  auto linear     = backend.CreateBuffer(count * sizeof(float));
  auto parameters = backend.CreateBuffer(vulkan::raw::kToLinearRefParameterBytes);
  vulkan::raw::EncodeToLinearRef(backend, context, ViewOf(source, cfa_offset), ViewOf(linear),
                                 ViewOf(parameters), width, rows, input.linearization,
                                 input.cfa_pattern);
  return DownloadVulkanFloats(backend, context, linear, count);
}

auto CfaClampOnVulkan(const std::vector<float>& linear, std::uint32_t width, std::uint32_t height)
    -> std::vector<float> {
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  auto               buffer  = UploadVulkanBuffer(backend, context, AsBytes(linear));
  vulkan::raw::EncodeCfaClamp01(backend, context, ViewOf(buffer), width, height);
  return DownloadVulkanFloats(backend, context, buffer, linear.size());
}

auto RcdOnVulkan(const std::vector<float>& linear, const BayerPattern2x2& pattern,
                 std::uint32_t width, std::uint32_t height) -> Planes {
  const std::size_t  bytes = linear.size() * sizeof(float);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  auto               cfa     = UploadVulkanBuffer(backend, context, AsBytes(linear));
  auto               r = backend.CreateBuffer(bytes), g = backend.CreateBuffer(bytes);
  auto               b = backend.CreateBuffer(bytes), vh = backend.CreateBuffer(bytes);
  auto               pq = backend.CreateBuffer(bytes);
  vulkan::raw::EncodeBayerRcd(backend, context, ViewOf(cfa),
                              {ViewOf(r), ViewOf(g), ViewOf(b), ViewOf(vh), ViewOf(pq)}, pattern,
                              width, height);
  return {DownloadVulkanFloats(backend, context, r, linear.size()),
          DownloadVulkanFloats(backend, context, g, linear.size()),
          DownloadVulkanFloats(backend, context, b, linear.size())};
}

auto XTransOnVulkan(const std::vector<float>& linear, const XTransPattern6x6& pattern,
                    std::uint32_t width, std::uint32_t height, int passes) -> XTransResult {
  VulkanRenderDevice device;
  auto&              backend    = device.Workspace().Device();
  auto&              context    = device.CommandContext();
  auto               cfa        = UploadVulkanBuffer(backend, context, AsBytes(linear));
  auto               green      = backend.CreateBuffer(linear.size() * sizeof(float));
  auto               rgba       = backend.CreateBuffer(linear.size() * 4 * sizeof(float));
  auto               parameters = backend.CreateBuffer(vulkan::raw::kXTransParameterBytes);
  vulkan::raw::EncodeXTrans(backend, context, ViewOf(cfa), ViewOf(green), ViewOf(rgba),
                            ViewOf(parameters), pattern, width, height, passes);
  return {DownloadVulkanFloats(backend, context, green, linear.size()),
          DownloadVulkanFloats(backend, context, rgba, linear.size() * 4)};
}

auto PackPlanesOnOpenCl(const Planes& planes, RectI crop, std::uint32_t width, const float* cam_mul,
                        int flip) -> std::vector<float> {
  const auto       extent = OrientedExtent(crop, flip);
  OpenClTestBuffer r(AsBytes(planes.r)), g(AsBytes(planes.g)), b(AsBytes(planes.b));
  OpenClTestImage  destination(extent[0], extent[1]);
  auto             queue = OpenClQueue();
  OpenCL::EncodePackPlanesCropInverseOrient(queue, r.View(), g.View(), b.View(),
                                            destination.Native(), crop, width, cam_mul, flip);
  FinishOpenCl();
  return destination.ReadFloats();
}

auto PackPlanesOnVulkan(const Planes& planes, RectI crop, std::uint32_t width, std::uint32_t height,
                        std::span<const float, 4> cam_mul, int flip) -> std::vector<float> {
  const auto         extent = OrientedExtent(crop, flip);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  auto               r       = UploadVulkanBuffer(backend, context, AsBytes(planes.r));
  auto               g       = UploadVulkanBuffer(backend, context, AsBytes(planes.g));
  auto               b       = UploadVulkanBuffer(backend, context, AsBytes(planes.b));
  auto destination = backend.CreateTexture2D(extent[0], extent[1], TextureFormat::Rgba32f);
  vulkan::raw::EncodePackPlanesCropInverseOrient(backend, context, ViewOf(r), ViewOf(g), ViewOf(b),
                                                 destination, crop, width, height, cam_mul, flip);
  return DownloadVulkanTexture(backend, context, destination);
}

auto CopyRgbaOnOpenCl(const std::vector<float>& rgba, RectI crop, std::uint32_t width,
                      const float* cam_mul, int flip) -> std::vector<float> {
  const auto       extent = OrientedExtent(crop, flip);
  OpenClTestBuffer source(AsBytes(rgba));
  OpenClTestImage  destination(extent[0], extent[1]);
  auto             queue = OpenClQueue();
  OpenCL::EncodeCopyRgbaCropInverseOrient(queue, source.View(), destination.Native(), crop, width,
                                          cam_mul, flip);
  FinishOpenCl();
  return destination.ReadFloats();
}

auto CopyRgbaOnVulkan(const std::vector<float>& rgba, RectI crop, std::uint32_t width,
                      std::uint32_t height, std::span<const float, 4> cam_mul, int flip)
    -> std::vector<float> {
  const auto         extent = OrientedExtent(crop, flip);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  auto               source  = UploadVulkanBuffer(backend, context, AsBytes(rgba));
  auto destination = backend.CreateTexture2D(extent[0], extent[1], TextureFormat::Rgba32f);
  vulkan::raw::EncodeCopyRgbaCropInverseOrient(backend, context, ViewOf(source), destination, crop,
                                               width, height, cam_mul, flip);
  return DownloadVulkanTexture(backend, context, destination);
}

// ---- Tests ----------------------------------------------------------------------------------

TEST_F(VulkanRawOperatorsTest, LinearizeRgbOfALinearDngEqualsOpenCl) {
  const auto input = RequireRaw(kLinearDng);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->pixels.format, HostPixelFormat::F32Rgba);
  const std::uint32_t       width  = input->host_extent.width;
  const std::uint32_t       height = input->host_extent.height;
  const auto*               first  = reinterpret_cast<const float*>(input->pixels.bytes.get());
  const std::vector<float>  rgba(first, first + TexelCount(*input) * 4);

  RawRgbLinearizationParams params = input->rgb_linearization.value_or(RawRgbLinearizationParams{});
  for (const std::uint32_t integer_codes : {0U, 1U}) {
    params.integer_codes = integer_codes;
    OpenClTestBuffer opencl_buffer(AsBytes(rgba));
    auto             queue = OpenClQueue();
    OpenCL::EncodeLinearizeRgb(queue, opencl_buffer.View(), width, height, params);
    FinishOpenCl();

    VulkanRenderDevice device;
    auto&              backend = device.Workspace().Device();
    auto&              context = device.CommandContext();
    auto               buffer  = UploadVulkanBuffer(backend, context, AsBytes(rgba));
    vulkan::raw::EncodeLinearizeRgb(backend, context, ViewOf(buffer), width, height, params);
    ExpectEqualWithinTolerance("LinearizeRgb integer_codes=" + std::to_string(integer_codes),
                               opencl_buffer.ReadFloats(rgba.size()),
                               DownloadVulkanFloats(backend, context, buffer, rgba.size()), 0.0,
                               DifferenceRule::Absolute);
  }
}

TEST_F(VulkanRawOperatorsTest, ToLinearRefOfABayerRawEqualsOpenCl) {
  const auto input = RequireRaw(kBayerRaw);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->cfa_pattern.kind, RawCfaKind::Bayer2x2);
  const auto rows = input->host_extent.height;
  ExpectEqualWithinTolerance("ToLinearRef Bayer", ToLinearRefOnOpenCl(*input, rows),
                             ToLinearRefOnVulkan(*input, rows), kLinearizeTolerance,
                             DifferenceRule::Absolute);
}

TEST_F(VulkanRawOperatorsTest, ToLinearRefOfAnXTransRawEqualsOpenCl) {
  const auto input = RequireRaw(kXTransRaw);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->cfa_pattern.kind, RawCfaKind::XTrans6x6);
  const auto rows = input->host_extent.height;
  ExpectEqualWithinTolerance("ToLinearRef X-Trans", ToLinearRefOnOpenCl(*input, rows),
                             ToLinearRefOnVulkan(*input, rows), kLinearizeTolerance,
                             DifferenceRule::Absolute);
}

TEST_F(VulkanRawOperatorsTest, ToLinearRefAtAnOddCfaOffsetEqualsOpenCl) {
  // A CFA view at 2 bytes: each sample is the upper half of a 32-bit word or the lower half of the
  // next one. 512 rows of the Bayer RAW.
  const auto input = RequireRaw(kBayerRaw);
  ASSERT_NE(input, nullptr);
  ExpectEqualWithinTolerance("ToLinearRef offset 2", ToLinearRefOnOpenCl(*input, 512, 2),
                             ToLinearRefOnVulkan(*input, 512, 2), kLinearizeTolerance,
                             DifferenceRule::Absolute);
  EXPECT_EQ(ToLinearRefOnVulkan(*input, 512, 2), ToLinearRefOnVulkan(*input, 512, 0));
}

TEST_F(VulkanRawOperatorsTest, CfaClampOfABayerRawEqualsOpenCl) {
  const auto input = RequireRaw(kBayerRaw);
  ASSERT_NE(input, nullptr);
  const auto width  = input->host_extent.width;
  const auto height = input->host_extent.height;
  // The linear CFA with values of [-0.5, 1.5]: the clamp changes values on both sides.
  auto       linear = ToLinearRefOnOpenCl(*input, height);
  for (std::size_t i = 0; i < linear.size(); i += 7) {
    linear[i] = linear[i] * 2.0F - 0.5F;
  }
  ExpectEqualWithinTolerance("CfaClamp01", CfaClampOnOpenCl(linear, width, height),
                             CfaClampOnVulkan(linear, width, height), 0.0,
                             DifferenceRule::Absolute);
}

TEST_F(VulkanRawOperatorsTest, BayerRcdOfARealRawEqualsOpenCl) {
  const auto input = RequireRaw(kBayerRaw);
  ASSERT_NE(input, nullptr);
  const auto width   = input->host_extent.width;
  const auto height  = input->host_extent.height;
  const auto linear  = ClampedLinearOnOpenCl(*input);
  const auto pattern = input->cfa_pattern.bayer_pattern;
  const auto opencl  = RcdOnOpenCl(linear, pattern, width, height);
  const auto vulkan  = RcdOnVulkan(linear, pattern, width, height);
  ExpectEqualWithinTolerance("RCD R", opencl.r, vulkan.r, kRcdTolerance,
                             DifferenceRule::ScaledAboveOne);
  ExpectEqualWithinTolerance("RCD G", opencl.g, vulkan.g, kRcdTolerance,
                             DifferenceRule::ScaledAboveOne);
  ExpectEqualWithinTolerance("RCD B", opencl.b, vulkan.b, kRcdTolerance,
                             DifferenceRule::ScaledAboveOne);
}

TEST_F(VulkanRawOperatorsTest, XTransOfARealRawEqualsOpenClForThreePassesAndOnePass) {
  const auto input = RequireRaw(kXTransRaw);
  ASSERT_NE(input, nullptr);
  const auto width   = input->host_extent.width;
  const auto height  = input->host_extent.height;
  const auto linear  = ClampedLinearOnOpenCl(*input);
  const auto pattern = input->cfa_pattern.xtrans_pattern;
  for (const int passes : {3, 1}) {
    const auto opencl = XTransOnOpenCl(linear, pattern, width, height, passes);
    const auto vulkan = XTransOnVulkan(linear, pattern, width, height, passes);
    const auto label  = " passes=" + std::to_string(passes);
    ExpectEqualWithinTolerance("X-Trans green" + label, opencl.green, vulkan.green,
                               kXTransTolerance, DifferenceRule::ScaledAboveOne);
    ExpectEqualWithinTolerance("X-Trans RGBA" + label, opencl.rgba, vulkan.rgba, kXTransTolerance,
                               DifferenceRule::ScaledAboveOne);
  }
}

TEST_F(VulkanRawOperatorsTest, PackPlanesWithTheCropOfTheRawAndEachOrientationEqualsOpenCl) {
  const auto input = RequireRaw(kBayerRaw);
  ASSERT_NE(input, nullptr);
  const auto width  = input->host_extent.width;
  const auto height = input->host_extent.height;
  const auto planes =
      RcdOnOpenCl(ClampedLinearOnOpenCl(*input), input->cfa_pattern.bayer_pattern, width, height);
  const RectI crop = CropOrFull(*input);
  for (const int flip : kFlips) {
    ExpectEqualWithinTolerance(
        "PackPlanes flip=" + std::to_string(flip),
        PackPlanesOnOpenCl(planes, crop, width, input->linearization.cam_mul, flip),
        PackPlanesOnVulkan(planes, crop, width, height, input->linearization.cam_mul, flip), 0.0,
        DifferenceRule::Absolute);
  }
}

TEST_F(VulkanRawOperatorsTest, CopyRgbaWithTheCropOfTheRawAndEachOrientationEqualsOpenCl) {
  const auto input = RequireRaw(kXTransRaw);
  ASSERT_NE(input, nullptr);
  const auto width  = input->host_extent.width;
  const auto height = input->host_extent.height;
  const auto rgba = XTransOnOpenCl(ClampedLinearOnOpenCl(*input), input->cfa_pattern.xtrans_pattern,
                                   width, height, 3)
                        .rgba;
  const RectI crop = CropOrFull(*input);
  for (const int flip : kFlips) {
    ExpectEqualWithinTolerance(
        "CopyRgba flip=" + std::to_string(flip),
        CopyRgbaOnOpenCl(rgba, crop, width, input->linearization.cam_mul, flip),
        CopyRgbaOnVulkan(rgba, crop, width, height, input->linearization.cam_mul, flip), 0.0,
        DifferenceRule::Absolute);
  }
}

TEST_F(VulkanRawOperatorsTest, RawStagesRecordIntoOneRecordingWithoutASubmission) {
  // The legacy Bayer chain of the Develop pass without highlight reconstruction on 512 rows:
  // linearize, clamp, RCD (5 kernels), pack. Nothing is submitted before the download.
  const auto input = RequireRaw(kBayerRaw);
  ASSERT_NE(input, nullptr);
  const std::uint32_t width  = input->host_extent.width;
  const std::uint32_t height = 512;
  const std::size_t   bytes  = static_cast<std::size_t>(width) * height * sizeof(float);
  const RectI         crop{8, 8, static_cast<std::int32_t>(width) - 16, 496};

  VulkanRenderDevice  device;
  auto&               backend = device.Workspace().Device();
  auto&               context = device.CommandContext();
  auto                cfa     = UploadVulkanBuffer(
      backend, context, input->pixels.Span().first(static_cast<std::size_t>(width) * height * 2));
  auto linear = backend.CreateBuffer(bytes), parameters = backend.CreateBuffer(512);
  auto r = backend.CreateBuffer(bytes), g = backend.CreateBuffer(bytes);
  auto b = backend.CreateBuffer(bytes), vh = backend.CreateBuffer(bytes);
  auto pq = backend.CreateBuffer(bytes);
  auto destination =
      backend.CreateTexture2D(static_cast<std::uint32_t>(crop.width),
                              static_cast<std::uint32_t>(crop.height), TextureFormat::Rgba32f);
  backend.ResetCounters();
  const auto before = vulkan::ReadVulkanApiCounters();
  vulkan::raw::EncodeToLinearRef(backend, context, ViewOf(cfa), ViewOf(linear), ViewOf(parameters),
                                 width, height, input->linearization, input->cfa_pattern);
  vulkan::raw::EncodeCfaClamp01(backend, context, ViewOf(linear), width, height);
  vulkan::raw::EncodeBayerRcd(backend, context, ViewOf(linear),
                              {ViewOf(r), ViewOf(g), ViewOf(b), ViewOf(vh), ViewOf(pq)},
                              input->cfa_pattern.bayer_pattern, width, height);
  vulkan::raw::EncodePackPlanesCropInverseOrient(backend, context, ViewOf(r), ViewOf(g), ViewOf(b),
                                                 destination, crop, width, height,
                                                 input->linearization.cam_mul, 0);
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 8U);
  EXPECT_EQ(delta.submits, 0U);
  EXPECT_EQ(delta.waits, 0U);
  EXPECT_EQ(backend.SubmitCount(), 0U);
  EXPECT_TRUE(context.IsRecording());
  // The parameter record is the only upload of the chain.
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 1U);
  EXPECT_EQ(backend.HostToDeviceBytes(), vulkan::raw::kToLinearRefParameterBytes);

  const auto pixels = DownloadVulkanTexture(backend, context, destination);
  EXPECT_FALSE(context.IsRecording());
  EXPECT_EQ(backend.SubmitCount(), 1U);
  // Every texel is written: alpha is 1.
  std::size_t alpha_ones = 0;
  for (std::size_t i = 3; i < pixels.size(); i += 4) {
    alpha_ones += pixels[i] == 1.0F ? 1 : 0;
  }
  EXPECT_EQ(alpha_ones, pixels.size() / 4);
}

TEST_F(VulkanRawOperatorsTest, InvalidRequestIsRejectedBeforeACommand) {
  VulkanRenderDevice      device;
  auto&                   backend = device.Workspace().Device();
  auto&                   context = device.CommandContext();
  constexpr std::uint32_t kWidth = 64, kHeight = 32;
  const std::size_t       plane = std::size_t{kWidth} * kHeight * sizeof(float);
  auto                    small = backend.CreateBuffer(plane - 4);
  auto                    full  = backend.CreateBuffer(plane + 64);
  auto                    rgba  = backend.CreateBuffer(plane * 4);
  auto        parameters        = backend.CreateBuffer(vulkan::raw::kToLinearRefParameterBytes);
  auto        texture           = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto        r32f              = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::R32f);
  const float cam_mul[4]        = {2.0F, 1.0F, 1.5F, 1.0F};
  const RawLinearizationParams linearization;
  const RawCfaPattern          pattern;
  const RectI                  full_crop{0, 0, kWidth, kHeight};

  struct Rejected {
    const char*           name;
    std::function<void()> encode;
  };
  const std::vector<Rejected> requests = {
      {"plane smaller than width x height",
       [&] { vulkan::raw::EncodeCfaClamp01(backend, context, ViewOf(small), kWidth, kHeight); }},
      {"float view at 2 bytes",
       [&] { vulkan::raw::EncodeCfaClamp01(backend, context, ViewOf(full, 2), kWidth, kHeight); }},
      {"view without a buffer",
       [&] {
         vulkan::raw::EncodeCfaClamp01(backend, context, VulkanBufferView{}, kWidth, kHeight);
       }},
      {"U16 view at 1 byte",
       [&] {
         vulkan::raw::EncodeToLinearRef(backend, context, ViewOf(full, 1), ViewOf(rgba),
                                        ViewOf(parameters), kWidth, kHeight, linearization,
                                        pattern);
       }},
      {"parameter view smaller than the record",
       [&] {
         vulkan::raw::EncodeToLinearRef(backend, context, ViewOf(full), ViewOf(rgba),
                                        ViewOf(parameters, 16), kWidth, kHeight, linearization,
                                        pattern);
       }},
      {"RGBA view at 4 bytes",
       [&] {
         vulkan::raw::EncodeLinearizeRgb(backend, context, ViewOf(rgba, 4), kWidth, kHeight / 2,
                                         RawRgbLinearizationParams{});
       }},
      {"crop outside the planes",
       [&] {
         vulkan::raw::EncodePackPlanesCropInverseOrient(
             backend, context, ViewOf(full), ViewOf(full), ViewOf(full), texture,
             RectI{1, 0, kWidth, kHeight}, kWidth, kHeight, cam_mul, 0);
       }},
      {"empty crop",
       [&] {
         vulkan::raw::EncodePackPlanesCropInverseOrient(
             backend, context, ViewOf(full), ViewOf(full), ViewOf(full), texture,
             RectI{0, 0, 0, kHeight}, kWidth, kHeight, cam_mul, 0);
       }},
      {"destination of the unturned size for flip 6",
       [&] {
         vulkan::raw::EncodeCopyRgbaCropInverseOrient(backend, context, ViewOf(rgba), texture,
                                                      full_crop, kWidth, kHeight, cam_mul, 6);
       }},
      {"R32F destination",
       [&] {
         vulkan::raw::EncodeCopyRgbaCropInverseOrient(backend, context, ViewOf(rgba), r32f,
                                                      full_crop, kWidth, kHeight, cam_mul, 0);
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
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
  EXPECT_FALSE(context.IsRecording());
}

}  // namespace
}  // namespace alcedo
