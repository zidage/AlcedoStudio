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
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
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
  const auto result = ExecuteCudaPrimaryGrade(device, plan, prepared, document);
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

void WriteConstantRgbCube(const std::filesystem::path& path, float r, float g, float b) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n";
  for (int i = 0; i < 8; ++i) {
    out << r << ' ' << g << ' ' << b << '\n';
  }
}

void WriteIdentityCube(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n"
      << "0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
      << "0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
}

void ResetProductLookToIdentity(PipelineDocument& document) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  auto* saturation = dynamic_cast<SaturationModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(saturation, nullptr);
  exposure->SetValue(0.0f);
  saturation->SetValue(1.0f);
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
    ResetProductLookToIdentity(document_);
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  }

  auto Render() -> CudaPrimaryGradeResult {
    device_.BeginRender();
    ExecuteCudaDevelop(device_, plan_, prepared_, document_);
    ExecuteCudaGeometryResample(device_, plan_);
    ExecuteCudaCameraColor(device_, plan_, document_);
    auto result = ExecuteCudaPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto RenderThroughDrtPost() -> CudaDrtResult {
    device_.BeginRender();
    ExecuteCudaDevelop(device_, plan_, prepared_, document_);
    ExecuteCudaGeometryResample(device_, plan_);
    ExecuteCudaCameraColor(device_, plan_, document_);
    (void)ExecuteCudaPrimaryGrade(device_, plan_, prepared_, document_);
    auto result = ExecuteCudaDrt(device_, plan_, document_);
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
    IOperatorModel* found = document_.PrimaryGrade()->FindAdjustmentByType(type);
    if (found == nullptr && document_.Drt() != nullptr) {
      found = document_.Drt()->FindAdjustmentByType(type);
    }
    auto* model = dynamic_cast<Model*>(found);
    EXPECT_NE(model, nullptr);
    return *model;
  }

  void UseNeighborhoodPlane(std::uint32_t width, std::uint32_t height, float surroundings,
                            float center, const RenderRequest& request = {}) {
    prepared_ =
        RawInputLoader::FromDirectRgb(MakeNeighborhoodPlane(width, height, surroundings, center),
                                      gpu_dag_test::FullSensor(width, height));
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
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

TEST_F(CudaPrimaryGradeFixture, CudaGradeParameterBindDoesNotCopyFullDto) {
  OperatorModelFullDtoCopyCount::Reset();
  (void)Render();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(1.0f);
  OperatorModelFullDtoCopyCount::Reset();
  (void)Render();
  EXPECT_EQ(OperatorModelFullDtoCopyCount::Peek(), 0);
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

TEST_F(CudaPrimaryGradeFixture, CudaSharpenUsesSurroundingPixelsForUnsharpMask) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  UseNeighborhoodPlane(width, height, 0.18f, 0.55f);
  auto& sharpen = ModelByType<SharpenModel>(type_ids::Sharpen());
  sharpen.SetAmount(100.0f);
  sharpen.SetRadius(3.0f);
  sharpen.SetThreshold(0.0f);

  const auto result         = RenderThroughDrtPost();
  const auto output         = Download(result.scene_post);
  const auto input          = Download(plan_.FirstGrade()->scene_output);
  const auto center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto neighbor_index = center - 1;
  const auto far_index      = static_cast<std::size_t>(height / 2) * width + 2;
  ASSERT_EQ(input.size(), output.size());
  EXPECT_EQ(result.post_neighborhood_count, 1U);
  EXPECT_GT(output[center].r, input[center].r);
  EXPECT_LT(output[neighbor_index].r, input[neighbor_index].r);
  EXPECT_NEAR(output[far_index].r, input[far_index].r, 1.0e-6f);
}

TEST_F(CudaPrimaryGradeFixture, CudaSharpenDarkRingFollowsPreviewResolution) {
  // Radius 4 is 12 taps at 1:1 and 6 taps at render_scale 0.5. Offset 10 sits between those
  // radii, so an unscaled kernel would still darken the half-res probe.
  constexpr std::uint32_t width   = 128;
  constexpr std::uint32_t height = 128;
  auto&                   sharpen = ModelByType<SharpenModel>(type_ids::Sharpen());
  sharpen.SetRadius(4.0f);
  sharpen.SetThreshold(0.0f);

  UseNeighborhoodPlane(width, height, 0.18f, 0.55f);
  sharpen.SetAmount(0.0f);
  const auto full_identity = Download(RenderThroughDrtPost().scene_post);
  sharpen.SetAmount(100.0f);
  const auto full_sharpened = Download(RenderThroughDrtPost().scene_post);
  const auto full_near =
      static_cast<std::size_t>(height / 2) * width + width / 2 + 1U;
  ASSERT_EQ(full_identity.size(), full_sharpened.size());
  EXPECT_GT(full_identity[full_near].r - full_sharpened[full_near].r, 1.0e-4f);

  RenderRequest half_request;
  half_request.resolution.render_scale = 0.5f;
  UseNeighborhoodPlane(width, height, 0.18f, 0.55f, half_request);
  sharpen.SetAmount(0.0f);
  const auto half_identity = Download(RenderThroughDrtPost().scene_post);
  sharpen.SetAmount(100.0f);
  const auto half_result    = RenderThroughDrtPost();
  const auto half_sharpened = Download(half_result.scene_post);
  ASSERT_EQ(half_identity.size(), static_cast<std::size_t>(64) * 64);
  const auto half_near = static_cast<std::size_t>(32) * 64U + 32U + 1U;
  const auto half_far  = static_cast<std::size_t>(32) * 64U + 32U + 10U;
  EXPECT_EQ(half_result.post_neighborhood_count, 1U);
  EXPECT_GT(half_identity[half_near].r - half_sharpened[half_near].r, 1.0e-5f);
  EXPECT_NEAR(half_sharpened[half_far].r, half_identity[half_far].r, 2.0e-3f);
}

TEST_F(CudaPrimaryGradeFixture, CudaClarityUsesLargeRadiusLocalContrast) {
  constexpr std::uint32_t width  = 96;
  constexpr std::uint32_t height = 96;
  UseNeighborhoodPlane(width, height, 0.22f, 0.48f);
  ModelByType<ClarityModel>(type_ids::Clarity()).SetValue(80.0f);

  const auto result         = RenderThroughDrtPost();
  const auto output         = Download(result.scene_post);
  const auto input          = Download(plan_.FirstGrade()->scene_output);
  const auto center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto neighbor_index = center - 1;
  const auto far_index      = static_cast<std::size_t>(height / 2) * width + 1;
  ASSERT_EQ(input.size(), output.size());
  EXPECT_EQ(result.post_neighborhood_count, 1U);
  EXPECT_GT(output[center].r, input[center].r);
  EXPECT_LT(output[neighbor_index].r, input[neighbor_index].r);
  EXPECT_NEAR(output[far_index].r, input[far_index].r, 1.0e-6f);
}

TEST_F(CudaPrimaryGradeFixture, CudaHalationSpreadsRedLightIntoDarkNeighbors) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  UseNeighborhoodPlane(width, height, 0.02f, 1.0f);
  ModelByType<HalationModel>(type_ids::Halation()).SetValue(1.0f);

  const auto result         = RenderThroughDrtPost();
  const auto output         = Download(result.scene_post);
  const auto input          = Download(plan_.FirstGrade()->scene_output);
  const auto center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto neighbor_index = center - 1;
  const auto far_index      = static_cast<std::size_t>(height / 2) * width + 2;
  ASSERT_EQ(input.size(), output.size());
  EXPECT_EQ(result.post_neighborhood_count, 1U);
  const float red_spill   = output[neighbor_index].r - input[neighbor_index].r;
  const float green_spill = output[neighbor_index].g - input[neighbor_index].g;
  EXPECT_GT(red_spill, 1.0e-4f);
  EXPECT_GT(red_spill, green_spill * 5.0f);
  EXPECT_NEAR(output[far_index].r, input[far_index].r, 1.0e-6f);
}

TEST_F(CudaPrimaryGradeFixture, CudaFilmGrainStrengthScalesDeterministicDensityVariation) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  UseNeighborhoodPlane(width, height, 0.35f, 0.35f);
  auto& grain = ModelByType<FilmGrainModel>(type_ids::FilmGrain());

  grain.SetValue(0.25f);
  const auto low   = Download(RenderThroughDrtPost().scene_post);
  const auto input = Download(plan_.FirstGrade()->scene_output);
  grain.SetValue(0.75f);
  const auto high       = Download(RenderThroughDrtPost().scene_post);
  const auto high_again = Download(RenderThroughDrtPost().scene_post);
  ASSERT_EQ(input.size(), high.size());

  double low_energy  = 0.0;
  double high_energy = 0.0;
  for (std::size_t index = 0; index < input.size(); ++index) {
    low_energy += std::pow(static_cast<double>(low[index].r - input[index].r), 2.0);
    high_energy += std::pow(static_cast<double>(high[index].r - input[index].r), 2.0);
    EXPECT_FLOAT_EQ(high[index].r, high_again[index].r);
    EXPECT_FLOAT_EQ(high[index].g, high_again[index].g);
    EXPECT_FLOAT_EQ(high[index].b, high_again[index].b);
  }
  EXPECT_GT(high_energy, low_energy * 6.0);
}

