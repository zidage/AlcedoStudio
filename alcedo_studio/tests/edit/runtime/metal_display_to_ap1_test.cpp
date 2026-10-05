//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Metal DisplayToAp1 kernels against OpenColorIO 2.5.1 and the host evaluation of the shared
// per-pixel code (raster_image_input_plan.md, section 5.7).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "aces2_inverse_ocio_reference.hpp"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/drt/aces2_inverse_runtime.hpp"
#include "edit/runtime/metal/metal_display_to_ap1_pass.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalDisplayToAp1Test : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
  }
};

/// Run the kernel over interleaved RGB and return interleaved RGB.
auto RunOnMetal(MetalRenderDevice& device, MetalDisplayToAp1Parameters& parameters,
                std::span<const float> packed, const std::vector<float>& rgb,
                DisplayToAp1Output output_kind) -> std::vector<float> {
  const std::size_t  pixels = rgb.size() / 3;
  const auto         width  = static_cast<std::uint32_t>(std::min<std::size_t>(pixels, 1024));
  const auto         height = static_cast<std::uint32_t>((pixels + width - 1) / width);
  std::vector<float> host(static_cast<std::size_t>(width) * height * 4, 0.0f);
  for (std::size_t i = 0; i < pixels; ++i) {
    host[i * 4]     = rgb[i * 3];
    host[i * 4 + 1] = rgb[i * 3 + 1];
    host[i * 4 + 2] = rgb[i * 3 + 2];
    host[i * 4 + 3] = 0.25f;
  }
  auto& backend  = device.Workspace().Device();
  auto& textures = device.Workspace().Textures();
  device.BeginRender();
  auto src = textures.Acquire({width, height, TextureFormat::Rgba32f});
  auto dst = textures.Acquire({width, height, TextureFormat::Rgba32f});
  backend.UploadTexture2D(src.Texture(), std::as_bytes(std::span<const float>(host)),
                          device.CommandContext());
  const auto& params = parameters.Upload(backend, device.CommandContext(), packed);
  EncodeMetalDisplayToAp1(backend, device.CommandContext(), src.Texture(), dst.Texture(), params,
                          output_kind);
  device.EndRender();
  device.WaitIdle();
  backend.DownloadTexture2D(dst.Texture(), std::as_writable_bytes(std::span<float>(host)),
                            device.CommandContext());
  std::vector<float> out(rgb.size());
  for (std::size_t i = 0; i < pixels; ++i) {
    out[i * 3]     = host[i * 4];
    out[i * 3 + 1] = host[i * 4 + 1];
    out[i * 3 + 2] = host[i * 4 + 2];
    EXPECT_EQ(host[i * 4 + 3], 0.25f);
  }
  return out;
}

TEST_F(MetalDisplayToAp1Test, Aces2InverseMatchesOcioCpuProcessor) {
  MetalRenderDevice           device;
  MetalDisplayToAp1Parameters parameters;
  for (const auto& c : aces2_inverse_test::InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = ResolveAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const auto grid    = aces2_inverse_test::DisplayGrid(c.peak_nits_);
    aces2_inverse_test::ExpectMatchesOcio(
        c, grid,
        RunOnMetal(device, parameters, runtime->packed_, grid, DisplayToAp1Output::LinearAp0));
  }
}

TEST_F(MetalDisplayToAp1Test, Aces2InverseReturnsBlackForBlackAndNeutralForSourceWhite) {
  MetalRenderDevice           device;
  MetalDisplayToAp1Parameters parameters;
  for (const auto& c : aces2_inverse_test::InverseCases()) {
    SCOPED_TRACE(c.name_);
    const auto         runtime = ResolveAces2InverseRuntime(c.primaries_, c.peak_nits_);
    const float        peak    = c.peak_nits_ / 100.0f;
    std::vector<float> rgb     = {0.0f, 0.0f, 0.0f};
    for (const float fraction : {0.0005f, 0.05f, 0.5f, 0.9f}) {
      rgb.insert(rgb.end(), 3, fraction * peak);
    }
    const auto out =
        RunOnMetal(device, parameters, runtime->packed_, rgb, DisplayToAp1Output::LinearAp0);
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

TEST_F(MetalDisplayToAp1Test, AcesccOutputMatchesHostEvaluationForBothBranches) {
  MetalRenderDevice           device;
  MetalDisplayToAp1Parameters parameters;
  const auto         display = ResolveAces2InverseRuntime(kRasterPrimariesDisplayP3, 100.0f);
  const auto         scene   = PackSceneLinearToAp1(kRasterPrimariesAp0);
  std::vector<float> rgb;
  for (int i = 0; i < 64; ++i) {
    rgb.push_back(static_cast<float>(i % 4) / 3.0f);
    rgb.push_back(static_cast<float>((i / 4) % 4) / 3.0f);
    rgb.push_back(static_cast<float>(i / 16) / 3.0f);
  }
  for (const std::span<const float> packed :
       {std::span<const float>(display->packed_), std::span<const float>(scene)}) {
    const auto gpu = RunOnMetal(device, parameters, packed, rgb, DisplayToAp1Output::AcesccAp1);
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

}  // namespace
}  // namespace alcedo
