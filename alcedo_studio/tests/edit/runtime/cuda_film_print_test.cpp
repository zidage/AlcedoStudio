//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/cuda/cuda_film_print_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_negative_scan.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/texture_format.hpp"
#include "film_print_reference.hpp"
#include "film_print_test_support.hpp"

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
 public:
  /// @ref RenderDrt with the linear AP1 of Negative Scan before the display transform.
  auto RenderScanLinear(const PipelineDocument& document, std::span<const Rgba> ap1)
      -> std::vector<Rgba> {
    plan_.film_scan_linear_output = true;
    auto pixels                   = RenderDrt(document, ap1);
    plan_.film_scan_linear_output = false;
    return pixels;
  }

  /// The auto colour shift of the last render that read a prescan.
  auto LastAutoShift() -> std::array<double, 3> { return device_.PassStats().film_scan_auto_shift; }

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

  /// Render @p render pixels of a @p reference frame. See `CudaDiffusionFilterFixture::SetView`.
  void SetView(Extent2D reference, Extent2D render, const Matrix3x3& render_to_reference) {
    auto& geometry                 = plan_.geometry;
    geometry.full_reference_extent = reference;
    geometry.edit_extent           = reference;
    geometry.render_extent         = render;
    geometry.reference_to_edit     = Matrix3x3::Identity();
    geometry.render_to_reference   = render_to_reference;
    geometry.reference_to_render   = InvertAffine(render_to_reference);
    geometry.edit_to_render        = geometry.reference_to_render;
  }

  /**
   * @brief Run only the DRT pass on @p ap1 with the current view.
   *
   * @p publish ends the render and publishes its results; otherwise the render is cancelled. With
   * @p prescan, the prescan of Negative Scan (the scan density T) that the render built or sampled.
   */
  auto RenderView(const PipelineDocument& document, std::span<const Rgba> ap1, bool publish,
                  std::vector<Rgba>* prescan = nullptr) -> std::vector<Rgba> {
    const auto width  = plan_.geometry.render_extent.width;
    const auto height = plan_.geometry.render_extent.height;
    device_.ResetPassStats();
    device_.BeginRender();
    auto& workspace = device_.Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(ap1),
                                       device_.CommandContext());
    const auto result =
        ExecuteCudaDrt(device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
    std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
    auto*             display = workspace.Images().Find(result.output);
    EXPECT_NE(display, nullptr);
    workspace.Device().DownloadTexture2D(
        display->Texture(), std::as_writable_bytes(std::span(pixels)), device_.CommandContext());
    if (prescan != nullptr) {
      auto* image = workspace.Images().Find(FilmScanPrescanResultId(plan_.drt.node_id));
      EXPECT_NE(image, nullptr);
      if (image != nullptr) {
        prescan->resize(static_cast<std::size_t>(image->Texture().Width()) *
                        image->Texture().Height());
        workspace.Device().DownloadTexture2D(image->Texture(),
                                             std::as_writable_bytes(std::span(*prescan)),
                                             device_.CommandContext());
      }
    }
    if (publish) {
      device_.EndRender();
      device_.PublishResults();
    } else {
      device_.CancelRender();
    }
    return pixels;
  }

  ExecutionPlan    plan_;
  PreparedRawInput input_;
  CudaRenderDevice device_;
};

/// A dim field with one bright square, as linear AP1.
auto PatchField(std::uint32_t width, std::uint32_t height, std::uint32_t x0, std::uint32_t y0,
                std::uint32_t size) -> std::vector<Rgba> {
  std::vector<Rgba> field(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float ramp = 0.01f + 0.04f * static_cast<float>(x + y) / static_cast<float>(width);
      const bool  in   = x >= x0 && x < x0 + size && y >= y0 && y < y0 + size;
      field[static_cast<std::size_t>(y) * width + x] =
          in ? Rgba{40.0f, 30.0f, 20.0f, 0.75f} : Rgba{ramp, ramp * 0.9f, ramp * 1.1f, 0.75f};
    }
  }
  return field;
}

auto CropField(const std::vector<Rgba>& field, std::uint32_t width, std::uint32_t x0,
               std::uint32_t y0, std::uint32_t crop_width, std::uint32_t crop_height)
    -> std::vector<Rgba> {
  std::vector<Rgba> crop(static_cast<std::size_t>(crop_width) * crop_height);
  for (std::uint32_t y = 0; y < crop_height; ++y) {
    for (std::uint32_t x = 0; x < crop_width; ++x) {
      crop[static_cast<std::size_t>(y) * crop_width + x] =
          field[static_cast<std::size_t>(y + y0) * width + x + x0];
    }
  }
  return crop;
}

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
  // kodak_gold_200 has no inhibition; the others have it. The reversal films have the light table
  // form: Astia 100F has no spread and no inhibition, and E100 has no inhibition.
  for (const auto* negative :
       {"kodak_5207", "kodak_portra_400", "kodak_gold_200", "fuji_velvia_50", "fuji_velvia_100",
        "fuji_provia_100f", "fuji_astia_100f", "kodak_ektachrome_e100"}) {
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
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                   gpu_dag_test::FullSensor(64, 48));
  const auto plan  = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  const auto id    = device_.Execute(plan, input, document);
  device_.WaitIdle();
  auto* display = device_.Workspace().Images().Find(id);
  ASSERT_NE(display, nullptr);
  std::vector<Rgba> pixels(64 * 48);
  device_.Workspace().Device().DownloadTexture2D(
      display->Texture(), std::as_writable_bytes(std::span(pixels)), device_.CommandContext());
  for (const auto& pixel : pixels) {
    ASSERT_TRUE(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
  }

  // This frame has an image plane, so the compiled plan published both canonical results.
  auto& images = device_.Workspace().Images();
  EXPECT_NE(images.PublishedLastWriter(FilmHalationResultId(plan.drt.node_id)), 0U);
  EXPECT_NE(images.PublishedLastWriter(FilmFlareResultId(plan.drt.node_id)), 0U);

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

TEST_F(CudaFilmPrintFixture, FilmPrintKeepsTheAlphaOfTheScene) {
  // The images between the steps have no alpha, so the display reads it from the scene.
  constexpr std::uint32_t kWidth  = 64;
  constexpr std::uint32_t kHeight = 48;
  const auto              field   = PatchField(kWidth, kHeight, 20, 20, 8);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  for (const auto& pixel : RenderView(FilmPrintDocument(), field, false)) {
    ASSERT_EQ(pixel.a, 0.75f);
  }
}

TEST_F(CudaFilmPrintFixture, FilmPrintUploadFailureLeavesTheDisplayResultInvalid) {
  auto document = FilmPrintDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                   gpu_dag_test::FullSensor(64, 48));
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

TEST_F(CudaFilmPrintFixture, FilmPrintViewportKeepsCanvas) {
  // kodak_gold_200 has no spread and no inhibition, so all image-plane light in this case comes
  // from the Halation and flare results. The tolerance is that of
  // CudaDiffusionFilterFixture.RoiRenderSamplesThePublishedFullFrameScatter.
  const auto negative = SharedFilmPrintTables().Bundle().Negative("kodak_gold_200");
  ASSERT_TRUE(std::ranges::all_of(negative.layer_spread_um, [](double v) { return v == 0.0; }));
  ASSERT_FALSE(negative.inhibition.Present());

  constexpr std::uint32_t kWidth   = 256;
  constexpr std::uint32_t kHeight  = 192;
  constexpr std::uint32_t kRoiX    = 96;
  constexpr std::uint32_t kRoiY    = 40;
  constexpr std::uint32_t kRoiSize = 128;
  // The bright patch ends at the left edge of the ROI; its light must still reach into the ROI.
  const auto              field    = PatchField(kWidth, kHeight, 80, 90, 16);
  auto                    document = FilmPrintDocument("kodak_gold_200");
  DrtParameterUpdate      strong;
  strong.film_halation       = 8.0f;
  strong.film_enlarger_flare = 0.1f;
  document.Drt()->Params().ApplyUpdate(strong);

  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  const auto full = RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 2U);
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 0U);
  auto&      images          = device_.Workspace().Images();
  const auto halation_writer = images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id));
  const auto flare_writer    = images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id));
  EXPECT_NE(halation_writer, 0U);
  EXPECT_NE(flare_writer, 0U);

  const auto roi = CropField(field, kWidth, kRoiX, kRoiY, kRoiSize, kRoiSize);
  SetView({kWidth, kHeight}, {kRoiSize, kRoiSize},
          Matrix3x3::Translate(static_cast<float>(kRoiX), static_cast<float>(kRoiY)));
  const auto detail = RenderView(document, roi, true);
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 2U);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id)), halation_writer);
  EXPECT_EQ(images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id)), flare_writer);
  for (std::uint32_t y = 0; y < kRoiSize; ++y) {
    for (std::uint32_t x = 0; x < kRoiSize; ++x) {
      const auto& got  = detail[static_cast<std::size_t>(y) * kRoiSize + x];
      const auto& want = full[static_cast<std::size_t>(y + kRoiY) * kWidth + x + kRoiX];
      ASSERT_NEAR(got.r, want.r, 1.0e-5f * (1.0f + want.r)) << x << ", " << y;
      ASSERT_NEAR(got.g, want.g, 1.0e-5f * (1.0f + want.g)) << x << ", " << y;
      ASSERT_NEAR(got.b, want.b, 1.0e-5f * (1.0f + want.b)) << x << ", " << y;
    }
  }

  // With both steps off, the same ROI has no light from outside: the results above did change it.
  DrtParameterUpdate off;
  off.film_halation       = 0.0f;
  off.film_enlarger_flare = 0.0f;
  document.Drt()->Params().ApplyUpdate(off);
  const auto plain = RenderView(document, roi, false);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 0U);
  const auto index = static_cast<std::size_t>(98 - kRoiY) * kRoiSize;
  EXPECT_GT(std::abs(detail[index].r - plain[index].r), 1.0f / 255.0f);
}

