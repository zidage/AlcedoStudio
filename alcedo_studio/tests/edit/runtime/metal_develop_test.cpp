//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "decoded_rgb_test_support.hpp"
#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "decoders/processor/operators/gpu/metal_encode.hpp"
#include "decoders/processor/raw_normalization.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "dng_profile_test_support.hpp"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

class MetalDevelopFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
  }
};

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto Download(MetalRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       tex = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(tex.Width()) * tex.Height());
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto DownloadR32(MetalRenderDevice& device, const MetalBackend::Texture2D& tex)
    -> std::vector<float> {
  std::vector<float> pixels(static_cast<std::size_t>(tex.Width()) * tex.Height());
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(float)),
      device.CommandContext());
  return pixels;
}

auto SetDevelopMethod(PipelineDocument& document, std::string method, bool highlights) -> void {
  auto payload                   = document.Develop()->Params().Params();
  payload.demosaic_method        = std::move(method);
  payload.highlights_reconstruct = highlights;
  document.Develop()->Params().ReplaceParams(std::move(payload));
}

auto MakeOverRangeCfa(const RawCfaPattern& pattern, std::uint32_t width, std::uint32_t height)
    -> HostImagePlane {
  auto  plane = gpu_dag_test::MakeU16CfaPlane(width, height, pattern);
  auto* samples =
      const_cast<std::uint16_t*>(reinterpret_cast<const std::uint16_t*>(plane.bytes.get()));
  for (std::uint32_t i = 0; i < width * height; i += 7) {
    samples[i] = 30000;
  }
  return plane;
}

auto MaxChannel(const std::vector<Rgba>& pixels) -> float {
  float max_value = 0.0f;
  for (const auto& p : pixels) {
    max_value = std::max(max_value, std::max(p.r, std::max(p.g, p.b)));
  }
  return max_value;
}

auto PixelsDiffer(const std::vector<Rgba>& a, const std::vector<Rgba>& b) -> bool {
  if (a.size() != b.size() || a.empty()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i].r - b[i].r) > 1.0e-4f || std::abs(a[i].g - b[i].g) > 1.0e-4f ||
        std::abs(a[i].b - b[i].b) > 1.0e-4f) {
      return true;
    }
  }
  return false;
}

auto CpuLinearize(const PreparedRawInput& input) -> std::vector<float> {
  const auto  w = input.host_extent.width;
  const auto  h = input.host_extent.height;
  const auto* samples = reinterpret_cast<const std::uint16_t*>(input.pixels.bytes.get());
  std::vector<float> out(static_cast<std::size_t>(w) * h);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const int   color = RawColorAt(input.cfa_pattern, static_cast<int>(y), static_cast<int>(x));
      float       pattern_black = 0.0f;
      if (input.linearization.black_tile_width > 0 && input.linearization.black_tile_height > 0) {
        const int tile_y = static_cast<int>(y) % input.linearization.black_tile_height;
        const int tile_x = static_cast<int>(x) % input.linearization.black_tile_width;
        pattern_black =
            input.linearization.pattern_black[tile_y * input.linearization.black_tile_width + tile_x];
      }
      const float black = input.linearization.black_level[color] + pattern_black;
      float       value =
          raw_norm::NormalizeSample(static_cast<float>(samples[y * w + x]), black,
                                    input.linearization.white_level[color]);
      value *= raw_norm::RelativeWhiteBalanceMultiplier(input.linearization.cam_mul, color,
                                                        input.linearization.apply_as_shot_wb != 0);
      out[y * w + x] = value;
    }
  }
  return out;
}

auto AcesccEncode(float value) -> float {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) {
    return kFloor + value;
  }
  if (value < kTransition) {
    return (std::log2(kOffset + value * 0.5f) + kA) / kB;
  }
  return (std::log2(value) + kA) / kB;
}

auto ReadBorder(const std::vector<Rgba>& src, int width, int height, int x, int y, Rgba border)
    -> Rgba {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return border;
  }
  return src[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(x)];
}

