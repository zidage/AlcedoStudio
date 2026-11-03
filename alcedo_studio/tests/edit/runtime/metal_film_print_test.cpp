//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/film/film_array_file.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_drt_pass.hpp"
#include "edit/runtime/metal/metal_film_print_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
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

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
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

class MetalFilmPrintFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device: unverified support, not a pass.";
    }
    if (!fs::exists(FilmPrintResourceDirectory() / "manifest.json")) {
      GTEST_SKIP() << "No film print resources at " << FilmPrintResourceDirectory().string()
                   << ". Configure with ALCEDO_FILM_PRINT_RESOURCE_DIR. This is not a pass.";
    }
    device_ = std::make_unique<MetalRenderDevice>();
  }

  /// Run only the DRT pass on @p ap1, with a scene image that holds these values with no change.
  auto RenderDrt(const PipelineDocument& document, std::span<const Rgba> ap1) -> std::vector<Rgba> {
    const auto width               = static_cast<std::uint32_t>(ap1.size());
    auto&      geometry            = plan_.geometry;
    geometry.full_reference_extent = {width, 1};
    geometry.edit_extent           = {width, 1};
    geometry.render_extent         = {width, 1};
    device_->BeginRender();
    auto& workspace = device_->Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, 1, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(ap1),
                                       device_->CommandContext());
    const auto result =
        ExecuteMetalDrt(*device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
    std::vector<Rgba> pixels(width);
    auto*             display = workspace.Images().Find(result.output);
    EXPECT_NE(display, nullptr);
    workspace.Device().DownloadTexture2D(
        display->Texture(), std::as_writable_bytes(std::span(pixels)), device_->CommandContext());
    device_->CancelRender();
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
   * @p publish ends the render and publishes its results; otherwise the render is cancelled.
   */
  auto RenderView(const PipelineDocument& document, std::span<const Rgba> ap1, bool publish)
      -> std::vector<Rgba> {
    const auto width  = plan_.geometry.render_extent.width;
    const auto height = plan_.geometry.render_extent.height;
    device_->ResetPassStats();
    device_->BeginRender();
    auto& workspace = device_->Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto& lease = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(ap1),
                                       device_->CommandContext());
    const auto result =
        ExecuteMetalDrt(*device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
    std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
    auto*             display = workspace.Images().Find(result.output);
    EXPECT_NE(display, nullptr);
    workspace.Device().DownloadTexture2D(
        display->Texture(), std::as_writable_bytes(std::span(pixels)), device_->CommandContext());
    if (publish) {
      device_->EndRender();
      device_->WaitIdle();
      device_->PublishResults();
    } else {
      device_->CancelRender();
    }
    return pixels;
  }

  ExecutionPlan                      plan_;
  PreparedRawInput                   input_;
  std::unique_ptr<MetalRenderDevice> device_;
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

TEST_F(MetalFilmPrintFixture, FilmPrintOutputIsFinite) {
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

TEST_F(MetalFilmPrintFixture, FilmPrintRendersThroughTheCompiledPlanAndUploadsTablesOnce) {
  auto document = FilmPrintDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                   gpu_dag_test::FullSensor(64, 48));
  const auto plan  = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  const auto id    = device_->Execute(plan, input, document);
  device_->WaitIdle();
  auto* display = device_->Workspace().Images().Find(id);
  ASSERT_NE(display, nullptr);
  std::vector<Rgba> pixels(64 * 48);
  device_->Workspace().Device().DownloadTexture2D(
      display->Texture(), std::as_writable_bytes(std::span(pixels)), device_->CommandContext());
  for (const auto& pixel : pixels) {
    ASSERT_TRUE(std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b));
  }

  // This frame has an image plane, so the compiled plan published both canonical results.
  auto& images = device_->Workspace().Images();
  EXPECT_NE(images.PublishedLastWriter(FilmHalationResultId(plan.drt.node_id)), 0U);
  EXPECT_NE(images.PublishedLastWriter(FilmFlareResultId(plan.drt.node_id)), 0U);

  // A new encoding renders the DRT again with the tables that the device holds.
  const auto uploaded = device_->Workspace().Device().LutUploadBytes();
  EXPECT_GT(uploaded, 0U);
  DrtParameterUpdate update;
  update.encoding_eotf = DrtEotf::Gamma22;
  document.Drt()->Params().ApplyUpdate(update);
  device_->ResetPassStats();
  (void)device_->Execute(GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{}),
                         input, document);
  device_->WaitIdle();
  EXPECT_EQ(device_->PassStats().drt_execute, 1U);
  EXPECT_EQ(device_->Workspace().Device().LutUploadBytes(), uploaded);
}

