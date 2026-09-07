//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r;
  float g;
  float b;
  float a;
};

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto TryDownload(CudaRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  device.WaitIdle();
  auto* image = device.Workspace().Images().Find(id);
  if (image == nullptr || image->Empty()) return {};
  const auto&       texture = image->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto Download(CudaRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto pixels = TryDownload(device, id);
  EXPECT_FALSE(pixels.empty());
  return pixels;
}

auto AllFiniteDisplayValues(const std::vector<Rgba>& pixels) -> bool {
  if (pixels.empty()) return false;
  for (const auto& pixel : pixels) {
    if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b) ||
        !std::isfinite(pixel.a)) {
      return false;
    }
  }
  return true;
}

auto MaxRgb(const std::vector<Rgba>& pixels) -> float {
  float max_value = 0.0f;
  for (const auto& pixel : pixels) {
    max_value = std::max(max_value, std::max(pixel.r, std::max(pixel.g, pixel.b)));
  }
  return max_value;
}

auto MaxRgbIndex(const std::vector<Rgba>& pixels) -> std::size_t {
  std::size_t max_index = 0;
  for (std::size_t i = 1; i < pixels.size(); ++i) {
    const float value = std::max(pixels[i].r, std::max(pixels[i].g, pixels[i].b));
    const float max_value =
        std::max(pixels[max_index].r, std::max(pixels[max_index].g, pixels[max_index].b));
    if (value > max_value) max_index = i;
  }
  return max_index;
}

auto Luma709(const Rgba& pixel) -> float {
  return 0.2126f * pixel.r + 0.7152f * pixel.g + 0.0722f * pixel.b;
}

auto ChannelMax(const Rgba& pixel) -> float {
  return std::max(pixel.r, std::max(pixel.g, pixel.b));
}

auto ChannelMin(const Rgba& pixel) -> float {
  return std::min(pixel.r, std::min(pixel.g, pixel.b));
}

auto IsolatedBrightCount(const std::vector<Rgba>& pixels, std::uint32_t width, std::uint32_t height,
                         float bright_floor, float neighbor_ceiling) -> std::size_t {
  if (pixels.size() != static_cast<std::size_t>(width) * height) return pixels.size();
  std::size_t isolated = 0;
  auto        at       = [&](std::uint32_t x, std::uint32_t y) -> const Rgba& {
    return pixels[static_cast<std::size_t>(y) * width + x];
  };
  auto peak = [&](const Rgba& pixel) { return std::max(ChannelMax(pixel), Luma709(pixel)); };
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      if (!(peak(at(x, y)) > bright_floor)) continue;
      float neighbor_max = 0.0f;
      if (x > 0) neighbor_max = std::max(neighbor_max, peak(at(x - 1, y)));
      if (x + 1 < width) neighbor_max = std::max(neighbor_max, peak(at(x + 1, y)));
      if (y > 0) neighbor_max = std::max(neighbor_max, peak(at(x, y - 1)));
      if (y + 1 < height) neighbor_max = std::max(neighbor_max, peak(at(x, y + 1)));
      if (neighbor_max < neighbor_ceiling) ++isolated;
    }
  }
  return isolated;
}

auto IsolatedWhiteCount(const std::vector<Rgba>& pixels, std::uint32_t width, std::uint32_t height,
                        float white_floor, float neighbor_ceiling) -> std::size_t {
  if (pixels.size() != static_cast<std::size_t>(width) * height) return pixels.size();
  std::size_t isolated = 0;
  auto        at       = [&](std::uint32_t x, std::uint32_t y) -> const Rgba& {
    return pixels[static_cast<std::size_t>(y) * width + x];
  };
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      if (!(ChannelMin(at(x, y)) > white_floor)) continue;
      float neighbor_max = 0.0f;
      if (x > 0) neighbor_max = std::max(neighbor_max, ChannelMin(at(x - 1, y)));
      if (x + 1 < width) neighbor_max = std::max(neighbor_max, ChannelMin(at(x + 1, y)));
      if (y > 0) neighbor_max = std::max(neighbor_max, ChannelMin(at(x, y - 1)));
      if (y + 1 < height) neighbor_max = std::max(neighbor_max, ChannelMin(at(x, y + 1)));
      if (neighbor_max < neighbor_ceiling) ++isolated;
    }
  }
  return isolated;
}

struct BrightPixelStats {
  float       max_rgb           = 0.0f;
  float       max_luma          = 0.0f;
  std::size_t hot_channel_count = 0;
  Rgba        hottest{};
};

