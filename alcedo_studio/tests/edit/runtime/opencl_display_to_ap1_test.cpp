//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// OpenCL DisplayToAp1 kernels against OpenColorIO 2.5.1 and the host evaluation of the shared
// per-pixel code (raster_image_input_plan.md, section 5.7).

#include <CL/cl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

#include "aces2_inverse_ocio_reference.hpp"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/drt/aces2_inverse_runtime.hpp"
#include "edit/runtime/opencl/opencl_display_to_ap1_pass.hpp"
#include "image/raster_color_description.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

class OpenClDisplayToAp1Test : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
  }
};

auto CreateRgba32fImage(cl_context context, std::size_t width, std::size_t height) -> cl_mem {
  const cl_image_format format{CL_RGBA, CL_FLOAT};
  cl_image_desc         desc{};
  desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
  desc.image_width  = width;
  desc.image_height = height;
  cl_int status     = CL_SUCCESS;
  cl_mem image      = clCreateImage(context, CL_MEM_READ_WRITE, &format, &desc, nullptr, &status);
  if (status != CL_SUCCESS) {
    throw std::runtime_error("clCreateImage failed");
  }
  return image;
}

/// Run the kernel over interleaved RGB (one row) and return interleaved RGB.
auto RunOnOpenCl(std::span<const float> packed, const std::vector<float>& rgb,
                 DisplayToAp1Output output_kind, OpenClDisplayToAp1Parameters& parameters)
    -> std::vector<float> {
  auto&              cl     = OpenClContext::Instance();
  const std::size_t  pixels = rgb.size() / 3;
  // Lay the pixels out in rows of at most 1024.
  const std::size_t  width  = std::min<std::size_t>(pixels, 1024);
  const std::size_t  height = (pixels + width - 1) / width;
  std::vector<float> host(width * height * 4, 0.0f);
  for (std::size_t i = 0; i < pixels; ++i) {
    host[i * 4]     = rgb[i * 3];
    host[i * 4 + 1] = rgb[i * 3 + 1];
    host[i * 4 + 2] = rgb[i * 3 + 2];
    host[i * 4 + 3] = 0.25f;
  }
  cl_mem                           src    = CreateRgba32fImage(cl.Context(), width, height);
  cl_mem                           dst    = CreateRgba32fImage(cl.Context(), width, height);
  const std::array<std::size_t, 3> origin = {0, 0, 0};
  const std::array<std::size_t, 3> region = {width, height, 1};
  EXPECT_EQ(clEnqueueWriteImage(cl.Queue(), src, CL_TRUE, origin.data(), region.data(), 0, 0,
                                host.data(), 0, nullptr, nullptr),
            CL_SUCCESS);
  cl_mem params = parameters.Upload(cl.Context(), cl.Queue(), packed);
  EnqueueOpenClDisplayToAp1(cl.Queue(), src, dst, static_cast<std::uint32_t>(width),
                            static_cast<std::uint32_t>(height), params, 0, output_kind, nullptr);
  EXPECT_EQ(clEnqueueReadImage(cl.Queue(), dst, CL_TRUE, origin.data(), region.data(), 0, 0,
                               host.data(), 0, nullptr, nullptr),
            CL_SUCCESS);
  clReleaseMemObject(src);
  clReleaseMemObject(dst);
  std::vector<float> out(rgb.size());
  for (std::size_t i = 0; i < pixels; ++i) {
    out[i * 3]     = host[i * 4];
    out[i * 3 + 1] = host[i * 4 + 1];
    out[i * 3 + 2] = host[i * 4 + 2];
    EXPECT_EQ(host[i * 4 + 3], 0.25f);
  }
  return out;
}

TEST_F(OpenClDisplayToAp1Test, Aces2InverseMatchesOcioCpuProcessor) {
  OpenClDisplayToAp1Parameters parameters;
  for (const auto& c : aces2_inverse_test::InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = ResolveAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const auto grid    = aces2_inverse_test::DisplayGrid(c.peak_nits_);
    aces2_inverse_test::ExpectMatchesOcio(
        c, grid, RunOnOpenCl(runtime->packed_, grid, DisplayToAp1Output::LinearAp0, parameters));
  }
}