TEST_F(CudaFilmPrintFixture, FilmPrintRoiWithNoPublishedResultPublishesNone) {
  const auto field    = PatchField(64, 64, 20, 20, 4);
  const auto document = FilmPrintDocument();
  SetView({128, 96}, {64, 64}, Matrix3x3::Translate(32.0f, 16.0f));
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 2U);
  auto& images = device_.Workspace().Images();
  EXPECT_EQ(images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id)), 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id)), 0U);
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 2U)
      << "an image from an ROI must not become the full-frame image";
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 0U);
}

TEST_F(CudaFilmPrintFixture, ReversalFilmPublishesTheLoupeFlareResultAndNotThePrintFlare) {
  const auto field    = PatchField(128, 96, 40, 40, 8);
  auto       document = FilmPrintDocument("fuji_velvia_50");
  SetView({128, 96}, {128, 96}, Matrix3x3::Identity());
  const auto pixels = RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 2U);
  auto& images = device_.Workspace().Images();
  EXPECT_NE(images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id)), 0U);
  EXPECT_NE(images.PublishedLastWriter(FilmLoupeFlareResultId(plan_.drt.node_id)), 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id)), 0U);
  for (const auto& pixel : pixels) {
    ASSERT_TRUE(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
    ASSERT_EQ(pixel.a, 0.75f);
  }

  // The print material is not read: another one gives the same pixels and reads the results.
  DrtParameterUpdate material;
  material.film_print_material = "cine_look";
  document.Drt()->Params().ApplyUpdate(material);
  const auto same = RenderView(document, field, false);
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    ASSERT_EQ(same[index].r, pixels[index].r) << index;
    ASSERT_EQ(same[index].g, pixels[index].g) << index;
    ASSERT_EQ(same[index].b, pixels[index].b) << index;
  }

  // The loupe flare acts on the image.
  DrtParameterUpdate off;
  off.film_enlarger_flare = 0.0f;
  document.Drt()->Params().ApplyUpdate(off);
  const auto plain   = RenderView(document, field, false);
  const auto largest = LargestCodeDifference(
      plain, plain.size(), [&pixels](std::size_t index) { return pixels[index]; });
  EXPECT_GT(largest, 0.5);
}

TEST_F(CudaFilmPrintFixture, LoupeFlareChangeKeepsTheCanvasesThatDoNotReadIt) {
  constexpr std::uint32_t kSize    = 128;
  const auto              field    = PatchField(kSize, kSize, 40, 40, 16);
  auto                    document = FilmPrintDocument("fuji_velvia_50");
  SetView({kSize, kSize}, {kSize, kSize}, Matrix3x3::Identity());
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 2U);

  const auto render_after = [&](auto&& change, std::uint64_t rebuilds) {
    DrtParameterUpdate update;
    change(update);
    document.Drt()->Params().ApplyUpdate(update);
    auto pixels = RenderView(document, field, true);
    EXPECT_EQ(device_.PassStats().film_canvas_rebuild, rebuilds);
    EXPECT_EQ(device_.PassStats().film_canvas_sample, 2U - rebuilds);
    return pixels;
  };
  // Halation changes the amounts, so the display light that the loupe flare blurs. Loupe flare
  // scales its canvas in the display stage.
  (void)render_after([](DrtParameterUpdate& update) { update.film_halation = 3.0f; }, 1U);
  const auto kept =
      render_after([](DrtParameterUpdate& update) { update.film_enlarger_flare = 0.05f; }, 0U);
  // Light spread changes the exposure, so a change and a change back build both canvases again.
  (void)render_after([](DrtParameterUpdate& update) { update.film_light_spread = 2.0f; }, 2U);
  const auto rebuilt =
      render_after([](DrtParameterUpdate& update) { update.film_light_spread = 1.0f; }, 2U);
  ASSERT_EQ(kept.size(), rebuilt.size());
  for (std::size_t index = 0; index < kept.size(); ++index) {
    ASSERT_EQ(kept[index].r, rebuilt[index].r) << index;
    ASSERT_EQ(kept[index].g, rebuilt[index].g) << index;
    ASSERT_EQ(kept[index].b, rebuilt[index].b) << index;
  }
}

TEST_F(CudaFilmPrintFixture, FilmPrintSliderBuildsNoTable) {
  // At this size the spread of slider 2.5 has a sigma of about 0.5 pixels, so it has taps.
  const auto field    = PatchField(1024, 768, 400, 400, 8);
  auto       document = FilmPrintDocument();
  SetView({1024, 768}, {1024, 768}, Matrix3x3::Identity());
  auto       previous = RenderView(document, field, true);
  auto&      tables   = SharedFilmPrintTables();
  const auto stats    = tables.Stats();
  const auto uploaded = device_.Workspace().Device().LutUploadBytes();

  const auto differs  = [](const std::vector<Rgba>& a, const std::vector<Rgba>& b) {
    for (std::size_t index = 0; index < a.size(); ++index) {
      if (a[index].r != b[index].r || a[index].g != b[index].g || a[index].b != b[index].b) {
        return true;
      }
    }
    return false;
  };
  std::vector<DrtParameterUpdate> updates(3);
  updates[0].film_light_spread   = 2.5f;
  updates[1].film_halation       = 6.0f;
  updates[2].film_enlarger_flare = 0.08f;
  for (const auto& update : updates) {
    document.Drt()->Params().ApplyUpdate(update);
    auto pixels = RenderView(document, field, true);
    EXPECT_TRUE(differs(pixels, previous)) << "each slider must change the image";
    previous = std::move(pixels);
  }
  const auto after = tables.Stats();
  EXPECT_EQ(after.uv_builds, stats.uv_builds);
  EXPECT_EQ(after.negative_stage_builds, stats.negative_stage_builds);
  EXPECT_EQ(after.paper_stage_builds, stats.paper_stage_builds);
  EXPECT_EQ(after.compose_builds, stats.compose_builds);
  EXPECT_EQ(device_.Workspace().Device().LutUploadBytes(), uploaded);
}