auto MakeBrightPixelStats(const std::vector<Rgba>& pixels, float hot_floor) -> BrightPixelStats {
  BrightPixelStats stats;
  for (const auto& pixel : pixels) {
    const float rgb = ChannelMax(pixel);
    if (rgb > stats.max_rgb) {
      stats.max_rgb = rgb;
      stats.hottest = pixel;
    }
    stats.max_luma = std::max(stats.max_luma, Luma709(pixel));
    if (rgb > hot_floor) ++stats.hot_channel_count;
  }
  return stats;
}

class CudaDrtProductFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    input_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                           gpu_dag_test::FullSensor(16, 12));
  }

  auto Render(PipelineDocument& document) -> std::vector<Rgba> {
    gpu_dag_test::EnsureTestCameraProfile(document);
    const auto plan   = GraphCompiler::Compile(document, input_.CompileSource(), RenderRequest{});
    const auto output = device_.Execute(plan, input_, document);
    EXPECT_TRUE(plan.Contains(GpuPassKind::Drt));
    return Download(device_, output);
  }

  void RenderDarkChromaticNoise(DrtMethod method, float shadows, float saturation,
                                float sharpen_amount, std::vector<Rgba>* display,
                                std::vector<Rgba>* grade) {
    constexpr std::uint32_t kWidth   = 64;
    constexpr std::uint32_t kHeight  = 48;
    auto                    document = CreateDefaultPipelineDocument();
    auto                    params   = document.Drt()->Params().Params();
    params.method                    = method;
    document.Drt()->Params().ReplaceParams(params);
    if (shadows != 0.0f) {
      auto* shadows_model = dynamic_cast<ShadowsModel*>(
          document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
      ASSERT_NE(shadows_model, nullptr);
      shadows_model->SetValue(shadows);
    }
    if (saturation != 1.0f) {
      auto* saturation_model = dynamic_cast<SaturationModel*>(
          document.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation()));
      ASSERT_NE(saturation_model, nullptr);
      saturation_model->SetValue(saturation);
    }
    if (sharpen_amount != 0.0f) {
      auto* sharpen_model =
          dynamic_cast<SharpenModel*>(document.Drt()->FindAdjustmentByType(type_ids::Sharpen()));
      ASSERT_NE(sharpen_model, nullptr);
      sharpen_model->SetAmount(sharpen_amount);
      sharpen_model->SetRadius(3.0f);
      sharpen_model->SetThreshold(0.0f);
    }
    input_ = RawInputLoader::FromDirectRgb(
        gpu_dag_test::MakeDarkChromaticNoisePlane(kWidth, kHeight, 0.0015f),
        gpu_dag_test::FullSensor(kWidth, kHeight));
    *display = Render(document);
    ASSERT_EQ(display->size(), static_cast<std::size_t>(kWidth) * kHeight);
    ASSERT_TRUE(AllFiniteDisplayValues(*display));
    *grade = TryDownload(device_, GraphValueId{NodeId{"grade.primary"}, PortId{"image"}});
    const auto isolated = IsolatedBrightCount(*display, kWidth, kHeight, 0.98f, 0.25f);
    const auto hottest_index = MaxRgbIndex(*display);
    EXPECT_EQ(isolated, 0U) << "max_display=" << MaxRgb(*display)
                            << " max_grade=" << MaxRgb(*grade)
                            << " hottest_display=(" << (*display)[hottest_index].r << ","
                            << (*display)[hottest_index].g << ","
                            << (*display)[hottest_index].b << ")"
                            << " matching_grade=(" << (*grade)[hottest_index].r << ","
                            << (*grade)[hottest_index].g << "," << (*grade)[hottest_index].b
                            << ")";
    const float max_ceiling = sharpen_amount > 0.0f ? 0.98f : 0.45f;
    EXPECT_LT(MaxRgb(*display), max_ceiling)
        << "dark field should stay below the method-specific sharpen ceiling";
  }

  void RenderAndExpectDarkDisplay(PipelineDocument& document, std::uint32_t width,
                                  std::uint32_t height, float max_rgb_ceiling) {
    const auto pixels = Render(document);
    ASSERT_TRUE(AllFiniteDisplayValues(pixels));
    auto* display =
        device_.Workspace().Images().Find(GraphValueId{NodeId{"drt"}, PortId{"display"}});
    ASSERT_NE(display, nullptr);
    const auto tex_w = display->Texture().Width();
    const auto tex_h = display->Texture().Height();
    if (width != 0 && height != 0) {
      ASSERT_EQ(tex_w, width);
      ASSERT_EQ(tex_h, height);
    }
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(tex_w) * tex_h);
    const auto stats    = MakeBrightPixelStats(pixels, 0.50f);
    const auto isolated = IsolatedWhiteCount(pixels, tex_w, tex_h, 0.98f, 0.25f);
    EXPECT_EQ(isolated, 0U) << "max_rgb=" << stats.max_rgb << " max_luma=" << stats.max_luma
                            << " hot=" << stats.hot_channel_count << " hottest=(" << stats.hottest.r
                            << "," << stats.hottest.g << "," << stats.hottest.b
                            << ") extent=" << tex_w << "x" << tex_h;
    EXPECT_LT(stats.max_rgb, max_rgb_ceiling)
        << "max_luma=" << stats.max_luma << " hot=" << stats.hot_channel_count << " hottest=("
        << stats.hottest.r << "," << stats.hottest.g << "," << stats.hottest.b << ")";
  }

  void RenderCrushedExposureAndExpectDark(DrtMethod method, float exposure_ev, HostImagePlane plane,
                                          std::uint32_t width, std::uint32_t height,
                                          float max_rgb_ceiling) {
    auto document = CreateDefaultPipelineDocument();
    auto params   = document.Drt()->Params().Params();
    params.method = method;
    document.Drt()->Params().ReplaceParams(params);
    auto* exposure = dynamic_cast<ExposureModel*>(
        document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
    ASSERT_NE(exposure, nullptr);
    exposure->SetValue(exposure_ev);
    input_            = RawInputLoader::FromDirectRgb(std::move(plane),
                                                      gpu_dag_test::FullSensor(width, height));
    const auto pixels = Render(document);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(width) * height);
    ASSERT_TRUE(AllFiniteDisplayValues(pixels));
    const auto isolated      = IsolatedBrightCount(pixels, width, height, 0.98f, 0.25f);
    const auto hottest_index = MaxRgbIndex(pixels);
    EXPECT_EQ(isolated, 0U) << "max_display=" << MaxRgb(pixels) << " hottest_display=("
                            << pixels[hottest_index].r << "," << pixels[hottest_index].g << ","
                            << pixels[hottest_index].b << ")";
    EXPECT_LT(MaxRgb(pixels), max_rgb_ceiling)
        << "crushed-exposure field must stay dark; hottest=(" << pixels[hottest_index].r << ","
        << pixels[hottest_index].g << "," << pixels[hottest_index].b << ")";
  }

  PreparedRawInput input_;
  CudaRenderDevice device_;
};