auto BilinearSample(const std::vector<Rgba>& src, int width, int height, float sx, float sy,
                    Rgba border) -> Rgba {
  const float px  = sx - 0.5f;
  const float py  = sy - 0.5f;
  const int   x0  = static_cast<int>(std::floor(px));
  const int   y0  = static_cast<int>(std::floor(py));
  const float fx  = px - static_cast<float>(x0);
  const float fy  = py - static_cast<float>(y0);
  const auto  p00 = ReadBorder(src, width, height, x0, y0, border);
  const auto  p10 = ReadBorder(src, width, height, x0 + 1, y0, border);
  const auto  p01 = ReadBorder(src, width, height, x0, y0 + 1, border);
  const auto  p11 = ReadBorder(src, width, height, x0 + 1, y0 + 1, border);
  const float w00 = (1.0f - fx) * (1.0f - fy);
  const float w10 = fx * (1.0f - fy);
  const float w01 = (1.0f - fx) * fy;
  const float w11 = fx * fy;
  Rgba        out;
  out.r = w00 * p00.r + w10 * p10.r + w01 * p01.r + w11 * p11.r;
  out.g = w00 * p00.g + w10 * p10.g + w01 * p01.g + w11 * p11.g;
  out.b = w00 * p00.b + w10 * p10.b + w01 * p01.b + w11 * p11.b;
  out.a = w00 * p00.a + w10 * p10.a + w01 * p01.a + w11 * p11.a;
  return out;
}

auto AsBytes(const std::vector<Rgba>& pixels) -> std::span<const std::byte> {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(pixels.data()),
                                    pixels.size() * sizeof(Rgba));
}

auto MakeSrcImage(std::uint32_t width, std::uint32_t height) -> std::vector<Rgba> {
  std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      auto& p = pixels[static_cast<std::size_t>(y) * width + x];
      p.r     = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
      p.g     = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
      p.b     = 0.25f;
      p.a     = 1.0f;
    }
  }
  return pixels;
}

}  // namespace

TEST_F(MetalDevelopFixture, CanonDngProfileRendersAtFullResolutionAndInvalidatesOnlyColorCache) {
  gpu_dag_test::VerifyCanonDngProfile<MetalRenderDevice>("metal");
}

TEST_F(MetalDevelopFixture, UnpackedRgbLevelsAndAppliedWhiteBalanceProduceEquivalentFullRenders) {
  gpu_dag_test::VerifyRgbWhiteBalanceAndLevels<MetalRenderDevice>();
}

TEST_F(MetalDevelopFixture, RgbDngWarpProducesFinalSensorImageAndReusesPublishedCache) {
  gpu_dag_test::VerifyRgbWarpPublishes<MetalRenderDevice>();
}

TEST_F(MetalDevelopFixture, LegacyRgbEntryNormalizesAndRemovesAppliedWhiteBalanceOnGpu) {
  gpu_dag_test::VerifyLegacyRgbGpu(RawGpuBackend::Metal);
}

TEST_F(MetalDevelopFixture, SonyYcbcrRgbRendersWithImportedCameraProfileAtFullResolution) {
  gpu_dag_test::VerifyCameraRgbFile<MetalRenderDevice>("DSC04739.ARW", ImageType::ARW, "metal");
}

TEST_F(MetalDevelopFixture, ConvertedLinearDngRendersWarpAndPublishesCacheOutputAtFullResolution) {
  gpu_dag_test::VerifyCameraRgbFile<MetalRenderDevice>("DSC04739_dng.dng", ImageType::DNG, "metal");
}

TEST_F(MetalDevelopFixture, MetalDevelopLinearizeMatchesCudaReferenceWithinTolerance) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(32, 24, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(32, 24), DecodeRes::FULL);
  const auto cpu = CpuLinearize(prepared);

  MetalRenderDevice device;
  auto&             textures = device.Workspace().Textures();
  textures.SetByteBudget(32ull * 24ull * 16ull);
  device.BeginRender();
  auto src = textures.Acquire({32, 24, TextureFormat::R16u});
  auto dst = textures.Acquire({32, 24, TextureFormat::R32f});
  device.Workspace().Device().UploadTexture2D(src.Texture(), prepared.pixels.Span(),
                                              device.CommandContext());
  device.Workspace().Device().EndCommandEncoders(device.CommandContext());
  metal::EncodeToLinearRef(device.CommandContext().NativeCommandBuffer(), src.Texture().Native(),
                           dst.Texture().Native(), prepared.linearization, prepared.cfa_pattern);
  device.EndRender();
  device.WaitIdle();
  const auto gpu = DownloadR32(device, dst.Texture());
  ASSERT_EQ(gpu.size(), cpu.size());
  float max_err = 0.0f;
  for (std::size_t i = 0; i < cpu.size(); ++i) {
    max_err = std::max(max_err, std::fabs(cpu[i] - gpu[i]));
  }
  EXPECT_LT(max_err, 1.0e-5f);
}

