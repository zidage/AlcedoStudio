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
#include <string>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_mask_pass.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

struct GradeFrame {
  OpenClPrimaryGradeResult result;
  std::vector<Rgba>        source;
  std::vector<Rgba>        output;
};

class OpenClMaskFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
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

  auto DownloadMask() -> std::vector<std::uint8_t> {
    auto* lease = device_->Workspace().Images().Find(plan_.FirstGrade()->mask_output);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_->Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_->CommandContext());
    return pixels;
  }

  auto DownloadImage(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_->Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<Rgba> pixels(static_cast<std::size_t>(lease->Texture().Width()) *
                             lease->Texture().Height());
    device_->Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_->CommandContext());
    return pixels;
  }

  auto DownloadBinding(const FrameSceneBinding& binding) -> std::vector<Rgba> {
    if (binding.IsWorkImage()) {
      auto& image = device_->Workspace().SceneWork().Member(binding.member);
      std::vector<Rgba> pixels(static_cast<std::size_t>(image.Width()) * image.Height());
      device_->Workspace().Device().DownloadBufferRange(
          image.Storage(), 0,
          std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                               pixels.size() * sizeof(Rgba)),
          device_->CommandContext());
      return pixels;
    }
    return DownloadImage(binding.graph_id);
  }

  auto DownloadGrade() -> std::vector<Rgba> { return DownloadBinding(last_grade_binding_); }

  auto DownloadR8(const GraphValueId& id) -> std::vector<std::uint8_t> {
    auto* lease = device_->Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_->Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_->CommandContext());
    return pixels;
  }

  void ExecutePlan() {
    ASSERT_EQ(device_->Execute(plan_, prepared_, document_), plan_.display_output);
    device_->WaitIdle();
  }

  void ExpectPrimaryUnionMatchesReference() {
    const auto expected = multi_mask_test::EvaluateEnabledUnionR8(
        document_.PrimaryGrade()->Masks(), plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(DownloadMask(), expected);
  }

  void RenderMask() {
    device_->BeginRender();
    (void)ExecuteOpenClMask(*device_, plan_, document_);
    device_->EndRender();
    device_->WaitIdle();
  }

  auto RenderGrade() -> GradeFrame {
    device_->BeginRender();
    try {
      ExecuteOpenClDevelop(*device_, plan_, prepared_, document_);
      ExecuteOpenClGeometryResample(*device_, plan_);
      ExecuteOpenClCameraColor(*device_, plan_, document_);
      if (plan_.FirstGrade() != nullptr && plan_.FirstGrade()->mask_stack.has_value()) {
        (void)ExecuteOpenClMask(*device_, plan_, document_);
        device_->Workspace().TransientBuffers().Reset();
      }
      auto result = ExecuteOpenClPrimaryGrade(*device_, plan_, prepared_, document_);
      last_grade_binding_ = result.output_binding;
      device_->EndRender();
      device_->WaitIdle();
      GradeFrame frame;
      frame.result = result;
      frame.source = DownloadImage(plan_.develop_output);
      frame.output = DownloadBinding(result.output_binding);
      device_->PublishResults();
      return frame;
    } catch (...) {
      device_->CancelRender();
      throw;
    }
  }

  std::uint32_t                       width_  = 0;
  std::uint32_t                       height_ = 0;
  PreparedRawInput                    prepared_;
  PipelineDocument                    document_;
  ExecutionPlan                       plan_;
  std::unique_ptr<OpenClRenderDevice> device_;
  FrameSceneBinding                   last_grade_binding_{};
};

TEST_F(OpenClMaskFixture, OpenClAnalyticMaskMatchesReferenceAtCropRotationAndDynamicResolution) {
  auto&            node   = AttachAnalytic(MaskSourceKind::Radial);
  auto             radial = std::get<RadialMaskSource>(node.source);
  radial.major_radius = 0.35f;
  radial.minor_radius = 0.25f;
  node.source = radial;
  document_.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  document_.Geometry().SetRotationDegrees(15.0f);
  Compile();
  RenderMask();
  auto check_analytic = [&]() {
    const auto pixels  = DownloadMask();
    float      max_err = 0.0f;
    for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
      for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
        const float expected =
            multi_mask_test::EffectiveCoverageAt(node, plan_.geometry, x, y);
        const float actual =
            pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
        max_err = std::max(max_err, std::fabs(expected - actual));
      }
    }
    EXPECT_LT(max_err, 2.0f / 255.0f);
  };
  check_analytic();

  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  RenderMask();
  check_analytic();
}

TEST_F(OpenClMaskFixture, OpenClLinearGradientMaskFollowsReferenceSpaceNormal) {
  auto&                    node = AttachAnalytic(MaskSourceKind::LinearGradient);
  LinearGradientMaskSource params;
  params.origin_x            = 0.35f;
  params.origin_y            = 0.4f;
  params.normal_x            = 0.6f;
  params.normal_y            = 0.8f;
  params.transition_distance = 0.7f;
  params.start_value         = 0.9f;
  params.end_value           = 0.1f;
  node.source                = params;
  node.invert                = true;
  document_.Geometry().SetCropRect({0.1f, 0.05f, 0.8f, 0.85f});
  document_.Geometry().SetRotationDegrees(12.0f);
  Compile();
  RenderMask();
  const auto pixels  = DownloadMask();
  float      max_err = 0.0f;
  for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
    for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
      const float expected = multi_mask_test::EffectiveCoverageAt(node, plan_.geometry, x, y);
      const float actual =
          pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
      max_err = std::max(max_err, std::fabs(expected - actual));
    }
  }
  EXPECT_LT(max_err, 2.0f / 255.0f);
}