TEST(GpuDagCudaDrtProduct, DefaultCudaPipelineBuildsThreeVisibleNodes) {
  const auto document = CreateDefaultPipelineDocument();
  EXPECT_EQ(document.Graph().Nodes().size(), 3U);
  EXPECT_EQ(document.Graph().Edges().size(), 2U);
  ASSERT_NE(document.Develop(), nullptr);
  ASSERT_NE(document.PrimaryGrade(), nullptr);
  ASSERT_NE(document.Drt(), nullptr);
  EXPECT_EQ(document.ToJson().at("format_version"), kPipelineDocumentFormatVersion);
}

TEST_F(CudaDrtProductFixture, CudaDrtOpenDrtProducesFiniteDisplayReferredOutput) {
  auto       document = CreateDefaultPipelineDocument();
  const auto pixels   = Render(document);
  EXPECT_TRUE(AllFiniteDisplayValues(pixels));
  EXPECT_NEAR(pixels.front().a, 1.0f, 1.0e-6f);
}

TEST_F(CudaDrtProductFixture, CudaDrtPackedWriteDoesNotCopyFullDto) {
  auto document = CreateDefaultPipelineDocument();
  OperatorModelFullDtoCopyCount::Reset();
  EXPECT_TRUE(AllFiniteDisplayValues(Render(document)));
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
}

TEST_F(CudaDrtProductFixture, CudaDrtAces20ProducesFiniteDisplayReferredOutput) {
  auto document = CreateDefaultPipelineDocument();
  auto params   = document.Drt()->Params().Params();
  params.method = DrtMethod::Aces20;
  document.Drt()->Params().ReplaceParams(params);
  EXPECT_TRUE(AllFiniteDisplayValues(Render(document)));
}

