//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statistical tests of the grain in the negative of the film print on CUDA (plan
// 'film_grain_format_and_stock_plan.md', phase G5). The kernel checks are those of
// film_grain_negative_checks.hpp, which the OpenCL and Metal tests of phase G6 share. The film
// print checks render the DRT pass with the Film Grain of the document. No stored pixels.

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

/// The film print with the Film Grain of the document, through the DRT pass.
class CudaFilmPrintGrainTest : public ::testing::Test {
 protected:
  struct Rgba {
    float r;
    float g;
    float b;
    float a;
  };

  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    if (!HasFilmPrintResources()) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string();
    }
  }

  /// A film print document with Film Grain at @p strength and the film format @p format.
  static auto Document(const std::string& negative, const std::string& print_material,
                       float strength, FilmFormat format = FilmFormat::Film35mm)
      -> PipelineDocument {
    auto               document = CreateDefaultPipelineDocument();
    DrtParameterUpdate update;
    update.method              = DrtMethod::FilmPrint;
    update.encoding_space      = DrtColorSpace::Rec709;
    update.encoding_eotf       = DrtEotf::SrgbPiecewise;
    update.film_negative       = negative;
    update.film_print_material = print_material;
    update.film_scene_light    = "reference";
    document.Drt()->Params().ApplyUpdate(update);
    document.Drt()->Params().ApplyFilmFormat(format);
    GrainOf(document).SetValue(strength);
    return document;
  }

  static auto GrainOf(PipelineDocument& document) -> FilmGrainModel& {
    auto* grain =
        dynamic_cast<FilmGrainModel*>(document.Drt()->FindAdjustmentByType(type_ids::FilmGrain()));
    if (grain == nullptr) throw std::runtime_error("The document has no Film Grain");
    return *grain;
  }

  /// The plan compiles the DRT/Post adjustments of @p document, as GraphCompiler lists them.
  void CompilePostAdjustments(const PipelineDocument& document) {
    plan.drt.post_adjustments.clear();
    const auto& drt = *document.Drt();
    for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
      plan.drt.post_adjustments.push_back({drt.AdjustmentIdAt(index),
                                           drt.AdjustmentAt(index).Type(),
                                           CompiledAdjustmentAlgorithm::Neighborhood});
    }
  }

  /// A render of @p size x @p size pixels at reference pixel (700, 500) of a 6000 x 4000 frame.
  void SetView(std::uint32_t size) {
    auto& geometry                 = plan.geometry;
    geometry.full_reference_extent = {6000, 4000};
    geometry.edit_extent           = geometry.full_reference_extent;
    geometry.render_extent         = {size, size};
    geometry.reference_to_edit     = Matrix3x3::Identity();
    geometry.render_to_reference   = Matrix3x3::Translate(700.0f, 500.0f);
    geometry.reference_to_render   = InvertAffine(geometry.render_to_reference);
    geometry.edit_to_render        = geometry.reference_to_render;
  }

  /// Run the DRT pass on a flat scene of the linear AP1 grey @p grey. @p linear_scan gives the
  /// linear AP1 of Negative Scan before its display transform. With @p compile_post false, the plan
  /// has no DRT/Post adjustment.
  auto Render(PipelineDocument& document, float grey, bool linear_scan = false,
              bool compile_post = true) -> std::vector<Rgba> {
    plan.drt.post_adjustments.clear();
    if (compile_post) CompilePostAdjustments(document);
    plan.film_scan_linear_output   = linear_scan;
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
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(std::span(scene)),
                                       device.CommandContext());
    const auto result =
        ExecuteCudaDrt(device, plan, document, FrameSceneBinding::CachedImage(scene_id));
    last_post_count = result.post_neighborhood_count;
    std::vector<Rgba> pixels(scene.size());
    auto*             display = workspace.Images().Find(result.output);
    EXPECT_NE(display, nullptr);
    workspace.Device().DownloadTexture2D(
        display->Texture(), std::as_writable_bytes(std::span(pixels)), device.CommandContext());
    device.CancelRender();
    plan.film_scan_linear_output = false;
    return pixels;
  }

  /// ln of the linear light of one channel: of the sRGB display, or of linear AP1.
  static auto LogLight(const std::vector<Rgba>& pixels, int channel, bool linear)
      -> std::vector<double> {
    std::vector<double> values(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const float v = channel == 0 ? pixels[i].r : (channel == 1 ? pixels[i].g : pixels[i].b);
      values[i]     = std::log(static_cast<double>(linear ? v : CeSrgbDecode(v)));
    }
    return values;
  }

  static auto MeanLight(const std::vector<Rgba>& pixels, int channel) -> double {
    double sum = 0.0;
    for (const auto& pixel : pixels) {
      sum += CeSrgbDecode(channel == 0 ? pixel.r : (channel == 1 ? pixel.g : pixel.b));
    }
    return sum / static_cast<double>(pixels.size());
  }

  ExecutionPlan    plan;
  PreparedRawInput input;
  CudaRenderDevice device;
  std::uint32_t    last_post_count = 0;
};

