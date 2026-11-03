//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_print_reference.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

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

auto FilmPrintDocument(const std::string& negative       = "kodak_5207",
                       const std::string& print_material = "kodak_2383",
                       const std::string& scene_light    = "reference") -> PipelineDocument {
  auto               document = CreateDefaultPipelineDocument();
  DrtParameterUpdate update;
  update.method              = DrtMethod::FilmPrint;
  update.encoding_space      = DrtColorSpace::Rec709;
  update.encoding_eotf       = DrtEotf::SrgbPiecewise;
  update.film_negative       = negative;
  update.film_print_material = print_material;
  update.film_scene_light    = scene_light;
  document.Drt()->Params().ApplyUpdate(update);
  return document;
}

/// The largest channel difference in 8 bit codes, over all pixels.
template <class Expected>
auto LargestCodeDifference(const std::vector<Rgba>& pixels, std::size_t count, Expected&& expected)
    -> double {
  double largest = 0.0;
  for (std::size_t index = 0; index < count; ++index) {
    const auto reference = expected(index);
    largest              = (std::max)({largest, std::abs(pixels[index].r - reference.r) * 255.0,
                                       std::abs(pixels[index].g - reference.g) * 255.0,
                                       std::abs(pixels[index].b - reference.b) * 255.0});
  }
  return largest;
}

class CudaFilmPrintFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string()
                   << ". Configure with ALCEDO_FILM_PRINT_RESOURCE_DIR. This is not a pass.";
    }
  }

  /// Run only the DRT pass on @p ap1, with a scene image that holds these values with no change.
  auto RenderDrt(const PipelineDocument& document, std::span<const Rgba> ap1) -> std::vector<Rgba> {
    const auto width               = static_cast<std::uint32_t>(ap1.size());
    auto&      geometry            = plan_.geometry;
    geometry.full_reference_extent = {width, 1};
    geometry.edit_extent           = {width, 1};
    geometry.render_extent         = {width, 1};
    device_.BeginRender();
    auto& workspace = device_.Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, 1, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(ap1),
                                       device_.CommandContext());
    const auto result =
        ExecuteCudaDrt(device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
    std::vector<Rgba> pixels(width);
    auto*             display = workspace.Images().Find(result.output);
    EXPECT_NE(display, nullptr);
    workspace.Device().DownloadTexture2D(
        display->Texture(), std::as_writable_bytes(std::span(pixels)), device_.CommandContext());
    device_.CancelRender();
    return pixels;
  }

  ExecutionPlan    plan_;
  PreparedRawInput input_;
  CudaRenderDevice device_;
};

auto ToRgba(std::span<const float> rgb) -> std::vector<Rgba> {
  std::vector<Rgba> out(rgb.size() / 3);
  for (std::size_t index = 0; index < out.size(); ++index) {
    out[index] = {rgb[index * 3], rgb[index * 3 + 1], rgb[index * 3 + 2], 0.75f};
  }
  return out;
}

TEST_F(CudaFilmPrintFixture, FilmPrintOutputIsFinite) {
  // Negative values, a negative luminance, values above the curve end, and very small luminance.
  const std::vector<Rgba> ap1 = {
      {0.18f, 0.18f, 0.18f, 1.0f},     {-0.5f, 0.2f, 0.1f, 1.0f},
      {0.2f, -0.5f, 0.1f, 1.0f},       {0.2f, 0.1f, -0.5f, 1.0f},
      {-1.0f, -1.0f, -1.0f, 1.0f},     {0.0f, 0.0f, 0.0f, 1.0f},
      {1.0e5f, 1.0e5f, 1.0e5f, 1.0f},  {1.0e5f, 0.0f, 0.0f, 1.0f},
      {0.0f, 1.0e5f, 0.0f, 1.0f},      {0.0f, 0.0f, 1.0e5f, 1.0f},
      {1.0e30f, -1.0e30f, 1.0f, 1.0f}, {1.0e-30f, 1.0e-30f, 1.0e-30f, 1.0f},
      {1.0e-30f, 1.0f, -1.0f, 1.0f},   {1.0f, 1.0e-30f, 1.0e-30f, 1.0f},
      {4.0f, 0.001f, 4.0f, 1.0f},      {0.001f, 4.0f, 4.0f, 1.0f},
  };
  // kodak_gold_200 has no inhibition; the others have it.
  for (const auto* negative : {"kodak_5207", "kodak_portra_400", "kodak_gold_200"}) {
    SCOPED_TRACE(negative);
    const auto pixels = RenderDrt(FilmPrintDocument(negative), ap1);
    ASSERT_EQ(pixels.size(), ap1.size());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
      SCOPED_TRACE(index);
      EXPECT_TRUE(std::isfinite(pixels[index].r) && std::isfinite(pixels[index].g) &&
                  std::isfinite(pixels[index].b));
      EXPECT_GE((std::min)({pixels[index].r, pixels[index].g, pixels[index].b}), 0.0f);
      EXPECT_LE((std::max)({pixels[index].r, pixels[index].g, pixels[index].b}), 1.0f + 1e-6f);
      EXPECT_EQ(pixels[index].a, 1.0f);
    }
  }
}