TEST_F(CudaDrtProductFixture, ChangingDrtPeakLuminanceKeepsDevelopAndGradeCacheValid) {
  auto document = CreateDefaultPipelineDocument();
  Render(document);
  auto* develop =
      device_.Workspace().Images().Find(GraphValueId{NodeId{"develop"}, PortId{"image"}});
  auto* grade =
      device_.Workspace().Images().Find(GraphValueId{NodeId{"grade.primary"}, PortId{"image"}});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(grade, nullptr);
  const auto develop_id = develop->Texture().ResourceId();
  const auto grade_id   = grade->Texture().ResourceId();

  auto       params     = document.Drt()->Params().Params();
  params.peak_luminance = 200.0f;
  document.Drt()->Params().ReplaceParams(params);
  EXPECT_TRUE(AllFiniteDisplayValues(Render(document)));
  EXPECT_EQ(device_.Workspace()
                .Images()
                .Find(GraphValueId{NodeId{"develop"}, PortId{"image"}})
                ->Texture()
                .ResourceId(),
            develop_id);
  EXPECT_EQ(device_.Workspace()
                .Images()
                .Find(GraphValueId{NodeId{"grade.primary"}, PortId{"image"}})
                ->Texture()
                .ResourceId(),
            grade_id);
}

TEST_F(CudaDrtProductFixture, LegacyPipelineImportRendersSameCudaReferenceWithinTolerance) {
  nlohmann::json legacy;
  legacy["Basic Adjustment"]["Basic Adjustment"]["exposure"] = {
      {"type", 2}, {"enable", true}, {"params", {{"exposure", 0.75f}}}};
  legacy["Output Transform"]["Output Transform"]["odt"] = {
      {"type", 17},
      {"enable", true},
      {"params", {{"odt", {{"method", "open_drt"}, {"peak_luminance", 100.0f}}}}}};
  auto imported = LegacyPipelineImporter::Import(legacy);
  ASSERT_TRUE(imported.Ok()) << imported.error;
  auto reference = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(reference);
  gpu_dag_test::EnsureTestCameraProfile(*imported.document);
  auto* exposure = dynamic_cast<ExposureModel*>(
      reference.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.75f);

  const auto       imported_pixels = Render(*imported.document);
  CudaRenderDevice reference_device;
  const auto plan = GraphCompiler::Compile(reference, input_.CompileSource(), RenderRequest{});
  const auto reference_pixels =
      Download(reference_device, reference_device.Execute(plan, input_, reference));
  ASSERT_EQ(imported_pixels.size(), reference_pixels.size());
  for (std::size_t i = 0; i < imported_pixels.size(); ++i) {
    EXPECT_NEAR(imported_pixels[i].r, reference_pixels[i].r, 1.0e-5f);
    EXPECT_NEAR(imported_pixels[i].g, reference_pixels[i].g, 1.0e-5f);
    EXPECT_NEAR(imported_pixels[i].b, reference_pixels[i].b, 1.0e-5f);
  }
}

TEST_F(CudaDrtProductFixture, CudaBackendFailureDoesNotEnterCpuImageProcessing) {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  std::string reported;
  device_.SetErrorReporter([&reported](std::string_view message) { reported = message; });
  device_.Workspace().Device().FailNextUpload();
  EXPECT_THROW(
      {
        const auto plan = GraphCompiler::Compile(document, input_.CompileSource(), RenderRequest{});
        (void)device_.Execute(plan, input_, document);
      },
      std::runtime_error);
  EXPECT_FALSE(reported.empty());
  EXPECT_FALSE(device_.Workspace().IsRendering());
}

auto MakeConstantRgb(std::uint32_t width, std::uint32_t height, float value) -> HostImagePlane {
  auto  plane = gpu_dag_test::MakeF32RgbaPlane(width, height);
  auto* px    = const_cast<float*>(reinterpret_cast<const float*>(plane.bytes.get()));
  for (std::uint32_t i = 0; i < width * height; ++i) {
    px[i * 4 + 0] = value;
    px[i * 4 + 1] = value;
    px[i * 4 + 2] = value;
    px[i * 4 + 3] = 1.0f;
  }
  return plane;
}