TEST_F(CudaFilmPrintFixture, FilmPrintFusedSpreadMatchesSeparatePasses) {
  // At this size the narrow Gaussian at slider 3 still fits one dispatch.
  constexpr std::uint32_t kWidth   = 1024;
  constexpr std::uint32_t kHeight  = 768;
  auto                    document = FilmPrintDocument();
  DrtParameterUpdate      update;
  update.film_light_spread = 3.0f;
  document.Drt()->Params().ApplyUpdate(update);
  const auto field = PatchField(kWidth, kHeight, 400, 400, 8);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());

  const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
  const auto         render = [&](bool separate) {
    device_.BeginRender();
    auto& workspace = device_.Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    auto& scene =
        workspace.AcquireImageForWrite(scene_id, {kWidth, kHeight, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(scene.Texture(), std::as_bytes(std::span(field)),
                                               device_.CommandContext());
    auto& display = workspace.AcquireImageForWrite(plan_.display_output,
                                                           {kWidth, kHeight, TextureFormat::Rgba32f});
    RecordCudaFilmPrint(
        device_, plan_, *document.Drt(), std::nullopt, FrameSceneBinding::CachedImage(scene_id),
        FrameSceneBinding::CachedImage(plan_.display_output), kWidth, kHeight, nullptr, separate);
    std::vector<Rgba> pixels(static_cast<std::size_t>(kWidth) * kHeight);
    workspace.Device().DownloadTexture2D(
        display.Texture(), std::as_writable_bytes(std::span(pixels)), device_.CommandContext());
    device_.CancelRender();
    return pixels;
  };
  const auto fused    = render(false);
  const auto separate = render(true);
  const auto largest  = LargestCodeDifference(
      fused, fused.size(), [&separate](std::size_t index) { return separate[index]; });
  std::cout << "fused spread against separate passes: largest " << largest << " codes\n";
  EXPECT_LE(largest, 0.01);
}

TEST_F(CudaFilmPrintFixture, FilmPrintHalfSizeCellsStayOnTheirPixelsAtAnOddSize) {
  // One more row at the bottom must not move the cells of the half-size inhibitor. Cells that cover
  // an odd size at a scale near 2 move up to half a pixel, and the bars show that in the inhibitor.
  constexpr std::uint32_t kWidth    = 4200;
  constexpr std::uint32_t kEven     = 256;
  constexpr std::uint32_t kOdd      = 257;
  // The rows past this one are in the reach of the blurs from the bottom edge.
  constexpr std::size_t   kCompared = 180;
  auto                    document  = FilmPrintDocument("fuji_eterna_500");
  DrtParameterUpdate      no_canvas;
  no_canvas.film_halation       = 0.0f;
  no_canvas.film_enlarger_flare = 0.0f;
  document.Drt()->Params().ApplyUpdate(no_canvas);
  const auto bars = [](std::uint32_t height) {
    std::vector<Rgba> field(static_cast<std::size_t>(kWidth) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
      const float value = (y / 3U) % 2U == 0U ? 2.0f : 0.02f;
      for (std::uint32_t x = 0; x < kWidth; ++x) {
        field[static_cast<std::size_t>(y) * kWidth + x] = {value, value, value, 1.0f};
      }
    }
    return field;
  };

  SetView({kWidth, kEven}, {kWidth, kEven}, Matrix3x3::Identity());
  const auto& bundle   = SharedFilmPrintTables().Bundle();
  const auto  film     = document.Drt()->Params().FilmPrint();
  const auto  negative = bundle.Negative(film.negative);
  EXPECT_TRUE(MakeFilmPrintImagePlan(plan_.geometry, bundle.ImagePlaneConstants(),
                                     negative.layer_spread_um, true, film)
                  .inhibitor_half_size);
  const auto even = RenderView(document, bars(kEven), false);
  SetView({kWidth, kOdd}, {kWidth, kOdd}, Matrix3x3::Identity());
  const auto odd = RenderView(document, bars(kOdd), false);
  EXPECT_LT(LargestCodeDifference(odd, kCompared * kWidth,
                                  [&](std::size_t index) { return even[index]; }),
            0.01);
}

/// A neutral step edge of 3 stops about the grey, at the middle of each row.
auto NeutralStepEdge(std::uint32_t width, std::uint32_t height) -> std::vector<Rgba> {
  std::vector<Rgba> field(static_cast<std::size_t>(width) * height);
  const float       low  = 0.18f * std::pow(2.0f, -1.5f);
  const float       high = 0.18f * std::pow(2.0f, 1.5f);
  for (std::size_t index = 0; index < field.size(); ++index) {
    const float value = index % width < width / 2 ? low : high;
    field[index]      = {value, value, value, 1.0f};
  }
  return field;
}

// Eterna 500 has a chart with no rise above 1, so its inhibition has no image part. The chart of
// Portra 400 rises, so that stock keeps an edge. Lab rule 'inhibition_image_part'.
TEST_F(CudaFilmPrintFixture, FilmPrintStepEdgeFollowsTheImagePartOfTheInhibition) {
  constexpr std::uint32_t kWidth  = 6000;
  constexpr std::uint32_t kHeight = 4000;
  const auto              field   = NeutralStepEdge(kWidth, kHeight);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  /// The larger of the overshoot and the undershoot of the green channel, in 8 bit codes.
  const auto largest_edge_codes = [&](const char* negative) {
    const auto  pixels = RenderView(FilmPrintDocument(negative), field, false);
    const auto* row    = pixels.data() + static_cast<std::size_t>(kHeight / 2) * kWidth;
    const float low    = row[kWidth / 2 - 1500].g;
    const float high   = row[kWidth / 2 + 1500].g;
    float       over   = 0.0f;
    float       under  = 0.0f;
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      if (x < kWidth / 2) {
        under = (std::max)(under, low - row[x].g);
      } else {
        over = (std::max)(over, row[x].g - high);
      }
    }
    return (std::max)(over, under) * 255.0f;
  };
  EXPECT_LE(largest_edge_codes("fuji_eterna_500"), 1.0f);
  EXPECT_GE(largest_edge_codes("kodak_portra_400"), 2.0f);
}

TEST_F(CudaFilmPrintFixture, FilmPrintWithNoImagePartRunsNoInhibitorBlur) {
  constexpr std::uint32_t kWidth  = 6000;
  constexpr std::uint32_t kHeight = 4000;
  const auto              field   = NeutralStepEdge(kWidth, kHeight);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  const auto runs_inhibitor_blur = [&](const char* negative) {
    const auto document = FilmPrintDocument(negative);
    device_.BeginRender();
    auto& workspace = device_.Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& scene = workspace.AcquireImageForWrite(scene_id, {kWidth, kHeight, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(scene.Texture(), std::as_bytes(std::span(field)),
                                       device_.CommandContext());
    const auto display_id = plan_.display_output;
    (void)workspace.AcquireImageForWrite(display_id, {kWidth, kHeight, TextureFormat::Rgba32f});
    std::vector<FilmPrintPassTime> times;
    RecordCudaFilmPrint(device_, plan_, *document.Drt(), std::nullopt,
                        FrameSceneBinding::CachedImage(scene_id),
                        FrameSceneBinding::CachedImage(display_id), kWidth, kHeight, &times);
    device_.CancelRender();
    return std::any_of(times.begin(), times.end(), [](const FilmPrintPassTime& time) {
      return time.name.find("inhibitor") != std::string::npos;
    });
  };
  EXPECT_FALSE(runs_inhibitor_blur("fuji_eterna_500"));
  EXPECT_TRUE(runs_inhibitor_blur("kodak_portra_400"));
}

TEST_F(CudaFilmPrintFixture, FilmPrintAllocationFailureReportsTheErrorAndPublishesNoPartialResult) {
  const auto field    = PatchField(128, 96, 40, 40, 8);
  const auto document = FilmPrintDocument();
  SetView({128, 96}, {128, 96}, Matrix3x3::Identity());
  auto&      images   = device_.Workspace().Images();
  const auto halation = FilmHalationResultId(plan_.drt.node_id);
  const auto flare    = FilmFlareResultId(plan_.drt.node_id);
  // Each try allows one more new allocation than the last try, so the failure moves through the
  // allocations of the render, up to the last one of the film print.
  int        failures = 0;
  for (; failures < 64; ++failures) {
    device_.Workspace().Device().FailTextureAllocationAfter(static_cast<std::uint32_t>(failures));
    try {
      (void)RenderView(document, field, true);
      break;
    } catch (const std::runtime_error& error) {
      EXPECT_FALSE(std::string(error.what()).empty());
      device_.CancelRender();
      device_.WaitIdle();
      EXPECT_EQ(images.PublishedLastWriter(halation), 0U);
      EXPECT_EQ(images.PublishedLastWriter(flare), 0U);
    }
  }
  std::cout << "allocation failures before the first complete render: " << failures << "\n";
  ASSERT_LT(failures, 64);
  // The scene, the display, and the images of the film print each had a failure.
  EXPECT_GE(failures, 5);
  device_.Workspace().Device().FailTextureAllocationAfter(1000000);
  EXPECT_NE(images.PublishedLastWriter(halation), 0U);
  EXPECT_NE(images.PublishedLastWriter(flare), 0U);
}

/// Linear AP1 values of the As Shot test: a neutral ramp of -8 to +8 stops and saturated colours,
/// one row with no image plane.
auto RampAndColourValues() -> std::vector<Rgba> {
  std::vector<Rgba> values;
  for (int step = -32; step <= 32; ++step) {
    const float v = 0.18f * std::exp2(static_cast<float>(step) / 4.0f);
    values.push_back({v, v, v, 0.75f});
  }
  for (const float level : {0.05f, 0.18f, 0.7f, 3.0f}) {
    for (const auto& [r, g, b] : std::vector<std::array<float, 3>>{{1.0f, 0.1f, 0.05f},
                                                                   {0.1f, 1.0f, 0.1f},
                                                                   {0.05f, 0.1f, 1.0f},
                                                                   {1.0f, 0.8f, 0.1f},
                                                                   {0.1f, 0.9f, 1.0f},
                                                                   {0.9f, 0.1f, 1.0f},
                                                                   {0.6f, 0.4f, 0.3f}}) {
      values.push_back({level * r, level * g, level * b, 0.75f});
    }
  }
  return values;
}

TEST_F(CudaFilmPrintFixture, AsShotFollowsTheColourTemperatureOfTheRawDecode) {
  const auto values  = RampAndColourValues();
  const auto render  = [&](PipelineDocument& document) { return RenderDrt(document, values); };
  const auto differs = [](const std::vector<Rgba>& a, const std::vector<Rgba>& b) {
    return LargestCodeDifference(a, a.size(), [&b](std::size_t index) { return b[index]; });
  };
  auto& tables = SharedFilmPrintTables();
  // A print, and Negative Scan, which shows the cast of the RAW white.
  for (const std::string& material :
       {std::string{"kodak_2383"}, std::string{kFilmNegativeScanId}}) {
    SCOPED_TRACE(material);
    auto document = FilmPrintDocument("kodak_5207", material, std::string{kFilmAsShotLightId});
    // A RAW file in mode As Shot uses its solved temperature: the same as mode Custom at it.
    film_print_test::SetRawAsShot(document, 5650.0);
    ASSERT_EQ(FilmSceneRawCct(document), std::optional<double>(5650.0));
    const auto as_shot = render(document);
    auto       custom  = FilmPrintDocument("kodak_5207", material, std::string{kFilmAsShotLightId});
    film_print_test::SetRawCustom(custom, 5650.0);
    ASSERT_EQ(FilmSceneRawCct(custom), std::optional<double>(5650.0));
    EXPECT_EQ(differs(as_shot, render(custom)), 0.0);

    // A change of the Kelvin in mode Custom changes the output and the keys of the tables.
    const auto key_of = [&](const PipelineDocument& d) {
      const auto light = tables.Bundle().SceneLightMix(kFilmAsShotLightId, FilmSceneRawCct(d));
      return tables.Resolve("kodak_5207", material, light)->uv_key;
    };
    const auto warm_key = key_of(custom);
    film_print_test::SetRawCustom(custom, 3100.0);
    EXPECT_NE(key_of(custom), warm_key);
    EXPECT_GT(differs(as_shot, render(custom)), 1.0);

    // An input with no RAW white balance renders As Shot as the reference.
    film_print_test::SetRasterInput(document);
    EXPECT_EQ(FilmSceneRawCct(document), std::nullopt);
    const auto raster    = render(document);
    auto       reference = FilmPrintDocument("kodak_5207", material, "reference");
    film_print_test::SetRasterInput(reference);
    EXPECT_EQ(differs(raster, render(reference)), 0.0);
  }

  // The as-shot neutral of a DNG: the solve of Develop gives the temperature of the scene light.
  auto document =
      FilmPrintDocument("kodak_5219", "kodak_2383", std::string{kFilmAsShotLightId});
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto payload      = document.Develop()->Params().Params();
  payload.wb_mode   = "as_shot";
  const auto solved = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(solved.ok);
  payload.as_shot_cct = solved.transform.resolved_cct;
  document.Develop()->Params().ReplaceParams(payload);
  ASSERT_EQ(FilmSceneRawCct(document), std::optional<double>(solved.transform.resolved_cct));
  const auto neutral = render(document);
  for (const auto& pixel : neutral) {
    ASSERT_TRUE(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
  }
  auto custom =
      FilmPrintDocument("kodak_5219", "kodak_2383", std::string{kFilmAsShotLightId});
  film_print_test::SetRawCustom(custom, solved.transform.resolved_cct);
  EXPECT_EQ(differs(neutral, render(custom)), 0.0);
}

/// A document of print material Negative Scan with auto colour @p auto_colour.
auto NegativeScanDocument(const std::string& negative    = "kodak_5207",
                          const std::string& auto_colour = "off",
                          const std::string& scene_light = "reference") -> PipelineDocument {
  auto document = FilmPrintDocument(negative, std::string{kFilmNegativeScanId}, scene_light);
  DrtParameterUpdate update;
  update.film_scan_auto = auto_colour;
  document.Drt()->Params().ApplyUpdate(update);
  return document;
}

void ExpectSamePixels(const std::vector<Rgba>& a, const std::vector<Rgba>& b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t index = 0; index < a.size(); ++index) {
    ASSERT_EQ(a[index].r, b[index].r) << index;
    ASSERT_EQ(a[index].g, b[index].g) << index;
    ASSERT_EQ(a[index].b, b[index].b) << index;
  }
}

auto LargestDifference(const std::vector<Rgba>& a, const std::vector<Rgba>& b) -> double {
  return LargestCodeDifference(a, a.size(), [&b](std::size_t index) { return b[index]; });
}

TEST_F(CudaFilmPrintFixture, NegativeScanBuildsNoFlareCanvasAndReadsNoPrescanWithAutoColourOff) {
  const auto         field    = PatchField(128, 96, 40, 40, 8);
  auto               document = NegativeScanDocument();
  DrtParameterUpdate strong;
  strong.film_enlarger_flare = 0.1f;
  document.Drt()->Params().ApplyUpdate(strong);
  SetView({128, 96}, {128, 96}, Matrix3x3::Identity());
  const auto pixels = RenderView(document, field, true);
  // Negative Scan has no enlarger, so only the Halation canvas is built. With auto colour off
  // (Neutral Scan) the prescan does not run and the host reads nothing.
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 1U);
  EXPECT_EQ(device_.PassStats().film_scan_prescan_reads, 0U);
  auto& images = device_.Workspace().Images();
  EXPECT_NE(images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id)), 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id)), 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmLoupeFlareResultId(plan_.drt.node_id)), 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmScanPrescanResultId(plan_.drt.node_id)), 0U);
  for (const auto& pixel : pixels) {
    ASSERT_TRUE(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
    ASSERT_GE((std::min)({pixel.r, pixel.g, pixel.b}), 0.0f);
    ASSERT_LE((std::max)({pixel.r, pixel.g, pixel.b}), 1.0f + 1e-6f);
    ASSERT_EQ(pixel.a, 0.75f);
  }

  // The stored Enlarger flare has no effect, and a change of it builds no canvas.
  DrtParameterUpdate off;
  off.film_enlarger_flare = 0.0f;
  document.Drt()->Params().ApplyUpdate(off);
  const auto same = RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 0U);
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 1U);
  ExpectSamePixels(same, pixels);

  // A print of the same negative is another image.
  DrtParameterUpdate material;
  material.film_print_material = "kodak_2383";
  document.Drt()->Params().ApplyUpdate(material);
  EXPECT_GT(LargestDifference(RenderView(document, field, false), pixels), 4.0);

  // A colour reversal film reads no print material, so a stored Negative Scan renders its light
  // table.
  const auto slide = RenderView(
      FilmPrintDocument("fuji_velvia_50", std::string{kFilmNegativeScanId}), field, true);
  ExpectSamePixels(slide, RenderView(FilmPrintDocument("fuji_velvia_50", "kodak_2383"), field, true));
}

