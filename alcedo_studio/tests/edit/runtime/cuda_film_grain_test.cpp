//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the CUDA DRT/Post Film Grain (plan 'film_grain_format_and_stock_plan.md',
// phase G3). The checks are those of film_grain_post_checks.hpp, which the OpenCL and Metal tests
// of phase G4 share. No stored pixels.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_grain_post_checks.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using namespace film_grain_post_test;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaFilmGrainPostTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { expected_ = LoadLabValues(); }
  static void TearDownTestSuite() { expected_.reset(); }

  void        SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string();
    }
  }

  void RequireLabValues() {
    if (expected_ == nullptr) GTEST_SKIP() << "No lab grain__expected.bin. This is not a pass.";
  }

  auto Renderer() -> Render {
    return [this](const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                  const std::vector<Rgba>& source, FilmGrainDisplayEncoding encoding) {
      const auto width  = geometry.render_extent.width;
      const auto height = geometry.render_extent.height;
      device_.BeginRender();
      auto& workspace = device_.Workspace();
      auto  src       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
      auto  dst       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
      workspace.Device().UploadTexture2D(src.Texture(), std::as_bytes(std::span(source)),
                                         device_.CommandContext());
      RecordCudaFilmGrainPost(device_, plan, geometry, encoding, src.Texture(), dst.Texture(),
                              width, height);
      std::vector<Rgba> output(source.size());
      workspace.Device().DownloadTexture2D(dst.Texture(), std::as_writable_bytes(std::span(output)),
                                           device_.CommandContext());
      device_.EndRender();
      device_.WaitIdle();
      return output;
    };
  }

  CudaRenderDevice                             device_;
  static inline std::unique_ptr<FilmArrayFile> expected_;
};

TEST_F(CudaFilmGrainPostTest, PixelSigmaAtGreyMatchesTheEvaluatorForEachFormatAndSize) {
  RequireLabValues();
  ExpectPixelSigmaAtGreyMatchesTheEvaluator(Renderer(), *expected_);
}

TEST_F(CudaFilmGrainPostTest, HalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled) {
  ExpectHalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled(Renderer());
}

TEST_F(CudaFilmGrainPostTest, ViewportRegionEqualsTheSameRegionOfTheFullRender) {
  ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(Renderer());
}

TEST_F(CudaFilmGrainPostTest, ChannelCorrelationAtGreyIsTheScanCorrelationOf660) {
  ExpectChannelCorrelationAtGreyIsTheScanCorrelationOf660(Renderer());
}

TEST_F(CudaFilmGrainPostTest, St2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput) {
  ExpectSt2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput(Renderer());
}

TEST_F(CudaFilmGrainPostTest, KernelEqualsTheHostFormOfItsStepsAtTheTileEdges) {
  ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(Renderer());
}

TEST_F(CudaFilmGrainPostTest, LargestBlurRadiusEqualsTheHostForm) {
  ExpectLargestBlurRadiusEqualsTheHostForm(Renderer());
}

TEST_F(CudaFilmGrainPostTest, DisabledPlanIsRejectedBeforeAnyKernel) {
  const auto geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, 64, 64);
  const auto plan     = Plan(geometry, FilmFormat::Film35mm, 0.0f);
  device_.BeginRender();
  auto& workspace = device_.Workspace();
  auto  src       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  auto  dst       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  EXPECT_THROW(
      RecordCudaFilmGrainPost(device_, plan, geometry, kSrgb, src.Texture(), dst.Texture(), 64, 64),
      std::invalid_argument);
  device_.EndRender();
}

}  // namespace
}  // namespace alcedo