TEST_F(CudaDrtProductFixture, DrtInputIsNotHardClampedToForwardLimitBeforeTonescale) {
  auto low_doc  = CreateDefaultPipelineDocument();
  auto high_doc = CreateDefaultPipelineDocument();
  auto params   = low_doc.Drt()->Params().Params();
  params.method = DrtMethod::Aces20;
  low_doc.Drt()->Params().ReplaceParams(params);
  high_doc.Drt()->Params().ReplaceParams(params);
  PreparedRawInput low_input  = RawInputLoader::FromDirectRgb(MakeConstantRgb(16, 12, 1.0f),
                                                              gpu_dag_test::FullSensor(16, 12));
  PreparedRawInput high_input = RawInputLoader::FromDirectRgb(MakeConstantRgb(16, 12, 4.0f),
                                                              gpu_dag_test::FullSensor(16, 12));
  gpu_dag_test::EnsureTestCameraProfile(low_doc);
  gpu_dag_test::EnsureTestCameraProfile(high_doc);
  const auto low_pixels = Download(
      device_,
      device_.Execute(GraphCompiler::Compile(low_doc, low_input.CompileSource(), RenderRequest{}),
                      low_input, low_doc));
  CudaRenderDevice high_device;
  const auto       high_pixels = Download(
      high_device, high_device.Execute(GraphCompiler::Compile(high_doc, high_input.CompileSource(),
                                                                    RenderRequest{}),
                                             high_input, high_doc));
  ASSERT_EQ(low_pixels.size(), high_pixels.size());
  bool differ = false;
  for (std::size_t i = 0; i < low_pixels.size(); ++i) {
    if (std::abs(low_pixels[i].r - high_pixels[i].r) > 1.0e-4f ||
        std::abs(low_pixels[i].g - high_pixels[i].g) > 1.0e-4f ||
        std::abs(low_pixels[i].b - high_pixels[i].b) > 1.0e-4f) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
  EXPECT_TRUE(AllFiniteDisplayValues(low_pixels));
  EXPECT_TRUE(AllFiniteDisplayValues(high_pixels));
}

TEST_F(CudaDrtProductFixture,
       HighlightReconstructOnSurvivesIntoDrtInsteadOfBeingClippedToUnitCube) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  auto       plane   = gpu_dag_test::MakeU16CfaPlane(64, 64, pattern);
  auto*      samples =
      const_cast<std::uint16_t*>(reinterpret_cast<const std::uint16_t*>(plane.bytes.get()));
  for (std::uint32_t i = 0; i < 64 * 64; i += 5) {
    samples[i] = 30000;
  }
  const auto prepared =
      RawInputLoader::FromUnpackedCfa(plane, pattern, gpu_dag_test::DefaultLinearization(),
                                      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto on_doc                    = CreateDefaultPipelineDocument();
  auto payload                   = on_doc.Develop()->Params().Params();
  payload.demosaic_method        = "legacy";
  payload.highlights_reconstruct = true;
  on_doc.Develop()->Params().ReplaceParams(payload);
  auto off_doc                   = CreateDefaultPipelineDocument();
  payload.highlights_reconstruct = false;
  off_doc.Develop()->Params().ReplaceParams(payload);
  gpu_dag_test::EnsureTestCameraProfile(on_doc);
  gpu_dag_test::EnsureTestCameraProfile(off_doc);
  const auto on_pixels = Download(
      device_,
      device_.Execute(GraphCompiler::Compile(on_doc, prepared.CompileSource(), RenderRequest{}),
                      prepared, on_doc));
  CudaRenderDevice off_device;
  const auto       off_pixels = Download(
      off_device,
      off_device.Execute(GraphCompiler::Compile(off_doc, prepared.CompileSource(), RenderRequest{}),
                               prepared, off_doc));
  EXPECT_TRUE(AllFiniteDisplayValues(on_pixels));
  EXPECT_TRUE(AllFiniteDisplayValues(off_pixels));
  bool differ = false;
  for (std::size_t i = 0; i < on_pixels.size() && i < off_pixels.size(); ++i) {
    if (std::abs(on_pixels[i].r - off_pixels[i].r) > 1.0e-4f ||
        std::abs(on_pixels[i].g - off_pixels[i].g) > 1.0e-4f ||
        std::abs(on_pixels[i].b - off_pixels[i].b) > 1.0e-4f) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
}

TEST_F(CudaDrtProductFixture, OpenDrtAndAcesStillLimitDisplayReferredOutput) {
  auto open_doc = CreateDefaultPipelineDocument();
  auto aces_doc = CreateDefaultPipelineDocument();
  auto aces     = aces_doc.Drt()->Params().Params();
  aces.method   = DrtMethod::Aces20;
  aces_doc.Drt()->Params().ReplaceParams(aces);
  auto input = RawInputLoader::FromDirectRgb(MakeConstantRgb(16, 12, 1000.0f),
                                             gpu_dag_test::FullSensor(16, 12));
  gpu_dag_test::EnsureTestCameraProfile(open_doc);
  gpu_dag_test::EnsureTestCameraProfile(aces_doc);
  const auto open_pixels = Download(
      device_,
      device_.Execute(GraphCompiler::Compile(open_doc, input.CompileSource(), RenderRequest{}),
                      input, open_doc));
  CudaRenderDevice aces_device;
  const auto       aces_pixels = Download(
      aces_device,
      aces_device.Execute(GraphCompiler::Compile(aces_doc, input.CompileSource(), RenderRequest{}),
                                input, aces_doc));
  EXPECT_TRUE(AllFiniteDisplayValues(open_pixels));
  EXPECT_TRUE(AllFiniteDisplayValues(aces_pixels));
  EXPECT_LT(MaxRgb(open_pixels), 4.0f);
  EXPECT_LT(MaxRgb(aces_pixels), 4.0f);
}

TEST_F(CudaDrtProductFixture, CudaDefaultPipelineSecondRenderCreatesNoGpuAllocation) {
  auto document = CreateDefaultPipelineDocument();
  Render(document);
  device_.Workspace().Device().ResetCounters();
  Render(document);
  EXPECT_EQ(device_.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(), 0U);
}

// Mix 1, no mask, non-default DRT followed by display-referred post-processing.
void ApplyUnmaskedReferencePostAndDrt(PipelineDocument& document) {
  auto* grade = document.PrimaryGrade();
  auto* drt   = document.Drt();
  ASSERT_NE(grade, nullptr);
  ASSERT_NE(drt, nullptr);
  EXPECT_TRUE(grade->Enabled());
  EXPECT_FLOAT_EQ(grade->Mix(), 1.0f);
  auto* clarity = dynamic_cast<ClarityModel*>(drt->FindAdjustmentByType(type_ids::Clarity()));
  auto* sharpen = dynamic_cast<SharpenModel*>(drt->FindAdjustmentByType(type_ids::Sharpen()));
  auto* halo    = dynamic_cast<HalationModel*>(drt->FindAdjustmentByType(type_ids::Halation()));
  auto* grain   = dynamic_cast<FilmGrainModel*>(drt->FindAdjustmentByType(type_ids::FilmGrain()));
  ASSERT_NE(clarity, nullptr);
  ASSERT_NE(sharpen, nullptr);
  ASSERT_NE(halo, nullptr);
  ASSERT_NE(grain, nullptr);
  clarity->SetValue(40.0f);
  sharpen->SetAmount(55.0f);
  sharpen->SetRadius(3.0f);
  sharpen->SetThreshold(0.0f);
  halo->SetValue(0.65f);
  grain->SetValue(0.35f);
  auto params           = drt->Params().Params();
  params.peak_luminance = 250.0f;
  drt->Params().ReplaceParams(params);
}

TEST_F(CudaDrtProductFixture, Aces20HueSweepStaysFiniteWithoutIsolatedBlackPixels) {
  constexpr std::uint32_t kHues    = 360;
  auto                    document = CreateDefaultPipelineDocument();
  auto                    params   = document.Drt()->Params().Params();
  params.method                    = DrtMethod::Aces20;
  document.Drt()->Params().ReplaceParams(params);
  input_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeSaturatedHueWheelPlane(kHues, 4.0f),
                                         gpu_dag_test::FullSensor(kHues, 1));
  const auto pixels = Render(document);
  ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kHues));
  ASSERT_TRUE(AllFiniteDisplayValues(pixels));

  auto        luma = [](const Rgba& p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; };
  std::size_t isolated_black = 0;
  for (std::uint32_t i = 0; i < kHues; ++i) {
    const float prev           = luma(pixels[(i + kHues - 1) % kHues]);
    const float curr           = luma(pixels[i]);
    const float next           = luma(pixels[(i + 1) % kHues]);
    const float neighbor_floor = std::min(prev, next);
    if (neighbor_floor > 0.08f && curr < 0.25f * neighbor_floor) {
      ++isolated_black;
    }
  }
  EXPECT_EQ(isolated_black, 0U);
}

