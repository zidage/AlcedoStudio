//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// vulkan::geometry::EncodeWarpRectilinear against the product OpenCL kernel
// warp_rectilinear_rgba32f (`EncodeWarp` of opencl_develop_pass.cpp) on the same GPU (plan phase
// VK5, decision B9). Each test runs with the validation layer. Set ALCEDO_VULKAN_DEVICE (for
// example "Iris") to select the device; the OpenCL device follows it.

#include "edit/runtime/vulkan/vulkan_dng_warp.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../vulkan/vulkan_opencl_comparison.hpp"
#include "decoders/dng_default_crop.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_kernel_cache.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using vulkan_opencl_comparison::DifferenceRule;
using vulkan_opencl_comparison::ExpectEqualWithinTolerance;

/// Texels whose bilinear footprint is inside the source: the tolerance of
/// OpenClGeometryUtilsTest.WarpRectilinearMatchesCuda. On the position ramp it is a position
/// difference of 1e-5 times the image size (0.006 pixel at 641 pixels).
constexpr double kInteriorTolerance  = 1.0e-5;
/// Texels whose footprint reaches the border value: there a value difference is the position
/// difference itself. The tolerance is this many ulp of the largest coordinate. Measured (VK5
/// record): 2 ulp on Iris Xe (9.8e-4 at about 8000 pixels), less on the RTX 3080 Laptop GPU.
constexpr double kBorderPositionUlps = 8.0;

class VulkanDngWarpTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

/**
 * @brief RGBA32F of the source position: R = (x + 0.5) / width, G = (y + 0.5) / height,
 * B = (R + G) / 2, alpha 0.75 + R / 4.
 *
 * On a plane the bilinear sample is the plane value, so a difference of R or G is a difference of
 * the source position divided by the image size (VK5 record).
 */
auto PositionRamp(std::uint32_t width, std::uint32_t height) -> std::vector<float> {
  std::vector<float> rgba(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      float*      texel = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      const float r     = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
      const float g     = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
      texel[0]          = r;
      texel[1]          = g;
      texel[2]          = 0.5F * (r + g);
      texel[3]          = 0.75F + 0.25F * r;
    }
  }
  return rgba;
}

/// The three coefficient sets of OpenClGeometryUtilsTest.WarpRectilinearMatchesCuda.
auto ThreeSetWarp() -> dng::WarpRectilinear {
  dng::WarpRectilinear warp{};
  warp.coefficient_set_count = 3;
  warp.center_x              = 0.48;
  warp.center_y              = 0.53;
  warp.coefficient_sets[0]   = {1.0, -0.08, 0.012, -0.002, 0.0015, -0.001};
  warp.coefficient_sets[1]   = {1.0, -0.06, 0.010, -0.001, 0.0010, -0.0005};
  warp.coefficient_sets[2]   = {1.0, -0.04, 0.008, -0.001, 0.0005, -0.0002};
  return warp;
}

void CheckCl(cl_int error, const char* operation) {
  if (error != CL_SUCCESS) {
    throw std::runtime_error(std::string("OpenCL DNG warp test: ") + operation +
                             " failed with error " + std::to_string(error));
  }
}

/// `ImageWarpParams` of opencl_develop_pass.cpp (the `OpenClImageWarpParams` of the kernel).
struct OpenClImageWarpParams {
  std::uint32_t coefficient_set_count = 0;
  std::uint32_t width                 = 0;
  std::uint32_t height                = 0;
  float         coefficient_sets[3][6]{};
  float         center_x = 0.5F;
  float         center_y = 0.5F;
};

auto CreateImage(std::uint32_t width, std::uint32_t height, const float* pixels) -> cl_mem {
  const cl_image_format format{CL_RGBA, CL_FLOAT};
  cl_image_desc         desc{};
  desc.image_type          = CL_MEM_OBJECT_IMAGE2D;
  desc.image_width         = width;
  desc.image_height        = height;
  cl_int             error = CL_SUCCESS;
  const cl_mem_flags flags =
      CL_MEM_READ_WRITE | (pixels != nullptr ? CL_MEM_COPY_HOST_PTR : cl_mem_flags{0});
  cl_mem image = clCreateImage(OpenClContext::Instance().Context(), flags, &format, &desc,
                               const_cast<float*>(pixels), &error);
  CheckCl(error, "clCreateImage");
  return image;
}

