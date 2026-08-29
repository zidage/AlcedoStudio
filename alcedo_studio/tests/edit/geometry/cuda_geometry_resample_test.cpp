//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/cuda/geometry_resample_pass.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaGeometryFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto ReadBorder(const std::vector<Rgba>& src, int width, int height, int x, int y, Rgba border)
    -> Rgba {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return border;
  }
  return src[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(x)];
}

auto BilinearSample(const std::vector<Rgba>& src, int width, int height, float sx, float sy,
                    Rgba border) -> Rgba {
  const float px = sx - 0.5f;
  const float py = sy - 0.5f;
  const int   x0 = static_cast<int>(std::floor(px));
  const int   y0 = static_cast<int>(std::floor(py));
  const float fx = px - static_cast<float>(x0);
  const float fy = py - static_cast<float>(y0);
  const auto  p00 = ReadBorder(src, width, height, x0, y0, border);
  const auto  p10 = ReadBorder(src, width, height, x0 + 1, y0, border);
  const auto  p01 = ReadBorder(src, width, height, x0, y0 + 1, border);
  const auto  p11 = ReadBorder(src, width, height, x0 + 1, y0 + 1, border);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;
  Rgba        out;
  out.r = w00 * p00.r + w10 * p10.r + w01 * p01.r + w11 * p11.r;
  out.g = w00 * p00.g + w10 * p10.g + w01 * p01.g + w11 * p11.g;
  out.b = w00 * p00.b + w10 * p10.b + w01 * p01.b + w11 * p11.b;
  out.a = w00 * p00.a + w10 * p10.a + w01 * p01.a + w11 * p11.a;
  return out;
}

auto MakeSrcImage(std::uint32_t width, std::uint32_t height) -> std::vector<Rgba> {
  std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      auto& p = pixels[static_cast<std::size_t>(y) * width + x];
      p.r     = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
      p.g     = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
      p.b     = 0.25f;
      p.a     = 1.0f;
    }
  }
  return pixels;
}

auto AsBytes(const std::vector<Rgba>& pixels) -> std::span<const std::byte> {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(pixels.data()),
                                    pixels.size() * sizeof(Rgba));
}

}  // namespace

TEST_F(CudaGeometryFixture, CropRotateViewportAndScaleExecuteAsOneCudaResample) {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.25f, 0.25f, 0.50f, 0.50f};
  image.rotation_degrees = 15.0f;
  image.expand_to_fit    = true;
  ViewRequest view;
  view.visible_rect_in_edit_space = NormalizedRect{0.10f, 0.10f, 0.80f, 0.80f};
  view.viewport_extent            = Extent2D{40, 30};
  const auto source               = MakeSourceGeometry({64, 48}, {64, 48});
  const auto geometry = ResolveRenderGeometry(source, image, view, {}, {});
  ASSERT_EQ(geometry.decoded_extent, (Extent2D{64, 48}));
  ASSERT_EQ(geometry.render_extent, (Extent2D{40, 30}));

  const auto host_src = MakeSrcImage(64, 48);
  CudaRenderDevice device;
  auto&            textures = device.Workspace().Textures();
  textures.SetByteBudget(64ull * 48ull * 16ull * 4ull);

  device.BeginRender();
  auto src = textures.Acquire({64, 48, TextureFormat::Rgba32f});
  auto dst = textures.Acquire(
      {geometry.render_extent.width, geometry.render_extent.height, TextureFormat::Rgba32f});
  device.Workspace().Device().UploadTexture2D(src.Texture(), AsBytes(host_src),
                                              device.CommandContext());

  GeometryResamplePass pass;
  pass.Encode(geometry, src.Texture(), dst.Texture(), device.CommandContext());
  EXPECT_EQ(pass.LaunchCount(), 1u);
  device.EndRender();
  device.WaitIdle();

  std::vector<Rgba> host_dst(static_cast<std::size_t>(geometry.render_extent.width) *
                             geometry.render_extent.height);
  auto              out_bytes = std::span<std::byte>(reinterpret_cast<std::byte*>(host_dst.data()),
                                                     host_dst.size() * sizeof(Rgba));
  device.BeginRender();
  device.Workspace().Device().DownloadTexture2D(dst.Texture(), out_bytes, device.CommandContext());
  device.EndRender();
  device.WaitIdle();

  const Rgba border{0.0f, 0.0f, 0.0f, 1.0f};
  float      max_err = 0.0f;
  for (std::uint32_t y = 0; y < geometry.render_extent.height; ++y) {
    for (std::uint32_t x = 0; x < geometry.render_extent.width; ++x) {
      const auto center = PixelCenter(x, y);
      const auto src_xy = TransformPoint(geometry.render_to_decoded, center);
      const auto cpu    = BilinearSample(host_src, 64, 48, src_xy.x, src_xy.y, border);
      const auto& gpu   = host_dst[static_cast<std::size_t>(y) * 40u + x];
      max_err = std::max(max_err, std::fabs(cpu.r - gpu.r));
      max_err = std::max(max_err, std::fabs(cpu.g - gpu.g));
      max_err = std::max(max_err, std::fabs(cpu.b - gpu.b));
      max_err = std::max(max_err, std::fabs(cpu.a - gpu.a));
    }
  }
  EXPECT_LT(max_err, 1.0e-4f);

  device.Workspace().Device().ResetCounters();
  device.BeginRender();
  pass.Encode(geometry, src.Texture(), dst.Texture(), device.CommandContext());
  EXPECT_EQ(pass.LaunchCount(), 2u);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0u);
  EXPECT_EQ(device.Workspace().Device().FreeCount(), 0u);
}

}  // namespace alcedo