TEST_F(CudaDrtProductFixture, Aces20ExtremeChromaticHdrStaysFiniteAndNotBlack) {
  auto document = CreateDefaultPipelineDocument();
  auto params   = document.Drt()->Params().Params();
  params.method = DrtMethod::Aces20;
  document.Drt()->Params().ReplaceParams(params);
  auto        plane        = gpu_dag_test::MakeF32RgbaPlane(8, 1);
  auto*       px           = const_cast<float*>(reinterpret_cast<const float*>(plane.bytes.get()));
  const float samples[][3] = {
      {1.0e5f, 1.0e5f, 1.0e5f}, {1.0e5f, 0.0f, 0.0f},    {0.0f, 1.0e5f, 0.0f},
      {0.0f, 0.0f, 1.0e5f},     {1.0e5f, 1.0e-4f, 0.0f}, {1.0e-4f, 1.0e5f, 1.0e-4f},
      {4.0f, 0.0f, 4.0f},       {0.18f, 0.18f, 0.18f},
  };
  for (int i = 0; i < 8; ++i) {
    px[i * 4 + 0] = samples[i][0];
    px[i * 4 + 1] = samples[i][1];
    px[i * 4 + 2] = samples[i][2];
    px[i * 4 + 3] = 1.0f;
  }
  input_            = RawInputLoader::FromDirectRgb(plane, gpu_dag_test::FullSensor(8, 1));
  const auto pixels = Render(document);
  ASSERT_EQ(pixels.size(), 8U);
  ASSERT_TRUE(AllFiniteDisplayValues(pixels));
  EXPECT_GT(MaxRgb(pixels), 0.05f);
  for (const auto& pixel : pixels) {
    EXPECT_GE(std::min(pixel.r, std::min(pixel.g, pixel.b)), 0.0f);
  }
}

