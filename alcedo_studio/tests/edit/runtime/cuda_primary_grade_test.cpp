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
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
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

class CudaPrimaryGradeFixture : public ::testing::Test {
 protected:
  // Direct RGB plus a stored dual-illuminant camera profile. Grade assertions
  // compare against develop.image after CameraColor, not camera RGB.
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  }

  auto Render() -> CudaPrimaryGradeResult {
    device_.BeginRender();
    ExecuteCudaDevelop(device_, plan_, prepared_, document_);
    ExecuteCudaGeometryResample(device_, plan_);
    ExecuteCudaCameraColor(device_, plan_, document_);
    auto result = ExecuteCudaPrimaryGrade(device_, plan_, prepared_.color_context, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto Download(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) return {};
    const auto&       texture = lease->Texture();
    std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
    device_.Workspace().Device().DownloadTexture2D(
        texture,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_.CommandContext());
    return pixels;
  }

  template <class Model>
  auto ModelByType(const OperatorTypeId& type) -> Model& {
    auto* model = dynamic_cast<Model*>(document_.PrimaryGrade()->FindAdjustmentByType(type));
    EXPECT_NE(model, nullptr);
    return *model;
  }

  PreparedRawInput prepared_;
  PipelineDocument document_;
  ExecutionPlan    plan_;
  CudaRenderDevice device_;
};

TEST_F(CudaPrimaryGradeFixture, CudaPrimaryGradeDefaultParametersPreserveDevelopOutput) {
  const auto result = Render();
  const auto input  = Download(plan_.develop_output);
  const auto output = Download(result.output);
  ASSERT_EQ(input.size(), output.size());
  std::size_t compared = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    // Default grade is identity inside the unclamped AP1 unit cube. CameraColor
    // of Direct RGB can produce values > 1; those are outside this assertion.
    if (input[i].r < 0.0f || input[i].r > 1.0f || input[i].g < 0.0f || input[i].g > 1.0f ||
        input[i].b < 0.0f || input[i].b > 1.0f) {
      continue;
    }
    EXPECT_NEAR(output[i].r, input[i].r, 1.0e-6f);
    EXPECT_NEAR(output[i].g, input[i].g, 1.0e-6f);
    EXPECT_NEAR(output[i].b, input[i].b, 1.0e-6f);
    ++compared;
  }
  EXPECT_GT(compared, 0U);
  EXPECT_TRUE(plan_.Contains(GpuPassKind::CameraToAp1));
  EXPECT_TRUE(plan_.Contains(GpuPassKind::PrimaryColorGrade));
}

TEST_F(CudaPrimaryGradeFixture, DefaultCurvePreservesSceneLinearHighlightsAboveOne) {
  const auto result = Render();
  const auto input  = Download(plan_.develop_output);
  const auto output = Download(result.output);
  ASSERT_EQ(input.size(), output.size());
  bool compared_highlight = false;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (std::max({input[i].r, input[i].g, input[i].b}) <= 1.0f) {
      continue;
    }
    EXPECT_NEAR(output[i].r, input[i].r, 1.0e-6f);
    EXPECT_NEAR(output[i].g, input[i].g, 1.0e-6f);
    EXPECT_NEAR(output[i].b, input[i].b, 1.0e-6f);
    compared_highlight = true;
  }
  EXPECT_TRUE(compared_highlight);
}

TEST_F(CudaPrimaryGradeFixture, CudaExposurePatchChangesOnlyExposureParameterRange) {
  Render();
  auto& exposure = ModelByType<ExposureModel>(type_ids::Exposure());
  exposure.SetValue(1.0f);
  const ParameterSlotKey exposure_key{document_.PrimaryGrade()->Id(),
                                      AdjustmentInstanceId{"grade.primary.exposure"}};
  const auto             exposure_binding = device_.Workspace().Parameters().Binding(exposure_key);
  device_.Workspace().Device().ResetCounters();
  Render();
  const auto& ranges = device_.Workspace().Device().LastHostToDeviceRanges();
  EXPECT_TRUE(std::ranges::any_of(ranges, [&](const ByteRange& range) {
    return range.offset == exposure_binding.offset && range.size == exposure_binding.size;
  }));
  EXPECT_FALSE(exposure.IsDirty());
}

TEST_F(CudaPrimaryGradeFixture, CudaCat02WhiteBalanceZeroOffsetPreservesAp1White) {
  auto& wb = ModelByType<Cat02WhiteBalanceModel>(type_ids::Cat02WhiteBalance());
  wb.SetTemperatureOffset(0.0f);
  wb.SetTintOffset(0.0f);
  const auto result = Render();
  const auto input  = Download(plan_.develop_output);
  const auto output = Download(result.output);
  ASSERT_FALSE(output.empty());
  EXPECT_NEAR(output.front().r, input.front().r, 1.0e-6f);
  EXPECT_NEAR(output.front().g, input.front().g, 1.0e-6f);
  EXPECT_NEAR(output.front().b, input.front().b, 1.0e-6f);
}