TEST_F(CudaPrimaryGradeFixture, CudaNeighborStagesReuseWorkspaceTexturesAfterFirstRender) {
  ModelByType<ClarityModel>(type_ids::Clarity()).SetValue(20.0f);
  ModelByType<SharpenModel>(type_ids::Sharpen()).SetAmount(10.0f);
  ModelByType<HalationModel>(type_ids::Halation()).SetValue(0.4f);
  ModelByType<FilmGrainModel>(type_ids::FilmGrain()).SetValue(0.3f);
  (void)RenderThroughDrtPost();

  device_.Workspace().Device().ResetCounters();
  (void)RenderThroughDrtPost();
  EXPECT_EQ(device_.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(), 0U);
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
  EXPECT_TRUE(first.local_tone_rebuilt_reference);
  EXPECT_FALSE(first.local_tone_sampled_canonical_reference);
  EXPECT_FALSE(second.local_tone_rebuilt_reference);
  EXPECT_TRUE(second.local_tone_sampled_canonical_reference);
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

auto MakeSplitPlane(std::uint32_t width, std::uint32_t height, float left, float right)
    -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* p) { delete[] p; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float value = x < width / 2 ? left : right;
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

auto DownloadLocalTonePlane(CudaRenderDevice& device, const NodeId& grade_id)
    -> std::vector<float> {
  auto* buffer = device.Workspace().Values().Find(grade_id, PortId{"local_tone.source.0"});
  EXPECT_NE(buffer, nullptr);
  if (buffer == nullptr) return {};
  std::vector<float> plane(buffer->Bytes() / sizeof(float));
  device.Workspace().Device().DownloadBufferRange(
      *buffer, 0, std::span<std::byte>(reinterpret_cast<std::byte*>(plane.data()), buffer->Bytes()),
      device.CommandContext());
  return plane;
}

auto RenderPreparedGrade(CudaRenderDevice& device, PipelineDocument& document,
                         const PreparedRawInput& prepared, const ExecutionPlan& plan)
    -> CudaPrimaryGradeResult {
  device.BeginRender();
  ExecuteCudaDevelop(device, plan, prepared, document);
  ExecuteCudaGeometryResample(device, plan);
  ExecuteCudaCameraColor(device, plan, document);
  auto result = ExecuteCudaPrimaryGrade(device, plan, prepared, document);
  device.EndRender();
  device.WaitIdle();
  return result;
}

auto DownloadGrade(CudaRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) return {};
  const auto&       texture = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

TEST(GpuDagCudaPrimaryGrade, RoiFrameSamplesCanonicalLlfReferenceInsteadOfRebuilding) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  constexpr std::uint32_t kWidth  = 64;
  constexpr std::uint32_t kHeight = 64;
  auto prepared = RawInputLoader::FromDirectRgb(MakeSplitPlane(kWidth, kHeight, 1.0f, 0.05f),
                                                gpu_dag_test::FullSensor(kWidth, kHeight));
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(80.0f);

  auto full_plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  CudaRenderDevice device;
  const auto       full_grade = RenderPreparedGrade(device, document, prepared, full_plan);
  EXPECT_TRUE(full_grade.local_tone_rebuilt_reference);
  EXPECT_FALSE(full_grade.local_tone_sampled_canonical_reference);
  const auto full_pixels = DownloadGrade(device, full_grade.output);
  const auto full_mask   = DownloadLocalTonePlane(device, document.PrimaryGrade()->Id());
  ASSERT_FALSE(full_pixels.empty());
  ASSERT_FALSE(full_mask.empty());

  RenderRequest roi_request;
  roi_request.view.visible_rect_in_edit_space = {0.5f, 0.0f, 0.5f, 1.0f};
  auto       roi_plan  = GraphCompiler::Compile(document, prepared.CompileSource(), roi_request);
  const auto roi_grade = RenderPreparedGrade(device, document, prepared, roi_plan);
  EXPECT_FALSE(roi_grade.local_tone_rebuilt_reference);
  EXPECT_TRUE(roi_grade.local_tone_sampled_canonical_reference);
  EXPECT_EQ(full_grade.local_tone_reference_resource_id,
            roi_grade.local_tone_reference_resource_id);
  const auto roi_pixels = DownloadGrade(device, roi_grade.output);
  const auto roi_mask   = DownloadLocalTonePlane(device, document.PrimaryGrade()->Id());
  ASSERT_FALSE(roi_pixels.empty());
  ASSERT_EQ(full_mask, roi_mask);

  const auto full_probe =
      TransformPoint(full_plan.geometry.render_to_reference, PixelCenter(48, 32));
  const auto roi_probe = TransformPoint(roi_plan.geometry.reference_to_render, full_probe);
  const auto roi_x     = static_cast<int>(std::floor(roi_probe.x));
  const auto roi_y     = static_cast<int>(std::floor(roi_probe.y));
  const auto roi_w     = static_cast<int>(roi_plan.geometry.render_extent.width);
  const auto roi_h     = static_cast<int>(roi_plan.geometry.render_extent.height);
  ASSERT_GE(roi_x, 0);
  ASSERT_GE(roi_y, 0);
  ASSERT_LT(roi_x, roi_w);
  ASSERT_LT(roi_y, roi_h);
  const float sampled = roi_pixels[static_cast<std::size_t>(roi_y) * roi_w + roi_x].r;
  const float full    = full_pixels[static_cast<std::size_t>(32) * kWidth + 48].r;
  EXPECT_NEAR(sampled, full, 2.0e-3f);

  CudaRenderDevice isolated;
  const auto       isolated_grade = RenderPreparedGrade(isolated, document, prepared, roi_plan);
  EXPECT_TRUE(isolated_grade.local_tone_rebuilt_reference);
  EXPECT_FALSE(isolated_grade.local_tone_sampled_canonical_reference);
  const auto isolated_pixels = DownloadGrade(isolated, isolated_grade.output);
  ASSERT_FALSE(isolated_pixels.empty());
  const float rebuilt = isolated_pixels[static_cast<std::size_t>(roi_y) * roi_w + roi_x].r;
  EXPECT_GT(std::abs(rebuilt - sampled), 1.0e-4f);
}

