//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_drt_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

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
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto Luma(const Rgba& c) -> float {
  return 0.272229f * c.r + 0.674082f * c.g + 0.053689f * c.b;
}

auto ExtrapolateCurve(float value, const GradeAdjustmentParams& p, std::uint32_t a,
                      std::uint32_t b) -> float {
  const float x0 = p.values[a * 2];
  const float y0 = p.values[a * 2 + 1];
  const float x1 = p.values[b * 2];
  const float y1 = p.values[b * 2 + 1];
  return y0 + (value - x0) * (y1 - y0) / std::max(x1 - x0, 1.0e-6f);
}

auto ApplyCurve(float value, const GradeAdjustmentParams& p) -> float {
  if (p.count < 2) {
    return value;
  }
  if (value <= p.values[0]) {
    return ExtrapolateCurve(value, p, 0, 1);
  }
  for (std::uint32_t i = 1; i < p.count; ++i) {
    const float x1 = p.values[i * 2];
    if (value <= x1) {
      const float x0 = p.values[(i - 1) * 2];
      const float y0 = p.values[(i - 1) * 2 + 1];
      const float y1 = p.values[i * 2 + 1];
      const float t  = (value - x0) / std::max(x1 - x0, 1.0e-6f);
      return y0 + t * (y1 - y0);
    }
  }
  return ExtrapolateCurve(value, p, p.count - 2, p.count - 1);
}

auto ApplyHls(Rgba c, const GradeAdjustmentParams& p) -> Rgba {
  const float maximum = std::max(c.r, std::max(c.g, c.b));
  const float minimum = std::min(c.r, std::min(c.g, c.b));
  const float chroma  = maximum - minimum;
  float       hue     = 0.0f;
  if (chroma > 1.0e-6f) {
    if (maximum == c.r) {
      hue = 60.0f * std::fmod((c.g - c.b) / chroma, 6.0f);
    } else if (maximum == c.g) {
      hue = 60.0f * ((c.b - c.r) / chroma + 2.0f);
    } else {
      hue = 60.0f * ((c.r - c.g) / chroma + 4.0f);
    }
  }
  if (hue < 0.0f) {
    hue += 360.0f;
  }
  const int   bin        = static_cast<int>((hue + 22.5f) / 45.0f) & 7;
  const float luma       = Luma(c);
  const float saturation = 1.0f + p.values[16 + bin];
  c.r                    = luma + (c.r - luma) * saturation;
  c.g                    = luma + (c.g - luma) * saturation;
  c.b                    = luma + (c.b - luma) * saturation;
  const float lightness  = p.values[8 + bin];
  c.r += lightness;
  c.g += lightness;
  c.b += lightness;
  return c;
}

