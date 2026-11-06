//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the grain in the negative of the film print on CUDA (plan
// 'film_grain_format_and_stock_plan.md', phase G5). The kernel checks are those of
// film_grain_negative_checks.hpp, and the film print checks those of film_print_grain_checks.hpp,
// which render the DRT pass with the Film Grain of the document; the OpenCL and Metal tests of
// phase G6 share both. No stored pixels.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "color/color_encoding_math.h"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/cuda/cuda_film_grain_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_grain_negative_checks.hpp"
#include "film_print_grain_checks.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using namespace film_grain_negative_test;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto HasFilmPrintResources() -> bool {
  return fs::exists(FilmPrintResourceDirectory() / "manifest.json");
}

class CudaFilmGrainNegativeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    if (!HasFilmPrintResources()) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string();
    }
  }

  auto Renderer() -> Render {
    return [this](const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry,
                  std::span<const double> dye_max, const std::vector<Amounts>& record) {
      const auto bytes   = record.size() * sizeof(Amounts);
      void*      amounts = nullptr;
      EXPECT_EQ(::cudaMalloc(&amounts, bytes), cudaSuccess);
      EXPECT_EQ(::cudaMemcpy(amounts, record.data(), bytes, cudaMemcpyHostToDevice), cudaSuccess);
      device.BeginRender();
      RecordCudaFilmGrainNegative(device, plan, geometry, dye_max, static_cast<float*>(amounts),
                                  geometry.render_extent.width, geometry.render_extent.height);
      device.EndRender();
      device.WaitIdle();
      std::vector<Amounts> output(record.size());
      EXPECT_EQ(::cudaMemcpy(output.data(), amounts, bytes, cudaMemcpyDeviceToHost), cudaSuccess);
      EXPECT_EQ(::cudaFree(amounts), cudaSuccess);
      return output;
    };
  }

  CudaRenderDevice device;
};

TEST_F(CudaFilmGrainNegativeTest, DyeAmountSigmaMatchesTheDatasheetForEachPrintStock) {
  ExpectDyeAmountSigmaMatchesTheDatasheet(Renderer());
}

TEST_F(CudaFilmGrainNegativeTest, PixelSigmaFollowsThePlanForEachFormat) {
  ExpectPixelSigmaFollowsThePlanForEachFormat(Renderer());
}

TEST_F(CudaFilmGrainNegativeTest, ViewportRegionEqualsTheSameRegionOfTheFullRender) {
  ExpectViewportRegionEqualsTheSameRegionOfTheFullRender(Renderer());
}

TEST_F(CudaFilmGrainNegativeTest, KernelEqualsTheHostFormOfItsStepsAtTheTileEdges) {
  ExpectKernelEqualsTheHostFormOfItsStepsAtTheTileEdges(Renderer());
}

TEST_F(CudaFilmGrainNegativeTest, DisabledPlanAndPostPlanAreRejectedBeforeAnyKernel) {
  const auto geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, 64, 64);
  float      amounts  = 0.0f;
  device.BeginRender();
  EXPECT_THROW(
      RecordCudaFilmGrainNegative(device, Plan("kodak_5207", geometry, FilmFormat::Film35mm, 0.0f),
                                  geometry, {}, &amounts, 64, 64),
      std::invalid_argument);
  const auto post = MakeFilmGrainPlan(geometry, FilmFormat::Film35mm,
                                      SharedFilmPrintTables().Bundle().Grain("kodak_5207"),
                                      FilmGrainTarget::Post, 1.0f);
  EXPECT_THROW(RecordCudaFilmGrainNegative(device, post, geometry, {}, &amounts, 64, 64),
               std::invalid_argument);
  device.EndRender();
}