TEST_F(MetalFilmPrintFixture, FilmPrintKeepsTheAlphaOfTheScene) {
  // The images between the steps have no alpha, so the display reads it from the scene.
  constexpr std::uint32_t kWidth  = 64;
  constexpr std::uint32_t kHeight = 48;
  const auto              field   = PatchField(kWidth, kHeight, 20, 20, 8);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  for (const auto& pixel : RenderView(FilmPrintDocument(), field, false)) {
    ASSERT_EQ(pixel.a, 0.75f);
  }
}

TEST_F(MetalFilmPrintFixture, FilmPrintReadsASceneWorkImageAndWritesAWorkImage) {
  constexpr std::uint32_t kWidth  = 64;
  constexpr std::uint32_t kHeight = 48;
  const auto              field   = PatchField(kWidth, kHeight, 20, 20, 8);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  const auto document = FilmPrintDocument();
  const auto images   = RenderView(document, field, false);

  device_->BeginRender();
  auto& workspace = device_->Workspace();
  workspace.PrepareResultValidity(plan_, document, input_);
  workspace.EnsureSceneWorkImages({kWidth, kHeight});
  auto& backend = workspace.Device();
  auto& source  = workspace.SceneWork().Member(SceneWorkMember::Member0);
  auto& output  = workspace.SceneWork().Member(SceneWorkMember::Member1);
  backend.UploadTexture2D(source, std::as_bytes(std::span(field)), device_->CommandContext());
  RecordMetalFilmPrint(*device_, plan_, *document.Drt(),
                       FrameSceneBinding::WorkImage(SceneWorkMember::Member0),
                       FrameSceneBinding::WorkImage(SceneWorkMember::Member1), kWidth, kHeight);
  std::vector<Rgba> work(field.size());
  backend.DownloadTexture2D(output, std::as_writable_bytes(std::span(work)),
                            device_->CommandContext());
  device_->CancelRender();
  EXPECT_LE(LargestCodeDifference(work, work.size(),
                                  [&images](std::size_t index) { return images[index]; }),
            0.0);
  EXPECT_EQ(work[0].a, 0.75f);
}

TEST_F(MetalFilmPrintFixture, FilmPrintLargerRenderDoesNotKeepTheSlabsOfASmallerRender) {
  const auto         document = FilmPrintDocument("kodak_portra_400");
  const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
  // BufferCreateCount of Metal also counts the shared staging pages of an upload and a download,
  // so this form counts only the buffers that the DRT pass creates.
  std::uint64_t      drt_buffers = 0;
  const auto         render      = [&](std::uint32_t width, std::uint32_t height) {
    SetView({width, height}, {width, height}, Matrix3x3::Identity());
    const auto field = PatchField(width, height, 40, 40, 8);
    device_->BeginRender();
    auto& workspace = device_->Workspace();
    auto& backend   = workspace.Device();
    workspace.PrepareResultValidity(plan_, document, input_);
    auto& scene = workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
    backend.UploadTexture2D(scene.Texture(), std::as_bytes(std::span(field)),
                                         device_->CommandContext());
    const auto before = backend.BufferCreateCount();
    (void)ExecuteMetalDrt(*device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
    drt_buffers += backend.BufferCreateCount() - before;
    device_->EndRender();
    device_->WaitIdle();
    device_->PublishResults();
    return workspace.TransientBuffers().capacity_bytes();
  };
  (void)render(2048, 1536);
  const auto grown = render(4096, 3072);

  // A second device has only the larger render, so it holds what that render needs.
  device_.reset();
  device_           = std::make_unique<MetalRenderDevice>();
  const auto needed = render(4096, 3072);
  EXPECT_EQ(grown, needed);

  // The same size allocates no new slab, and a smaller size uses the slabs that it has.
  drt_buffers = 0;
  EXPECT_EQ(render(4096, 3072), needed);
  EXPECT_EQ(render(2048, 1536), needed);
  EXPECT_EQ(drt_buffers, 0U);
}

TEST_F(MetalFilmPrintFixture, FilmPrintUploadFailureLeavesTheDisplayResultInvalid) {
  auto document = FilmPrintDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                   gpu_dag_test::FullSensor(64, 48));
  (void)device_->Execute(GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{}),
                         input, document);
  device_->WaitIdle();

  // The earlier passes hold published results, so a new selection first uploads its print table.
  DrtParameterUpdate update;
  update.film_scene_light = "tungsten";
  document.Drt()->Params().ApplyUpdate(update);
  const auto plan   = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  auto&      images = device_->Workspace().Images();
  const auto writer = images.PublishedLastWriter(plan.display_output);
  device_->Workspace().Device().FailNextUpload();
  EXPECT_THROW((void)device_->Execute(plan, input, document), std::runtime_error);
  device_->WaitIdle();
  // The failed render publishes nothing, so the display result of this selection stays invalid.
  EXPECT_EQ(images.PublishedLastWriter(plan.display_output), writer);
  EXPECT_FALSE(device_->Workspace().IsRendering());

  // The same device runs the DRT pass again once the upload works.
  device_->ResetPassStats();
  (void)device_->Execute(plan, input, document);
  device_->WaitIdle();
  EXPECT_EQ(device_->PassStats().drt_execute, 1U);
  EXPECT_NE(images.PublishedLastWriter(plan.display_output), writer);
}