auto CpuApplyAdjustment(Rgba c, const GradeAdjustmentParams& p, std::uint32_t pixel_index) -> Rgba {
  const auto  behavior = static_cast<AdjustmentBehavior>(p.behavior);
  const float value    = p.values[0];
  if (behavior == AdjustmentBehavior::Cat02WhiteBalance && value != 0.0f) {
    const float temperature = p.values[1] * 0.001f;
    const float tint        = p.values[2] * 0.001f;
    c.r *= std::exp2(temperature - tint * 0.5f);
    c.g *= std::exp2(tint);
    c.b *= std::exp2(-temperature - tint * 0.5f);
  } else if (behavior == AdjustmentBehavior::Exposure) {
    const float offset = value / 17.52f;
    c.r += offset;
    c.g += offset;
    c.b += offset;
  } else if (behavior == AdjustmentBehavior::Contrast) {
    const float scale = 1.0f + value * 0.01f;
    c.r               = (c.r - 0.18f) * scale + 0.18f;
    c.g               = (c.g - 0.18f) * scale + 0.18f;
    c.b               = (c.b - 0.18f) * scale + 0.18f;
  } else if (behavior == AdjustmentBehavior::White) {
    const float gain = 1.0f + std::max(value, 0.0f) * 0.005f;
    c.r *= gain;
    c.g *= gain;
    c.b *= gain;
  } else if (behavior == AdjustmentBehavior::Black) {
    const float offset = value * 0.001f;
    c.r += offset;
    c.g += offset;
    c.b += offset;
  } else if (behavior == AdjustmentBehavior::Clarity) {
    const float l      = Luma(c);
    float       weight = 1.0f - std::min(l / std::max(local_tone_mapping::kAcesccMiddleGray, 1.0e-4f),
                                         1.0f);
    weight             = 0.5f - std::fabs(weight - 0.5f);
    const float gain   = 1.0f + value * 0.01f * weight;
    c.r *= gain;
    c.g *= gain;
    c.b *= gain;
  } else if (behavior == AdjustmentBehavior::Curve) {
    c.r = ApplyCurve(c.r, p);
    c.g = ApplyCurve(c.g, p);
    c.b = ApplyCurve(c.b, p);
  } else if (behavior == AdjustmentBehavior::Hls) {
    c = ApplyHls(c, p);
  } else if (behavior == AdjustmentBehavior::Saturation ||
             behavior == AdjustmentBehavior::Vibrance) {
    const float l = Luma(c);
    float scale   = behavior == AdjustmentBehavior::Saturation ? value : 1.0f + value * 0.01f;
    if (behavior == AdjustmentBehavior::Vibrance) {
      const float maximum = std::max(c.r, std::max(c.g, c.b));
      const float minimum = std::min(c.r, std::min(c.g, c.b));
      scale               = 1.0f + (scale - 1.0f) * (1.0f - std::min(maximum - minimum, 1.0f));
    }
    c.r = l + (c.r - l) * scale;
    c.g = l + (c.g - l) * scale;
    c.b = l + (c.b - l) * scale;
  } else if (behavior == AdjustmentBehavior::ColorWheel) {
    const float gamma_x = std::max(p.values[4] + p.values[7], 1.0e-4f);
    const float gamma_y = std::max(p.values[5] + p.values[7], 1.0e-4f);
    const float gamma_z = std::max(p.values[6] + p.values[7], 1.0e-4f);
    c.r = std::copysign(std::pow(std::fabs(c.r + p.values[0] + p.values[3]), 1.0f / gamma_x), c.r) *
          p.values[8];
    c.g = std::copysign(std::pow(std::fabs(c.g + p.values[1] + p.values[3]), 1.0f / gamma_y), c.g) *
          p.values[9];
    c.b = std::copysign(std::pow(std::fabs(c.b + p.values[2] + p.values[3]), 1.0f / gamma_z), c.b) *
          p.values[10];
  } else if (behavior == AdjustmentBehavior::Sharpen) {
    const float l     = Luma(c);
    const float scale = 1.0f + value * 0.0025f;
    c.r               = l + (c.r - l) * scale;
    c.g               = l + (c.g - l) * scale;
    c.b               = l + (c.b - l) * scale;
  } else if (behavior == AdjustmentBehavior::Halation) {
    c.r += std::max(Luma(c) - 0.6f, 0.0f) * value * 0.15f;
  } else if (behavior == AdjustmentBehavior::FilmGrain && value != 0.0f) {
    std::uint32_t hash = pixel_index * 747796405u + 2891336453u;
    hash               = (hash >> ((hash >> 28u) + 4u)) ^ hash;
    const float noise  = (static_cast<float>(hash & 0xffffu) / 32767.5f - 1.0f) * value * 0.02f;
    c.r += noise;
    c.g += noise;
    c.b += noise;
  }
  return c;
}

auto WriteIdentityCube(const std::filesystem::path& path) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n"
      << "0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
      << "0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
}