TEST_F(OpenClDisplayToAp1Test, Aces2InverseReturnsBlackForBlackAndNeutralForSourceWhite) {
  OpenClDisplayToAp1Parameters parameters;
  for (const auto& c : aces2_inverse_test::InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto         runtime = ResolveAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const float        peak    = c.peak_nits_ / 100.0f;
    std::vector<float> rgb     = {0.0f, 0.0f, 0.0f};
    for (const float fraction : {0.0005f, 0.05f, 0.5f, 0.9f}) {
      rgb.insert(rgb.end(), 3, fraction * peak);
    }
    const auto out = RunOnOpenCl(runtime->packed_, rgb, DisplayToAp1Output::LinearAp0, parameters);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[1], 0.0f);
    EXPECT_EQ(out[2], 0.0f);
    for (std::size_t p = 1; p < out.size() / 3; ++p) {
      const auto [lo, hi] = std::minmax({out[p * 3], out[p * 3 + 1], out[p * 3 + 2]});
      ASSERT_GT(lo, 0.0f);
      EXPECT_LT(hi / lo - 1.0f, 1e-4f) << "level index " << p;
    }
  }
}

TEST_F(OpenClDisplayToAp1Test, AcesccOutputMatchesHostEvaluationForBothBranches) {
  OpenClDisplayToAp1Parameters parameters;
  const auto                   display =
      ResolveAces2InverseRuntime(color::GamutPrimariesXy(color::ColorGamutId::P3D65), 100.0f);
  const auto scene = PackSceneLinearToAp1(color::GamutPrimariesXy(color::ColorGamutId::Ap0));
  std::vector<float> rgb;
  for (int i = 0; i < 64; ++i) {
    rgb.push_back(static_cast<float>(i % 4) / 3.0f);
    rgb.push_back(static_cast<float>((i / 4) % 4) / 3.0f);
    rgb.push_back(static_cast<float>(i / 16) / 3.0f);
  }
  for (const std::span<const float> packed :
       {std::span<const float>(display->packed_), std::span<const float>(scene)}) {
    const auto gpu = RunOnOpenCl(packed, rgb, DisplayToAp1Output::AcesccAp1, parameters);
    // 2e-4 in ACEScc is 0.25 percent in linear light: device and host pow differ in the last
    // bits.
    for (std::size_t p = 0; p < rgb.size() / 3; ++p) {
      const auto host =
          D2aSourceToAcesccAp1(D2aMake3(rgb[p * 3], rgb[p * 3 + 1], rgb[p * 3 + 2]), packed.data());
      EXPECT_NEAR(gpu[p * 3], host.x, 2e-4f) << "pixel " << p;
      EXPECT_NEAR(gpu[p * 3 + 1], host.y, 2e-4f) << "pixel " << p;
      EXPECT_NEAR(gpu[p * 3 + 2], host.z, 2e-4f) << "pixel " << p;
    }
  }
}

TEST_F(OpenClDisplayToAp1Test, ParameterBlockIsUploadedOnlyWhenItChanges) {
  auto&                        cl = OpenClContext::Instance();
  OpenClDisplayToAp1Parameters parameters;
  const auto                   a =
      ResolveAces2InverseRuntime(color::GamutPrimariesXy(color::ColorGamutId::Rec709), 100.0f);
  const auto b =
      ResolveAces2InverseRuntime(color::GamutPrimariesXy(color::ColorGamutId::Rec2020), 100.0f);
  (void)parameters.Upload(cl.Context(), cl.Queue(), a->packed_);
  (void)parameters.Upload(cl.Context(), cl.Queue(), a->packed_);
  EXPECT_EQ(parameters.UploadCount(), 1u);
  (void)parameters.Upload(cl.Context(), cl.Queue(), b->packed_);
  EXPECT_EQ(parameters.UploadCount(), 2u);
}

}  // namespace
}  // namespace alcedo