TEST_F(MetalDevelopFixture, MetalDevelopRcdOrderMatchesCudaDemosaicThenHighlightRecovery) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      MakeOverRangeCfa(pattern, 64, 64), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto on_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(on_doc, "legacy", true);
  auto off_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(off_doc, "legacy", false);
  const auto on_plan  = GraphCompiler::Compile(on_doc, prepared.CompileSource(), RenderRequest{});
  const auto off_plan = GraphCompiler::Compile(off_doc, prepared.CompileSource(), RenderRequest{});
  EXPECT_LT(on_plan.IndexOf(GpuPassKind::Demosaic), on_plan.IndexOf(GpuPassKind::HighlightRecover));

  auto render = [&](PipelineDocument& document, const ExecutionPlan& plan) {
    MetalRenderDevice device;
    device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
    device.BeginRender();
    ExecuteMetalDevelop(device, plan, prepared, document);
    device.EndRender();
    device.WaitIdle();
    return Download(device, plan.sensor_linear_output);
  };
  const auto on_px  = render(on_doc, on_plan);
  const auto off_px = render(off_doc, off_plan);
  ASSERT_FALSE(on_px.empty());
  EXPECT_GT(MaxChannel(on_px), 1.0f);
  EXPECT_TRUE(PixelsDiffer(on_px, off_px));
}

TEST_F(MetalDevelopFixture, MetalDevelopScratchResourcesReleaseAfterRecordedWorkCompletes) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "legacy", true);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  MetalRenderDevice device;
  auto&             workspace = device.Workspace();
  workspace.Device().ResetCounters();
  device.BeginRender();
  ExecuteMetalDevelop(device, plan, prepared, document);

  const auto scratch_count = workspace.Device().RecordedWorkScratchTextureCount();
  ASSERT_GT(scratch_count, 0U);
  const auto scratch_buffer_count = workspace.Device().RecordedWorkScratchBufferCount();
  ASSERT_GT(scratch_buffer_count, 0U);
  EXPECT_EQ(workspace.Textures().EntryCount(), 1U);
  EXPECT_EQ(workspace.Textures().UsedBytes(),
            static_cast<std::size_t>(plan.source.develop_output_extent.width) *
                plan.source.develop_output_extent.height *
                TextureFormatBytesPerPixel(TextureFormat::Rgba32f));
  const auto free_before = workspace.Device().FreeCount();

  workspace.Device().SynchronizeRecordedWork(device.CommandContext());

  EXPECT_EQ(workspace.Device().RecordedWorkScratchBufferCount(), 0U);
  EXPECT_EQ(workspace.Device().RecordedWorkScratchTextureCount(), 0U);
  EXPECT_EQ(workspace.Device().FreeCount() - free_before, scratch_count + scratch_buffer_count);
  device.EndRender();
  device.WaitIdle();
}

TEST_F(MetalDevelopFixture, MetalPlanExecutorDoesNotReserveDevelopTransientArena) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  SetDevelopMethod(document, "legacy", false);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  MetalRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();

  EXPECT_EQ(device.Workspace().DevelopTransientHighWater().ObservedCapacity(
                plan.source, MetalBackend::kCapabilityVersion, RawDemosaicMethod::Legacy),
            0U);
  EXPECT_EQ(device.Workspace().TransientBuffers().capacity_bytes(), 0U);
  EXPECT_EQ(device.Workspace().Device().RecordedWorkScratchBufferCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().RecordedWorkScratchTextureCount(), 0U);
}

