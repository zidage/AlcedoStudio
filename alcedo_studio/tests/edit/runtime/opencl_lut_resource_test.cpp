//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "lut_resource_runtime_test_support.hpp"
#include "opencl/opencl_runtime.hpp"

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

/// One OpenCL device rendering whole plans; the last Grade writes scene-work member 0 or 1.
class OpenClLutHarness {
 public:
  OpenClLutHarness()
      : prepared_(RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12))),
        device_(std::make_unique<OpenClRenderDevice>()) {}

  void UseLutResources(std::shared_ptr<const LutResourceResolver> resources) {
    device_->Workspace().SetLutResources(std::move(resources));
  }

  auto Render(PipelineDocument& document) -> RenderedGrades {
    const auto plan = GraphCompiler::Compile(document, prepared_.CompileSource(), RenderRequest{});
    device_->ResetPassStats();
    (void)device_->Execute(plan, prepared_, document);
    device_->WaitIdle();
    RenderedGrades rendered;
    if (auto* develop = device_->Workspace().Images().Find(plan.develop_output)) {
      const auto&       texture = develop->Texture();
      std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
      device_->Workspace().Device().DownloadTexture2D(
          texture,
          std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                               pixels.size() * sizeof(Rgba)),
          device_->CommandContext());
      rendered.develop = ToRgb(pixels);
    }
    const auto member =
        plan.grade_nodes.size() % 2 == 0 ? SceneWorkMember::Member1 : SceneWorkMember::Member0;
    auto&             image = device_->Workspace().SceneWork().Member(member);
    std::vector<Rgba> output(static_cast<std::size_t>(image.Width()) * image.Height());
    device_->Workspace().Device().DownloadBufferRange(
        image.Storage(), 0,
        std::span<std::byte>(reinterpret_cast<std::byte*>(output.data()),
                             output.size() * sizeof(Rgba)),
        device_->CommandContext());
    rendered.output        = ToRgb(output);
    rendered.grade_execute = device_->PassStats().primary_grade_execute;
    rendered.display_execute = device_->PassStats().drt_execute;
    rendered.display_skip    = device_->PassStats().drt_skip;
    return rendered;
  }

 private:
  PreparedRawInput                    prepared_;
  std::unique_ptr<OpenClRenderDevice> device_;
};

class OpenClLutResourceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
    harness_ = std::make_unique<OpenClLutHarness>();
  }

  std::unique_ptr<OpenClLutHarness> harness_;
};

}  // namespace

TEST_F(OpenClLutResourceFixture, LutStrengthZeroHalfAndOneMatchExpectedPixels) {
  lut_resource_test::CheckStrengthZeroHalfAndOne(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, LutStrengthDoesNotScaleOtherAdjustments) {
  lut_resource_test::CheckStrengthDoesNotScaleOtherAdjustments(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, RawCctEditsWithSelectedLutMatchExpectedPixels) {
  const auto cube = lut_resource_test::FixtureDirectory("opencl", "raw-cct-lut") / "affine.cube";
  lut_resource_test::WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  lut_resource_test::Lmt(document).SetCubePath(cube.string());
  lut_resource_test::Lmt(document).SetStrength(1.0f);
  const auto initial = harness_->Render(document);
  ASSERT_TRUE(lut_resource_test::PixelsMatch(initial, lut_resource_test::ApplyAffineLut));

  for (const float cct : {3000.0f, 4800.0f, 9000.0f}) {
    SCOPED_TRACE(cct);
    document.Develop()->Params().ApplyColorTemperatureUpdate(
        DevelopColorTemperatureUpdate{.wb_mode = "custom", .custom_cct = cct});
    const auto rendered = harness_->Render(document);
    EXPECT_TRUE(lut_resource_test::PixelsMatch(rendered, lut_resource_test::ApplyAffineLut));
    EXPECT_FALSE(lut_resource_test::SamePixels(initial.develop, rendered.develop));
    EXPECT_EQ(rendered.grade_execute, 1U);
    EXPECT_EQ(rendered.display_execute, 1U);
  }
}

TEST_F(OpenClLutResourceFixture, MissingLutKeepsOtherGradeAdjustments) {
  OpenClLutHarness without_lut;
  lut_resource_test::CheckMissingLutKeepsOtherGradeAdjustments(*harness_, without_lut, "opencl");
}

TEST_F(OpenClLutResourceFixture, InvalidCubeRemainsAnError) {
  lut_resource_test::CheckInvalidCubeRemainsAnError(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, OfficialUpdateChangesPixelsWithWarmResultCache) {
  lut_resource_test::CheckOfficialUpdateChangesPixelsWithWarmResultCache(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, ReturnedLutRestoresConfiguredStrengthWithoutHistoryEdit) {
  lut_resource_test::CheckReturnedLutRestoresConfiguredStrength(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, ReturnedFileDoesNotRestoreClearedLut) {
  lut_resource_test::CheckReturnedFileDoesNotRestoreClearedLut(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, ReturnedLutAtZeroStrengthRemainsVisuallyInactive) {
  lut_resource_test::CheckReturnedLutAtZeroStrengthRemainsInactive(*harness_, "opencl");
}

TEST_F(OpenClLutResourceFixture, NonDefaultEncodingRendersHostCompositeTableWithin2PowMinus17) {
  lut_resource_test::CheckNonDefaultEncodingSamplesHostCompositeTable(*harness_, "opencl");
}

}  // namespace alcedo
