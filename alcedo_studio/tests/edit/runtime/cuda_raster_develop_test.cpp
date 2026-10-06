//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Raster input through the CUDA develop graph: UploadRgb with LinearizeRaster, then DisplayToAp1
// (raster_image_input_plan.md, Phase R3).

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "raster_render_test_support.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {
namespace {

using raster_render_test::Rgba;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaRasterDevelopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }

  auto Download(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    auto&             texture = lease->Texture();
    std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
    device_.Workspace().Device().DownloadTexture2D(
        texture,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_.CommandContext());
    return pixels;
  }

  /// Develop, Geometry and DisplayToAp1 only; develop_output stays readable.
  auto RenderDevelop(const raster_render_test::RasterCase& c) -> std::vector<Rgba> {
    const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
    device_.BeginRender();
    ExecuteCudaDevelop(device_, plan, c.input, c.document);
    ExecuteCudaGeometryResample(device_, plan);
    ExecuteCudaDisplayToAp1(device_, plan, c.input, c.document);
    device_.EndRender();
    device_.WaitIdle();
    auto pixels = Download(plan.develop_output);
    device_.PublishResults();
    return pixels;
  }

  CudaRenderDevice device_;
};

TEST_F(CudaRasterDevelopTest, RasterJpegRendersThroughDisplayToAp1WithoutCameraToAp1Pass) {
  auto       c    = raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  device_.ResetPassStats();
  (void)device_.Execute(plan, c.input, c.document);
  device_.WaitIdle();
  const auto& stats = device_.PassStats();
  EXPECT_EQ(stats.sensor_develop_execute, 1u);
  EXPECT_EQ(stats.display_to_ap1_execute, 1u);
  EXPECT_EQ(stats.camera_color_execute, 0u);
  EXPECT_EQ(stats.camera_color_skip, 0u);
  EXPECT_EQ(stats.drt_execute, 1u);
}

TEST_F(CudaRasterDevelopTest, DevelopOutputMatchesHostEvaluationForEachRasterKind) {
  struct Case {
    const char*    name_;
    RasterFileKind kind_;
  };
  for (const auto& entry : {Case{"srgb_chunk.png", RasterFileKind::Png},
                            Case{"display_p3_icc_8bit.jpg", RasterFileKind::Jpeg},
                            Case{"cicp_rec2020_pq_16bit.png", RasterFileKind::Png},
                            Case{"gray_gamma22.png", RasterFileKind::Png},
                            Case{"prophoto_icc_16bit.tif", RasterFileKind::Tiff},
                            Case{"lut_based_rgb_icc.tif", RasterFileKind::Tiff},
                            Case{"chromaticities_p3_half.exr", RasterFileKind::OpenExr},
                            Case{"float32_srgb_icc.tif", RasterFileKind::Tiff},
                            Case{"alcedo_export_rec2020_hlg.tif", RasterFileKind::Tiff},
                            Case{"alcedo_export_upstream_rec2020_v4.tif", RasterFileKind::Tiff}}) {
    SCOPED_TRACE(entry.name_);
    const auto c = raster_render_test::LoadCase(entry.name_, entry.kind_);
    raster_render_test::ExpectAcesccNear(
        RenderDevelop(c), raster_render_test::HostReferenceDevelopOutput(c.input, c.description),
        entry.name_);
  }
}

TEST_F(CudaRasterDevelopTest, RgbaPngRendersColorAndIgnoresAlpha) {
  const auto rgba =
      RenderDevelop(raster_render_test::LoadCase("rgba_with_alpha.png", RasterFileKind::Png));
  const auto rgb = RenderDevelop(
      raster_render_test::LoadCase("rgb_same_as_rgba_with_alpha.png", RasterFileKind::Png));
  ASSERT_EQ(rgba.size(), rgb.size());
  for (std::size_t i = 0; i < rgb.size(); ++i) {
    EXPECT_EQ(rgba[i].r, rgb[i].r);
    EXPECT_EQ(rgba[i].g, rgb[i].g);
    EXPECT_EQ(rgba[i].b, rgb[i].b);
  }
  // Colored pixels are not gray: the color survives.
  EXPECT_GT(std::abs(rgba[15].r - rgba[15].g), 0.05f);
}