// The plan: through Negative Scan of 5207, the channel correlation rg is within 0.05 of 0.661 (the
// 5207 scans of #660). The lab fitted the layer correlation R at net green density 0.1 to 0.7 of
// the default scanner (P15 `fit_layer_correlation.py`); these greys cover that range.
TEST_F(CudaFilmPrintGrainTest, NegativeScanChannelCorrelationIsTheScanCorrelationOf660) {
  SetView(512);
  auto                  document = Document("kodak_5207", std::string{kFilmNegativeScanId}, 1.0f);
  std::array<double, 3> mean{};
  const std::array<float, 4> greys{0.03f, 0.09f, 0.25f, 0.7f};
  for (const float grey : greys) {
    const auto pixels = Render(document, grey, true);
    EXPECT_EQ(device.PassStats().film_grain_negative, 1U);
    const auto                  r = LogLight(pixels, 0, true);
    const auto                  g = LogLight(pixels, 1, true);
    const auto                  b = LogLight(pixels, 2, true);
    const std::array<double, 3> correlation{film_grain_post_test::Correlation(r, g),
                                            film_grain_post_test::Correlation(g, b),
                                            film_grain_post_test::Correlation(r, b)};
    std::cout << "Negative Scan of 5207, AP1 grey " << grey << ": rg " << correlation[0] << ", gb "
              << correlation[1] << ", rb " << correlation[2] << "\n";
    for (std::size_t pair = 0; pair < 3; ++pair) mean[pair] += correlation[pair] / greys.size();
  }
  EXPECT_NEAR(mean[0], 0.661, 0.05);
  EXPECT_NEAR(mean[1], 0.41, 0.05);
  EXPECT_NEAR(mean[2], 0.246, 0.05);
}

// The plan: with strength 0 the output equals the film print without grain, and no grain kernel
// runs. In each form of FP6: the print, Negative Scan, and the light table.
TEST_F(CudaFilmPrintGrainTest, StrengthZeroEqualsTheFilmPrintWithNoGrainKernel) {
  SetView(96);
  struct Form {
    const char* negative;
    std::string print_material;
  };
  for (const auto& form :
       {Form{"kodak_5207", "kodak_2383"}, Form{"kodak_5207", std::string{kFilmNegativeScanId}},
        Form{"fuji_velvia_50", "kodak_2383"}}) {
    SCOPED_TRACE(std::string(form.negative) + " " + form.print_material);
    auto       zero  = Document(form.negative, form.print_material, 0.0f);
    const auto plain = Render(zero, 0.18f);
    EXPECT_EQ(device.PassStats().film_grain_negative, 0U);
    // The same document with no compiled DRT/Post adjustment: the film print of F4 and F10.
    const auto none = Render(zero, 0.18f, false, false);
    ASSERT_EQ(plain.size(), none.size());
    for (std::size_t i = 0; i < plain.size(); ++i) {
      ASSERT_EQ(plain[i].r, none[i].r) << i;
      ASSERT_EQ(plain[i].g, none[i].g) << i;
      ASSERT_EQ(plain[i].b, none[i].b) << i;
    }
    auto       grain = Document(form.negative, form.print_material, 1.0f);
    const auto with  = Render(grain, 0.18f);
    EXPECT_EQ(device.PassStats().film_grain_negative, 1U);
    bool different = false;
    for (std::size_t i = 0; i < with.size(); ++i) {
      different = different || with[i].r != plain[i].r || with[i].g != plain[i].g;
    }
    EXPECT_TRUE(different);
  }
}

