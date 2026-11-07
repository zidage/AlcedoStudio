//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the grain in the negative of the film print on OpenCL (plan
// 'film_grain_format_and_stock_plan.md', phase G6): the kernel checks of
// film_grain_negative_checks.hpp and the film print checks of film_print_grain_checks.hpp that the
// CUDA test of phase G5 runs, and the standard deviation against CUDA. No stored pixels.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>

#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#endif

#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_drt_pass.hpp"
#include "edit/runtime/opencl/opencl_film_grain_pass.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_grain_negative_checks.hpp"
#include "film_print_grain_checks.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using namespace film_grain_negative_test;

auto HasOpenClImageDevice() -> bool {
  return TryInitializeOpenClRuntime() && OpenClContext::Instance().Capabilities().image_support;
}

class OpenClFilmGrainNegativeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasOpenClImageDevice()) {
      GTEST_SKIP() << "No OpenCL image device: unverified support, not a pass.";
    }
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string()
                   << ". Configure with ALCEDO_FILM_PRINT_RESOURCE_DIR. This is not a pass.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
  }

  /// The grain step alone, in place on a buffer of dye amounts.
  auto Renderer() -> Render {
    return [this](const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                  std::span<const double> dye_max, const std::vector<Amounts>& record) {
      auto& backend = device_->Workspace().Device();
      auto  buffer  = backend.CreateBuffer(record.size() * sizeof(Amounts));
      device_->BeginRender();
      backend.UploadBufferRange(buffer, 0, std::as_bytes(std::span(record)),
                                device_->CommandContext());
      RecordOpenClFilmGrainNegative(*device_, plan, geometry, dye_max, buffer.Native(), 0,
                                    geometry.render_extent.width, geometry.render_extent.height);
      std::vector<Amounts> output(record.size());
      backend.DownloadBufferRange(buffer, 0, std::as_writable_bytes(std::span(output)),
                                  device_->CommandContext());
      device_->EndRender();
      device_->WaitIdle();
      return output;
    };
  }

  std::unique_ptr<OpenClRenderDevice> device_;
};

TEST_F(OpenClFilmGrainNegativeTest, DyeAmountSigmaMatchesTheDatasheetForEachPrintStock) {
  ExpectDyeAmountSigmaMatchesTheDatasheet(Renderer());
}

TEST_F(OpenClFilmGrainNegativeTest, PixelSigmaFollowsThePlanForEachFormat) {
  ExpectPixelSigmaFollowsThePlanForEachFormat(Renderer());
}

TEST_F(OpenClFilmGrainNegativeTest, ViewportRegionEqualsTheSameRegionOfTheFullRender) {
  ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(Renderer());
}

TEST_F(OpenClFilmGrainNegativeTest, KernelEqualsTheHostFormOfItsStepsAtTheTileEdges) {
  ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(Renderer());
}

TEST_F(OpenClFilmGrainNegativeTest, StandardDeviationIsWithin3PercentOfCuda) {
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
              std::span<const double> dye_max, const std::vector<Amounts>& record) {
        const auto bytes   = record.size() * sizeof(Amounts);
        void*      amounts = nullptr;
        EXPECT_EQ(::cudaMalloc(&amounts, bytes), cudaSuccess);
        EXPECT_EQ(::cudaMemcpy(amounts, record.data(), bytes, cudaMemcpyHostToDevice), cudaSuccess);
        cuda.BeginRender();
        RecordCudaFilmGrainNegative(cuda, plan, geometry, dye_max, static_cast<float*>(amounts),
                                    geometry.render_extent.width, geometry.render_extent.height);
        cuda.EndRender();
        cuda.WaitIdle();
        std::vector<Amounts> output(record.size());
        EXPECT_EQ(::cudaMemcpy(output.data(), amounts, bytes, cudaMemcpyDeviceToHost), cudaSuccess);
        EXPECT_EQ(::cudaFree(amounts), cudaSuccess);
        return output;
      };
  ExpectSigmaMatches(Renderer(), cuda_render, 0.03);
#endif
}