TEST_F(CudaPrimaryGradeFixture, CudaCat02WhiteBalanceMaskedSampleMatchesFullAdjustmentAtMaskOne) {
  auto& wb = ModelByType<Cat02WhiteBalanceModel>(type_ids::Cat02WhiteBalance());
  wb.SetTemperatureOffset(150.0f);
  document_.PrimaryGrade()->SetMix(1.0f);
  const auto full = Download(Render().output);
  document_.PrimaryGrade()->SetMix(0.5f);
  const auto half   = Download(Render().output);
  const auto source = Download(plan_.develop_output);
  ASSERT_FALSE(full.empty());
  EXPECT_NEAR(half.front().r, (full.front().r + source.front().r) * 0.5f, 1.0e-5f);
  EXPECT_NEAR(full.front().g, source.front().g, 1.0e-6f);
}

TEST_F(CudaPrimaryGradeFixture, CudaPointAdjustmentsExecuteInSerializedModelOrder) {
  // Exposure before contrast is intentionally non-commutative around the 0.18 pivot.
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(1.0f);
  ModelByType<ContrastModel>(type_ids::Contrast()).SetValue(100.0f);
  const auto output = Download(Render().output);
  const auto input  = Download(plan_.develop_output);
  ASSERT_FALSE(output.empty());
  EXPECT_NEAR(output.front().r, (input.front().r * 2.0f - 0.18f) * 2.0f + 0.18f, 1.0e-5f);
}

TEST_F(CudaPrimaryGradeFixture, CudaLocalToneReferenceReusesAcrossViewportChanges) {
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto    first = Render();
  RenderRequest request;
  request.view.visible_rect_in_edit_space = {0.1f, 0.1f, 0.8f, 0.8f};
  plan_             = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
  const auto second = Render();
  EXPECT_NE(first.local_tone_reference_resource_id, 0U);
  EXPECT_EQ(first.local_tone_reference_resource_id, second.local_tone_reference_resource_id);
}

TEST_F(CudaPrimaryGradeFixture, CudaLocalToneUsesWorkspaceInsteadOfPrivateAllocation) {
  ModelByType<HighlightsModel>(type_ids::Highlights()).SetValue(-30.0f);
  const auto result = Render();
  EXPECT_NE(result.local_tone_reference_resource_id, 0U);
  EXPECT_NE(device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                              PortId{"local_tone.reference"}),
            nullptr);
}

TEST_F(CudaPrimaryGradeFixture, CudaColorGradeSecondRenderCreatesNoGpuAllocation) {
  Render();
  device_.Workspace().Device().ResetCounters();
  Render();
  EXPECT_EQ(device_.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(), 0U);
}

TEST_F(CudaPrimaryGradeFixture,
       MovingAdjustmentChangesExecutionOrderWithoutChangingOtherParameters) {
  auto& grade = *document_.PrimaryGrade();
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(1.0f);
  ModelByType<ContrastModel>(type_ids::Contrast()).SetValue(100.0f);
  const auto before         = Download(Render().output);
  const auto exposure_id    = AdjustmentInstanceId{"grade.primary.exposure"};
  const auto contrast_id    = AdjustmentInstanceId{"grade.primary.contrast"};
  const auto contrast_index = [&] {
    for (std::size_t i = 0; i < grade.AdjustmentCount(); ++i) {
      if (grade.AdjustmentIdAt(i) == contrast_id) return i;
    }
    return grade.AdjustmentCount();
  }();
  grade.MoveAdjustment(exposure_id, contrast_index + 1);
  document_.MarkTopologyDirty();
  plan_            = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  const auto after = Download(Render().output);
  ASSERT_FALSE(before.empty());
  ASSERT_EQ(before.size(), after.size());
  EXPECT_GT(std::abs(before.front().r - after.front().r), 0.05f);
  EXPECT_FLOAT_EQ(ModelByType<ExposureModel>(type_ids::Exposure()).Value(), 1.0f);
  EXPECT_FLOAT_EQ(ModelByType<ContrastModel>(type_ids::Contrast()).Value(), 100.0f);
}

TEST_F(CudaPrimaryGradeFixture, DevelopImageGraphValueIsAp1SceneLinear) {
  (void)Render();
  const auto pixels = Download(plan_.develop_output);
  ASSERT_FALSE(pixels.empty());
  const auto resolved = ResolveDevelopColorTransform(document_.Develop()->Params().Params());
  ASSERT_TRUE(resolved.ok);
  const float  src_r = 0.5f / 16.0f;
  const float  src_g = 0.5f / 12.0f;
  const float  src_b = 0.25f;
  const float* m     = resolved.transform.camera_to_ap1.data();
  EXPECT_NEAR(pixels.front().r, m[0] * src_r + m[1] * src_g + m[2] * src_b, 1.0e-5f);
  EXPECT_NEAR(pixels.front().g, m[3] * src_r + m[4] * src_g + m[5] * src_b, 1.0e-5f);
  EXPECT_NEAR(pixels.front().b, m[6] * src_r + m[7] * src_g + m[8] * src_b, 1.0e-5f);
  EXPECT_NEAR(pixels.front().a, 1.0f, 1.0e-6f);
}

TEST(GpuDagCudaPrimaryGrade, ExecuteCudaCameraColorRejectsMissingCameraMatrices) {
  if (!HasCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  ExecuteCudaGeometryResample(device, plan);
  EXPECT_THROW(ExecuteCudaCameraColor(device, plan, document), std::runtime_error);
  device.EndRender();
}

}  // namespace
}  // namespace alcedo