TEST_F(CudaFilmPrintFixture, FilmPrintRendersThroughTheCompiledPlanAndUploadsTablesOnce) {
  auto document = FilmPrintDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                   gpu_dag_test::FullSensor(16, 12));
  const auto plan  = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  const auto id    = device_.Execute(plan, input, document);
  device_.WaitIdle();
  auto* display = device_.Workspace().Images().Find(id);
  ASSERT_NE(display, nullptr);
  std::vector<Rgba> pixels(16 * 12);
  device_.Workspace().Device().DownloadTexture2D(
      display->Texture(), std::as_writable_bytes(std::span(pixels)), device_.CommandContext());
  for (const auto& pixel : pixels) {
    ASSERT_TRUE(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
  }

  // A new encoding renders the DRT again with the tables that the device holds.
  const auto uploaded = device_.Workspace().Device().LutUploadBytes();
  EXPECT_GT(uploaded, 0U);
  DrtParameterUpdate update;
  update.encoding_eotf = DrtEotf::Gamma22;
  document.Drt()->Params().ApplyUpdate(update);
  device_.ResetPassStats();
  (void)device_.Execute(GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{}),
                        input, document);
  device_.WaitIdle();
  EXPECT_EQ(device_.PassStats().drt_execute, 1U);
  EXPECT_EQ(device_.Workspace().Device().LutUploadBytes(), uploaded);
}

TEST_F(CudaFilmPrintFixture, FilmPrintUploadFailureLeavesTheDisplayResultInvalid) {
  auto document = FilmPrintDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                   gpu_dag_test::FullSensor(16, 12));
  (void)device_.Execute(GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{}),
                        input, document);
  device_.WaitIdle();

  // The earlier passes hold published results, so a new selection first uploads its print table.
  DrtParameterUpdate update;
  update.film_scene_light = "tungsten";
  document.Drt()->Params().ApplyUpdate(update);
  const auto plan   = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  auto&      images = device_.Workspace().Images();
  const auto writer = images.PublishedLastWriter(plan.display_output);
  device_.Workspace().Device().FailNextUpload();
  EXPECT_THROW((void)device_.Execute(plan, input, document), std::runtime_error);
  device_.WaitIdle();
  // The failed render publishes nothing, so the display result of this selection stays invalid.
  EXPECT_EQ(images.PublishedLastWriter(plan.display_output), writer);
  EXPECT_FALSE(device_.Workspace().IsRendering());

  // The same device runs the DRT pass again once the upload works.
  device_.ResetPassStats();
  (void)device_.Execute(plan, input, document);
  device_.WaitIdle();
  EXPECT_EQ(device_.PassStats().drt_execute, 1U);
  EXPECT_NE(images.PublishedLastWriter(plan.display_output), writer);
}

// The lab values come from build_display_expected.py of alcedo-film-lab. They are not in Git, so a
// skip here means that the comparison did not run.
#if defined(ALCEDO_FILM_PRINT_EXPECTED_DIR)

struct DisplaySelection {
  FilmArrayFile file;
  std::string   negative;
  std::string   print_material;
  std::string   scene_light;
};

