//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"

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

auto Transform(const Matrix3x3& matrix, float x, float y) -> Vector2 {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

auto CpuAnalytic(const MaskModel& mask, const ResolvedRenderGeometry& geometry, std::uint32_t x,
                 std::uint32_t y) -> float {
  const auto  reference = Transform(geometry.render_to_reference, static_cast<float>(x) + 0.5f,
                                    static_cast<float>(y) + 0.5f);
  const float nx        = reference.x / static_cast<float>(geometry.full_reference_extent.width);
  const float ny        = reference.y / static_cast<float>(geometry.full_reference_extent.height);
  float       value     = 0.0f;
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    const float c      = std::cos(radial->rotation);
    const float s      = std::sin(radial->rotation);
    const float dx     = nx - radial->center_x;
    const float dy     = ny - radial->center_y;
    const float rx     = (c * dx + s * dy) / std::max(radial->major_radius, 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / std::max(radial->minor_radius, 1.0e-6f);
    const float radius = std::sqrt(rx * rx + ry * ry);
    const float inner  = std::max(0.0f, 1.0f - radial->inner_feather);
    const float outer  = 1.0f + radial->outer_feather;
    value =
        1.0f - std::min(std::max((radius - inner) / std::max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
  } else {
    const auto& graduated     = std::get<LinearGradientMaskSource>(mask.source);
    const float normal_length = std::hypot(graduated.normal_x, graduated.normal_y);
    const float normal_x      = graduated.normal_x / std::max(normal_length, 1.0e-6f);
    const float normal_y      = graduated.normal_y / std::max(normal_length, 1.0e-6f);
    const float distance =
        (nx - graduated.origin_x) * normal_x + (ny - graduated.origin_y) * normal_y;
    const float t = std::min(
        std::max(distance / std::max(graduated.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value = graduated.start_value + (graduated.end_value - graduated.start_value) * t;
  }
  if (mask.invert) {
    value = 1.0f - value;
  }
  return value;
}

class MetalMaskFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
    SetExtent(16, 12);
  }

  void SetExtent(std::uint32_t width, std::uint32_t height) {
    width_    = width;
    height_   = height;
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(width, height),
                                              gpu_dag_test::FullSensor(width, height));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
  }

  auto AttachAnalytic(MaskSourceKind kind) -> MaskModel& {
    MaskModel mask;
    mask.id = MaskId{"mask.analytic"};
    if (kind == MaskSourceKind::Radial) {
      mask.source = RadialMaskSource{};
    } else {
      mask.source = LinearGradientMaskSource{};
    }
    auto& result = grade_mask_test::AddMask(*document_.PrimaryGrade(), std::move(mask));
    document_.MarkTopologyDirty();
    return result;
  }

  void Compile(RenderRequest request = {}) {
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
  }

  void RenderMask() {
    device_.BeginRender();
    (void)ExecuteMetalMask(device_, plan_, document_);
    device_.EndRender();
    device_.WaitIdle();
  }

  auto RenderGrade() -> MetalPrimaryGradeResult {
    device_.BeginRender();
    ExecuteMetalDevelop(device_, plan_, prepared_, document_);
    ExecuteMetalGeometryResample(device_, plan_);
    ExecuteMetalCameraColor(device_, plan_, document_);
    if (plan_.FirstGrade() != nullptr && plan_.FirstGrade()->mask_stack.has_value()) {
      (void)ExecuteMetalMask(device_, plan_, document_);
    }
    auto result = ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  void ExecutePlan() {
    ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
    device_.WaitIdle();
  }

  void ExpectPrimaryUnionMatchesReference() {
    const auto expected = multi_mask_test::EvaluateEnabledUnionR8(
        document_.PrimaryGrade()->Masks(), plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(DownloadMask(), expected);
  }

  auto ResourceIdOf(const GraphValueId& id) -> std::uint64_t {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    return lease == nullptr ? 0 : lease->Texture().ResourceId();
  }

  auto DownloadR8(const GraphValueId& id) -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_.CommandContext());
    return pixels;
  }

  auto DownloadMask() -> std::vector<std::uint8_t> {
    return DownloadR8(plan_.FirstGrade()->mask_output);
  }

  auto DownloadImage(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<Rgba> pixels(static_cast<std::size_t>(lease->Texture().Width()) *
                             lease->Texture().Height());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_.CommandContext());
    return pixels;
  }

  auto DownloadWork(SceneWorkMember member) -> std::vector<Rgba> {
    auto& texture = device_.Workspace().SceneWork().Member(member);
    std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
    device_.Workspace().Device().DownloadTexture2D(
        texture,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_.CommandContext());
    return pixels;
  }

  std::uint32_t    width_  = 0;
  std::uint32_t    height_ = 0;
  PreparedRawInput prepared_;
  PipelineDocument document_;
  ExecutionPlan    plan_;
  MetalRenderDevice device_;
};

TEST_F(MetalMaskFixture, MetalMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution) {
  auto& node          = AttachAnalytic(MaskSourceKind::Radial);
  auto  radial        = std::get<RadialMaskSource>(node.source);
  radial.major_radius = 0.35f;
  radial.minor_radius = 0.25f;
  node.source         = radial;
  document_.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  document_.Geometry().SetRotationDegrees(15.0f);
  Compile();
  RenderMask();
  auto check = [&](const std::vector<std::uint8_t>& pixels) {
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(plan_.geometry.render_extent.width) *
                                 plan_.geometry.render_extent.height);
    float max_err = 0.0f;
    for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
      for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
        const float expected = CpuAnalytic(node, plan_.geometry, x, y);
        const float got =
            pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
        max_err = std::max(max_err, std::fabs(expected - got));
      }
    }
    EXPECT_LT(max_err, 2.0f / 255.0f);
  };
  check(DownloadMask());

  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  RenderMask();
  check(DownloadMask());
}