TEST_F(MetalFilmPrintFixture, FilmPrintViewportKeepsCanvas) {
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
  EXPECT_EQ(device_->PassStats().film_canvas_rebuild, 2U);
  EXPECT_EQ(device_->PassStats().film_canvas_sample, 0U);
  auto&      images          = device_->Workspace().Images();
  const auto halation_writer = images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id));
  const auto flare_writer    = images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id));
  EXPECT_NE(halation_writer, 0U);
  EXPECT_NE(flare_writer, 0U);

  const auto roi = CropField(field, kWidth, kRoiX, kRoiY, kRoiSize, kRoiSize);
  SetView({kWidth, kHeight}, {kRoiSize, kRoiSize},
          Matrix3x3::Translate(static_cast<float>(kRoiX), static_cast<float>(kRoiY)));
  const auto detail = RenderView(document, roi, true);
  EXPECT_EQ(device_->PassStats().film_canvas_sample, 2U);
  EXPECT_EQ(device_->PassStats().film_canvas_rebuild, 0U);
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
  EXPECT_EQ(device_->PassStats().film_canvas_rebuild, 0U);
  const auto index = static_cast<std::size_t>(98 - kRoiY) * kRoiSize;
  EXPECT_GT(std::abs(detail[index].r - plain[index].r), 1.0f / 255.0f);
}

TEST_F(MetalFilmPrintFixture, FilmPrintRoiWithNoPublishedResultPublishesNone) {
  const auto field    = PatchField(64, 64, 20, 20, 4);
  const auto document = FilmPrintDocument();
  SetView({128, 96}, {64, 64}, Matrix3x3::Translate(32.0f, 16.0f));
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_->PassStats().film_canvas_rebuild, 2U);
  auto& images = device_->Workspace().Images();
  EXPECT_EQ(images.PublishedLastWriter(FilmHalationResultId(plan_.drt.node_id)), 0U);
  EXPECT_EQ(images.PublishedLastWriter(FilmFlareResultId(plan_.drt.node_id)), 0U);
  (void)RenderView(document, field, true);
  EXPECT_EQ(device_->PassStats().film_canvas_rebuild, 2U)
      << "an image from an ROI must not become the full-frame image";
  EXPECT_EQ(device_->PassStats().film_canvas_sample, 0U);
}

TEST_F(MetalFilmPrintFixture, FilmPrintSliderBuildsNoTable) {
  // At this size the spread of slider 2.5 has a sigma of about 0.5 pixels, so it has taps.
  const auto field    = PatchField(1024, 768, 400, 400, 8);
  auto       document = FilmPrintDocument();
  SetView({1024, 768}, {1024, 768}, Matrix3x3::Identity());
  auto       previous = RenderView(document, field, true);
  auto&      tables   = SharedFilmPrintTables();
  const auto stats    = tables.Stats();
  const auto uploaded = device_->Workspace().Device().LutUploadBytes();

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
  EXPECT_EQ(device_->Workspace().Device().LutUploadBytes(), uploaded);
}