TEST_F(CudaRasterDevelopTest, ExifOrientationSixRotatesPixelsClockwise) {
  // Both files hold the same JPEG pixels; one also has EXIF Orientation 6 (rotate 90 degrees
  // clockwise for display).
  const auto plain =
      RenderDevelop(raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg));
  const auto rotated = RenderDevelop(
      raster_render_test::LoadCase("srgb_orientation6_8bit.jpg", RasterFileKind::Jpeg));
  constexpr std::size_t kWidth = 16, kHeight = 12;
  ASSERT_EQ(plain.size(), kWidth * kHeight);
  ASSERT_EQ(rotated.size(), kWidth * kHeight);
  // Output is kHeight wide and kWidth high: out(x, y) = in(y, kHeight - 1 - x).
  for (std::size_t y = 0; y < kWidth; ++y) {
    for (std::size_t x = 0; x < kHeight; ++x) {
      const auto& out = rotated[y * kHeight + x];
      const auto& in  = plain[(kHeight - 1 - x) * kWidth + y];
      EXPECT_EQ(out.r, in.r) << "x " << x << " y " << y;
      EXPECT_EQ(out.g, in.g) << "x " << x << " y " << y;
      EXPECT_EQ(out.b, in.b) << "x " << x << " y " << y;
    }
  }
}

TEST_F(CudaRasterDevelopTest, RasterDevelopOutputIsCachedAcrossColorGradeEdits) {
  auto       c    = raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  device_.ResetPassStats();
  (void)device_.Execute(plan, c.input, c.document);
  auto* exposure = dynamic_cast<ExposureModel*>(
      c.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.7f);
  (void)device_.Execute(plan, c.input, c.document);
  device_.WaitIdle();
  const auto& stats = device_.PassStats();
  EXPECT_EQ(stats.display_to_ap1_execute, 1u);
  EXPECT_EQ(stats.display_to_ap1_skip, 1u);
  EXPECT_EQ(stats.sensor_develop_execute, 1u);
  EXPECT_EQ(stats.primary_grade_execute, 2u);
}

TEST_F(CudaRasterDevelopTest, InputProfileOverrideReRunsTheRasterDevelopChain) {
  auto       c      = raster_render_test::LoadCase("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  const auto plan   = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  const auto before = RenderDevelop(c);
  device_.ResetPassStats();
  (void)device_.Execute(plan, c.input, c.document);
  c.document.Develop()->Params().ApplyInputProfileUpdate(DevelopInputProfileUpdate{"display_p3"});
  (void)device_.Execute(plan, c.input, c.document);
  device_.WaitIdle();
  const auto& stats = device_.PassStats();
  // The override changes the transfer, which LinearizeRaster applies before lens and geometry,
  // so the sensor result runs again together with DisplayToAp1. Grade edits stay cached (see
  // RasterDevelopOutputIsCachedAcrossColorGradeEdits).
  EXPECT_EQ(stats.sensor_develop_execute, 2u);
  EXPECT_EQ(stats.display_to_ap1_execute, 2u);
  const auto after = RenderDevelop(c);
  auto       expected_description =
      ResolveEffectiveRasterDescription(*c.document.Develop()->Params().RasterInput());
  raster_render_test::ExpectAcesccNear(
      after, raster_render_test::HostReferenceDevelopOutput(c.input, expected_description),
      "display_p3 override");
  EXPECT_NE(before[15].r, after[15].r);
}

TEST_F(CudaRasterDevelopTest, Cat02WhiteBalanceShiftsRasterImageLikeRawImage) {
  auto  c  = raster_render_test::LoadCase("srgb_chunk.png", RasterFileKind::Png);
  auto& wb = *dynamic_cast<Cat02WhiteBalanceModel*>(
      c.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Cat02WhiteBalance()));
  wb.ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, 4300.0f, 20.0f});
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  device_.BeginRender();
  ExecuteCudaDevelop(device_, plan, c.input, c.document);
  ExecuteCudaGeometryResample(device_, plan);
  ExecuteCudaDisplayToAp1(device_, plan, c.input, c.document);
  const auto grade = ExecuteCudaPrimaryGrade(device_, plan, c.input, c.document);
  device_.EndRender();
  device_.WaitIdle();
  const auto develop = Download(plan.develop_output);
  ASSERT_FALSE(develop.empty());
  auto&             texture = CudaSceneTexture(device_, grade.output_binding);
  std::vector<Rgba> graded(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device_.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(graded.data()),
                           graded.size() * sizeof(Rgba)),
      device_.CommandContext());
  // The CAT02 white balance works on linear AP1 inside the Color Grade, as for a RAW image: the
  // graded pixel is the AP1 CAT02 matrix times the develop pixel.
  const auto matrix = BuildAp1Cat02WhiteBalanceMatrix(4300.0, 20.0);
  using raster_render_test::AcesccDecode;
  for (std::size_t i = 0; i < develop.size(); i += 7) {
    const float r = AcesccDecode(develop[i].r), g = AcesccDecode(develop[i].g),
                b           = AcesccDecode(develop[i].b);
    const float expected[3] = {matrix[0] * r + matrix[1] * g + matrix[2] * b,
                               matrix[3] * r + matrix[4] * g + matrix[5] * b,
                               matrix[6] * r + matrix[7] * g + matrix[8] * b};
    const float actual[3]   = {AcesccDecode(graded[i].r), AcesccDecode(graded[i].g),
                               AcesccDecode(graded[i].b)};
    for (int ch = 0; ch < 3; ++ch) {
      EXPECT_NEAR(actual[ch], expected[ch], 1e-4f * std::max(1.0f, std::abs(expected[ch])))
          << "pixel " << i << " channel " << ch;
    }
  }
}