TEST_F(CudaFilmPrintFixture, NegativeScanAutoColourReadsThePrescanOfTheFullFrame) {
  constexpr std::uint32_t kWidth  = 256;
  constexpr std::uint32_t kHeight = 192;
  const auto              field   = PatchField(kWidth, kHeight, 60, 50, 24);
  auto                    document = NegativeScanDocument("kodak_5207", "neutral");
  auto&                   images   = device_.Workspace().Images();
  const auto              prescan_id = FilmScanPrescanResultId(plan_.drt.node_id);
  const auto              full_view  = [&] {
    SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  };
  full_view();
  const auto full = RenderView(document, field, true);
  // The Halation canvas and the prescan, and one read of the prescan.
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 2U);
  EXPECT_EQ(device_.PassStats().film_scan_prescan_reads, 1U);
  EXPECT_NE(images.PublishedLastWriter(prescan_id), 0U);
  const auto shift = device_.PassStats().film_scan_auto_shift;
  EXPECT_GT(std::abs(shift[0]) + std::abs(shift[1]) + std::abs(shift[2]), 1e-4);

  // A viewport ROI samples the prescan of the full frame, so it has the same shift.
  constexpr std::uint32_t kRoiX = 80;
  constexpr std::uint32_t kRoiY = 64;
  SetView({kWidth, kHeight}, {96, 64},
          Matrix3x3::Translate(static_cast<float>(kRoiX), static_cast<float>(kRoiY)));
  (void)RenderView(document, CropField(field, kWidth, kRoiX, kRoiY, 96, 64), false);
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 2U);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 0U);
  EXPECT_EQ(device_.PassStats().film_scan_prescan_reads, 1U);
  EXPECT_EQ(device_.PassStats().film_scan_auto_shift, shift);

  // A rendering setting reads the kept prescan: the contrast scales x, so the shift changes, but no
  // canonical result is built.
  full_view();
  DrtParameterUpdate contrast;
  contrast.film_scan_contrast = 1.25f;
  document.Drt()->Params().ApplyUpdate(contrast);
  const auto steeper = RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 0U);
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 2U);
  EXPECT_NE(device_.PassStats().film_scan_auto_shift, shift);
  EXPECT_GT(LargestDifference(steeper, full), 1.0);

  // A scanner setting builds the scanner table and the prescan again.
  DrtParameterUpdate scanner;
  scanner.film_scanner_red_nm = 630.0f;
  document.Drt()->Params().ApplyUpdate(scanner);
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild, 1U);
  EXPECT_EQ(device_.PassStats().film_canvas_sample, 1U);
  EXPECT_EQ(device_.PassStats().film_scan_prescan_reads, 1U);

  // Auto colour off runs no reduction and reads no pixel.
  DrtParameterUpdate neutral_scan;
  neutral_scan.film_scan_auto = std::string{"off"};
  document.Drt()->Params().ApplyUpdate(neutral_scan);
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_.PassStats().film_canvas_rebuild + device_.PassStats().film_canvas_sample, 1U);
  EXPECT_EQ(device_.PassStats().film_scan_prescan_reads, 0U);
}

TEST_F(CudaFilmPrintFixture, NegativeScanRenderingSettingsBuildNoTableAndAScannerSettingDoes) {
  const auto field    = PatchField(64, 48, 20, 20, 8);
  auto       document = NegativeScanDocument("kodak_portra_400");
  SetView({64, 48}, {64, 48}, Matrix3x3::Identity());
  auto        previous = RenderView(document, field, true);
  const auto& backend  = device_.Workspace().Device();
  // The film print tables of the pass live in the CUDA library, so this test reads the GPU side: a
  // table of a new content key is an upload, and a kept table is none.
  const auto  uploaded = backend.LutUploadBytes();
  std::vector<DrtParameterUpdate> rendering(5);
  rendering[0].film_scan_contrast       = 1.2f;
  rendering[1].film_scan_exposure_stops = -0.5f;
  rendering[2].film_scan_cyan           = 0.05f;
  rendering[3].film_scan_yellow         = -0.05f;
  rendering[4].film_scan_black          = 0.5f;
  for (std::size_t change = 0; change < rendering.size(); ++change) {
    SCOPED_TRACE(change);
    document.Drt()->Params().ApplyUpdate(rendering[change]);
    auto pixels = RenderView(document, field, true);
    EXPECT_GT(LargestDifference(pixels, previous), 0.5);
    EXPECT_EQ(backend.LutUploadBytes(), uploaded);
    previous = std::move(pixels);
  }
  // A scanner band builds the scanner table of the new scanner on the host, and uploads it: one
  // table of 65^3 RGBA32F nodes.
  DrtParameterUpdate band;
  band.film_scanner_blue_nm = 460.0f;
  document.Drt()->Params().ApplyUpdate(band);
  const auto pixels = RenderView(document, field, true);
  EXPECT_GT(LargestDifference(pixels, previous), 0.5);
  EXPECT_EQ(backend.LutUploadBytes() - uploaded, 65ull * 65ull * 65ull * 4ull * sizeof(float));
}

