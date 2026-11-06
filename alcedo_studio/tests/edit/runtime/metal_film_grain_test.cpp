//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the Metal DRT/Post Film Grain (plan 'film_grain_format_and_stock_plan.md',
// phase G4): the checks of film_grain_post_checks.hpp that the CUDA test runs. The Mac has no
// CUDA, so the standard deviation is checked against the host form of the steps, which the CUDA
// kernel equals within 2e-5
// (CudaFilmGrainPostTest.KernelEqualsTheHostFormOfItsStepsAtTheTileEdges). No stored pixels.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_film_grain_pass.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_grain_post_checks.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using namespace film_grain_post_test;

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalFilmGrainPostTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { expected_ = LoadLabValues(); }
  static void TearDownTestSuite() { expected_.reset(); }

  void        SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device: unverified support, not a pass.";
    }
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string()
                   << ". Configure with ALCEDO_FILM_PRINT_RESOURCE_DIR. This is not a pass.";
    }
    device_ = std::make_unique<MetalRenderDevice>();
  }

  void RequireLabValues() {
    if (expected_ == nullptr) GTEST_SKIP() << "No lab grain__expected.bin. This is not a pass.";
  }

  auto Renderer() -> Render {
    return [this](const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                  const std::vector<Rgba>& source, FilmGrainDisplayEncoding encoding) {
      const auto width  = geometry.render_extent.width;
      const auto height = geometry.render_extent.height;
      device_->BeginRender();
      auto& workspace = device_->Workspace();
      auto  src       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
      auto  dst       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
      workspace.Device().UploadTexture2D(src.Texture(), std::as_bytes(std::span(source)),
                                         device_->CommandContext());
      RecordMetalFilmGrainPost(*device_, plan, geometry, encoding, src.Texture(), dst.Texture(),
                               width, height);
      std::vector<Rgba> output(source.size());
      workspace.Device().DownloadTexture2D(dst.Texture(), std::as_writable_bytes(std::span(output)),
                                           device_->CommandContext());
      device_->CancelRender();
      return output;
    };
  }

  std::unique_ptr<MetalRenderDevice>           device_;
  static inline std::unique_ptr<FilmArrayFile> expected_;
};

TEST_F(MetalFilmGrainPostTest, PixelSigmaAtGreyMatchesTheEvaluatorForEachFormatAndSize) {
  RequireLabValues();
  ExpectPixelSigmaAtGreyMatchesTheEvaluator(Renderer(), *expected_);
}

TEST_F(MetalFilmGrainPostTest, HalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled) {
  ExpectHalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled(Renderer());
}

TEST_F(MetalFilmGrainPostTest, ViewportRegionEqualsTheSameRegionOfTheFullRender) {
  ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(Renderer());
}

TEST_F(MetalFilmGrainPostTest, ChannelCorrelationAtGreyIsTheScanCorrelationOf660) {
  ExpectChannelCorrelationAtGreyIsTheScanCorrelationOf660(Renderer());
}

TEST_F(MetalFilmGrainPostTest, St2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput) {
  ExpectSt2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput(Renderer());
}

TEST_F(MetalFilmGrainPostTest, KernelEqualsTheHostFormOfItsStepsAtTheTileEdges) {
  ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(Renderer());
}

TEST_F(MetalFilmGrainPostTest, LargestBlurRadiusFitsTheThreadgroupMemoryAndEqualsTheHostForm) {
  ExpectLargestBlurRadiusEqualsTheHostForm(Renderer());
}

TEST_F(MetalFilmGrainPostTest, StandardDeviationIsWithin3PercentOfTheFormOfCuda) {
  const Render host = [](const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                         const std::vector<Rgba>& source, FilmGrainDisplayEncoding encoding) {
    return HostForm(plan, geometry, source, encoding);
  };
  ExpectSigmaAtGreyMatches(Renderer(), host, 0.03);
}

TEST_F(MetalFilmGrainPostTest, DisabledPlanIsRejectedBeforeAnyKernel) {
  const auto geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, 64, 64);
  const auto plan     = Plan(geometry, FilmFormat::Film35mm, 0.0f);
  device_->BeginRender();
  auto& workspace = device_->Workspace();
  auto  src       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  auto  dst       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  EXPECT_THROW(RecordMetalFilmGrainPost(*device_, plan, geometry, kSrgb, src.Texture(),
                                        dst.Texture(), 64, 64),
               std::invalid_argument);
  device_->CancelRender();
}

}  // namespace
}  // namespace alcedo
