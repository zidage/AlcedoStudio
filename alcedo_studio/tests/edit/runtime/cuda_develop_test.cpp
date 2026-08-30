//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
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
#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "dng_profile_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_sensor_demosaic.hpp"
#include "edit/runtime/develop_transient.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaDevelopFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto DownloadDevelop(CudaRenderDevice& device, const ExecutionPlan& plan) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(plan.sensor_linear_output);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto&       tex = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(tex.Width()) * tex.Height());
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
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

auto NeuralEngineAvailable(DemosaicNetVariant variant) -> bool {
  DemosaicNetLoadOptions options;
  return DemosaicNetModelCache::Instance().EnsureLoaded(variant, options);
}

auto RenderDevelop(PipelineDocument& document, const PreparedRawInput& prepared)
    -> std::vector<Rgba> {
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();
  return DownloadDevelop(device, plan);
}

auto AllFiniteNonZero(const std::vector<Rgba>& pixels) -> bool {
  bool any = false;
  for (const auto& p : pixels) {
    if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b) || !std::isfinite(p.a)) {
      return false;
    }
    any = any || p.r != 0.0f || p.g != 0.0f || p.b != 0.0f;
  }
  return any;
}

}  // namespace

TEST_F(CudaDevelopFixture, CanonDngProfileRendersAtFullResolutionAndInvalidatesOnlyColorCache) {
  gpu_dag_test::VerifyCanonDngProfile<CudaRenderDevice>("cuda");
}

TEST_F(CudaDevelopFixture, UnpackedRgbLevelsAndAppliedWhiteBalanceProduceEquivalentFullRenders) {
  gpu_dag_test::VerifyRgbWhiteBalanceAndLevels<CudaRenderDevice>();
}

TEST_F(CudaDevelopFixture, RgbDngWarpProducesFinalSensorImageAndReusesPublishedCache) {
  gpu_dag_test::VerifyRgbWarpPublishes<CudaRenderDevice>();
}

TEST_F(CudaDevelopFixture, LegacyRgbEntryNormalizesAndRemovesAppliedWhiteBalanceOnGpu) {
  gpu_dag_test::VerifyLegacyRgbGpu(RawGpuBackend::CUDA);
}

TEST_F(CudaDevelopFixture, SonyYcbcrRgbRendersWithImportedCameraProfileAtFullResolution) {
  gpu_dag_test::VerifyCameraRgbFile<CudaRenderDevice>("DSC04739.ARW", ImageType::ARW, "cuda");
}

TEST_F(CudaDevelopFixture, ConvertedLinearDngRendersWarpAndPublishesCacheOutputAtFullResolution) {
  gpu_dag_test::VerifyCameraRgbFile<CudaRenderDevice>("DSC04739_dng.dng", ImageType::DNG, "cuda");
}

TEST_F(CudaDevelopFixture, CudaDevelopProducesFiniteCameraSceneLinearRgbFromBayerInput) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_EQ(plan.source.kind, DevelopInputKind::BayerCfa);
  EXPECT_LT(plan.IndexOf(GpuPassKind::Demosaic), plan.IndexOf(GpuPassKind::HighlightRecover));

  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();

  const auto pixels = DownloadDevelop(device, plan);
  ASSERT_FALSE(pixels.empty());
  EXPECT_TRUE(AllFiniteNonZero(pixels));
  EXPECT_EQ(prepared.working_space, SceneWorkingSpace::CameraRgb);
}

TEST_F(CudaDevelopFixture, CudaDevelopProducesFiniteCameraSceneLinearRgbFromXTransInput) {
  const auto pattern  = gpu_dag_test::MakeXTransPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "legacy", true);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_EQ(plan.source.kind, DevelopInputKind::XTransCfa);

  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();

  const auto pixels = DownloadDevelop(device, plan);
  ASSERT_FALSE(pixels.empty());
  EXPECT_TRUE(AllFiniteNonZero(pixels));
}