TEST_F(MetalMaskFixture, MetalDisconnectedMaskUsesConstantOneCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  document_.PrimaryGrade()->SetMix(0.5f);
  const auto result = RenderGrade();
  EXPECT_EQ(device_.Workspace().Images().Find(plan_.FirstGrade()->mask_output), nullptr);
  const auto source = DownloadImage(plan_.develop_output);
  const auto mixed  = DownloadImage(result.output);
  document_.PrimaryGrade()->SetMix(1.0f);
  const auto full = DownloadImage(RenderGrade().output);
  ASSERT_EQ(source.size(), mixed.size());
  ASSERT_EQ(full.size(), mixed.size());
  float max_err = 0.0f;
  for (std::size_t i = 0; i < mixed.size(); ++i) {
    max_err =
        std::max(max_err, std::fabs(mixed[i].r - (source[i].r + (full[i].r - source[i].r) * 0.5f)));
  }
  EXPECT_LT(max_err, 2.0e-3f);
}

TEST_F(MetalMaskFixture, MetalNormalMixMatchesCudaReferenceWithinTolerance) {
  LinearGradientMaskSource full_coverage;
  full_coverage.start_value = 1.0f;
  full_coverage.end_value   = 1.0f;
  grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, full_coverage);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  const auto full_result = RenderGrade();
  const auto full        = DownloadImage(full_result.output);

  LinearGradientMaskSource split;
  split.origin_x            = 0.5f;
  split.normal_x            = 1.0f;
  split.normal_y            = 0.0f;
  split.transition_distance = 0.001f;
  split.start_value         = 0.0f;
  split.end_value           = 1.0f;
  document_.PrimaryGrade()->ReplaceMaskSource(MaskId{"mask.linear"}, split);
  Compile();
  const auto mixed  = RenderGrade();
  const auto source = DownloadImage(plan_.develop_output);
  const auto output = DownloadImage(mixed.output);
  ASSERT_EQ(source.size(), output.size());
  const auto left  = 5 * width_ + 2;
  const auto right = 5 * width_ + width_ - 2;
  EXPECT_NEAR(output[left].r, source[left].r, 1.0e-5f);
  EXPECT_NEAR(output[right].r, full[right].r, 1.0e-5f);
}

TEST_F(MetalMaskFixture, MetalMaskWarmupCachesFillZeroAndUnionMax) {
  std::vector<MetalPipelineWarmup> pipelines;
  AppendMetalMaskWarmup(pipelines);
  bool has_fill_zero = false;
  bool has_union_max = false;
  for (const auto& pipeline : pipelines) {
    const auto name =
        std::string_view{pipeline.function_name == nullptr ? "" : pipeline.function_name};
    has_fill_zero = has_fill_zero || name == "mask_fill_zero";
    has_union_max = has_union_max || name == "mask_union_max";
  }
  EXPECT_TRUE(has_fill_zero);
  EXPECT_TRUE(has_union_max);
  ASSERT_FALSE(pipelines.empty());

  auto& backend = device_.Workspace().Device();
  backend.ResetCounters();
  backend.WarmUpPipelines(pipelines);
  EXPECT_EQ(backend.PipelineCreateCount() + backend.PipelineHitCount(), pipelines.size());
  backend.ResetCounters();
  backend.WarmUpPipelines(pipelines);
  EXPECT_EQ(backend.PipelineCreateCount(), 0U);
  EXPECT_EQ(backend.PipelineHitCount(), pipelines.size());
}