TEST_F(CudaPrimaryGradeFixture, CudaLutRemapChangesGradePixels) {
  const auto cube_path = std::filesystem::absolute("build/tmp/gpu_dag_cuda_lut/red.cube");
  WriteConstantRgbCube(cube_path, 1.0f, 0.0f, 0.0f);
  auto& lmt = ModelByType<LmtModel>(type_ids::Lmt());
  lmt.SetCubePath(cube_path.string());
  const auto result = Render();
  const auto input  = Download(plan_.develop_output);
  const auto output = Download(result.output);
  ASSERT_FALSE(output.empty());
  ASSERT_EQ(input.size(), output.size());
  EXPECT_NE(result.lut_resource_id, 0U);
  EXPECT_NEAR(output.front().r, 1.0f, 1.0e-4f);
  EXPECT_NEAR(output.front().g, 0.0f, 1.0e-4f);
  EXPECT_NEAR(output.front().b, 0.0f, 1.0e-4f);
  EXPECT_GT(std::abs(output.front().r - input.front().r) + std::abs(output.front().g - input.front().g) +
                std::abs(output.front().b - input.front().b),
            1.0e-3f);
}

TEST_F(CudaPrimaryGradeFixture, CudaLutResourceIsReusedByContentKey) {
  const auto cube_path = std::filesystem::absolute("build/tmp/gpu_dag_cuda_lut/identity.cube");
  WriteIdentityCube(cube_path);
  auto& lmt = ModelByType<LmtModel>(type_ids::Lmt());
  lmt.SetCubePath(cube_path.string());
  const auto first = Render();
  EXPECT_NE(first.lut_resource_id, 0U);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.25f);
  device_.Workspace().Device().ResetCounters();
  const auto second = Render();
  EXPECT_EQ(second.lut_resource_id, first.lut_resource_id);
  EXPECT_EQ(device_.Workspace().Device().LutUploadBytes(), 0U);
  EXPECT_EQ(device_.Workspace().Device().LastLutResourceId(), first.lut_resource_id);
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