/// The product OpenCL kernel with the parameters of `EncodeWarp`.
auto WarpOnOpenCl(const std::vector<float>& rgba, std::uint32_t width, std::uint32_t height,
                  const dng::WarpRectilinear& warp) -> std::vector<float> {
  OpenClImageWarpParams params;
  params.coefficient_set_count = warp.coefficient_set_count;
  params.width                 = width;
  params.height                = height;
  params.center_x              = static_cast<float>(warp.center_x);
  params.center_y              = static_cast<float>(warp.center_y);
  for (std::size_t set = 0; set < 3; ++set) {
    for (std::size_t i = 0; i < 6; ++i) {
      params.coefficient_sets[set][i] = static_cast<float>(warp.coefficient_sets[set][i]);
    }
  }
  cl_mem source      = CreateImage(width, height, rgba.data());
  cl_mem destination = CreateImage(width, height, nullptr);
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kGeometryCameraProgramName,
                                                        OpenCL::GpuDag::kWarpRectilinearKernelName);
  CheckCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &source), "arg0");
  CheckCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &destination), "arg1");
  CheckCl(clSetKernelArg(kernel, 2, sizeof(params), &params), "arg2");
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((std::size_t{width} + 15) / 16) * 16,
                                 ((std::size_t{height} + 15) / 16) * 16};
  auto              queue     = OpenClContext::Instance().Queue();
  CheckCl(clEnqueueNDRangeKernel(queue, kernel, 2, nullptr, global, local, 0, nullptr, nullptr),
          "clEnqueueNDRangeKernel");
  std::vector<float> result(rgba.size());
  const std::size_t  origin[3] = {0, 0, 0};
  const std::size_t  region[3] = {width, height, 1};
  CheckCl(clEnqueueReadImage(queue, destination, CL_TRUE, origin, region, 0, 0, result.data(), 0,
                             nullptr, nullptr),
          "clEnqueueReadImage");
  clReleaseMemObject(source);
  clReleaseMemObject(destination);
  return result;
}

auto WarpOnVulkan(const std::vector<float>& rgba, std::uint32_t width, std::uint32_t height,
                  const dng::WarpRectilinear& warp) -> std::vector<float> {
  VulkanRenderDevice device;
  auto&              backend     = device.Workspace().Device();
  auto&              context     = device.CommandContext();
  auto               source      = backend.CreateTexture2D(width, height, TextureFormat::Rgba32f);
  auto               destination = backend.CreateTexture2D(width, height, TextureFormat::Rgba32f);
  backend.UploadTexture2D(source, std::as_bytes(std::span<const float>(rgba)), context);
  vulkan::geometry::EncodeWarpRectilinear(backend, context, source, destination, warp);
  std::vector<float> result(rgba.size());
  backend.DownloadTexture2D(destination, std::as_writable_bytes(std::span<float>(result)), context);
  return result;
}

/// The source position of output texel (x, y) for @p plane, in double: the model of
/// `WarpRectilinearSource` of geometry_camera.cl.
auto SourcePosition(int x, int y, int plane, std::uint32_t width, std::uint32_t height,
                    const dng::WarpRectilinear& warp) -> std::array<double, 2> {
  const double x1 = std::max(static_cast<int>(width) - 1, 0);
  const double y1 = std::max(static_cast<int>(height) - 1, 0);
  const double cx = warp.center_x * x1;
  const double cy = warp.center_y * y1;
  const double m  = std::hypot(std::max(cx, x1 - cx), std::max(cy, y1 - cy));
  const auto&  c  = warp.coefficient_sets[warp.coefficient_set_count <= 1 ? 0 : plane];
  const double dx = (x - cx) / m;
  const double dy = (y - cy) / m;
  const double r2 = dx * dx + dy * dy;
  const double f  = c[0] + c[1] * r2 + c[2] * r2 * r2 + c[3] * r2 * r2 * r2;
  return {cx + m * (f * dx + c[4] * 2.0 * dx * dy + c[5] * (r2 + 2.0 * dx * dx)),
          cy + m * (f * dy + c[5] * 2.0 * dx * dy + c[4] * (r2 + 2.0 * dy * dy))};
}

/// Whether the bilinear footprint of @p position (texel centers at integers) reaches outside the
/// source, with a margin of one texel for the position difference of the two backends.
auto ReachesBorder(std::array<double, 2> position, std::uint32_t width, std::uint32_t height)
    -> bool {
  return position[0] < 1.0 || position[1] < 1.0 || position[0] > width - 2.0 ||
         position[1] > height - 2.0;
}