TEST_F(MetalMaskFixture, EmptyMaskListUsesFullGradeCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskUnion));
  ExecutePlan();
  const auto empty_grade = DownloadWork(SceneWorkMember::Member0);
  const auto empty_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_TRUE(empty_keys.mask.Empty());
  EXPECT_EQ(device_.Workspace().Images().Find(plan_.FirstGrade()->mask_output), nullptr);

  grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"});
  Compile();
  ExecutePlan();
  const auto masked_grade = DownloadWork(SceneWorkMember::Member0);

  document_.PrimaryGrade()->RemoveMask(MaskId{"mask.radial"});
  Compile();
  ExecutePlan();
  const auto restored_grade = DownloadWork(SceneWorkMember::Member0);
  const auto restored_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_EQ(empty_grade.size(), restored_grade.size());
  EXPECT_NEAR(empty_grade.front().r, restored_grade.front().r, 1.0e-5f);
  EXPECT_EQ(restored_keys.primary_grade, empty_keys.primary_grade);
  EXPECT_GT(std::abs(empty_grade.front().r - masked_grade.front().r), 1.0e-4f);
}

TEST_F(MetalMaskFixture, AllDisabledMasksUseZeroGradeCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"});
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"});
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.a"}, false);
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.z"}, false);
  Compile();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  ASSERT_TRUE(plan_.Contains(GpuPassKind::MaskUnion));
  device_.ResetPassStats();
  ExecutePlan();
  EXPECT_EQ(device_.PassStats().mask_execute, 0U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  const auto keys = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(keys.Value(plan_.FirstGrade()->mask_output), AllDisabledMaskUnionKey());
  const auto coverage = DownloadMask();
  ASSERT_FALSE(coverage.empty());
  EXPECT_TRUE(
      std::all_of(coverage.begin(), coverage.end(), [](std::uint8_t value) { return value == 0; }));
  const auto scene = DownloadImage(plan_.FirstGrade()->scene_input);
  const auto grade = DownloadWork(SceneWorkMember::Member0);
  ASSERT_EQ(scene.size(), grade.size());
  EXPECT_NEAR(grade.front().r, scene.front().r, 1.0e-5f);
  EXPECT_NEAR(grade[grade.size() / 2].r, scene[scene.size() / 2].r, 1.0e-5f);
}

TEST_F(MetalMaskFixture, SingleEnabledMaskUnionAliasesSourceTexture) {
  RadialMaskSource full;
  full.major_radius = 2.0f;
  full.minor_radius = 2.0f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"}, full);
  Compile();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  ExecutePlan();
  const auto source_id = plan_.FirstGrade()->mask_stack->sources.front().effective_output;
  const auto union_id  = plan_.FirstGrade()->mask_output;
  EXPECT_EQ(ResourceIdOf(union_id), ResourceIdOf(source_id));
  const auto coverage = DownloadR8(union_id);
  ASSERT_FALSE(coverage.empty());
  EXPECT_TRUE(
      std::all_of(coverage.begin(), coverage.end(), [](std::uint8_t value) { return value == 255; }));
}

TEST_F(MetalMaskFixture, EnabledMasksUseMaximumCoverage) {
  RadialMaskSource left;
  left.center_x     = 0.25f;
  left.major_radius = 0.2f;
  left.minor_radius = 0.2f;
  RadialMaskSource right;
  right.center_x     = 0.75f;
  right.major_radius = 0.2f;
  right.minor_radius = 0.2f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"}, left);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"}, right);
  document_.PrimaryGrade()->FindMask(MaskId{"mask.a"})->opacity = 180.0f / 255.0f;
  document_.PrimaryGrade()->FindMask(MaskId{"mask.z"})->opacity = 200.0f / 255.0f;
  Compile();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  ASSERT_EQ(plan_.FirstGrade()->mask_stack->sources.size(), 2U);
  device_.ResetPassStats();
  ExecutePlan();
  EXPECT_EQ(device_.PassStats().mask_execute, 2U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  const auto source_a = plan_.FirstGrade()->mask_stack->sources[0].effective_output;
  const auto source_z = plan_.FirstGrade()->mask_stack->sources[1].effective_output;
  const auto union_id = plan_.FirstGrade()->mask_output;
  EXPECT_NE(ResourceIdOf(union_id), ResourceIdOf(source_a));
  EXPECT_NE(ResourceIdOf(union_id), ResourceIdOf(source_z));
  EXPECT_NE(ResourceIdOf(source_a), ResourceIdOf(source_z));
  const auto a       = DownloadR8(source_a);
  const auto z       = DownloadR8(source_z);
  const auto unified = DownloadR8(union_id);
  ASSERT_EQ(a.size(), z.size());
  ASSERT_EQ(unified.size(), a.size());
  for (std::size_t i = 0; i < unified.size(); ++i) {
    EXPECT_EQ(unified[i], std::max(a[i], z[i]));
  }
  const auto left_px  = (height_ / 2) * width_ + width_ / 4;
  const auto right_px = (height_ / 2) * width_ + width_ - width_ / 4;
  EXPECT_NEAR(unified[left_px], 180, multi_mask_test::kR8ToleranceCodes);
  EXPECT_NEAR(unified[right_px], 200, multi_mask_test::kR8ToleranceCodes);
  ExpectPrimaryUnionMatchesReference();
}

