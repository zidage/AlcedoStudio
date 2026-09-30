//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "lut_resource_runtime_test_support.hpp"

namespace alcedo {
namespace {

using lut_resource_test::RenderedGrades;

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto ToRgb(const std::vector<Rgba>& pixels) -> std::vector<multi_grade_test::AcesccRgb> {
  std::vector<multi_grade_test::AcesccRgb> rgb;
  rgb.reserve(pixels.size());
  for (const auto& pixel : pixels) rgb.push_back({pixel.r, pixel.g, pixel.b});
  return rgb;
}

auto DownloadTexture(CudaRenderDevice& device, CudaBackend::Texture2D& texture)
    -> std::vector<Rgba> {
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

/// One CUDA device rendering whole plans; the last Grade writes scene-work member 0 or 1.
class CudaLutHarness {
 public:
  CudaLutHarness()
      : prepared_(RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12))) {}

  void UseLutResources(std::shared_ptr<const LutResourceResolver> resources) {
    device_.Workspace().SetLutResources(std::move(resources));
  }

  auto Render(PipelineDocument& document) -> RenderedGrades {
    const auto plan = GraphCompiler::Compile(document, prepared_.CompileSource(), RenderRequest{});
    device_.ResetPassStats();
    (void)device_.Execute(plan, prepared_, document);
    device_.WaitIdle();
    RenderedGrades rendered;
    auto*          develop = device_.Workspace().Images().Find(plan.develop_output);
    EXPECT_NE(develop, nullptr);
    if (develop != nullptr) rendered.develop = ToRgb(DownloadTexture(device_, develop->Texture()));
    const auto member =
        plan.grade_nodes.size() % 2 == 0 ? SceneWorkMember::Member1 : SceneWorkMember::Member0;
    rendered.output =
        ToRgb(DownloadTexture(device_, device_.Workspace().SceneWork().Member(member)));
    rendered.grade_execute = device_.PassStats().primary_grade_execute;
    rendered.display_execute = device_.PassStats().drt_execute;
    rendered.display_skip    = device_.PassStats().drt_skip;
    return rendered;
  }

 private:
  PreparedRawInput prepared_;
  CudaRenderDevice device_;
};

class CudaLutResourceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    int count = 0;
    if (::cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    harness_ = std::make_unique<CudaLutHarness>();
  }

  std::unique_ptr<CudaLutHarness> harness_;
};

}  // namespace

TEST_F(CudaLutResourceFixture, LutStrengthZeroHalfAndOneMatchExpectedPixels) {
  lut_resource_test::CheckStrengthZeroHalfAndOne(*harness_, "cuda");
}

TEST_F(CudaLutResourceFixture, LutStrengthDoesNotScaleOtherAdjustments) {
  lut_resource_test::CheckStrengthDoesNotScaleOtherAdjustments(*harness_, "cuda");
}

TEST_F(CudaLutResourceFixture, MissingLutKeepsOtherGradeAdjustments) {
  CudaLutHarness without_lut;
  lut_resource_test::CheckMissingLutKeepsOtherGradeAdjustments(*harness_, without_lut, "cuda");
}

TEST_F(CudaLutResourceFixture, InvalidCubeRemainsAnError) {
  lut_resource_test::CheckInvalidCubeRemainsAnError(*harness_, "cuda");
}

TEST_F(CudaLutResourceFixture, OfficialUpdateChangesPixelsWithWarmResultCache) {
  lut_resource_test::CheckOfficialUpdateChangesPixelsWithWarmResultCache(*harness_, "cuda");
}

TEST_F(CudaLutResourceFixture, ReturnedLutRestoresConfiguredStrengthWithoutHistoryEdit) {
  lut_resource_test::CheckReturnedLutRestoresConfiguredStrength(*harness_, "cuda");
}

TEST_F(CudaLutResourceFixture, ReturnedFileDoesNotRestoreClearedLut) {
  lut_resource_test::CheckReturnedFileDoesNotRestoreClearedLut(*harness_, "cuda");
}

TEST_F(CudaLutResourceFixture, ReturnedLutAtZeroStrengthRemainsVisuallyInactive) {
  lut_resource_test::CheckReturnedLutAtZeroStrengthRemainsInactive(*harness_, "cuda");
}

}  // namespace alcedo