/// The grain step alone at 6000 x 4000 on a Release build: GPU time, median of 9, for 35mm and
/// 16mm. Run it with --gtest_also_run_disabled_tests.
TEST_F(CudaFilmGrainNegativeTest, DISABLED_KernelCostRecordAt24Megapixels) {
  constexpr std::uint32_t kWidth   = 6000;
  constexpr std::uint32_t kHeight  = 4000;
  const auto              geometry = TileGeometry(6000, 1.0f, 0.0f, 0.0f, kWidth, kHeight);
  void*                   amounts  = nullptr;
  ASSERT_EQ(::cudaMalloc(&amounts, std::size_t{kWidth} * kHeight * sizeof(Amounts)), cudaSuccess);
  ASSERT_EQ(::cudaMemset(amounts, 0, std::size_t{kWidth} * kHeight * sizeof(Amounts)), cudaSuccess);
  for (const auto format : {FilmFormat::Film35mm, FilmFormat::Film16mm}) {
    const auto          plan = Plan("kodak_5207", geometry, format);
    std::vector<double> times;
    for (int run = 0; run < 10; ++run) {
      cudaEvent_t start = nullptr;
      cudaEvent_t stop  = nullptr;
      ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
      ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);
      device.BeginRender();
      const auto stream = device.CommandContext().Stream();
      ASSERT_EQ(::cudaEventRecord(start, stream), cudaSuccess);
      RecordCudaFilmGrainNegative(device, plan, geometry, {}, static_cast<float*>(amounts), kWidth,
                                  kHeight);
      ASSERT_EQ(::cudaEventRecord(stop, stream), cudaSuccess);
      device.EndRender();
      ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);
      float milliseconds = 0.0f;
      ASSERT_EQ(::cudaEventElapsedTime(&milliseconds, start, stop), cudaSuccess);
      (void)::cudaEventDestroy(start);
      (void)::cudaEventDestroy(stop);
      if (run > 0) times.push_back(milliseconds);
    }
    std::sort(times.begin(), times.end());
    std::cout << "grain in the negative, " << FilmFormatId(format) << ", 6000 x 4000: GPU "
              << times[times.size() / 2] << " ms\n";
  }
  ASSERT_EQ(::cudaFree(amounts), cudaSuccess);
}

/// The film print with the Film Grain of the document, through the DRT pass: the checks of
/// film_print_grain_checks.hpp, which the OpenCL and Metal tests of phase G6 share.
class CudaFilmPrintGrainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    if (!HasFilmPrintResources()) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string();
    }
  }

  auto Renderer() -> film_print_grain_test::Render {
    return [this](const ExecutionPlan& plan, const PipelineDocument& document, float grey) {
      using film_print_grain_test::Rgba;
      const auto              width  = plan.geometry.render_extent.width;
      const auto              height = plan.geometry.render_extent.height;
      const std::vector<Rgba> scene(static_cast<std::size_t>(width) * height,
                                    Rgba{grey, grey, grey, 1.0f});
      device.ResetPassStats();
      device.BeginRender();
      auto& workspace = device.Workspace();
      workspace.PrepareResultValidity(plan, document, input);
      workspace.EnsureSceneWorkImages({width, height});
      const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
      auto&              lease =
          workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
      workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(std::span(scene)),
                                         device.CommandContext());
      const auto result =
          ExecuteCudaDrt(device, plan, document, FrameSceneBinding::CachedImage(scene_id));
      film_print_grain_test::Frame frame;
      frame.post_count = result.post_neighborhood_count;
      frame.pixels.resize(scene.size());
      auto* display = workspace.Images().Find(result.output);
      EXPECT_NE(display, nullptr);
      workspace.Device().DownloadTexture2D(display->Texture(),
                                           std::as_writable_bytes(std::span(frame.pixels)),
                                           device.CommandContext());
      device.CancelRender();
      frame.grain_negative_passes = device.PassStats().film_grain_negative;
      return frame;
    };
  }

  PreparedRawInput input;
  CudaRenderDevice device;
};

TEST_F(CudaFilmPrintGrainTest, NegativeScanChannelCorrelationIsTheScanCorrelationOf660) {
  film_print_grain_test::ExpectNegativeScanChannelCorrelationIsTheScanCorrelationOf660(Renderer());
}

TEST_F(CudaFilmPrintGrainTest, StrengthZeroEqualsTheFilmPrintWithNoGrainKernel) {
  film_print_grain_test::ExpectStrengthZeroEqualsTheFilmPrintWithNoGrainKernel(Renderer());
}

TEST_F(CudaFilmPrintGrainTest, FilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue) {
  film_print_grain_test::ExpectFilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue(Renderer());
}

TEST_F(CudaFilmPrintGrainTest, GrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows) {
  film_print_grain_test::ExpectGrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows(Renderer());
}

}  // namespace
}  // namespace alcedo