TEST_F(CudaFilmPrintFixture, NegativeScanAndAPrintMaterialChangeKeepTheResultsOfEachOther) {
  constexpr std::uint32_t kSize    = 128;
  const auto              field    = PatchField(kSize, kSize, 40, 40, 16);
  auto                    document = FilmPrintDocument("kodak_portra_400", "kodak_endura_premier");
  SetView({kSize, kSize}, {kSize, kSize}, Matrix3x3::Identity());
  const auto select = [&document](const std::string& material) {
    DrtParameterUpdate update;
    update.film_print_material = material;
    document.Drt()->Params().ApplyUpdate(update);
  };
  const auto print = RenderView(document, field, true);
  select(std::string{kFilmNegativeScanId});
  const auto scan     = RenderView(document, field, true);
  const auto uploaded = device_.Workspace().Device().LutUploadBytes();
  // A change back and forth gives the same images: FP1 to FP5 read the same values in both forms,
  // and no result of one form stays in the other.
  select("kodak_endura_premier");
  const auto print_again = RenderView(document, field, true);
  select(std::string{kFilmNegativeScanId});
  const auto scan_again = RenderView(document, field, true);
  ExpectSamePixels(print_again, print);
  ExpectSamePixels(scan_again, scan);
  // The device keeps the print table, the scanner table, and the u'v' table of the negative.
  EXPECT_EQ(device_.Workspace().Device().LutUploadBytes(), uploaded);
}

TEST_F(CudaFilmPrintFixture, NegativeScanShowsItsLinearAp1ThroughTheOpenDrtOfTheDocument) {
  // Negative Scan ends in linear AP1. Method OpenDRT with the same settings on that value gives the
  // same display codes, for an SDR and an HDR encoding.
  const auto values = RampAndColourValues();
  struct Encoding {
    const char*   name;
    DrtColorSpace space;
    DrtEotf       eotf;
    float         peak;
  };
  for (const auto& encoding :
       {Encoding{"Rec. 709 sRGB", DrtColorSpace::Rec709, DrtEotf::SrgbPiecewise, 100.0f},
        Encoding{"Rec. 2020 PQ 1000 nits", DrtColorSpace::Rec2020, DrtEotf::St2084, 1000.0f}}) {
    SCOPED_TRACE(encoding.name);
    for (const auto* auto_colour : {"off", "neutral"}) {
      SCOPED_TRACE(auto_colour);
      auto               scan = NegativeScanDocument("kodak_5219", auto_colour);
      DrtParameterUpdate output;
      output.encoding_space   = encoding.space;
      output.encoding_eotf    = encoding.eotf;
      output.peak_luminance   = encoding.peak;
      output.look_preset      = std::string{"arriba"};
      output.tonescale_preset = std::string{"use_look_preset"};
      scan.Drt()->Params().ApplyUpdate(output);
      plan_.film_scan_linear_output = true;
      const auto linear             = RenderDrt(scan, values);
      plan_.film_scan_linear_output = false;
      const auto shown              = RenderDrt(scan, values);

      auto open_drt = CreateDefaultPipelineDocument();
      output.method = DrtMethod::OpenDrt;
      open_drt.Drt()->Params().ApplyUpdate(output);
      const auto expected = RenderDrt(open_drt, linear);
      const auto largest  = LargestDifference(shown, expected);
      std::cout << "Negative Scan against method OpenDRT on its linear AP1, " << encoding.name
                << ", auto " << auto_colour << ": largest " << largest << " codes\n";
      EXPECT_LE(largest, 0.1);
      for (const auto& pixel : shown) {
        ASSERT_EQ(pixel.a, 0.75f);
      }
    }
  }
}

// The lab values come from build_display_expected.py of alcedo-film-lab. They are not in Git, so a
// skip here means that the comparison did not run.
#if defined(ALCEDO_FILM_PRINT_EXPECTED_DIR)

struct DisplaySelection {
  FilmArrayFile file;
  std::string   negative;
  std::string   print_material;
  /// A Negative Scan file names a colour temperature between grid nodes as cct<kelvin>.
  std::string   scene_light;
  std::string   auto_colour = "off";  ///< Negative Scan only

  [[nodiscard]] auto Scan() const -> bool { return print_material == kFilmNegativeScanId; }
};

/// The document of a selection of the lab. On Negative Scan a colour temperature is scene light As
/// Shot with that temperature as the solved as-shot temperature of the RAW decode.
auto SelectionDocument(const std::string& negative, const std::string& print_material,
                       const std::string& scene_light, const std::string& auto_colour)
    -> PipelineDocument {
  if (print_material != kFilmNegativeScanId) {
    return FilmPrintDocument(negative, print_material, scene_light);
  }
  auto document = FilmPrintDocument(negative, print_material);
  film_print_test::SetNegativeScan(document, scene_light, auto_colour);
  return document;
}

auto SelectionDocument(const DisplaySelection& selection) -> PipelineDocument {
  return SelectionDocument(selection.negative, selection.print_material, selection.scene_light,
                           selection.auto_colour);
}

auto DisplaySelections() -> std::vector<DisplaySelection> {
  std::vector<DisplaySelection> selections;
  const auto                    directory = fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "display";
  if (!fs::exists(directory)) return selections;
  for (const auto& entry : fs::directory_iterator(directory)) {
    auto       file     = FilmArrayFile::Parse(ReadFilmResourceBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    if (metadata.contains("reversal")) {
      // A colour reversal film has no print material. The document keeps its default one.
      selections.push_back({std::move(file), metadata.at("reversal").get<std::string>(),
                            FilmPrintParams{}.print_material,
                            metadata.at("scene_light").get<std::string>()});
      continue;
    }
    selections.push_back({std::move(file), metadata.at("negative").get<std::string>(),
                          metadata.at("print_material").get<std::string>(),
                          metadata.at("scene_light").get<std::string>(),
                          metadata.value("auto", std::string{"off"})});
  }
  return selections;
}

constexpr double kRampLimitCodes         = 0.5;
constexpr double kChartLimitCodes        = 1.0;
// Negative Scan in linear AP1 before the display transform against the lab, in stops for values
// above 2^-10 (issue 664).
constexpr double kScanRampLimitStops     = 0.005;
constexpr double kScanChartLimitStops    = 0.01;
/// The auto colour shift of the GPU prescan against the lab, in log10 units (issue 664).
constexpr double kScanShiftLimit         = 1e-4;
// The 6000 x 4006 image: p99.9 of issue 664. The largest difference and the shift have the
// measured state of the image-plane steps FP2 to FP5 against the lab image (issue 668; the limits
// of issue 664 are 0.1 stops and 1e-4).
constexpr double kScanImageP999Stops     = 0.02;
constexpr double kScanImageLargestStops  = 0.2;
constexpr double kScanImageShiftLimit    = 5e-4;
/// The lab keeps its stages in float32, so the double-precision reference differs from it a little.
constexpr double kReferenceLabLimitCodes = 1.0;

struct Float3 {
  float r;
  float g;
  float b;
};

/// Negative Scan before the display transform on the frame of one row @p part of a lab file: the
/// linear AP1 against the lab in stops, and with auto colour the shift against the lab.
template <class Fixture>
auto CompareScanRow(Fixture& fixture, const DisplaySelection& selection, const std::string& part,
                    double limit_stops) -> void {
  const auto count  = selection.file.Shape(part + "_ap1")[0];
  const auto ap1    = selection.file.F32(part + "_ap1", {count, 3});
  const auto lab    = selection.file.F32(part + "_linear", {count, 3});
  const auto linear = fixture.RenderScanLinear(SelectionDocument(selection), ToRgba(ap1));
  double     shift  = 0.0;
  if (selection.auto_colour != "off") {
    shift = film_print_test::LargestShiftDifference(fixture.LastAutoShift(),
                                                    selection.file.F64(part + "_shift", {3}));
    EXPECT_LE(shift, kScanShiftLimit);
  }
  // The ramp is neutral. A chart value can be far outside AP1, so the chart also has the floor of
  // film_print_test::ChartStopsDifference (issue 667); the strict value is in the record.
  const bool  chart   = part == "chart";
  double      strict  = 0.0;
  double      limited = 0.0;
  std::size_t worst   = 0;
  for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
    const auto got  = film_print_test::Rgb(linear[index]);
    const std::array<double, 3> want{lab[index * 3], lab[index * 3 + 1], lab[index * 3 + 2]};
    const auto d    = film_print_test::StopsDifference(got, want);
    if (d > strict) {
      strict = d;
      worst  = index;
    }
    limited = (std::max)(limited, chart ? film_print_test::ChartStopsDifference(got, want) : d);
  }
  std::cout << part << " " << selection.negative << " on Negative Scan under "
            << selection.scene_light << ", auto " << selection.auto_colour << ": largest " << limited
            << " stops";
  if (chart) {
    std::cout << " (with no relative floor " << strict << " at value " << worst << ", AP1 "
              << ap1[worst * 3] << " " << ap1[worst * 3 + 1] << " " << ap1[worst * 3 + 2]
              << ": GPU " << linear[worst].r << " " << linear[worst].g << " " << linear[worst].b
              << ", lab " << lab[worst * 3] << " " << lab[worst * 3 + 1] << " "
              << lab[worst * 3 + 2] << ")";
  }
  std::cout << ", shift " << shift << "\n";
  EXPECT_LE(limited, limit_stops);
}