TEST_F(CudaDrtProductFixture, OpenDrtDarkChromaticNoiseHasNoIsolatedWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::OpenDrt, 0.0f, 1.0f, 0.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, Aces20DarkChromaticNoiseHasNoIsolatedWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::Aces20, 0.0f, 1.0f, 0.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, Aces20DarkChromaticNoiseWithShadowsHasNoIsolatedWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::Aces20, 55.0f, 1.0f, 0.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, Aces20DarkChromaticNoiseWithHighSaturationHasNoWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::Aces20, 0.0f, 2.0f, 0.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, OpenDrtDarkChromaticNoiseWithHighSaturationHasNoWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::OpenDrt, 0.0f, 2.0f, 0.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, Aces20DarkChromaticNoiseWithSharpenHasNoWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::Aces20, 0.0f, 1.0f, 100.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, OpenDrtDarkChromaticNoiseWithSharpenHasNoWhitePixels) {
  std::vector<Rgba> display;
  std::vector<Rgba> grade;
  RenderDarkChromaticNoise(DrtMethod::OpenDrt, 0.0f, 1.0f, 100.0f, &display, &grade);
}

TEST_F(CudaDrtProductFixture, OpenDrtDarkSingleChannelSpikesHaveNoIsolatedWhitePixels) {
  constexpr std::uint32_t kWidth   = 64;
  constexpr std::uint32_t kHeight  = 48;
  auto                    document = CreateDefaultPipelineDocument();
  auto                    params   = document.Drt()->Params().Params();
  params.method                    = DrtMethod::OpenDrt;
  document.Drt()->Params().ReplaceParams(params);
  input_ = RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeDarkSingleChannelSpikePlane(kWidth, kHeight, 0.0015f, 0.03f),
      gpu_dag_test::FullSensor(kWidth, kHeight));
  RenderAndExpectDarkDisplay(document, kWidth, kHeight, 1.0f);
}

TEST_F(CudaDrtProductFixture, Aces20DarkSingleChannelSpikesHaveNoIsolatedWhitePixels) {
  constexpr std::uint32_t kWidth   = 64;
  constexpr std::uint32_t kHeight  = 48;
  auto                    document = CreateDefaultPipelineDocument();
  auto                    params   = document.Drt()->Params().Params();
  params.method                    = DrtMethod::Aces20;
  document.Drt()->Params().ReplaceParams(params);
  input_ = RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeDarkSingleChannelSpikePlane(kWidth, kHeight, 0.0015f, 0.03f),
      gpu_dag_test::FullSensor(kWidth, kHeight));
  RenderAndExpectDarkDisplay(document, kWidth, kHeight, 1.05f);
}