TEST_F(MetalDevelopFixture, MetalDevelopXTransMatchesCudaReferenceWithinTolerance) {
  const auto pattern  = gpu_dag_test::MakeXTransPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "legacy", false);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
  device.BeginRender();
  ExecuteMetalDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();
  const auto pixels = Download(device, plan.sensor_linear_output);
  const auto linear = CpuLinearize(prepared);
  ASSERT_FALSE(pixels.empty());
  const auto crop = prepared.demosaic_output_crop;
  float      max_green_err = 0.0f;
  std::size_t green_count  = 0;
  for (int y = 0; y < crop.height; ++y) {
    for (int x = 0; x < crop.width; ++x) {
      const int src_x = crop.x + x;
      const int src_y = crop.y + y;
      if (RgbColorAt(pattern, src_y, src_x) != 1) {
        continue;
      }
      const float expected =
          linear[static_cast<std::size_t>(src_y) * prepared.host_extent.width +
                 static_cast<std::size_t>(src_x)];
      const auto& gpu = pixels[static_cast<std::size_t>(y) * crop.width + x];
      max_green_err   = std::max(max_green_err, std::fabs(gpu.g - expected));
      ++green_count;
    }
  }
  EXPECT_GT(green_count, 0U);
  EXPECT_LT(max_green_err, 2.0e-3f);
}

TEST_F(MetalDevelopFixture,
       MetalDevelopNeuralEngineFinishesWithoutReusingACommittedCommandBuffer) {
  auto& cache = MetalDemosaicNetModelCache::Instance();
  if (!cache.EnsureLoaded(MetalDemosaicNetVariant::Bayer)) {
    GTEST_SKIP() << "Bayer Neural Engine weights are not available: " << cache.LastError();
  }
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  SetDevelopMethod(document, "neural_engine", true);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  MetalRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();

  EXPECT_EQ(device.Workspace().Device().RecordedWorkScratchTextureCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().RecordedWorkScratchBufferCount(), 0U);

  const auto pixels = Download(device, plan.sensor_linear_output);
  ASSERT_FALSE(pixels.empty());
  for (const auto& p : pixels) {
    EXPECT_TRUE(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b));
    EXPECT_NEAR(p.a, 1.0f, 1.0e-3f);
  }

  auto legacy_doc = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(legacy_doc);
  SetDevelopMethod(legacy_doc, "legacy", true);
  const auto legacy_plan =
      GraphCompiler::Compile(legacy_doc, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice legacy_device;
  (void)legacy_device.Execute(legacy_plan, prepared, legacy_doc);
  legacy_device.WaitIdle();
  EXPECT_TRUE(PixelsDiffer(Download(legacy_device, legacy_plan.sensor_linear_output), pixels));
}

TEST_F(MetalDevelopFixture, MetalDevelopNeuralUsesSessionWorkspaceAndDoesNotSelectLegacyOnFailure) {
  MetalDemosaicNetModelCache failing;
  SetMetalDevelopNeuralModelCacheForTesting(&failing);
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "neural_engine", true);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
  device.BeginRender();
  try {
    ExecuteMetalDevelop(device, plan, prepared, document);
    SetMetalDevelopNeuralModelCacheForTesting(nullptr);
    FAIL() << "Neural Engine failure must throw";
  } catch (const std::runtime_error& ex) {
    SetMetalDevelopNeuralModelCacheForTesting(nullptr);
    const std::string message = ex.what();
    EXPECT_NE(message.find("Neural Engine"), std::string::npos);
    EXPECT_EQ(message.find("legacy"), std::string::npos);
    EXPECT_EQ(message.find("Legacy"), std::string::npos);
    EXPECT_GT(device.Workspace().Device().RecordedWorkScratchTextureCount(), 0U);
    device.CancelRender();
    EXPECT_EQ(device.Workspace().Device().RecordedWorkScratchTextureCount(), 0U);
    EXPECT_EQ(device.Workspace().Images().Find(plan.sensor_linear_output), nullptr);
  } catch (...) {
    SetMetalDevelopNeuralModelCacheForTesting(nullptr);
    throw;
  }
}