TEST_F(CudaRasterDevelopTest, LutIccPixelsWithAnotherProfileHashAreARenderError) {
  auto c       = raster_render_test::LoadCase("lut_based_rgb_icc.tif", RasterFileKind::Tiff);
  auto payload = c.document.Develop()->Params().Params();
  payload.input->source_color_.icc_sha256_ = std::string(64, 'f');
  c.document.Develop()->Params().ReplaceParams(payload);
  const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
  EXPECT_THROW((void)device_.Execute(plan, c.input, c.document), std::runtime_error);
}

TEST_F(CudaRasterDevelopTest, DisplayToAp1PassTimeIsMeasuredAgainstTheForwardDrtPass) {
  // Section 5.4: the DisplayToAp1 pass takes at most 1.25x the forward Drt pass at the same
  // extent. The ratio is checked in release builds; a debug CUDA build (-G) only reports it.
  constexpr std::uint32_t kWidth = 3840, kHeight = 2160;
  HostImagePlane          plane;
  plane.extent       = Extent2D{kWidth, kHeight};
  plane.stride_bytes = kWidth * 4;
  plane.format       = HostPixelFormat::U8Rgba;
  auto storage       = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  std::default_delete<std::byte[]>());
  for (std::size_t i = 0; i < plane.ByteCount(); ++i) {
    storage.get()[i] = static_cast<std::byte>((i * 2654435761u) >> 24);
  }
  plane.bytes                  = storage;
  const auto             input = RasterInputLoader::FromHostPlane(plane);
  RasterColorDescription description;
  description.primaries_xy_ = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
  auto       document       = CreateDefaultRasterPipelineDocument(description);
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});

  diag::PreviewPerformance::ResetForTesting();
  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
  std::vector<diag::PreviewRequestRecord> records;
  diag::PreviewPerformance::InstallRecordSink(
      [&](const diag::PreviewRequestRecord& record) { records.push_back(record); });
  for (std::uint64_t request = 1; request <= 3; ++request) {
    // Each render runs every pass: the override toggles invalidate the develop chain.
    document.Develop()->Params().ApplyInputProfileUpdate(
        DevelopInputProfileUpdate{request % 2 == 0 ? "srgb" : "display_p3"});
    diag::PreviewPerformance::NoteSubmit(request, diag::PreviewFrameRole::InteractivePrimary,
                                         diag::PreviewQuality::Interactive, "RasterTiming", false);
    diag::PreviewPerformance::BindCurrentRequest(request);
    (void)device_.Execute(plan, input, document);
    device_.WaitIdle();
    diag::PreviewPerformance::NoteDisplayed(request);
    diag::PreviewPerformance::ClearCurrentRequest();
  }
  diag::PreviewPerformance::FlushWriter();
  diag::PreviewPerformance::InstallRecordSink({});
  diag::PreviewPerformance::ResetForTesting();
  ASSERT_EQ(records.size(), 3u);
  // Use the last render: the first one includes module loading.
  std::uint64_t display_to_ap1_ns = 0, drt_ns = 0;
  for (const auto& pass : records.back().passes) {
    if (pass.kind == diag::PreviewPassKind::DisplayToAp1) display_to_ap1_ns = pass.gpu_ns;
    if (pass.kind == diag::PreviewPassKind::Drt) drt_ns = pass.gpu_ns;
  }
  ASSERT_GT(display_to_ap1_ns, 0u);
  ASSERT_GT(drt_ns, 0u);
  const double ratio = static_cast<double>(display_to_ap1_ns) / static_cast<double>(drt_ns);
  std::printf("UHD DisplayToAp1 %.3f ms, Drt %.3f ms, ratio %.2f\n", display_to_ap1_ns / 1.0e6,
              drt_ns / 1.0e6, ratio);
  RecordProperty("display_to_ap1_over_drt", std::to_string(ratio));
#ifdef NDEBUG
  EXPECT_LE(ratio, 1.25);
#endif
}

}  // namespace
}  // namespace alcedo
