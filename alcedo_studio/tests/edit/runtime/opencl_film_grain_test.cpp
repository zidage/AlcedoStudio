//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the OpenCL DRT/Post Film Grain (plan 'film_grain_format_and_stock_plan.md',
// phase G4): the checks of film_grain_post_checks.hpp that the CUDA test runs, and the standard
// deviation against CUDA. No stored pixels.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>

#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#endif

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_film_grain_pass.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_grain_post_checks.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using namespace film_grain_post_test;

auto HasOpenClImageDevice() -> bool {
  return TryInitializeOpenClRuntime() && OpenClContext::Instance().Capabilities().image_support;
}

auto ImageView(const OpenClBackend::Texture2D& texture) -> OpenClSceneView {
  return OpenClSceneView{texture.Native(), texture.Width(), texture.Height(), false};
}

class OpenClFilmGrainPostTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { expected_ = LoadLabValues(); }
  static void TearDownTestSuite() { expected_.reset(); }

  void        SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device: unverified support, not a pass.";
    }
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string()
                   << ". Configure with ALCEDO_FILM_PRINT_RESOURCE_DIR. This is not a pass.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
  }

  void RequireLabValues() {
    if (expected_ == nullptr) GTEST_SKIP() << "No lab grain__expected.bin. This is not a pass.";
  }

  /// The grain pass from one image to another.
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
      RecordOpenClFilmGrainPost(*device_, plan, geometry, encoding, ImageView(src.Texture()),
                                ImageView(dst.Texture()), width, height);
      std::vector<Rgba> output(source.size());
      workspace.Device().DownloadTexture2D(dst.Texture(), std::as_writable_bytes(std::span(output)),
                                           device_->CommandContext());
      device_->EndRender();
      device_->WaitIdle();
      return output;
    };
  }

  std::unique_ptr<OpenClRenderDevice>          device_;
  static inline std::unique_ptr<FilmArrayFile> expected_;
};

TEST_F(OpenClFilmGrainPostTest, PixelSigmaAtGreyMatchesTheEvaluatorForEachFormatAndSize) {
  RequireLabValues();
  ExpectPixelSigmaAtGreyMatchesTheEvaluator(Renderer(), *expected_);
}

TEST_F(OpenClFilmGrainPostTest, HalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled) {
  ExpectHalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled(Renderer());
}

TEST_F(OpenClFilmGrainPostTest, ViewportRegionEqualsTheSameRegionOfTheFullRender) {
  ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(Renderer());
}

TEST_F(OpenClFilmGrainPostTest, ChannelCorrelationAtGreyIsTheScanCorrelationOf660) {
  ExpectChannelCorrelationAtGreyIsTheScanCorrelationOf660(Renderer());
}

TEST_F(OpenClFilmGrainPostTest, St2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput) {
  ExpectSt2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput(Renderer());
}

TEST_F(OpenClFilmGrainPostTest, KernelEqualsTheHostFormOfItsStepsAtTheTileEdges) {
  ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(Renderer());
}

TEST_F(OpenClFilmGrainPostTest, LargestBlurRadiusFitsTheLocalMemoryAndEqualsTheHostForm) {
  ExpectLargestBlurRadiusEqualsTheHostForm(Renderer());
}

TEST_F(OpenClFilmGrainPostTest, SceneWorkBuffersGiveTheResultOfImages) {
  // The DRT/Post writes read and write the scene work buffers between the display transform and
  // the display image.
  constexpr std::uint32_t kWidth   = 70;
  constexpr std::uint32_t kHeight  = 45;
  const auto              geometry = TileGeometry(6000, 1.0f, 40.0f, 20.0f, kWidth, kHeight);
  const auto              plan     = Plan(geometry, FilmFormat::Film35mm);
  const auto              input    = PostGrey(plan, geometry);
  const auto              images   = Renderer()(plan, geometry, input, kSrgb);

  device_->BeginRender();
  auto& workspace = device_->Workspace();
  workspace.EnsureSceneWorkImages({kWidth, kHeight});
  auto& backend = workspace.Device();
  auto& source  = workspace.SceneWork().Member(SceneWorkMember::Member0);
  auto& output  = workspace.SceneWork().Member(SceneWorkMember::Member1);
  backend.UploadBufferRange(source.Storage(), 0, std::as_bytes(std::span(input)),
                            device_->CommandContext());
  RecordOpenClFilmGrainPost(
      *device_, plan, geometry, kSrgb,
      OpenClBindScene(*device_, FrameSceneBinding::WorkImage(SceneWorkMember::Member0)),
      OpenClBindScene(*device_, FrameSceneBinding::WorkImage(SceneWorkMember::Member1)), kWidth,
      kHeight);
  std::vector<Rgba> buffers(input.size());
  backend.DownloadBufferRange(output.Storage(), 0, std::as_writable_bytes(std::span(buffers)),
                              device_->CommandContext());
  device_->CancelRender();
  EXPECT_EQ(LargestDifference(buffers, images), 0.0f);
}

TEST_F(OpenClFilmGrainPostTest, StandardDeviationIsWithin3PercentOfCuda) {
#ifndef HAVE_CUDA
  GTEST_SKIP() << "This build has no CUDA. This is not a pass.";
#else
  int count = 0;
  if (::cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
    GTEST_SKIP() << "No CUDA device available. This is not a pass.";
  }
  CudaRenderDevice cuda;
  const Render     cuda_render =
      [&cuda](const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
              const std::vector<Rgba>& source, FilmGrainDisplayEncoding encoding) {
        const auto width  = geometry.render_extent.width;
        const auto height = geometry.render_extent.height;
        cuda.BeginRender();
        auto& workspace = cuda.Workspace();
        auto  src       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
        auto  dst       = workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
        workspace.Device().UploadTexture2D(src.Texture(), std::as_bytes(std::span(source)),
                                           cuda.CommandContext());
        RecordCudaFilmGrainPost(cuda, plan, geometry, encoding, src.Texture(), dst.Texture(), width,
                                height);
        std::vector<Rgba> output(source.size());
        workspace.Device().DownloadTexture2D(
            dst.Texture(), std::as_writable_bytes(std::span(output)), cuda.CommandContext());
        cuda.EndRender();
        cuda.WaitIdle();
        return output;
      };
  ExpectSigmaAtGreyMatches(Renderer(), cuda_render, 0.03);
#endif
}

TEST_F(OpenClFilmGrainPostTest, DisabledPlanIsRejectedBeforeAnyKernel) {
  const auto geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, 64, 64);
  const auto plan     = Plan(geometry, FilmFormat::Film35mm, 0.0f);
  device_->BeginRender();
  auto& workspace = device_->Workspace();
  auto  src       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  auto  dst       = workspace.Textures().Acquire({64, 64, TextureFormat::Rgba32f});
  EXPECT_THROW(RecordOpenClFilmGrainPost(*device_, plan, geometry, kSrgb, ImageView(src.Texture()),
                                         ImageView(dst.Texture()), 64, 64),
               std::invalid_argument);
  device_->CancelRender();
}

}  // namespace
}  // namespace alcedo