TEST_F(MetalDevelopFixture, MetalGeometryUsesOneResampleForCropRotationViewportAndScale) {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.25f, 0.25f, 0.50f, 0.50f};
  image.rotation_degrees = 15.0f;
  image.expand_to_fit    = true;
  ViewRequest view;
  view.visible_rect_in_edit_space = NormalizedRect{0.10f, 0.10f, 0.80f, 0.80f};
  view.viewport_extent            = Extent2D{40, 30};
  const auto source               = MakeSourceGeometry({64, 48}, {64, 48});
  const auto geometry             = ResolveRenderGeometry(source, image, view, {}, {});
  ASSERT_EQ(geometry.decoded_extent, (Extent2D{64, 48}));
  ASSERT_EQ(geometry.render_extent, (Extent2D{40, 30}));

  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                gpu_dag_test::FullSensor(64, 48));
  RenderRequest request;
  request.view = view;
  auto plan    = GraphCompiler::Compile(document, prepared.CompileSource(), request);
  plan.geometry = geometry;
  plan.encode_geometry_resample = true;

  const auto host_src = MakeSrcImage(64, 48);
  MetalRenderDevice device;
  device.Workspace().Textures().SetByteBudget(64ull * 48ull * 16ull * 8ull);
  device.BeginRender();
  ExecuteMetalDevelop(device, plan, prepared, document);
  auto* sensor = device.Workspace().Images().Find(plan.sensor_linear_output);
  ASSERT_NE(sensor, nullptr);
  device.Workspace().Device().UploadTexture2D(sensor->Texture(), AsBytes(host_src),
                                              device.CommandContext());
  ExecuteMetalGeometryResample(device, plan);
  device.EndRender();
  device.WaitIdle();

  const auto host_dst = Download(device, plan.geometry_output);
  ASSERT_EQ(host_dst.size(), 40U * 30U);
  const Rgba border{0.0f, 0.0f, 0.0f, 1.0f};
  float      max_err = 0.0f;
  for (std::uint32_t y = 0; y < 30; ++y) {
    for (std::uint32_t x = 0; x < 40; ++x) {
      const auto  src_xy = TransformPoint(geometry.render_to_decoded, PixelCenter(x, y));
      const auto  cpu    = BilinearSample(host_src, 64, 48, src_xy.x, src_xy.y, border);
      const auto& gpu    = host_dst[static_cast<std::size_t>(y) * 40u + x];
      max_err = std::max(max_err, std::fabs(cpu.r - gpu.r));
      max_err = std::max(max_err, std::fabs(cpu.g - gpu.g));
      max_err = std::max(max_err, std::fabs(cpu.b - gpu.b));
    }
  }
  EXPECT_LT(max_err, 1.5e-4f);
}

TEST_F(MetalDevelopFixture, MetalCameraColorConsumesSharedDualIlluminantTransform) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  device.BeginRender();
  ExecuteMetalDevelop(device, plan, prepared, document);
  ExecuteMetalGeometryResample(device, plan);
  ExecuteMetalCameraColor(device, plan, document);
  device.EndRender();
  device.WaitIdle();

  const auto pixels = Download(device, plan.develop_output);
  ASSERT_FALSE(pixels.empty());
  const auto resolved = ResolveDevelopColorTransform(document.Develop()->Params().Params());
  ASSERT_TRUE(resolved.ok);
  const float  src_r = 0.5f / 16.0f;
  const float  src_g = 0.5f / 12.0f;
  const float  src_b = 0.25f;
  const float* m     = resolved.transform.camera_to_ap1.data();
  EXPECT_NEAR(pixels.front().r, AcesccEncode(m[0] * src_r + m[1] * src_g + m[2] * src_b), 1.0e-5f);
  EXPECT_NEAR(pixels.front().g, AcesccEncode(m[3] * src_r + m[4] * src_g + m[5] * src_b), 1.0e-5f);
  EXPECT_NEAR(pixels.front().b, AcesccEncode(m[6] * src_r + m[7] * src_g + m[8] * src_b), 1.0e-5f);
  EXPECT_NEAR(pixels.front().a, 1.0f, 1.0e-6f);
}