auto DisplaySelections() -> std::vector<DisplaySelection> {
  std::vector<DisplaySelection> selections;
  const auto                    directory = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "display";
  if (!fs::exists(directory)) return selections;
  for (const auto& entry : fs::directory_iterator(directory)) {
    auto       file     = FilmArrayFile::Parse(ReadFilmResourceBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    selections.push_back({std::move(file), metadata.at("negative").get<std::string>(),
                          metadata.at("print_material").get<std::string>(),
                          metadata.at("scene_light").get<std::string>()});
  }
  return selections;
}

constexpr double kRampLimitCodes         = 0.5;
constexpr double kChartLimitCodes        = 1.0;
/// The lab keeps its stages in float32, so the double-precision reference differs from it a little.
constexpr double kReferenceLabLimitCodes = 1.0;

struct Float3 {
  float r;
  float g;
  float b;
};

TEST_F(CudaFilmPrintFixture, FilmPrintRampMatchesLab) {
  const auto selections = DisplaySelections();
  if (selections.empty()) GTEST_SKIP() << "No lab values. Run build_display_expected.py.";
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " on " + selection.print_material + " under " +
                 selection.scene_light);
    const auto count  = selection.file.Shape("ramp_ap1")[0];
    const auto ap1    = selection.file.F32("ramp_ap1", {count, 3});
    const auto lab    = selection.file.F32("ramp_srgb", {count, 3});
    const auto pixels = RenderDrt(
        FilmPrintDocument(selection.negative, selection.print_material, selection.scene_light),
        ToRgba(ap1));
    const auto largest =
        LargestCodeDifference(pixels, static_cast<std::size_t>(count), [&lab](std::size_t index) {
          return Float3{lab[index * 3], lab[index * 3 + 1], lab[index * 3 + 2]};
        });
    std::cout << "ramp " << selection.negative << ": largest " << largest << " codes\n";
    EXPECT_LE(largest, kRampLimitCodes);
  }
}

TEST_F(CudaFilmPrintFixture, FilmPrintChartMatchesReference) {
  const auto selections = DisplaySelections();
  if (selections.empty()) GTEST_SKIP() << "No lab values. Run build_display_expected.py.";
  auto& tables = SharedFilmPrintTables();
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " on " + selection.print_material + " under " +
                 selection.scene_light);
    const auto count = selection.file.Shape("chart_ap1")[0];
    const auto ap1   = selection.file.F32("chart_ap1", {count, 3});
    const auto lab   = selection.file.F32("chart_srgb", {count, 3});
    const auto data =
        tables.Resolve(selection.negative, selection.print_material, selection.scene_light);
    const film_print_reference::FilmPrintPointReference reference(
        tables.Bundle(), *data, selection.negative, selection.print_material,
        selection.scene_light);
    std::vector<film_print_reference::Rgb> expected(static_cast<std::size_t>(count));
    double                                 reference_against_lab = 0.0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
      expected[index] = reference.Srgb(ap1[index * 3], ap1[index * 3 + 1], ap1[index * 3 + 2]);
      reference_against_lab =
          (std::max)({reference_against_lab, std::abs(expected[index].r - lab[index * 3]) * 255.0,
                      std::abs(expected[index].g - lab[index * 3 + 1]) * 255.0,
                      std::abs(expected[index].b - lab[index * 3 + 2]) * 255.0});
    }
    const auto pixels = RenderDrt(
        FilmPrintDocument(selection.negative, selection.print_material, selection.scene_light),
        ToRgba(ap1));
    const auto largest = LargestCodeDifference(
        pixels, expected.size(), [&expected](std::size_t index) { return expected[index]; });
    std::cout << "chart " << selection.negative << ": largest " << largest
              << " codes against the reference, reference against the lab " << reference_against_lab
              << " codes\n";
    EXPECT_LE(largest, kChartLimitCodes);
    EXPECT_LE(reference_against_lab, kReferenceLabLimitCodes);
  }
}

#endif  // ALCEDO_FILM_PRINT_EXPECTED_DIR

}  // namespace
}  // namespace alcedo