TEST_F(CudaFilmPrintFixture, FilmPrintRampMatchesLab) {
  const auto selections = DisplaySelections();
  if (selections.empty()) GTEST_SKIP() << "No lab values. Run build_display_expected.py.";
  int scans = 0;
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " on " + selection.print_material + " under " +
                 selection.scene_light + ", auto " + selection.auto_colour);
    if (selection.Scan()) {
      CompareScanRow(*this, selection, "ramp", kScanRampLimitStops);
      ++scans;
      continue;
    }
    const auto count  = selection.file.Shape("ramp_ap1")[0];
    const auto ap1    = selection.file.F32("ramp_ap1", {count, 3});
    const auto lab    = selection.file.F32("ramp_srgb", {count, 3});
    const auto pixels = RenderDrt(SelectionDocument(selection), ToRgba(ap1));
    const auto largest =
        LargestCodeDifference(pixels, static_cast<std::size_t>(count), [&lab](std::size_t index) {
          return Float3{lab[index * 3], lab[index * 3 + 1], lab[index * 3 + 2]};
        });
    std::cout << "ramp " << selection.negative << ": largest " << largest << " codes\n";
    EXPECT_LE(largest, kRampLimitCodes);
  }
  // 4 negatives, 4 scene lights, auto colour off and Neutral.
  EXPECT_EQ(scans, 32);
}

TEST_F(CudaFilmPrintFixture, FilmPrintChartMatchesReference) {
  const auto selections = DisplaySelections();
  if (selections.empty()) GTEST_SKIP() << "No lab values. Run build_display_expected.py.";
  auto& tables = SharedFilmPrintTables();
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.negative + " on " + selection.print_material + " under " +
                 selection.scene_light + ", auto " + selection.auto_colour);
    if (selection.Scan()) {
      CompareScanRow(*this, selection, "chart", kScanChartLimitStops);
      continue;
    }
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
    const auto pixels  = RenderDrt(SelectionDocument(selection), ToRgba(ap1));
    const auto largest = LargestCodeDifference(
        pixels, expected.size(), [&expected](std::size_t index) { return expected[index]; });
    std::cout << "chart " << selection.negative << ": largest " << largest
              << " codes against the reference, reference against the lab " << reference_against_lab
              << " codes\n";
    EXPECT_LE(largest, kChartLimitCodes);
    EXPECT_LE(reference_against_lab, kReferenceLabLimitCodes);
  }
}

/// Per pixel the largest channel difference, in 8 bit codes: the mean, p99.9, and the largest.
struct ImageDifference {
  double mean    = 0.0;
  double p999    = 0.0;
  double largest = 0.0;
};

auto DifferenceFromLab(const std::vector<Rgba>& pixels, std::span<const float> lab)
    -> ImageDifference {
  std::vector<float> difference(pixels.size());
  double             sum = 0.0;
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    difference[index] = (std::max)({std::abs(pixels[index].r - lab[index * 3]),
                                    std::abs(pixels[index].g - lab[index * 3 + 1]),
                                    std::abs(pixels[index].b - lab[index * 3 + 2])}) *
                        255.0f;
    sum += difference[index];
  }
  ImageDifference result;
  result.mean     = sum / static_cast<double>(difference.size());
  const auto rank = static_cast<std::ptrdiff_t>(0.999 * static_cast<double>(difference.size() - 1));
  std::nth_element(difference.begin(), difference.begin() + rank, difference.end());
  result.p999    = difference[static_cast<std::size_t>(rank)];
  result.largest = *std::max_element(difference.begin() + rank, difference.end());
  return result;
}

auto ImageExpectedDirectory() -> fs::path {
  return fs::path(ALCEDO_FILM_PRINT_EXPECTED_DIR) / "image";
}

auto ReadLabAp1(std::uint32_t& width, std::uint32_t& height) -> std::vector<Rgba> {
  const auto path  = ImageExpectedDirectory() / "ap1.bin";
  auto       file  = FilmArrayFile::Parse(ReadFilmResourceBytes(path), path);
  const auto shape = file.Shape("ap1");
  height           = static_cast<std::uint32_t>(shape[0]);
  width            = static_cast<std::uint32_t>(shape[1]);
  return ToRgba(file.F32("ap1", {shape[0], shape[1], 3}));
}

/// The largest difference of a GPU prescan of the scan density, mapped to x on the host, from the
/// prescan of x of the lab.
auto PrescanDifference(const std::vector<Rgba>& density, std::span<const double> lab,
                       const film_negative_scan::RenderTerms& terms) -> double {
  double largest = 0.0;
  for (std::size_t index = 0; index < density.size(); ++index) {
    const auto rgb = film_print_test::Rgb(density[index]);
    for (std::size_t k = 0; k < 3; ++k) {
      largest = (std::max)(largest,
                           std::abs(terms.slope[k] * rgb[k] + terms.offset[k] - lab[index * 3 + k]));
    }
  }
  return largest;
}

TEST_F(CudaFilmPrintFixture, FilmPrintImageMatchesLab) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
  const auto    ap1    = ReadLabAp1(width, height);
  SetView({width, height}, {width, height}, Matrix3x3::Identity());
  int   compared = 0;
  auto& tables   = SharedFilmPrintTables();
  for (const auto& entry : fs::directory_iterator(ImageExpectedDirectory())) {
    if (entry.path().filename() == "ap1.bin") continue;
    auto       file     = FilmArrayFile::Parse(ReadFilmResourceBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    // A colour reversal film has no print material, so its file names none.
    const bool reversal = metadata.contains("reversal");
    const auto negative = metadata.at(reversal ? "reversal" : "negative").get<std::string>();
    const auto material = reversal ? FilmPrintParams{}.print_material
                                   : metadata.at("print_material").get<std::string>();
    const auto light    = metadata.at("scene_light").get<std::string>();
    const auto rows     = static_cast<std::int64_t>(height);
    const auto columns  = static_cast<std::int64_t>(width);
    if (material == kFilmNegativeScanId) {
      // Negative Scan before the display transform, against the linear AP1 of the lab.
      const auto auto_colour = metadata.at("auto").get<std::string>();
      SCOPED_TRACE(negative + " on Negative Scan under " + light + ", auto " + auto_colour);
      const auto document = SelectionDocument(negative, material, light, auto_colour);
      const bool automatic = auto_colour != "off";
      std::vector<Rgba> prescan;
      plan_.film_scan_linear_output = true;
      const auto linear = RenderView(document, ap1, automatic, automatic ? &prescan : nullptr);
      plan_.film_scan_linear_output = false;
      const auto stops = film_print_test::ImageStops(linear, file.F32("linear", {rows, columns, 3}));
      double     shift = 0.0;
      double     x     = 0.0;
      if (automatic) {
        shift = film_print_test::LargestShiftDifference(LastAutoShift(), file.F64("shift", {3}));
        const auto& film  = document.Drt()->Params().FilmPrint();
        const auto  mix   = tables.Bundle().SceneLightMix(film.scene_light, FilmSceneRawCct(document));
        const auto  data  = tables.Resolve(negative, material, mix);
        const auto  lab_x = file.F64("prescan", {64, 96, 3});
        EXPECT_EQ(prescan.size(), 64U * 96U);
        x = PrescanDifference(prescan, lab_x,
                              film_negative_scan::MakePrescanTerms(data->negative_scan, film.scan));
        EXPECT_LE(shift, kScanImageShiftLimit);
      }
      std::cout << "image " << negative << " on Negative Scan under " << light << ", auto "
                << auto_colour << " (" << width << " x " << height << "): mean " << stops.mean
                << ", p99.9 " << stops.p999 << ", largest " << stops.largest
                << " stops; shift " << shift << ", prescan x " << x << "\n";
      EXPECT_LE(stops.p999, kScanImageP999Stops);
      EXPECT_LE(stops.largest, kScanImageLargestStops);
      ++compared;
      continue;
    }
    SCOPED_TRACE(negative);
    const auto pixels     = RenderView(FilmPrintDocument(negative, material, light), ap1, false);
    const auto difference = DifferenceFromLab(pixels, file.F32("srgb", {rows, columns, 3}));
    std::cout << "image " << negative << " (" << width << " x " << height << "): mean "
              << difference.mean << ", p99.9 " << difference.p999 << ", largest "
              << difference.largest << " codes\n";
    EXPECT_LE(difference.p999, 1.0);
    EXPECT_LE(difference.largest, 4.0);
    ++compared;
  }
  // 5 selections of the negatives, the 5 reversal films, and 32 of Negative Scan: 4 negatives
  // under 4 scene lights, auto colour off and Neutral.
  EXPECT_EQ(compared, 42);
}