TEST_F(CudaDevelopFixture, CudaDevelopUsesWorkspaceForAllTemporaryBuffers) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  EXPECT_GT(device.Workspace().TransientBuffers().capacity_bytes(), 0U);
  device.EndRender();
  device.WaitIdle();
  EXPECT_NE(device.Workspace().Images().Find(plan.sensor_linear_output), nullptr);
}

TEST_F(CudaDevelopFixture, CudaDevelopSecondRenderCreatesNoGpuAllocation) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();
  device.Workspace().Device().ResetCounters();

  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();

  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().FreeCount(), 0U);
}

TEST_F(CudaDevelopFixture, DirectRgbInputBypassesLibRawAndEntersDevelopEndpoint) {
  auto       prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(32, 24),
                                                      gpu_dag_test::FullSensor(32, 24));
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  ASSERT_TRUE(plan.Contains(GpuPassKind::UploadRgb));

  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();

  const auto pixels = DownloadDevelop(device, plan);
  ASSERT_EQ(pixels.size(), 32U * 24U);
  EXPECT_NEAR(pixels.front().a, 1.0f, 1e-5f);
  EXPECT_TRUE(std::isfinite(pixels.front().r));
}

TEST_F(CudaDevelopFixture, CudaDevelopDefaultBayerUsesLegacyRcdNotNeural) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto def_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(def_doc, "default", true);
  auto legacy_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(legacy_doc, "legacy", true);
  const auto default_pixels = RenderDevelop(def_doc, prepared);
  const auto legacy_pixels  = RenderDevelop(legacy_doc, prepared);
  ASSERT_EQ(default_pixels.size(), legacy_pixels.size());
  EXPECT_FALSE(PixelsDiffer(default_pixels, legacy_pixels));
  if (NeuralEngineAvailable(DemosaicNetVariant::Bayer)) {
    auto neural_doc = CreateDefaultPipelineDocument();
    SetDevelopMethod(neural_doc, "neural_engine", true);
    const auto neural_pixels = RenderDevelop(neural_doc, prepared);
    EXPECT_TRUE(PixelsDiffer(default_pixels, neural_pixels));
  }
}

TEST_F(CudaDevelopFixture, CudaDevelopDefaultXTransUsesNeuralEngine) {
  if (!NeuralEngineAvailable(DemosaicNetVariant::XTrans)) {
    GTEST_SKIP() << "X-Trans Neural Engine weights are not available.";
  }
  RawCfaPattern pattern;
  pattern.kind = RawCfaKind::XTrans6x6;
  for (int i = 0; i < 36; ++i) {
    pattern.xtrans_pattern.rgb_fc[i] = kDemosaicNetXTransTargetRgb[i];
    pattern.xtrans_pattern.raw_fc[i] = kDemosaicNetXTransTargetRgb[i];
  }
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(72, 72, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(72, 72), DecodeRes::FULL);
  auto def_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(def_doc, "default", true);
  auto neural_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(neural_doc, "neural_engine", true);
  auto legacy_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(legacy_doc, "legacy", true);
  const auto default_pixels = RenderDevelop(def_doc, prepared);
  const auto neural_pixels  = RenderDevelop(neural_doc, prepared);
  const auto legacy_pixels  = RenderDevelop(legacy_doc, prepared);
  EXPECT_FALSE(PixelsDiffer(default_pixels, neural_pixels));
  EXPECT_TRUE(PixelsDiffer(default_pixels, legacy_pixels));
}

TEST_F(CudaDevelopFixture, CudaDevelopExplicitNeuralEngineChangesBayerPixelsVersusLegacy) {
  if (!NeuralEngineAvailable(DemosaicNetVariant::Bayer)) {
    GTEST_SKIP() << "Bayer Neural Engine weights are not available.";
  }
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto legacy_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(legacy_doc, "legacy", true);
  auto neural_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(neural_doc, "neural_engine", true);
  EXPECT_TRUE(
      PixelsDiffer(RenderDevelop(legacy_doc, prepared), RenderDevelop(neural_doc, prepared)));
}