TEST_F(MetalFilmPrintFixture, FilmPrintFusedSpreadMatchesSeparatePasses) {
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
    device_->BeginRender();
    auto& workspace = device_->Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    auto& scene =
        workspace.AcquireImageForWrite(scene_id, {kWidth, kHeight, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(scene.Texture(), std::as_bytes(std::span(field)),
                                               device_->CommandContext());
    auto& display = workspace.AcquireImageForWrite(plan_.display_output,
                                                           {kWidth, kHeight, TextureFormat::Rgba32f});
    RecordMetalFilmPrint(*device_, plan_, *document.Drt(), FrameSceneBinding::CachedImage(scene_id),
                                 FrameSceneBinding::CachedImage(plan_.display_output), kWidth, kHeight,
                                 nullptr, separate);
    std::vector<Rgba> pixels(static_cast<std::size_t>(kWidth) * kHeight);
    workspace.Device().DownloadTexture2D(
        display.Texture(), std::as_writable_bytes(std::span(pixels)), device_->CommandContext());
    device_->CancelRender();
    return pixels;
  };
  const auto fused    = render(false);
  const auto separate = render(true);
  const auto largest  = LargestCodeDifference(
      fused, fused.size(), [&separate](std::size_t index) { return separate[index]; });
  std::cout << "fused spread against separate passes: largest " << largest << " codes\n";
  EXPECT_LE(largest, 0.01);
}

TEST_F(MetalFilmPrintFixture, FilmPrintHalfSizeCellsStayOnTheirPixelsAtAnOddSize) {
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
TEST_F(MetalFilmPrintFixture, FilmPrintStepEdgeFollowsTheImagePartOfTheInhibition) {
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

TEST_F(MetalFilmPrintFixture, FilmPrintWithNoImagePartRunsNoInhibitorBlur) {
  constexpr std::uint32_t kWidth  = 6000;
  constexpr std::uint32_t kHeight = 4000;
  const auto              field   = NeutralStepEdge(kWidth, kHeight);
  SetView({kWidth, kHeight}, {kWidth, kHeight}, Matrix3x3::Identity());
  const auto runs_inhibitor_blur = [&](const char* negative) {
    const auto document = FilmPrintDocument(negative);
    device_->BeginRender();
    auto& workspace = device_->Workspace();
    workspace.PrepareResultValidity(plan_, document, input_);
    const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
    auto&              scene =
        workspace.AcquireImageForWrite(scene_id, {kWidth, kHeight, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(scene.Texture(), std::as_bytes(std::span(field)),
                                       device_->CommandContext());
    const auto display_id = plan_.display_output;
    (void)workspace.AcquireImageForWrite(display_id, {kWidth, kHeight, TextureFormat::Rgba32f});
    std::vector<std::string> names;
    RecordMetalFilmPrint(*device_, plan_, *document.Drt(), FrameSceneBinding::CachedImage(scene_id),
                         FrameSceneBinding::CachedImage(display_id), kWidth, kHeight, &names);
    device_->CancelRender();
    return std::any_of(names.begin(), names.end(), [](const std::string& name) {
      return name.find("inhibitor") != std::string::npos;
    });
  };
  EXPECT_FALSE(runs_inhibitor_blur("fuji_eterna_500"));
  EXPECT_TRUE(runs_inhibitor_blur("kodak_portra_400"));
}

TEST_F(MetalFilmPrintFixture,
       FilmPrintAllocationFailureReportsTheErrorAndPublishesNoPartialResult) {
  const auto field    = PatchField(128, 96, 40, 40, 8);
  const auto document = FilmPrintDocument();
  SetView({128, 96}, {128, 96}, Matrix3x3::Identity());
  auto&      images   = device_->Workspace().Images();
  const auto halation = FilmHalationResultId(plan_.drt.node_id);
  const auto flare    = FilmFlareResultId(plan_.drt.node_id);
  // Each try allows one more new allocation than the last try, so the failure moves through the
  // allocations of the render, up to the last one of the film print.
  int        failures = 0;
  for (; failures < 64; ++failures) {
    device_->Workspace().Device().FailTextureAllocationAfter(static_cast<std::uint32_t>(failures));
    try {
      (void)RenderView(document, field, true);
      break;
    } catch (const std::runtime_error& error) {
      EXPECT_FALSE(std::string(error.what()).empty());
      device_->CancelRender();
      device_->WaitIdle();
      EXPECT_EQ(images.PublishedLastWriter(halation), 0U);
      EXPECT_EQ(images.PublishedLastWriter(flare), 0U);
    }
  }
  std::cout << "allocation failures before the first complete render: " << failures << "\n";
  ASSERT_LT(failures, 64);
  // The scene, the display, and the two canonical results each had a failure. The images between
  // the steps are transient buffers.
  EXPECT_GE(failures, 4);
  device_->Workspace().Device().FailTextureAllocationAfter(1000000);
  EXPECT_NE(images.PublishedLastWriter(halation), 0U);
  EXPECT_NE(images.PublishedLastWriter(flare), 0U);
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

TEST_F(MetalFilmPrintFixture, FilmPrintRampMatchesLab) {
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

TEST_F(MetalFilmPrintFixture, FilmPrintChartMatchesReference) {
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

TEST_F(MetalFilmPrintFixture, FilmPrintImageMatchesLab) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
  const auto    ap1    = ReadLabAp1(width, height);
  SetView({width, height}, {width, height}, Matrix3x3::Identity());
  int compared = 0;
  for (const auto& entry : fs::directory_iterator(ImageExpectedDirectory())) {
    if (entry.path().filename() == "ap1.bin") continue;
    auto       file     = FilmArrayFile::Parse(ReadFilmResourceBytes(entry.path()), entry.path());
    const auto metadata = file.Metadata();
    const auto negative = metadata.at("negative").get<std::string>();
    SCOPED_TRACE(negative);
    const auto pixels =
        RenderView(FilmPrintDocument(negative, metadata.at("print_material").get<std::string>(),
                                     metadata.at("scene_light").get<std::string>()),
                   ap1, false);
    const auto difference = DifferenceFromLab(
        pixels,
        file.F32("srgb", {static_cast<std::int64_t>(height), static_cast<std::int64_t>(width), 3}));
    std::cout << "image " << negative << " (" << width << " x " << height << "): mean "
              << difference.mean << ", p99.9 " << difference.p999 << ", largest "
              << difference.largest << " codes\n";
    EXPECT_LE(difference.p999, 1.0);
    EXPECT_LE(difference.largest, 4.0);
    ++compared;
  }
  EXPECT_EQ(compared, 5);
}

TEST_F(MetalFilmPrintFixture, FilmPrintRoiEdgeDiffersOnlyInsideTheReachOfItsBlurs) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t           width    = 0;
  std::uint32_t           height   = 0;
  const auto              ap1      = ReadLabAp1(width, height);
  // The light spread at slider 1 reaches 5 pixels, and each of the 2 inhibitor blurs 2 * 11 pixels.
  constexpr std::uint32_t kReach   = 5 + 2 * 2 * 11;
  constexpr std::uint32_t kRoiW    = 1600;
  constexpr std::uint32_t kRoiH    = 1200;
  const std::uint32_t     roi_x    = (width - kRoiW) / 2;
  const std::uint32_t     roi_y    = (height - kRoiH) / 2;
  const auto              document = FilmPrintDocument();

  SetView({width, height}, {width, height}, Matrix3x3::Identity());
  const auto full = RenderView(document, ap1, true);
  const auto crop = CropField(ap1, width, roi_x, roi_y, kRoiW, kRoiH);
  SetView({width, height}, {kRoiW, kRoiH},
          Matrix3x3::Translate(static_cast<float>(roi_x), static_cast<float>(roi_y)));
  const auto detail = RenderView(document, crop, false);
  EXPECT_EQ(device_->PassStats().film_canvas_sample, 2U);

  std::vector<float> edge;
  double             interior = 0.0;
  for (std::uint32_t y = 0; y < kRoiH; ++y) {
    for (std::uint32_t x = 0; x < kRoiW; ++x) {
      const auto&  got        = detail[static_cast<std::size_t>(y) * kRoiW + x];
      const auto&  want       = full[static_cast<std::size_t>(y + roi_y) * width + x + roi_x];
      const double difference = (std::max)({std::abs(got.r - want.r), std::abs(got.g - want.g),
                                            std::abs(got.b - want.b)}) *
                                255.0;
      const bool inside = x >= kReach && y >= kReach && x < kRoiW - kReach && y < kRoiH - kReach;
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
  const double p99  = edge[static_cast<std::size_t>(0.99 * static_cast<double>(edge.size() - 1))];
  const double p999 = edge[static_cast<std::size_t>(0.999 * static_cast<double>(edge.size() - 1))];
  std::cout << "ROI against the full frame, edge band of " << kReach << " pixels: mean "
            << sum / static_cast<double>(edge.size()) << ", p99 " << p99 << ", p99.9 " << p999
            << ", largest " << edge.back() << " codes; inside the band, largest " << interior
            << " codes\n";
  // Inside the band only the phase of the half-size inhibitor grid differs.
  EXPECT_LE(interior, 0.1);
  // A real outside neighbor can differ a lot at one pixel. The largest value is in the plan record.
  EXPECT_LE(p99, 4.0);
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

/// The DRT pass at the sizes of the editor on a Release build, with the wall time to the end of its
/// GPU work: `SynchronizeRecordedWork` commits and waits. Run it with
/// --gtest_also_run_disabled_tests.
TEST_F(MetalFilmPrintFixture, DISABLED_DrtCostRecordAtEditorSizes) {
  if (!fs::exists(ImageExpectedDirectory() / "ap1.bin")) {
    GTEST_SKIP() << "No lab image. Run build_image_expected.py. This is not a pass.";
  }
  std::uint32_t      reference_width  = 0;
  std::uint32_t      reference_height = 0;
  const auto         ap1              = ReadLabAp1(reference_width, reference_height);
  const GraphValueId scene_id{NodeId{"grade.primary"}, PortId{"image"}};
  for (const std::uint32_t width : {2560U, 4096U, 6000U}) {
    const auto height = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(reference_height) * width / static_cast<double>(reference_width)));
    const auto input  = BoxReduced(ap1, reference_width, reference_height, width, height);
    SetView({reference_width, reference_height}, {width, height},
            Matrix3x3::Scale(static_cast<float>(reference_width) / static_cast<float>(width),
                             static_cast<float>(reference_height) / static_cast<float>(height)));
    auto render_size_result =
        device_->Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
    auto                document = FilmPrintDocument();
    float               halation = 1.0f;
    std::vector<double> times;
    for (int run = 0; run < 10; ++run) {
      halation = halation >= 7.0f ? 1.0f : halation + 0.5f;
      DrtParameterUpdate update;
      update.film_halation = halation;
      document.Drt()->Params().ApplyUpdate(update);
      device_->BeginRender();
      auto& workspace = device_->Workspace();
      workspace.PrepareResultValidity(plan_, document, input_);
      auto& scene =
          workspace.AcquireImageForWrite(scene_id, {width, height, TextureFormat::Rgba32f});
      workspace.Device().UploadTexture2D(scene.Texture(), std::as_bytes(std::span(input)),
                                         device_->CommandContext());
      workspace.Device().SynchronizeRecordedWork(device_->CommandContext());
      const auto start = std::chrono::steady_clock::now();
      (void)ExecuteMetalDrt(*device_, plan_, document, FrameSceneBinding::CachedImage(scene_id));
      workspace.Device().SynchronizeRecordedWork(device_->CommandContext());
      const std::chrono::duration<double, std::milli> elapsed =
          std::chrono::steady_clock::now() - start;
      if (run != 0) times.push_back(elapsed.count());
      device_->EndRender();
      device_->WaitIdle();
      device_->PublishResults();
    }
    std::sort(times.begin(), times.end());
    std::cout << width << " x " << height << ", slider change: " << times[times.size() / 2]
              << " ms; transient capacity "
              << device_->Workspace().TransientBuffers().capacity_bytes() / (1 << 20) << " MiB\n";
  }
}

#endif  // ALCEDO_FILM_PRINT_EXPECTED_DIR

}  // namespace
}  // namespace alcedo