auto WriteConstantRgbCube(const std::filesystem::path& path, float r, float g, float b) -> void {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE 2\n";
  for (int i = 0; i < 8; ++i) {
    out << r << ' ' << g << ' ' << b << '\n';
  }
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

class MetalGradeFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    ResetProductLookToIdentity(document_);
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), RenderRequest{});
  }

  auto RenderGrade() -> MetalPrimaryGradeResult {
    device_.BeginRender();
    ExecuteMetalDevelop(device_, plan_, prepared_, document_);
    ExecuteMetalGeometryResample(device_, plan_);
    ExecuteMetalCameraColor(device_, plan_, document_);
    auto result = ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto RenderThroughDrtPost() -> MetalDrtResult {
    device_.BeginRender();
    ExecuteMetalDevelop(device_, plan_, prepared_, document_);
    ExecuteMetalGeometryResample(device_, plan_);
    ExecuteMetalCameraColor(device_, plan_, document_);
    (void)ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_);
    auto result = ExecuteMetalDrt(device_, plan_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
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

  PreparedRawInput    prepared_;
  PipelineDocument    document_;
  ExecutionPlan       plan_;
  MetalRenderDevice   device_;
};

TEST_F(MetalGradeFixture, MetalPrimaryGradePreservesCompiledAdjustmentOrder) {
  ASSERT_NE(plan_.FirstGrade(), nullptr);
  ASSERT_FALSE(plan_.FirstGrade()->adjustments.empty());
  ASSERT_EQ(plan_.FirstGrade()->adjustments.size(), document_.PrimaryGrade()->AdjustmentCount());
  for (std::size_t i = 0; i < plan_.FirstGrade()->adjustments.size(); ++i) {
    EXPECT_EQ(plan_.FirstGrade()->adjustments[i].instance_id,
              document_.PrimaryGrade()->AdjustmentIdAt(i));
    EXPECT_EQ(plan_.FirstGrade()->adjustments[i].type,
              document_.PrimaryGrade()->AdjustmentAt(i).Type());
  }
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(1.0f);
  ModelByType<ContrastModel>(type_ids::Contrast()).SetValue(100.0f);
  const auto result = RenderGrade();
  const auto input  = Download(device_, plan_.develop_output);
  const auto output = Download(device_, result.output);
  ASSERT_FALSE(output.empty());
  EXPECT_NEAR(output.front().r, (input.front().r + 1.0f / 17.52f - 0.18f) * 2.0f + 0.18f, 1.0e-5f);
}

TEST_F(MetalGradeFixture, MetalPointwiseAdjustmentsUseOneDispatchPerLlfSegment) {
  const auto identity = RenderGrade();
  EXPECT_EQ(identity.pointwise_dispatch_count, 1U);
  ModelByType<ShadowsModel>(type_ids::Shadows()).SetValue(25.0f);
  const auto split = RenderGrade();
  EXPECT_EQ(split.pointwise_dispatch_count, 2U);
}

TEST_F(MetalGradeFixture, MetalSingleSliderEditUploadsOnlyItsParameterRange) {
  (void)RenderGrade();
  auto& exposure = ModelByType<ExposureModel>(type_ids::Exposure());
  exposure.SetValue(1.0f);
  const ParameterSlotKey exposure_key{document_.PrimaryGrade()->Id(),
                                      AdjustmentInstanceId{"grade.primary.exposure"}};
  const auto             exposure_binding = device_.Workspace().Parameters().Binding(exposure_key);
  device_.Workspace().Device().ResetCounters();
  (void)RenderGrade();
  const auto& ranges = device_.Workspace().Device().LastHostToDeviceRanges();
  EXPECT_TRUE(std::ranges::any_of(ranges, [&](const ByteRange& range) {
    return range.offset == exposure_binding.offset && range.size == exposure_binding.size;
  }));
  EXPECT_FALSE(exposure.IsDirty());
  EXPECT_EQ(device_.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().PipelineCreateCount(), 0U);
  EXPECT_NE(device_.Workspace().Device().GradeCommandTopologyHash(), 0U);
}

TEST_F(MetalGradeFixture, MetalExposureEditRunsOnlyPrimaryGradeAndDrt) {
  (void)device_.Execute(plan_, prepared_, document_);
  device_.WaitIdle();
  device_.ResetPassStats();
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.75f);
  (void)device_.Execute(plan_, prepared_, document_);
  device_.WaitIdle();
  const auto stats = device_.PassStats();
  EXPECT_EQ(stats.source_h2d_count, 0U);
  EXPECT_EQ(stats.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.geometry_execute, 0U);
  EXPECT_EQ(stats.camera_color_execute, 0U);
  EXPECT_EQ(stats.primary_grade_execute, 1U);
  EXPECT_EQ(stats.drt_execute, 1U);
  EXPECT_EQ(stats.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.geometry_skip, 1U);
  EXPECT_EQ(stats.camera_color_skip, 1U);
}