TEST_F(CudaFilmPrintFixture, FilmPrintRoiEdgeDiffersOnlyInsideTheReachOfItsBlurs) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t           width  = 0;
  std::uint32_t           height = 0;
  const auto              ap1    = ReadLabAp1(width, height);
  // The light spread at slider 1 reaches 5 pixels, and each of the 2 inhibitor blurs 2 * 11 pixels.
  constexpr std::uint32_t kReach = 5 + 2 * 2 * 11;
  constexpr std::uint32_t kRoiW  = 1600;
  constexpr std::uint32_t kRoiH  = 1200;
  const std::uint32_t     roi_x  = (width - kRoiW) / 2;
  // The middle row of this frame is odd, so the half-size cells of the inhibitor in the ROI have
  // another phase than in the full frame. One row up, the cells are the same.
  const std::uint32_t     odd_y  = (height - kRoiH) / 2;
  ASSERT_EQ(odd_y % 2U, 1U);
  // A negative, and a reversal film with the light table and the loupe flare.
  for (const auto* film : {"kodak_5207", "fuji_velvia_50"}) {
    SCOPED_TRACE(film);
    const bool reversal = IsFilmReversalIdentifier(film);
    const auto document = FilmPrintDocument(film);
    SetView({width, height}, {width, height}, Matrix3x3::Identity());
    const auto full = RenderView(document, ap1, true);
    for (const std::uint32_t roi_y : {odd_y, odd_y - 1U}) {
      const bool same_cells = roi_y % 2U == 0U;
      SCOPED_TRACE(same_cells ? "ROI on the cells of the frame" : "ROI with other cells");
      const auto crop = CropField(ap1, width, roi_x, roi_y, kRoiW, kRoiH);
      SetView({width, height}, {kRoiW, kRoiH},
              Matrix3x3::Translate(static_cast<float>(roi_x), static_cast<float>(roi_y)));
      const auto detail = RenderView(document, crop, false);
      EXPECT_EQ(device_.PassStats().film_canvas_sample, 2U);

      std::vector<float> edge;
      double             interior = 0.0;
      for (std::uint32_t y = 0; y < kRoiH; ++y) {
        for (std::uint32_t x = 0; x < kRoiW; ++x) {
          const auto&  got        = detail[static_cast<std::size_t>(y) * kRoiW + x];
          const auto&  want       = full[static_cast<std::size_t>(y + roi_y) * width + x + roi_x];
          const double difference = (std::max)({std::abs(got.r - want.r), std::abs(got.g - want.g),
                                                std::abs(got.b - want.b)}) *
                                    255.0;
          const bool inside =
              x >= kReach && y >= kReach && x < kRoiW - kReach && y < kRoiH - kReach;
          if (inside) {
            interior = (std::max)(interior, difference);
          } else {
            edge.push_back(static_cast<float>(difference));
          }
        }
      }
      std::sort(edge.begin(), edge.end());
      double sum = 0.0;
      for (const float value : edge) sum += value;
      const double p99 =
          edge[static_cast<std::size_t>(0.99 * static_cast<double>(edge.size() - 1))];
      const double p999 =
          edge[static_cast<std::size_t>(0.999 * static_cast<double>(edge.size() - 1))];
      std::cout << film << ", ROI at row " << roi_y << " against the full frame, edge band of "
                << kReach << " pixels: mean " << sum / static_cast<double>(edge.size()) << ", p99 "
                << p99 << ", p99.9 " << p999 << ", largest " << edge.back()
                << " codes; inside the band, largest " << interior << " codes\n";
      // Inside the band only the phase of the half-size inhibitor grid differs. A slide has a
      // steeper display response than a print, so that phase gives it more codes: the plan record
      // of F8 and issue #636 hold the value.
      if (same_cells || !reversal) {
        EXPECT_LE(interior, 0.1);
      }
      // A real outside neighbor can differ a lot at one pixel. The largest value is in the plan
      // record.
      EXPECT_LE(p99, 4.0);
    }
  }
}

TEST_F(CudaFilmPrintFixture, NegativeScanRoiMatchesTheFullFrameInsideTheReachOfItsBlurs) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t           width  = 0;
  std::uint32_t           height = 0;
  const auto              ap1    = ReadLabAp1(width, height);
  // The reach of the blurs of FilmPrintRoiEdgeDiffersOnlyInsideTheReachOfItsBlurs.
  constexpr std::uint32_t kReach = 5 + 2 * 2 * 11;
  constexpr std::uint32_t kRoiW  = 1600;
  constexpr std::uint32_t kRoiH  = 1200;
  const std::uint32_t     roi_x  = (width - kRoiW) / 2;
  const std::uint32_t     odd_y  = (height - kRoiH) / 2;
  ASSERT_EQ(odd_y % 2U, 1U);
  // Linear AP1 before the display transform, with auto colour Neutral: the ROI samples the
  // prescan of the full frame, so the shift is that of the full frame.
  const auto document = NegativeScanDocument("kodak_5207", "neutral");
  plan_.film_scan_linear_output = true;
  SetView({width, height}, {width, height}, Matrix3x3::Identity());
  const auto full       = RenderView(document, ap1, true);
  const auto full_shift = LastAutoShift();
  for (const std::uint32_t roi_y : {odd_y, odd_y - 1U}) {
    const bool same_cells = roi_y % 2U == 0U;
    SCOPED_TRACE(same_cells ? "ROI on the cells of the frame" : "ROI with other cells");
    SetView({width, height}, {kRoiW, kRoiH},
            Matrix3x3::Translate(static_cast<float>(roi_x), static_cast<float>(roi_y)));
    const auto detail =
        RenderView(document, CropField(ap1, width, roi_x, roi_y, kRoiW, kRoiH), false);
    // The Halation canvas and the prescan.
    EXPECT_EQ(device_.PassStats().film_canvas_sample, 2U);
    EXPECT_EQ(LastAutoShift(), full_shift);
    double interior = 0.0;
    for (std::uint32_t y = kReach; y < kRoiH - kReach; ++y) {
      for (std::uint32_t x = kReach; x < kRoiW - kReach; ++x) {
        interior = (std::max)(
            interior, film_print_test::StopsDifference(
                          film_print_test::Rgb(detail[static_cast<std::size_t>(y) * kRoiW + x]),
                          film_print_test::Rgb(
                              full[static_cast<std::size_t>(y + roi_y) * width + x + roi_x])));
      }
    }
    std::cout << "Negative Scan, ROI at row " << roi_y
              << " against the full frame, inside the band of " << kReach << " pixels: largest "
              << interior << " stops\n";
    // At the odd row the phase of the half-size inhibitor grid differs too (issue #636); the limit
    // of issue 664 holds at both rows.
    EXPECT_LE(interior, 0.002);
  }
  plan_.film_scan_linear_output = false;
}

/// One array of the lab, box-reduced to @p width x @p height.
auto BoxReduced(const std::vector<Rgba>& image, std::uint32_t image_width,
                std::uint32_t image_height, std::uint32_t width, std::uint32_t height)
    -> std::vector<Rgba> {
  std::vector<Rgba> out(static_cast<std::size_t>(width) * height);
  const auto        cell = [](std::uint32_t index, std::uint32_t full, std::uint32_t count) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(index) * full / count);
  };
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto y0 = cell(y, image_height, height);
    const auto y1 = (std::max)(y0 + 1, cell(y + 1, image_height, height));
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto x0 = cell(x, image_width, width);
      const auto x1 = (std::max)(x0 + 1, cell(x + 1, image_width, width));
      Rgba       sum{0.0f, 0.0f, 0.0f, 1.0f};
      for (std::uint32_t sy = y0; sy < y1; ++sy) {
        for (std::uint32_t sx = x0; sx < x1; ++sx) {
          const auto& pixel = image[static_cast<std::size_t>(sy) * image_width + sx];
          sum.r += pixel.r;
          sum.g += pixel.g;
          sum.b += pixel.b;
        }
      }
      const float scale = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
      out[static_cast<std::size_t>(y) * width + x] = {sum.r * scale, sum.g * scale, sum.b * scale,
                                                      1.0f};
    }
  }
  return out;
}

