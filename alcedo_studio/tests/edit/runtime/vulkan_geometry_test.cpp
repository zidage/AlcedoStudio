//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// vulkan::geometry::EncodeCropResize against OpenCL::Geometry::CropResize with the bilinear kernel
// on the same GPU (plan phase VK3, decision B9). The tolerances are those of
// OpenClGeometryUtilsTest: 0 for a crop at its own size, 1e-5 for a resize. Each test runs with the
// validation layer. Set ALCEDO_VULKAN_DEVICE (for example "Iris") to select the device; the OpenCL
// device follows it.

#include "edit/runtime/vulkan/vulkan_geometry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../vulkan/vulkan_opencl_comparison.hpp"
#include "edit/operators/geometry/resize_algorithm.hpp"
#include "edit/runtime/texture_format.hpp"
#include "image/opencl_image.hpp"
#include "opencl/opencl_geometry_utils.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using vulkan_opencl_comparison::DifferenceRule;
using vulkan_opencl_comparison::ExpectEqualWithinTolerance;

constexpr double kResizeTolerance = 1.0e-5;

class VulkanGeometryTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

/// RGBA32F pixels: the pattern of opencl_geometry_utils_test.cpp (a gradient in [0, 1.1)).
auto Pattern(int width, int height) -> cv::Mat {
  cv::Mat image(height, width, CV_32FC4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto* pixel = image.ptr<float>(y) + x * 4;
      for (int c = 0; c < 4; ++c) {
        pixel[c] = static_cast<float>((x * 17 + y * 29 + c * 11) % 257) / 257.0F +
                   static_cast<float>(c) * 0.03125F;
      }
    }
  }
  return image;
}

auto Values(const cv::Mat& image) -> std::vector<float> {
  const cv::Mat packed = image.isContinuous() ? image : image.clone();
  return {packed.ptr<float>(0), packed.ptr<float>(0) + packed.total() * 4};
}

auto RunOpenCl(const cv::Mat& source, RectI crop, int width, int height) -> std::vector<float> {
  opencl::OpenClImage source_image;
  source_image.Upload(source);
  opencl::OpenClImage destination;
  OpenCL::Geometry::CropResize(source_image, destination,
                               cv::Rect(crop.x, crop.y, crop.width, crop.height),
                               cv::Size(width, height), ResizeDownsampleAlgorithm::Bilinear);
  cv::Mat output;
  destination.Download(output);
  return Values(output);
}

auto RunVulkan(const cv::Mat& source, RectI crop, int width, int height) -> std::vector<float> {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               source_texture =
      backend.CreateTexture2D(static_cast<std::uint32_t>(source.cols),
                              static_cast<std::uint32_t>(source.rows), TextureFormat::Rgba32f);
  auto destination =
      backend.CreateTexture2D(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                              TextureFormat::Rgba32f);
  const auto pixels = Values(source);
  backend.UploadTexture2D(source_texture, std::as_bytes(std::span<const float>(pixels)),
                          command_context);
  vulkan::geometry::EncodeCropResize(backend, command_context, source_texture, destination, crop);
  std::vector<float> output(static_cast<std::size_t>(width) * height * 4);
  backend.DownloadTexture2D(destination, std::as_writable_bytes(std::span<float>(output)),
                            command_context);
  return output;
}

struct ResizeCase {
  const char* name;
  int         source_width;
  int         source_height;
  RectI       crop;
  int         width;
  int         height;
};

void ExpectCasesEqualOpenCl(std::initializer_list<ResizeCase> cases, double tolerance) {
  for (const ResizeCase& c : cases) {
    const cv::Mat source = Pattern(c.source_width, c.source_height);
    ExpectEqualWithinTolerance(c.name, RunOpenCl(source, c.crop, c.width, c.height),
                               RunVulkan(source, c.crop, c.width, c.height), tolerance,
                               DifferenceRule::Absolute);
  }
}

TEST_F(VulkanGeometryTest, CropAtItsOwnSizeCopiesTheRegionExactly) {
  ExpectCasesEqualOpenCl({{"CropAtItsOwnSize", 6, 5, {2, 1, 3, 2}, 3, 2}}, 0.0);

  // The region of the source, texel for texel.
  const cv::Mat source = Pattern(6, 5);
  EXPECT_EQ(RunVulkan(source, {2, 1, 3, 2}, 3, 2), Values(source(cv::Rect(2, 1, 3, 2)).clone()));
}

TEST_F(VulkanGeometryTest, ResizeOfACropEqualsOpenCl) {
  ExpectCasesEqualOpenCl(
      {
          {"CropDownsample2x", 8, 6, {1, 1, 6, 4}, 3, 2},
          {"CropDownsampleFractionalScale", 257, 191, {13, 7, 200, 150}, 37, 29},
          {"CropUpsampleOneAxis", 40, 10, {3, 2, 30, 6}, 10, 15},
      },
      kResizeTolerance);
}

TEST_F(VulkanGeometryTest, ResizeOfTheFullSourceEqualsOpenCl) {
  ExpectCasesEqualOpenCl(
      {
          {"FullSourceUpsample", 7, 5, {0, 0, 7, 5}, 13, 9},
          {"FullSourceDownsample", 11, 9, {0, 0, 11, 9}, 6, 4},
          {"FullSourceQuarter", 640, 427, {0, 0, 640, 427}, 160, 107},
      },
      kResizeTolerance);
}

TEST_F(VulkanGeometryTest, FullSourceAtItsOwnSizeIsACopyWithNoDispatch) {
  const cv::Mat source = Pattern(19, 11);
  const auto    before = vulkan::ReadVulkanApiCounters();
  const auto    vulkan = RunVulkan(source, {0, 0, 19, 11}, 19, 11);
  const auto    delta  = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(vulkan, Values(source));
}

TEST_F(VulkanGeometryTest, InvalidRequestIsRejectedBeforeACommand) {
  VulkanRenderDevice device;
  auto&              backend         = device.Workspace().Device();
  auto&              command_context = device.CommandContext();
  auto               source          = backend.CreateTexture2D(1300, 130, TextureFormat::Rgba32f);
  auto               destination     = backend.CreateTexture2D(10, 2, TextureFormat::Rgba32f);
  auto               r32f            = backend.CreateTexture2D(10, 2, TextureFormat::R32f);

  struct Rejected {
    const char*                     name;
    const VulkanBackend::Texture2D* source;
    VulkanBackend::Texture2D*       destination;
    RectI                           crop;
  };
  const auto before = vulkan::ReadVulkanApiCounters();
  for (const Rejected& r : {
           Rejected{"crop outside the source", &source, &destination, {1200, 0, 101, 130}},
           Rejected{"negative crop origin", &source, &destination, {-1, 0, 10, 2}},
           Rejected{"empty crop", &source, &destination, {0, 0, 0, 130}},
           Rejected{"R32F destination", &source, &r32f, {0, 0, 1300, 130}},
           Rejected{"one texture", &destination, &destination, {0, 0, 10, 2}},
       }) {
    EXPECT_THROW(vulkan::geometry::EncodeCropResize(backend, command_context, *r.source,
                                                    *r.destination, r.crop),
                 std::invalid_argument)
        << r.name;
  }
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(delta.pipeline_creations + delta.pipeline_cache_hits, 0U);
  EXPECT_FALSE(command_context.IsRecording());
}

}  // namespace
}  // namespace alcedo