TEST_F(CudaDevelopFixture, CudaDevelopHighlightReconstructOnSkipsCfaClamp01ForBayerAndXTrans) {
  const auto bayer          = gpu_dag_test::MakeRggbPattern();
  const auto bayer_prepared = RawInputLoader::FromUnpackedCfa(
      MakeOverRangeCfa(bayer, 64, 64), bayer, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto bayer_on = CreateDefaultPipelineDocument();
  SetDevelopMethod(bayer_on, "legacy", true);
  auto bayer_off = CreateDefaultPipelineDocument();
  SetDevelopMethod(bayer_off, "legacy", false);
  const auto bayer_on_px  = RenderDevelop(bayer_on, bayer_prepared);
  const auto bayer_off_px = RenderDevelop(bayer_off, bayer_prepared);
  EXPECT_GT(MaxChannel(bayer_on_px), 1.0f);
  EXPECT_TRUE(PixelsDiffer(bayer_on_px, bayer_off_px));

  const auto xtrans          = gpu_dag_test::MakeXTransPattern();
  const auto xtrans_prepared = RawInputLoader::FromUnpackedCfa(
      MakeOverRangeCfa(xtrans, 64, 64), xtrans, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto xtrans_on = CreateDefaultPipelineDocument();
  SetDevelopMethod(xtrans_on, "legacy", true);
  auto xtrans_off = CreateDefaultPipelineDocument();
  SetDevelopMethod(xtrans_off, "legacy", false);
  const auto xtrans_on_px  = RenderDevelop(xtrans_on, xtrans_prepared);
  const auto xtrans_off_px = RenderDevelop(xtrans_off, xtrans_prepared);
  EXPECT_TRUE(PixelsDiffer(xtrans_on_px, xtrans_off_px));
}

TEST_F(CudaDevelopFixture, CudaDevelopHighlightReconstructOffAppliesCfaClamp01) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      MakeOverRangeCfa(pattern, 64, 64), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "legacy", false);
  const auto pixels = RenderDevelop(document, prepared);
  EXPECT_TRUE(AllFiniteNonZero(pixels));
}

TEST_F(CudaDevelopFixture, CudaDevelopHighlightReconstructChangesXTransRgb) {
  const auto pattern  = gpu_dag_test::MakeXTransPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      MakeOverRangeCfa(pattern, 64, 64), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto on_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(on_doc, "legacy", true);
  auto off_doc = CreateDefaultPipelineDocument();
  SetDevelopMethod(off_doc, "legacy", false);
  EXPECT_TRUE(PixelsDiffer(RenderDevelop(on_doc, prepared), RenderDevelop(off_doc, prepared)));
}

TEST_F(CudaDevelopFixture, CudaDevelopAppliesPreparedDngRectilinearWarpAfterDemosaic) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  auto       prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(96, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(96, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "legacy", false);
  const auto           unwarped = RenderDevelop(document, prepared);

  dng::WarpRectilinear warp;
  warp.coefficient_set_count    = 1;
  warp.coefficient_sets[0]      = {1.0, 0.28, 0.0, 0.0, 0.0, 0.0};
  prepared.dng_warp_rectilinear = warp;
  const auto warped             = RenderDevelop(document, prepared);

  ASSERT_EQ(unwarped.size(), warped.size());
  EXPECT_TRUE(PixelsDiffer(unwarped, warped));
}

TEST_F(CudaDevelopFixture,
       CudaDevelopNeuralEngineFailureThrowsErrorStringAndDoesNotFallBackToLegacy) {
  DemosaicNetModelCache failing;
  SetDevelopNeuralModelCacheForTesting(&failing);
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  SetDevelopMethod(document, "neural_engine", true);
  try {
    (void)RenderDevelop(document, prepared);
    SetDevelopNeuralModelCacheForTesting(nullptr);
    FAIL() << "Neural Engine failure must throw";
  } catch (const std::runtime_error& ex) {
    SetDevelopNeuralModelCacheForTesting(nullptr);
    const std::string message = ex.what();
    EXPECT_NE(message.find("Neural Engine"), std::string::npos);
  } catch (...) {
    SetDevelopNeuralModelCacheForTesting(nullptr);
    throw;
  }
}