TEST_F(MetalDevelopFixture, IdentityGeometryResampleAliasesSensorLinearWithoutASecondTexture) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  ASSERT_FALSE(plan.encode_geometry_resample);

  MetalRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();

  auto* sensor   = device.Workspace().Images().Find(plan.sensor_linear_output);
  auto* geometry = device.Workspace().Images().Find(plan.geometry_output);
  auto* develop  = device.Workspace().Images().Find(plan.develop_output);
  ASSERT_NE(sensor, nullptr);
  ASSERT_NE(geometry, nullptr);
  ASSERT_NE(develop, nullptr);
  EXPECT_EQ(sensor->Texture().ResourceId(), geometry->Texture().ResourceId());
  EXPECT_NE(sensor->Texture().ResourceId(), develop->Texture().ResourceId());
}

TEST_F(MetalDevelopFixture, ViewportGeometryResampleAllocatesADistinctDisplaySizedTexture) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(32, 24),
                                                gpu_dag_test::FullSensor(32, 24));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  RenderRequest request;
  request.view.visible_rect_in_edit_space = {0.0f, 0.0f, 1.0f, 1.0f};
  request.view.viewport_extent            = {16, 12};
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), request);
  ASSERT_TRUE(plan.encode_geometry_resample);

  MetalRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();

  auto* sensor   = device.Workspace().Images().Find(plan.sensor_linear_output);
  auto* geometry = device.Workspace().Images().Find(plan.geometry_output);
  ASSERT_NE(sensor, nullptr);
  ASSERT_NE(geometry, nullptr);
  EXPECT_NE(sensor->Texture().ResourceId(), geometry->Texture().ResourceId());
  EXPECT_EQ(sensor->Texture().Width(), 32U);
  EXPECT_EQ(sensor->Texture().Height(), 24U);
  EXPECT_EQ(geometry->Texture().Width(), 16U);
  EXPECT_EQ(geometry->Texture().Height(), 12U);
}

TEST_F(MetalDevelopFixture, MetalCctEditReusesSensorAndGeometryResults) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  const auto first_keys = BuildFrameResultContentKeys(plan, prepared, document);
  device.ResetPassStats();
  device.Workspace().Device().ResetCounters();
  auto develop       = document.Develop()->Params().Params();
  develop.wb_mode    = "custom";
  develop.custom_cct = 4800.0f;
  document.Develop()->Params().ReplaceParams(develop);
  const auto second_keys = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(first_keys.sensor_linear, second_keys.sensor_linear);
  EXPECT_EQ(first_keys.geometry_scene_source, second_keys.geometry_scene_source);
  EXPECT_NE(first_keys.develop_image, second_keys.develop_image);
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  const auto stats = device.PassStats();
  EXPECT_EQ(stats.source_h2d_count, 0U);
  EXPECT_EQ(stats.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.geometry_execute, 0U);
  EXPECT_EQ(stats.camera_color_execute, 1U);
  EXPECT_EQ(stats.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.geometry_skip, 1U);
  EXPECT_EQ(device.Workspace().Device().PipelineCreateCount(), 0U);
}

TEST_F(MetalDevelopFixture, MetalSecondDevelopRenderRunsNoSourceUploadOrDevelopPass) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "legacy", false);
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  device.ResetPassStats();
  device.Workspace().Device().ResetCounters();
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  const auto stats = device.PassStats();
  EXPECT_EQ(stats.source_h2d_count, 0U);
  EXPECT_EQ(stats.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.geometry_execute, 0U);
  EXPECT_EQ(stats.camera_color_execute, 0U);
  EXPECT_EQ(device.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().HeapCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().PipelineCreateCount(), 0U);
}

TEST_F(MetalDevelopFixture, MetalDevelopPassesUseOneCommandBuffer) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  MetalRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  device.Workspace().Device().ResetCounters();
  (void)device.Execute(plan, prepared, document);
  EXPECT_EQ(device.Workspace().Device().CommandBufferCreateCount(), 1U);
  device.WaitIdle();
}

TEST_F(MetalDevelopFixture, MetalGeometryResampleMissingMetallibThrowsExplicitError) {
  EXPECT_THROW(
      (void)metal::ComputePipelineCache::Instance().GetPipelineState(
          "/alcedo/missing/geometry_resample.metallib", "geometry_resample_rgba32f",
          "Metal GeometryResample"),
      std::runtime_error);
}

}  // namespace alcedo
