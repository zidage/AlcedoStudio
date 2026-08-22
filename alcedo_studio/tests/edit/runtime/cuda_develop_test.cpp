//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/texture_format.hpp"
#include "../input/prepared_raw_test_support.hpp"

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
  auto* lease = device.Workspace().Images().Find(plan.develop_output);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto& tex = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(tex.Width()) * tex.Height());
  device.Workspace().Device().DownloadTexture2D(
      tex,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
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

TEST_F(CudaDevelopFixture, CudaDevelopProducesFiniteCameraSceneLinearRgbFromBayerInput) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
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
  const auto pattern = gpu_dag_test::MakeXTransPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
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
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  EXPECT_GT(device.Workspace().TransientBuffers().capacity_bytes(), 0U);
  device.EndRender();
  device.WaitIdle();
  EXPECT_NE(device.Workspace().Images().Find(plan.develop_output), nullptr);
}

TEST_F(CudaDevelopFixture, CudaDevelopSecondRenderCreatesNoGpuAllocation) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
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
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(32, 24),
                                                gpu_dag_test::FullSensor(32, 24));
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
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

TEST_F(CudaDevelopFixture, CudaDevelopUploadFailureRestoresDirtyAndDoesNotFallback) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern,
      gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  auto document = CreateDefaultPipelineDocument();
  const auto plan =
      GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  CudaRenderDevice device;
  device.Workspace().TransientBuffers().Reserve(plan.peak_transient_bytes);
  device.Workspace().Device().FailNextUpload();
  device.BeginRender();
  EXPECT_THROW(ExecuteCudaDevelop(device, plan, prepared, document), std::runtime_error);
  device.EndRender();
  EXPECT_TRUE(document.Develop()->Params().IsDirty());
}

}  // namespace alcedo