TEST_F(CudaDevelopFixture, IdentityGeometryResampleAliasesSensorLinearWithoutASecondTexture) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(32, 24, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(32, 24), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  SetDevelopMethod(document, "legacy", false);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  ASSERT_FALSE(plan.encode_geometry_resample);

  CudaRenderDevice device;
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
  EXPECT_EQ(device.Workspace().TransientBuffers().used_bytes(), 0U);
  EXPECT_EQ(device.Workspace().TransientBuffers().capacity_bytes(), 0U);
}

TEST_F(CudaDevelopFixture, PlanExecuteFreesDevelopScratchAfterSensorDevelop) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  SetDevelopMethod(document, "legacy", false);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  ASSERT_GT(plan.peak_transient_bytes, 64U * 64U);

  CudaRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  EXPECT_EQ(device.Workspace().TransientBuffers().used_bytes(), 0U);
  EXPECT_EQ(device.Workspace().TransientBuffers().capacity_bytes(), 0U);
  EXPECT_NE(device.Workspace().Images().Find(plan.sensor_linear_output), nullptr);
}

TEST_F(CudaDevelopFixture, PlanExecuteCachesObservedDevelopTransientCapacityForTheNextLayout) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  SetDevelopMethod(document, "legacy", false);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  const auto observed = device.Workspace().DevelopTransientHighWater().ObservedCapacity(
      plan.source, CudaBackend::kCapabilityVersion, RawDemosaicMethod::Legacy);
  EXPECT_GT(observed, ConservativeDevelopInitialBytes(plan.source));
  EXPECT_EQ(device.Workspace().TransientBuffers().capacity_bytes(), 0U);

  device.WaitIdle();
  device.Workspace().ReleaseSessionResources();
  EXPECT_EQ(device.Workspace().DevelopTransientHighWater().SuggestInitial(
                plan.source, CudaBackend::kCapabilityVersion, RawDemosaicMethod::Legacy),
            ApplyDevelopTransientSafetyMargin(observed));
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();
  EXPECT_EQ(device.Workspace().TransientBuffers().capacity_bytes(), 0U);
}

TEST_F(CudaDevelopFixture, ViewportGeometryResampleAllocatesADistinctDisplaySizedTexture) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(32, 24, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(32, 24), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  SetDevelopMethod(document, "legacy", false);
  RenderRequest request;
  request.view.visible_rect_in_edit_space = {0.0f, 0.0f, 1.0f, 1.0f};
  request.view.viewport_extent            = {16, 12};
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), request);
  ASSERT_TRUE(plan.encode_geometry_resample);
  ASSERT_EQ(plan.geometry.render_extent.width, 16U);
  ASSERT_EQ(plan.geometry.render_extent.height, 12U);
  ASSERT_NE(plan.source.develop_output_extent, plan.geometry.render_extent);

  CudaRenderDevice device;
  (void)device.Execute(plan, prepared, document);
  device.WaitIdle();

  auto* sensor   = device.Workspace().Images().Find(plan.sensor_linear_output);
  auto* geometry = device.Workspace().Images().Find(plan.geometry_output);
  ASSERT_NE(sensor, nullptr);
  ASSERT_NE(geometry, nullptr);
  EXPECT_NE(sensor->Texture().ResourceId(), geometry->Texture().ResourceId());
  EXPECT_EQ(sensor->Texture().Width(), plan.source.develop_output_extent.width);
  EXPECT_EQ(sensor->Texture().Height(), plan.source.develop_output_extent.height);
  EXPECT_EQ(geometry->Texture().Width(), plan.geometry.render_extent.width);
  EXPECT_EQ(geometry->Texture().Height(), plan.geometry.render_extent.height);
}

TEST_F(CudaDevelopFixture, CudaDevelopUploadFailureRestoresDirtyAndDoesNotFallback) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  device.Workspace().Device().FailNextUpload();
  device.BeginRender();
  EXPECT_THROW(ExecuteCudaDevelop(device, plan, prepared, document), std::runtime_error);
  device.EndRender();
  EXPECT_TRUE(document.Develop()->Params().IsDirty());
}

}  // namespace alcedo