TEST_F(MetalGradeFixture, MetalLutTextureIsReusedByContentKey) {
  const auto cube_path = std::filesystem::absolute("build/tmp/gpu_dag_metal_m3/identity.cube");
  WriteIdentityCube(cube_path);
  auto* lmt = dynamic_cast<LmtModel*>(document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
  ASSERT_NE(lmt, nullptr);
  lmt->SetCubePath(cube_path.string());
  const auto first = RenderGrade();
  EXPECT_NE(first.lut_resource_id, 0U);
  device_.Workspace().Device().ResetCounters();
  const auto second = RenderGrade();
  EXPECT_EQ(second.lut_resource_id, first.lut_resource_id);
  EXPECT_EQ(device_.Workspace().Device().LutUploadBytes(), 0U);
  EXPECT_EQ(device_.Workspace().Device().LastLutResourceId(), first.lut_resource_id);
  EXPECT_EQ(device_.Workspace().Device().BufferCreateCount(), 0U);
}

TEST_F(MetalGradeFixture, MetalLutRemapChangesGradePixels) {
  const auto cube_path = std::filesystem::absolute("build/tmp/gpu_dag_metal_lut/red.cube");
  WriteConstantRgbCube(cube_path, 1.0f, 0.0f, 0.0f);
  auto& lmt = ModelByType<LmtModel>(type_ids::Lmt());
  lmt.SetCubePath(cube_path.string());
  const auto result = RenderGrade();
  const auto input  = Download(device_, plan_.develop_output);
  const auto output = Download(device_, result.output);
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

TEST_F(MetalGradeFixture, MetalDrtPostNeighborhoodPassesReuseWorkspaceImages) {
  ModelByType<ClarityModel>(type_ids::Clarity()).SetValue(20.0f);
  ModelByType<SharpenModel>(type_ids::Sharpen()).SetAmount(10.0f);
  ModelByType<HalationModel>(type_ids::Halation()).SetValue(0.4f);
  ModelByType<FilmGrainModel>(type_ids::FilmGrain()).SetValue(0.3f);
  const auto first = RenderThroughDrtPost();
  EXPECT_EQ(first.post_neighborhood_count, 4U);
  EXPECT_GT(device_.Workspace().Textures().EntryCount(), 0U);
  device_.Workspace().Device().ResetCounters();
  const auto second = RenderThroughDrtPost();
  EXPECT_EQ(second.post_neighborhood_count, 4U);
  EXPECT_EQ(device_.Workspace().Device().PipelineCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().HeapCreateCount(), 0U);
}

TEST_F(MetalGradeFixture, MetalPrimaryGradeMatchesCudaReferenceWithinTolerance) {
  ModelByType<Cat02WhiteBalanceModel>(type_ids::Cat02WhiteBalance()).SetTemperatureOffset(120.0f);
  ModelByType<ExposureModel>(type_ids::Exposure()).SetValue(0.5f);
  ModelByType<ContrastModel>(type_ids::Contrast()).SetValue(40.0f);
  ModelByType<WhiteModel>(type_ids::White()).SetValue(12.0f);
  ModelByType<SaturationModel>(type_ids::Saturation()).SetValue(1.2f);
  const auto result = RenderGrade();
  const auto input  = Download(device_, plan_.develop_output);
  const auto output = Download(device_, result.output);
  ASSERT_EQ(input.size(), output.size());
  ASSERT_FALSE(input.empty());

  std::vector<GradeAdjustmentParams> params;
  auto* grade = document_.PrimaryGrade();
  ASSERT_NE(plan_.FirstGrade(), nullptr);
  for (const auto& compiled : plan_.FirstGrade()->adjustments) {
    auto* model = grade->FindAdjustment(compiled.instance_id);
    ASSERT_NE(model, nullptr);
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value() || IsLocalToneBehavior(*behavior)) {
      continue;
    }
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::Neighborhood &&
        MakeGradeRuntimeParams(*model, *behavior).values[0] == 0.0f) {
      continue;
    }
    params.push_back(MakeGradeRuntimeParams(*model, *behavior));
  }

  float max_err = 0.0f;
  for (std::size_t i = 0; i < input.size(); ++i) {
    Rgba cpu = input[i];
    for (const auto& p : params) {
      cpu = CpuApplyAdjustment(cpu, p, static_cast<std::uint32_t>(i));
    }
    max_err = std::max(max_err, std::fabs(cpu.r - output[i].r));
    max_err = std::max(max_err, std::fabs(cpu.g - output[i].g));
    max_err = std::max(max_err, std::fabs(cpu.b - output[i].b));
  }
  EXPECT_LT(max_err, 2.0e-4f);
}

TEST_F(MetalGradeFixture, MetalUnknownAdjustmentIsRejectedAtInsert) {
  EXPECT_THROW(document_.InsertAdjustment(document_.PrimaryGrade()->Id(),
                                          document_.PrimaryGrade()->AdjustmentCount(),
                                          AdjustmentInstanceId{"grade.primary.tint"},
                                          std::make_unique<TintModel>()),
               std::runtime_error);
}

TEST_F(MetalGradeFixture, MetalPrimaryGradeMissingMetallibThrowsExplicitError) {
  EXPECT_THROW((void)metal::ComputePipelineCache::Instance().GetPipelineState(
                   "/alcedo/missing/primary_grade.metallib", "primary_grade_pointwise",
                   "Metal PrimaryGrade"),
               std::runtime_error);
}

}  // namespace
}  // namespace alcedo