TEST_F(MetalMaskFixture, OneMaskEditReusesSiblingAndUpstreamResults) {
  RadialMaskSource wide;
  wide.major_radius = 0.45f;
  RadialMaskSource narrow;
  narrow.major_radius = 0.2f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"}, wide);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"}, narrow);
  Compile();
  ExecutePlan();
  const auto before = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  const auto sibling = plan_.FirstGrade()->mask_stack->sources[1].effective_output;
  document_.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"}, 0.4f);
  const auto after = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(after.develop_image, before.develop_image);
  EXPECT_EQ(after.Value(sibling), before.Value(sibling));
  EXPECT_NE(after.mask, before.mask);
  device_.ResetPassStats();
  ExecutePlan();
  EXPECT_GE(device_.PassStats().camera_color_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_skip, 0U);
}

TEST_F(MetalMaskFixture, RadialAndLinearGradientShareUnionRules) {
  RadialMaskSource radial;
  radial.major_radius = 0.25f;
  radial.minor_radius = 0.25f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"}, radial);
  LinearGradientMaskSource gradient;
  gradient.transition_distance = 1.0f;
  grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, gradient);
  Compile();
  ExecutePlan();
  const auto combined = DownloadMask();
  ExpectPrimaryUnionMatchesReference();
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.linear"}, false);
  Compile();
  ExecutePlan();
  const auto radial_only = DownloadMask();
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.radial"}, false);
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.linear"}, true);
  Compile();
  ExecutePlan();
  const auto linear_only = DownloadMask();
  ASSERT_EQ(combined.size(), radial_only.size());
  for (std::size_t i = 0; i < combined.size(); ++i) {
    const auto separate_max =
        static_cast<std::uint8_t>(std::max(radial_only[i], linear_only[i]));
    EXPECT_NEAR(combined[i], separate_max, multi_mask_test::kR8ToleranceCodes) << "index " << i;
  }
}

TEST_F(MetalMaskFixture, MaskOpacityAndInvertApplyBeforeUnion) {
  RadialMaskSource radial;
  radial.major_radius = 0.45f;
  radial.minor_radius = 0.45f;
  auto& inverted = grade_mask_test::AddRadialMask(document_, MaskId{"mask.invert"}, radial, true);
  inverted.opacity = 0.25f;
  LinearGradientMaskSource flat;
  flat.start_value         = 128.0f / 255.0f;
  flat.end_value           = 128.0f / 255.0f;
  flat.transition_distance = 1.0f;
  grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.flat"}, flat);
  Compile();
  ExecutePlan();
  ExpectPrimaryUnionMatchesReference();
  EXPECT_NEAR(DownloadMask().front(), 128, multi_mask_test::kR8ToleranceCodes);
}

TEST_F(MetalMaskFixture, MetalMultiMaskUnionMatchesReference) {
  {
    SCOPED_TRACE("empty list");
    Compile();
    EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskEvaluate));
    ExecutePlan();
  }
  {
    SCOPED_TRACE("three disabled");
    SetExtent(16, 12);
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"});
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.b"});
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.c"});
    document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.a"}, false);
    document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.b"}, false);
    document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.c"}, false);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("one radial");
    SetExtent(16, 12);
    RadialMaskSource radial;
    radial.major_radius = 0.35f;
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"}, radial);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("one linear gradient");
    SetExtent(16, 12);
    LinearGradientMaskSource gradient;
    gradient.transition_distance = 0.8f;
    grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, gradient);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("two source kinds");
    SetExtent(16, 12);
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"});
    grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"});
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("two grades");
    SetExtent(16, 12);
    multi_grade_test::AddCleanGradesBeforeDrt(document_, {"grade.b"});
    RadialMaskSource wide;
    wide.major_radius = 0.4f;
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.primary"}, wide);
    auto* extra = multi_grade_test::GradeNode(document_, "grade.b");
    ASSERT_NE(extra, nullptr);
    LinearGradientMaskSource gradient;
    gradient.transition_distance = 1.0f;
    grade_mask_test::AddMask(
        *extra, grade_mask_test::MakeLinearGradientMask(MaskId{"mask.second"}, gradient));
    document_.MarkTopologyDirty();
    Compile();
    ExecutePlan();
    ASSERT_EQ(plan_.grade_nodes.size(), 2U);
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[0].mask_output),
        multi_mask_test::EvaluateEnabledUnionR8(document_.PrimaryGrade()->Masks(),
                                                plan_.geometry));
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[1].mask_output),
        multi_mask_test::EvaluateEnabledUnionR8(extra->Masks(), plan_.geometry));
  }
}

}  // namespace
}  // namespace alcedo