TEST_F(OpenClMaskFixture, OpenClDisconnectedMaskUsesConstantOneWithoutImageAllocation) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  document_.PrimaryGrade()->SetMix(0.5f);
  Compile();
  const auto mixed = RenderGrade();
  EXPECT_EQ(device_->Workspace().Images().Find(plan_.FirstGrade()->mask_output), nullptr);
  document_.PrimaryGrade()->SetMix(1.0f);
  const auto full = RenderGrade();
  ASSERT_EQ(mixed.source.size(), mixed.output.size());
  ASSERT_EQ(full.output.size(), mixed.output.size());
  float max_error = 0.0f;
  for (std::size_t index = 0; index < mixed.output.size(); ++index) {
    max_error = std::max(
        max_error,
        std::fabs(mixed.output[index].r -
                  (mixed.source[index].r + (full.output[index].r - mixed.source[index].r) * 0.5f)));
  }
  EXPECT_LT(max_error, 2.0e-3f);
}

TEST_F(OpenClMaskFixture, OpenClColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne) {
  LinearGradientMaskSource full_coverage;
  full_coverage.start_value         = 1.0f;
  full_coverage.end_value           = 1.0f;
  grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, full_coverage);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  const auto full = RenderGrade();

  LinearGradientMaskSource split;
  split.origin_x            = 0.5f;
  split.normal_x            = 1.0f;
  split.normal_y            = 0.0f;
  split.transition_distance = 0.001f;
  split.start_value         = 0.0f;
  split.end_value           = 1.0f;
  document_.PrimaryGrade()->ReplaceMaskSource(MaskId{"mask.linear"}, split);
  Compile();
  const auto mixed = RenderGrade();
  ASSERT_EQ(mixed.source.size(), mixed.output.size());
  const auto left  = static_cast<std::size_t>(5 * width_ + 2);
  const auto right = static_cast<std::size_t>(5 * width_ + width_ - 2);
  EXPECT_NEAR(mixed.output[left].r, mixed.source[left].r, 1.0e-5f);
  EXPECT_NEAR(mixed.output[right].r, full.output[right].r, 1.0e-5f);
}

TEST_F(OpenClMaskFixture, OpenClPlanExecutorRunsMaskBeforePrimaryGrade) {
  LinearGradientMaskSource full_coverage;
  full_coverage.start_value         = 1.0f;
  full_coverage.end_value           = 1.0f;
  grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, full_coverage);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  ExecutePlan();
  const auto mask = DownloadMask();
  ASSERT_EQ(mask.size(), static_cast<std::size_t>(plan_.geometry.render_extent.width) *
                            plan_.geometry.render_extent.height);
  EXPECT_TRUE(
      std::all_of(mask.begin(), mask.end(), [](std::uint8_t value) { return value == 255; }));
  ASSERT_NE(plan_.FirstGrade(), nullptr);
  last_grade_binding_ = FrameSceneBinding::WorkImage(SceneWorkMember::Member0);
  const auto output   = DownloadGrade();
  EXPECT_FALSE(output.empty());
}

TEST_F(OpenClMaskFixture, EnabledMasksUseMaximumCoverage) {
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
  ExecutePlan();
  ExpectPrimaryUnionMatchesReference();
  const auto unified  = DownloadMask();
  const auto left_px  = (height_ / 2) * width_ + width_ / 4;
  const auto right_px = (height_ / 2) * width_ + width_ - width_ / 4;
  EXPECT_NEAR(unified[left_px], 180, multi_mask_test::kR8ToleranceCodes);
  EXPECT_NEAR(unified[right_px], 200, multi_mask_test::kR8ToleranceCodes);
  EXPECT_NE(unified[left_px], unified[right_px]);
}

TEST_F(OpenClMaskFixture, RadialAndLinearGradientShareUnionRules) {
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

TEST_F(OpenClMaskFixture, MaskOpacityAndInvertApplyBeforeUnion) {
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
  const auto unified = DownloadMask();
  EXPECT_NEAR(unified.front(), 128, multi_mask_test::kR8ToleranceCodes);
  EXPECT_LT(unified.front(), 200);
  EXPECT_NEAR(unified[(height_ / 2) * width_ + width_ / 2], 128,
              multi_mask_test::kR8ToleranceCodes);
}

TEST_F(OpenClMaskFixture, OpenClMultiMaskUnionMatchesReference) {
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
    const auto disabled = DownloadMask();
    EXPECT_TRUE(std::all_of(disabled.begin(), disabled.end(),
                            [](std::uint8_t value) { return value == 0; }));
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
    const auto primary = multi_mask_test::EvaluateEnabledUnionR8(
        document_.PrimaryGrade()->Masks(), plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(DownloadR8(plan_.grade_nodes[0].mask_output),
                                             primary);
    const auto second =
        multi_mask_test::EvaluateEnabledUnionR8(extra->Masks(), plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(DownloadR8(plan_.grade_nodes[1].mask_output),
                                             second);
    EXPECT_NE(primary, second);
  }
}

}  // namespace
}  // namespace alcedo