TEST_F(OpenClFilmGrainNegativeTest, DisabledPlanAndPostPlanAreRejectedBeforeAnyKernel) {
  const auto geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, 64, 64);
  auto       buffer   = device_->Workspace().Device().CreateBuffer(64U * 64U * sizeof(Amounts));
  device_->BeginRender();
  EXPECT_THROW(RecordOpenClFilmGrainNegative(
                   *device_, Plan("kodak_5207", geometry, FilmFormat::Film35mm, 0.0f), geometry, {},
                   buffer.Native(), 0, 64, 64),
               std::invalid_argument);
  const auto post = MakeFilmGrainPlan(geometry, FilmFormat::Film35mm,
                                      SharedFilmPrintTables().Bundle().Grain("kodak_5207"),
                                      FilmGrainTarget::Post, 1.0f);
  EXPECT_THROW(
      RecordOpenClFilmGrainNegative(*device_, post, geometry, {}, buffer.Native(), 0, 64, 64),
      std::invalid_argument);
  device_->CancelRender();
}

/// Wall time of the grain step alone on 6000 x 4000 amounts, 35mm and 16mm, median of 9. Each run
/// waits for the GPU. Run it with --gtest_also_run_disabled_tests.
TEST_F(OpenClFilmGrainNegativeTest, DISABLED_KernelCostRecordAt24Megapixels) {
  constexpr std::uint32_t kWidth   = 6000;
  constexpr std::uint32_t kHeight  = 4000;
  const auto              geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, kWidth, kHeight);
  auto&                   backend  = device_->Workspace().Device();
  auto buffer = backend.CreateBuffer(std::size_t{kWidth} * kHeight * sizeof(Amounts));
  for (const auto format : {FilmFormat::Film35mm, FilmFormat::Film16mm}) {
    const auto plan = Plan("kodak_5207", geometry, format);
    const auto run  = [&]() {
      device_->BeginRender();
      device_->WaitIdle();
      const auto start = std::chrono::steady_clock::now();
      RecordOpenClFilmGrainNegative(*device_, plan, geometry, {}, buffer.Native(), 0, kWidth,
                                     kHeight);
      device_->WaitIdle();
      const std::chrono::duration<double, std::milli> elapsed =
          std::chrono::steady_clock::now() - start;
      device_->CancelRender();
      return elapsed.count();
    };
    (void)run();
    std::vector<double> runs;
    for (int index = 0; index < 9; ++index) runs.push_back(run());
    std::sort(runs.begin(), runs.end());
    std::cout << "grain in the negative, " << FilmFormatId(format)
              << ", 6000 x 4000: " << runs[runs.size() / 2] << " ms\n";
  }
}

/// The film print with the Film Grain of the document, through the DRT pass.
class OpenClFilmPrintGrainTest : public OpenClFilmGrainNegativeTest {
 protected:
  auto FilmPrintRenderer() -> film_print_grain_test::Render {
    return [this](const ExecutionPlan& plan, const PipelineDocument& document, float grey) {
      using film_print_grain_test::Rgba;
      const auto              width  = plan.geometry.render_extent.width;
      const auto              height = plan.geometry.render_extent.height;
      const std::vector<Rgba> scene(static_cast<std::size_t>(width) * height,
                                    Rgba{grey, grey, grey, 1.0f});
      device_->ResetPassStats();
      device_->BeginRender();
      auto& workspace = device_->Workspace();
      workspace.PrepareResultValidity(plan, document, input_);
      workspace.EnsureSceneWorkImages({width, height});
      const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
      auto&              lease =
          workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
      workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(std::span(scene)),
                                         device_->CommandContext());
      const auto result =
          ExecuteOpenClDrt(*device_, plan, document, FrameSceneBinding::CachedImage(scene_id));
      film_print_grain_test::Frame frame;
      frame.post_count = result.post_neighborhood_count;
      frame.pixels.resize(scene.size());
      auto* display = workspace.Images().Find(result.output);
      EXPECT_NE(display, nullptr);
      workspace.Device().DownloadTexture2D(display->Texture(),
                                           std::as_writable_bytes(std::span(frame.pixels)),
                                           device_->CommandContext());
      device_->CancelRender();
      frame.grain_negative_passes = device_->PassStats().film_grain_negative;
      return frame;
    };
  }

  PreparedRawInput input_;
};