// Decision U4: the film print skips the DRT/Post Film Grain and keeps its stored value; the grain
// of the print grows with the slider.
TEST_F(CudaFilmPrintGrainTest, FilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue) {
  SetView(256);
  for (const auto* negative : {"kodak_5207", "fuji_velvia_50"}) {
    SCOPED_TRACE(negative);
    auto       half       = Document(negative, "kodak_2383", 0.5f);
    auto       full       = Document(negative, "kodak_2383", 1.0f);
    const auto half_light = Render(half, 0.18f);
    EXPECT_EQ(last_post_count, 0U);
    const auto full_light = Render(full, 0.18f);
    EXPECT_EQ(last_post_count, 0U);
    EXPECT_EQ(GrainOf(full).Read([](const ScalarFloatPayload& payload) { return payload.value; }),
              1.0f);
    for (int channel = 0; channel < 3; ++channel) {
      SCOPED_TRACE(channel);
      const double half_sigma = film_grain_post_test::StdDev(LogLight(half_light, channel, false));
      const double full_sigma = film_grain_post_test::StdDev(LogLight(full_light, channel, false));
      EXPECT_GT(half_sigma, 0.0);
      EXPECT_NEAR(full_sigma / half_sigma, 2.0, 0.2);
    }
  }
}

// Risk "Grain in the negative moves the mean through the print curve" (section 10): the change of
// the mean display light of a flat grey with the grain of 5207 at 100 %, in stops, at 6000 px. The
// print turns a density change into light through a convex curve, so the grain makes the mean
// light of a print shadow brighter, as on a real print; the mid tones and the highlights keep their
// mean. The record is printed for the plan; each bound is the measured largest value with a
// margin, so a change of the model shows here.
TEST_F(CudaFilmPrintGrainTest, GrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows) {
  SetView(256);
  struct Bound {
    FilmFormat format;
    double     shadows;  ///< AP1 grey below 0.18
    double     rest;     ///< AP1 grey 0.18 and above
  };
  for (const auto& bound :
       {Bound{FilmFormat::Film35mm, 0.2, 0.06}, Bound{FilmFormat::Film16mm, 0.35, 0.1}}) {
    auto zero = Document("kodak_5207", "kodak_2383", 0.0f, bound.format);
    auto full = Document("kodak_5207", "kodak_2383", 1.0f, bound.format);
    for (const float grey : {0.005f, 0.02f, 0.06f, 0.18f, 0.5f, 1.5f, 4.0f}) {
      const auto plain = Render(zero, grey);
      const auto grain = Render(full, grey);
      std::cout << "5207 on 2383, " << FilmFormatId(bound.format) << ", AP1 grey " << grey
                << ": mean shift";
      for (int channel = 0; channel < 3; ++channel) {
        const double stops = std::log2(MeanLight(grain, channel) / MeanLight(plain, channel));
        std::cout << " " << stops;
        EXPECT_LT(std::abs(stops), grey < 0.18f ? bound.shadows : bound.rest)
            << FilmFormatId(bound.format) << " grey " << grey << " channel " << channel;
      }
      std::cout << " stops\n";
    }
  }
}

}  // namespace
}  // namespace alcedo