void ExpectWarpEqualsOpenCl(const std::string& label, std::uint32_t width, std::uint32_t height,
                            const dng::WarpRectilinear& warp) {
  const auto rgba   = PositionRamp(width, height);
  const auto opencl = WarpOnOpenCl(rgba, width, height, warp);
  const auto vulkan = WarpOnVulkan(rgba, width, height, warp);
  // The warp moves the texels: the result is not the input.
  EXPECT_NE(opencl, rgba) << label;

  std::vector<float> interior_opencl, interior_vulkan, border_opencl, border_vulkan;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        // Alpha samples at the green position.
        const int  plane  = channel == 3 ? 1 : channel;
        const auto index  = (static_cast<std::size_t>(y) * width + x) * 4 + channel;
        const bool border = ReachesBorder(
            SourcePosition(static_cast<int>(x), static_cast<int>(y), plane, width, height, warp),
            width, height);
        (border ? border_opencl : interior_opencl).push_back(opencl[index]);
        (border ? border_vulkan : interior_vulkan).push_back(vulkan[index]);
      }
    }
  }
  const float  largest_coordinate = static_cast<float>(std::max(width, height));
  const double border_tolerance =
      kBorderPositionUlps *
      (std::nextafter(largest_coordinate, 2.0F * largest_coordinate) - largest_coordinate);
  ExpectEqualWithinTolerance(label + " interior", interior_opencl, interior_vulkan,
                             kInteriorTolerance, DifferenceRule::Absolute);
  ExpectEqualWithinTolerance(label + " border", border_opencl, border_vulkan, border_tolerance,
                             DifferenceRule::Absolute);
}

TEST_F(VulkanDngWarpTest, OneCoefficientSetEqualsOpenCl) {
  dng::WarpRectilinear warp  = ThreeSetWarp();
  warp.coefficient_set_count = 1;
  ExpectWarpEqualsOpenCl("one set", 641, 427, warp);
}

TEST_F(VulkanDngWarpTest, ThreeCoefficientSetsEqualOpenCl) {
  ExpectWarpEqualsOpenCl("three sets", 641, 427, ThreeSetWarp());
}

TEST_F(VulkanDngWarpTest, WarpOfARealDngEqualsOpenCl) {
  // The OpcodeList3 WarpRectilinear of a real DNG, at the size of its sensor image.
  const auto path = std::filesystem::path(ALCEDO_CI_RAW_FIXTURE_ROOT) /
                    "tag @ryanbreitkreutz - free raws from @signatureeditsco - DSC06683.dng";
  std::ifstream file(path, std::ios::binary);
  ASSERT_TRUE(file) << "test image is missing: " << path;
  const std::vector<char> chars((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  const auto input =
      RawInputLoader::LoadEncoded(std::span<const std::byte>(bytes), DecodeRes::FULL);
  ASSERT_TRUE(input.dng_warp_rectilinear.has_value());
  ExpectWarpEqualsOpenCl(
      "real DNG (" + std::to_string(input.dng_warp_rectilinear->coefficient_set_count) + " sets)",
      input.host_extent.width, input.host_extent.height, *input.dng_warp_rectilinear);
}

TEST_F(VulkanDngWarpTest, InvalidRequestIsRejectedBeforeACommand) {
  VulkanRenderDevice       device;
  auto&                    backend = device.Workspace().Device();
  auto&                    context = device.CommandContext();
  auto                     image   = backend.CreateTexture2D(64, 48, TextureFormat::Rgba32f);
  auto                     other   = backend.CreateTexture2D(64, 48, TextureFormat::Rgba32f);
  auto                     smaller = backend.CreateTexture2D(64, 47, TextureFormat::Rgba32f);
  auto                     r32f    = backend.CreateTexture2D(64, 48, TextureFormat::R32f);
  VulkanBackend::Texture2D empty;
  const auto               warp    = ThreeSetWarp();
  dng::WarpRectilinear     no_sets = warp;
  no_sets.coefficient_set_count    = 0;

  struct Rejected {
    const char*           name;
    std::function<void()> encode;
  };
  const std::vector<Rejected> requests = {
      {"one texture as source and destination",
       [&] { vulkan::geometry::EncodeWarpRectilinear(backend, context, image, image, warp); }},
      {"destination of another size",
       [&] { vulkan::geometry::EncodeWarpRectilinear(backend, context, image, smaller, warp); }},
      {"R32F destination",
       [&] { vulkan::geometry::EncodeWarpRectilinear(backend, context, image, r32f, warp); }},
      {"empty source",
       [&] { vulkan::geometry::EncodeWarpRectilinear(backend, context, empty, other, warp); }},
      {"no coefficient set",
       [&] { vulkan::geometry::EncodeWarpRectilinear(backend, context, image, other, no_sets); }},
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
