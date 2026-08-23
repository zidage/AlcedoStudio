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

auto AcesccEncode(float value) -> float {
  constexpr float kA          = 9.72f;
  constexpr float kB          = 17.52f;
  constexpr float kOffset     = 0.0000152587890625f;
  constexpr float kTransition = 0.000030517578125f;
  constexpr float kFloor      = (-16.0f + kA) / kB;
  if (value < 0.0f) return kFloor + value;
  if (value < kTransition) return (std::log2(kOffset + value * 0.5f) + kA) / kB;
  return (std::log2(value) + kA) / kB;
}

auto MakeNeighborhoodPlane(std::uint32_t width, std::uint32_t height, float surroundings,
                           float center) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* p) { delete[] p; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float value = x == width / 2 && y == height / 2 ? center : surroundings;
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = value;
      pixels[index + 1] = value;
      pixels[index + 2] = value;
      pixels[index + 3] = 1.0f;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto RenderLocalToneCenter(float surroundings, float center, float shadows_value,
                           float highlights_value) -> float {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  auto                    prepared =
      RawInputLoader::FromDirectRgb(MakeNeighborhoodPlane(width, height, surroundings, center),
                                    gpu_dag_test::FullSensor(width, height));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  auto* highlights = dynamic_cast<HighlightsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Highlights()));
  EXPECT_NE(shadows, nullptr);
  EXPECT_NE(highlights, nullptr);
  shadows->SetValue(shadows_value);
  highlights->SetValue(highlights_value);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  CudaRenderDevice device;
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  ExecuteCudaGeometryResample(device, plan);
  ExecuteCudaCameraColor(device, plan, document);
  const auto result = ExecuteCudaPrimaryGrade(device, plan, prepared.color_context, document);
  device.EndRender();
  device.WaitIdle();
  auto* lease = device.Workspace().Images().Find(result.output);
  EXPECT_NE(lease, nullptr);
  std::vector<Rgba> pixels(static_cast<std::size_t>(width) * height);
  device.Workspace().Device().DownloadTexture2D(
      lease->Texture(),
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels[static_cast<std::size_t>(height / 2) * width + width / 2].r;
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

TEST_F(CudaPrimaryGradeFixture, DefaultCurvePreservesAcesccWorkingValuesWithoutClipping) {
  const auto result = Render();
  const auto input  = Download(plan_.develop_output);
  const auto output = Download(result.output);
  ASSERT_EQ(input.size(), output.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    EXPECT_NEAR(output[i].r, input[i].r, 1.0e-6f);
    EXPECT_NEAR(output[i].g, input[i].g, 1.0e-6f);
    EXPECT_NEAR(output[i].b, input[i].b, 1.0e-6f);
  }
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
  EXPECT_NEAR(output.front().r, (input.front().r + 1.0f / 17.52f - 0.18f) * 2.0f + 0.18f, 1.0e-5f);
}

TEST_F(CudaPrimaryGradeFixture, CudaLocalTonePyramidBuffersReuseAcrossViewportChanges) {
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
  const auto* source        = device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                                                PortId{"local_tone.source.0"});
  const auto* result_buffer = device_.Workspace().Values().Find(document_.PrimaryGrade()->Id(),
                                                                PortId{"local_tone.result.0"});
  ASSERT_NE(source, nullptr);
  ASSERT_NE(result_buffer, nullptr);
  EXPECT_GT(source->Bytes(), sizeof(float));
  EXPECT_EQ(source->Bytes(), result_buffer->Bytes());
  device_.Workspace().Device().ResetCounters();
  (void)Render();
  EXPECT_EQ(device_.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(), 0U);
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

TEST_F(CudaPrimaryGradeFixture, CameraColorEncodesAp1AsAcesccGraphWorkingSpace) {
  (void)Render();
  const auto pixels = Download(plan_.develop_output);
  ASSERT_FALSE(pixels.empty());
  const auto resolved = ResolveDevelopColorTransform(document_.Develop()->Params().Params());
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

TEST(GpuDagCudaPrimaryGrade, ShadowsLlfRespondsToNeighborhoodWithIdenticalCenterPixel) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  const float dark_neighborhood   = RenderLocalToneCenter(0.02f, 0.08f, 80.0f, 0.0f);
  const float bright_neighborhood = RenderLocalToneCenter(0.45f, 0.08f, 80.0f, 0.0f);
  EXPECT_GT(std::abs(dark_neighborhood - bright_neighborhood), 1.0e-4f);
}

TEST(GpuDagCudaPrimaryGrade, HighlightsLlfRespondsToNeighborhoodWithIdenticalCenterPixel) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  const float dark_neighborhood   = RenderLocalToneCenter(0.10f, 0.80f, 0.0f, -80.0f);
  const float bright_neighborhood = RenderLocalToneCenter(1.50f, 0.80f, 0.0f, -80.0f);
  EXPECT_GT(std::abs(dark_neighborhood - bright_neighborhood), 1.0e-4f);
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