TEST_F(OpenClFilmPrintGrainTest, NegativeScanChannelCorrelationIsTheScanCorrelationOf660) {
  film_print_grain_test::ExpectNegativeScanChannelCorrelationIsTheScanCorrelationOf660(
      FilmPrintRenderer());
}

TEST_F(OpenClFilmPrintGrainTest, StrengthZeroEqualsTheFilmPrintWithNoGrainKernel) {
  film_print_grain_test::ExpectStrengthZeroEqualsTheFilmPrintWithNoGrainKernel(FilmPrintRenderer());
}

TEST_F(OpenClFilmPrintGrainTest, FilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue) {
  film_print_grain_test::ExpectFilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue(
      FilmPrintRenderer());
}

TEST_F(OpenClFilmPrintGrainTest, GrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows) {
  film_print_grain_test::ExpectGrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows(
      FilmPrintRenderer());
}

/// Wall time of the DRT pass of the film print (5207 on 2383) at 6000 x 4000 with Film Grain 0 %
/// and 100 % at 35mm and 16mm, median of 9; the difference is the time of the grain. Each run waits
/// for the GPU. Run it with --gtest_also_run_disabled_tests.
TEST_F(OpenClFilmPrintGrainTest, DISABLED_FilmPrintGrainCostRecordAt24Megapixels) {
  using film_print_grain_test::Rgba;
  constexpr std::uint32_t kWidth  = 6000;
  constexpr std::uint32_t kHeight = 4000;
  const std::vector<Rgba> scene(std::size_t{kWidth} * kHeight, Rgba{0.18f, 0.18f, 0.18f, 1.0f});
  const auto              median = [&](const PipelineDocument& document) {
    auto plan                           = film_print_grain_test::Plan(document, 1);
    plan.geometry.full_reference_extent = {kWidth, kHeight};
    plan.geometry.edit_extent           = {kWidth, kHeight};
    plan.geometry.render_extent         = {kWidth, kHeight};
    plan.geometry.render_to_reference   = Matrix3x3::Identity();
    plan.geometry.reference_to_render   = Matrix3x3::Identity();
    plan.geometry.edit_to_render        = Matrix3x3::Identity();
    const auto run                      = [&]() {
      device_->BeginRender();
      auto& workspace = device_->Workspace();
      workspace.PrepareResultValidity(plan, document, input_);
      workspace.EnsureSceneWorkImages({kWidth, kHeight});
      const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
      auto&              lease =
          workspace.AcquireImageForWrite(scene_id, {kWidth, kHeight, TextureFormat::Rgba32f});
      workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(std::span(scene)),
                                                                           device_->CommandContext());
      device_->WaitIdle();
      const auto start = std::chrono::steady_clock::now();
      (void)ExecuteOpenClDrt(*device_, plan, document, FrameSceneBinding::CachedImage(scene_id));
      device_->WaitIdle();
      const std::chrono::duration<double, std::milli> elapsed =
          std::chrono::steady_clock::now() - start;
      device_->CancelRender();
      return elapsed.count();
    };
    (void)run();
    std::vector<double> runs;
    for (int index = 0; index < 9; ++index) runs.push_back(run());
    std::sort(runs.begin(), runs.end());
    return runs[runs.size() / 2];
  };
  // The format also sets the image plane of the film print (phase G7), so each format has its own
  // time without grain.
  for (const auto format : {FilmFormat::Film35mm, FilmFormat::Film16mm}) {
    const double without =
        median(film_print_grain_test::Document("kodak_5207", "kodak_2383", 0.0f, format));
    const double with =
        median(film_print_grain_test::Document("kodak_5207", "kodak_2383", 1.0f, format));
    std::cout << "film print at 6000 x 4000, " << FilmFormatId(format) << ": Film Grain 0 % "
              << without << " ms, 100 % " << with << " ms, grain " << with - without << " ms\n";
  }
}

}  // namespace
}  // namespace alcedo