/// The DRT pass of each method at the sizes of the editor, on a Release build: GPU time and memory.
/// Run it with --gtest_also_run_disabled_tests.
TEST_F(CudaFilmPrintFixture, DISABLED_DrtCostRecordAtEditorSizes) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t      reference_width  = 0;
  std::uint32_t      reference_height = 0;
  const auto         ap1              = ReadLabAp1(reference_width, reference_height);
  const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
  constexpr int      kRuns  = 9;
  const auto         median = [](std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  for (const std::uint32_t width : {2560U, 4096U, 6000U}) {
    const auto height = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(reference_height) * width / static_cast<double>(reference_width)));
    const auto input  = BoxReduced(ap1, reference_width, reference_height, width, height);
    // A render reads a scene that earlier GPU passes wrote, so each render here copies the scene on
    // the GPU. A host upload just before the clock adds about 30 ms at 24 MP to any method.
    void*      source = nullptr;
    ASSERT_EQ(::cudaMalloc(&source, input.size() * sizeof(Rgba)), cudaSuccess);
    ASSERT_EQ(
        ::cudaMemcpy(source, input.data(), input.size() * sizeof(Rgba), cudaMemcpyHostToDevice),
        cudaSuccess);
    SetView({reference_width, reference_height}, {width, height},
            Matrix3x3::Scale(static_cast<float>(reference_width) / static_cast<float>(width),
                             static_cast<float>(reference_height) / static_cast<float>(height)));
    // A render holds published results at its own size, so the pool keeps its images of that size.
    auto render_size_result =
        device_.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
    /// GPU time of the DRT pass, and the wall time from its start to the end of its GPU work.
    const auto render = [&](const PipelineDocument& document, double& gpu_ms, double& wall_ms) {
      device_.ResetPassStats();
      device_.BeginRender();
      auto& workspace = device_.Workspace();
      workspace.PrepareResultValidity(plan_, document, input_);
      // A DRT/Post write between the display transform and the display output uses a scene work
      // image, as after the Color Grade of a full render.
      workspace.EnsureSceneWorkImages({width, height});
      auto& scene =
          workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
      auto stream = device_.CommandContext().Stream();
      ASSERT_EQ(::cudaMemcpyAsync(scene.Texture().DevicePointer(), source,
                                  input.size() * sizeof(Rgba), cudaMemcpyDeviceToDevice, stream),
                cudaSuccess);
      cudaEvent_t start = nullptr;
      cudaEvent_t stop  = nullptr;
      ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
      ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);
      ASSERT_EQ(::cudaEventRecord(start, stream), cudaSuccess);
      const auto wall_start = std::chrono::steady_clock::now();
      (void)ExecuteCudaDrt(device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
      ASSERT_EQ(::cudaEventRecord(stop, stream), cudaSuccess);
      ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);
      const std::chrono::duration<double, std::milli> wall =
          std::chrono::steady_clock::now() - wall_start;
      float elapsed = 0.0f;
      ASSERT_EQ(::cudaEventElapsedTime(&elapsed, start, stop), cudaSuccess);
      (void)::cudaEventDestroy(start);
      (void)::cudaEventDestroy(stop);
      gpu_ms  = elapsed;
      wall_ms = wall.count();
      device_.EndRender();
      device_.PublishResults();
    };
    /// With @p change_slider, each run changes Halation; with @p change_scan_contrast, the Scan
    /// Contrast of Negative Scan, a rendering setting that reads the kept prescan.
    const auto record = [&](const char* title, PipelineDocument document, bool change_slider,
                            bool change_scan_contrast = false) {
      std::vector<double> gpu;
      std::vector<double> wall;
      float               halation   = 1.0f;
      float               contrast   = 1.0f;
      double              first_gpu  = 0.0;
      double              first_wall = 0.0;
      render(document, first_gpu, first_wall);
      for (int run = 0; run < kRuns; ++run) {
        DrtParameterUpdate update;
        if (change_slider) {
          halation             = halation >= 7.0f ? 1.0f : halation + 0.5f;
          update.film_halation = halation;
        }
        if (change_scan_contrast) {
          contrast                  = contrast >= 1.4f ? 0.8f : contrast + 0.05f;
          update.film_scan_contrast = contrast;
        }
        document.Drt()->Params().ApplyUpdate(update);
        double gpu_ms  = 0.0;
        double wall_ms = 0.0;
        render(document, gpu_ms, wall_ms);
        gpu.push_back(gpu_ms);
        wall.push_back(wall_ms);
      }
      std::cout << width << " x " << height << ", " << title << ": GPU " << median(gpu)
                << " ms, wall " << median(wall) << " ms (first render: GPU " << first_gpu
                << " ms, wall " << first_wall << " ms)\n";
    };
    const auto method_document = [](DrtMethod method) {
      auto               document = CreateDefaultPipelineDocument();
      DrtParameterUpdate update;
      update.method         = method;
      update.encoding_space = DrtColorSpace::Rec709;
      update.encoding_eotf  = DrtEotf::SrgbPiecewise;
      document.Drt()->Params().ApplyUpdate(update);
      return document;
    };
    record("ACES 2.0", method_document(DrtMethod::Aces20), false);
    record("OpenDRT", method_document(DrtMethod::OpenDrt), false);
    // The DRT/Post Film Grain at 100 % next to OpenDRT alone: the difference is the time of the
    // grain (plan 'film_grain_format_and_stock_plan.md', phase G3).
    const auto grain_document = [&method_document](FilmFormat format) {
      auto document = method_document(DrtMethod::OpenDrt);
      document.Drt()->Params().ApplyFilmFormat(format);
      auto* grain =
          dynamic_cast<FilmGrainModel*>(document.Drt()->FindAdjustmentByType(type_ids::FilmGrain()));
      if (grain == nullptr) throw std::runtime_error("The document has no Film Grain");
      grain->SetValue(1.0f);
      return document;
    };
    // This plan has no compiled DRT/Post adjustments, so the grain records add those of the
    // document, as GraphCompiler lists them, and the records after them run without them again.
    {
      const auto  document = grain_document(FilmFormat::Film35mm);
      const auto& drt      = *document.Drt();
      for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
        plan_.drt.post_adjustments.push_back({drt.AdjustmentIdAt(index),
                                              drt.AdjustmentAt(index).Type(),
                                              CompiledAdjustmentAlgorithm::Neighborhood});
      }
    }
    record("OpenDRT, Film Grain 100 %, 35mm", grain_document(FilmFormat::Film35mm), false);
    record("OpenDRT, Film Grain 100 %, 16mm", grain_document(FilmFormat::Film16mm), false);
    record("OpenDRT, Film Grain 0 %, post adjustments compiled",
           method_document(DrtMethod::OpenDrt), false);
    plan_.drt.post_adjustments.clear();
    record("film print, results valid", FilmPrintDocument(), false);
    record("film print, slider change", FilmPrintDocument(), true);
    // A stock with only the point part of the inhibition, and a stock with both parts.
    record("film print, slider change, fuji_eterna_500", FilmPrintDocument("fuji_eterna_500"), true);
    record("film print, slider change, kodak_ultramax_400", FilmPrintDocument("kodak_ultramax_400"),
           true);
    // The light table form, next to the print form: a film with both parts of the inhibition, and a
    // film with no inhibition.
    record("light table, slider change, fuji_velvia_50", FilmPrintDocument("fuji_velvia_50"), true);
    record("light table, slider change, kodak_ektachrome_e100",
           FilmPrintDocument("kodak_ektachrome_e100"), true);
    // Negative Scan next to the print form of the same negative: auto colour off reads no pixel;
    // Neutral builds the prescan after a Halation change and reads the kept prescan after a change
    // of a rendering setting.
    record("Negative Scan, auto off, slider change", NegativeScanDocument(), true);
    record("Negative Scan, auto Neutral, slider change", NegativeScanDocument("kodak_5207", "neutral"),
           true);
    record("Negative Scan, auto off, Scan Contrast change", NegativeScanDocument(), false, true);
    record("Negative Scan, auto Neutral, Scan Contrast change",
           NegativeScanDocument("kodak_5207", "neutral"), false, true);

    // The passes of the film print and of Negative Scan with auto colour Neutral, after a slider
    // change.
    auto& workspace = device_.Workspace();
    for (int form = 0; form < 2; ++form) {
      auto document =
          form == 0 ? FilmPrintDocument() : NegativeScanDocument("kodak_5207", "neutral");
      double unused_gpu  = 0.0;
      double unused_wall = 0.0;
      render(document, unused_gpu, unused_wall);
      DrtParameterUpdate update;
      update.film_halation = 2.0f;
      document.Drt()->Params().ApplyUpdate(update);
      const auto                     display_id = plan_.display_output;
      std::vector<FilmPrintPassTime> times;
      device_.BeginRender();
      workspace.PrepareResultValidity(plan_, document, input_);
      auto& scene =
          workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
      ASSERT_EQ(
          ::cudaMemcpyAsync(scene.Texture().DevicePointer(), source, input.size() * sizeof(Rgba),
                            cudaMemcpyDeviceToDevice, device_.CommandContext().Stream()),
          cudaSuccess);
      (void)workspace.AcquireImageForWrite(display_id, {width, height, TextureFormat::Rgba32f});
      RecordCudaFilmPrint(device_, plan_, *document.Drt(), std::nullopt,
                          FrameSceneBinding::CachedImage(scene_id),
                          FrameSceneBinding::CachedImage(display_id), width, height, &times);
      device_.EndRender();
      device_.PublishResults();
      std::cout << "  passes of " << document.Drt()->Params().FilmPrint().print_material << ":\n";
      for (const auto& time : times) {
        std::cout << "  " << time.name << ": " << time.milliseconds << " ms\n";
      }
    }
    ASSERT_EQ(::cudaFree(source), cudaSuccess);
    std::cout << "  texture pool peak "
              << workspace.CaptureResourceSnapshot().texture_peak_used_bytes / (1 << 20)
              << " MiB\n";
  }
}

#endif  // ALCEDO_FILM_PRINT_EXPECTED_DIR

}  // namespace
}  // namespace alcedo