TEST_F(CudaDrtProductFixture, Aces20CrushedExposureHueWheelHasNoIsolatedWhitePixels) {
  constexpr std::uint32_t kHues = 72;
  RenderCrushedExposureAndExpectDark(DrtMethod::Aces20, -8.0f,
                                     gpu_dag_test::MakeSaturatedHueWheelPlane(kHues, 0.18f), kHues,
                                     1, 0.55f);
}

TEST_F(CudaDrtProductFixture, OpenDrtCrushedExposureHueWheelHasNoIsolatedWhitePixels) {
  constexpr std::uint32_t kHues = 72;
  RenderCrushedExposureAndExpectDark(DrtMethod::OpenDrt, -8.0f,
                                     gpu_dag_test::MakeSaturatedHueWheelPlane(kHues, 0.18f), kHues,
                                     1, 0.55f);
}

TEST_F(CudaDrtProductFixture, Aces20CrushedExposureChromaticSceneHasNoIsolatedWhitePixels) {
  constexpr std::uint32_t kWidth  = 48;
  constexpr std::uint32_t kHeight = 32;
  RenderCrushedExposureAndExpectDark(
      DrtMethod::Aces20, -10.0f, gpu_dag_test::MakeDarkChromaticNoisePlane(kWidth, kHeight, 0.18f),
      kWidth, kHeight, 0.45f);
}

TEST_F(CudaDrtProductFixture, OpenDrtCrushedExposureChromaticSceneHasNoIsolatedWhitePixels) {
  constexpr std::uint32_t kWidth  = 48;
  constexpr std::uint32_t kHeight = 32;
  RenderCrushedExposureAndExpectDark(
      DrtMethod::OpenDrt, -10.0f, gpu_dag_test::MakeDarkChromaticNoisePlane(kWidth, kHeight, 0.18f),
      kWidth, kHeight, 0.45f);
}

TEST_F(CudaDrtProductFixture, Aces20NearBlackBayerRawHasNoIsolatedWhitePixels) {
  constexpr std::uint32_t kWidth  = 64;
  constexpr std::uint32_t kHeight = 64;
  const auto              pattern = gpu_dag_test::MakeRggbPattern();
  input_ =
      RawInputLoader::FromUnpackedCfa(gpu_dag_test::MakeDarkU16CfaPlane(kWidth, kHeight, pattern),
                                      pattern, gpu_dag_test::NearBlackLinearization(),
                                      gpu_dag_test::FullSensor(kWidth, kHeight), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  auto params   = document.Drt()->Params().Params();
  params.method = DrtMethod::Aces20;
  document.Drt()->Params().ReplaceParams(params);
  auto* develop = document.Develop();
  ASSERT_NE(develop, nullptr);
  auto payload                   = develop->Params().Params();
  payload.demosaic_method        = "legacy";
  payload.highlights_reconstruct = true;
  develop->Params().ReplaceParams(payload);
  RenderAndExpectDarkDisplay(document, 0, 0, 0.98f);
}

TEST_F(CudaDrtProductFixture, DrtPostRunsAfterDisplayTransformAndIgnoresGradeMix) {
  auto document = CreateDefaultPipelineDocument();
  ApplyUnmaskedReferencePostAndDrt(document);
  const auto pixels = Render(document);
  ASSERT_TRUE(AllFiniteDisplayValues(pixels));

  auto mix_off_clarity = CreateDefaultPipelineDocument();
  mix_off_clarity.PrimaryGrade()->SetMix(0.0f);
  auto* on =
      dynamic_cast<ClarityModel*>(mix_off_clarity.Drt()->FindAdjustmentByType(type_ids::Clarity()));
  ASSERT_NE(on, nullptr);
  on->SetValue(40.0f);
  auto mix_off_identity = CreateDefaultPipelineDocument();
  mix_off_identity.PrimaryGrade()->SetMix(0.0f);
  const auto on_pixels  = Render(mix_off_clarity);
  const auto off_pixels = Render(mix_off_identity);
  ASSERT_EQ(on_pixels.size(), off_pixels.size());
  bool differ = false;
  for (std::size_t i = 0; i < on_pixels.size(); ++i) {
    if (std::abs(on_pixels[i].r - off_pixels[i].r) > 1.0e-4f ||
        std::abs(on_pixels[i].g - off_pixels[i].g) > 1.0e-4f ||
        std::abs(on_pixels[i].b - off_pixels[i].b) > 1.0e-4f) {
      differ = true;
      break;
    }
  }
  EXPECT_TRUE(differ);
}

}  // namespace
}  // namespace alcedo
