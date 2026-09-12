//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaMaskFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
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
    (void)ExecuteCudaMask(device_, plan_, document_);
    device_.EndRender();
    device_.WaitIdle();
  }

  auto RenderGrade() -> CudaPrimaryGradeResult {
    device_.BeginRender();
    (void)ExecuteCudaDevelop(device_, plan_, prepared_, document_);
    ExecuteCudaGeometryResample(device_, plan_);
    ExecuteCudaCameraColor(device_, plan_, document_);
    (void)ExecuteCudaMask(device_, plan_, document_);
    auto result = ExecuteCudaPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto DownloadMask() -> std::vector<std::uint8_t> {
    return DownloadR8(plan_.FirstGrade()->mask_output);
  }

  auto DownloadR8(const GraphValueId& id) -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) return {};
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_.CommandContext());
    return pixels;
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

  struct Rgba {
    float r, g, b, a;
  };
  auto DownloadImage(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) return {};
    std::vector<Rgba> pixels(static_cast<std::size_t>(lease->Texture().Width()) *
                             lease->Texture().Height());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
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
  CudaRenderDevice device_;
};

TEST_F(CudaMaskFixture, CudaRadialMaskMatchesReferenceSpaceEllipseAtPreviewScales) {
  auto&            node = AttachAnalytic(MaskSourceKind::Radial);
  auto             radial = std::get<RadialMaskSource>(node.source);
  radial.major_radius = 0.3f;
  radial.minor_radius = 0.2f;
  node.source = radial;
  Compile();
  RenderMask();
  const auto full = DownloadMask();
  EXPECT_GT(full[(height_ / 2) * width_ + width_ / 2], 240);
  EXPECT_LT(full.front(), 10);
  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  RenderMask();
  const auto preview = DownloadMask();
  EXPECT_GT(preview[(plan_.geometry.render_extent.height / 2) * plan_.geometry.render_extent.width +
                    plan_.geometry.render_extent.width / 2],
            230);
  EXPECT_LT(preview.front(), 20);
}

TEST_F(CudaMaskFixture, CudaLinearGradientMaskFollowsReferenceSpaceNormal) {
  auto&                    node = AttachAnalytic(MaskSourceKind::LinearGradient);
  LinearGradientMaskSource params;
  params.normal_x            = 0.0f;
  params.normal_y            = 1.0f;
  params.transition_distance = 1.0f;
  node.source = params;
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_GT(pixels[width_ / 2], pixels[(height_ - 1) * width_ + width_ / 2]);
}

TEST_F(CudaMaskFixture, CudaColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne) {
  LinearGradientMaskSource full_coverage;
  full_coverage.start_value         = 1.0f;
  full_coverage.end_value           = 1.0f;
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
  EXPECT_NEAR(output[left].r, source[left].r, 1.0e-6f);
  EXPECT_NEAR(output[right].r, full[right].r, 1.0e-6f);
}

TEST_F(CudaMaskFixture, EmptyMaskListUsesFullGradeCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskEvaluate));
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
  device_.WaitIdle();
  const auto empty_grade = DownloadImage(plan_.FirstGrade()->scene_output);
  const auto empty_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);

  grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"});
  Compile();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
  device_.WaitIdle();
  const auto masked_grade = DownloadImage(plan_.FirstGrade()->scene_output);

  document_.PrimaryGrade()->RemoveMask(MaskId{"mask.radial"});
  Compile();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
  device_.WaitIdle();
  const auto restored_grade = DownloadImage(plan_.FirstGrade()->scene_output);
  const auto restored_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_EQ(empty_grade.size(), restored_grade.size());
  EXPECT_NEAR(empty_grade.front().r, restored_grade.front().r, 1.0e-5f);
  EXPECT_EQ(restored_keys.primary_grade, empty_keys.primary_grade);
  EXPECT_GT(std::abs(empty_grade.front().r - masked_grade.front().r), 1.0e-4f);
}

TEST_F(CudaMaskFixture, AllDisabledMasksUseZeroGradeCoverage) {
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
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
  device_.WaitIdle();
  const auto keys = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(keys.Value(plan_.FirstGrade()->mask_output), AllDisabledMaskUnionKey());
  const auto scene = DownloadImage(plan_.FirstGrade()->scene_input);
  const auto grade = DownloadImage(plan_.FirstGrade()->scene_output);
  ASSERT_EQ(scene.size(), grade.size());
  EXPECT_NEAR(grade.front().r, scene.front().r, 1.0e-5f);
  EXPECT_NEAR(grade[grade.size() / 2].r, scene[scene.size() / 2].r, 1.0e-5f);
}

TEST_F(CudaMaskFixture, OneMaskEditReusesSiblingAndUpstreamResults) {
  RadialMaskSource wide;
  wide.major_radius = 0.45f;
  RadialMaskSource narrow;
  narrow.major_radius = 0.2f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"}, wide);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"}, narrow);
  Compile();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
  device_.WaitIdle();
  const auto before = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  const auto sibling = plan_.FirstGrade()->mask_stack->sources[1].effective_output;
  document_.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"}, 0.4f);
  const auto after = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(after.develop_image, before.develop_image);
  EXPECT_EQ(after.Value(sibling), before.Value(sibling));
  EXPECT_NE(after.mask, before.mask);
  device_.ResetPassStats();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_), plan_.display_output);
  device_.WaitIdle();
  EXPECT_GE(device_.PassStats().camera_color_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_skip, 0U);
}

TEST_F(CudaMaskFixture, EnabledMasksUseMaximumCoverage) {
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
  const auto unified = DownloadMask();
  const auto left_px  = (height_ / 2) * width_ + width_ / 4;
  const auto right_px = (height_ / 2) * width_ + width_ - width_ / 4;
  EXPECT_NEAR(unified[left_px], 180, multi_mask_test::kR8ToleranceCodes);
  EXPECT_NEAR(unified[right_px], 200, multi_mask_test::kR8ToleranceCodes);
  EXPECT_NE(unified[left_px], unified[right_px]);
}

TEST_F(CudaMaskFixture, RadialAndLinearGradientShareUnionRules) {
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

TEST_F(CudaMaskFixture, MaskOpacityAndInvertApplyBeforeUnion) {
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

TEST_F(CudaMaskFixture, CudaMultiMaskUnionMatchesReference) {
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
    grade_mask_test::AddMask(*extra, grade_mask_test::MakeLinearGradientMask(
                                         MaskId{"mask.second"}, gradient));
    document_.MarkTopologyDirty();
    Compile();
    ExecutePlan();
    ASSERT_EQ(plan_.grade_nodes.size(), 2U);
    const auto primary = multi_mask_test::EvaluateEnabledUnionR8(
        document_.PrimaryGrade()->Masks(), plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[0].mask_output), primary);
    const auto second =
        multi_mask_test::EvaluateEnabledUnionR8(extra->Masks(), plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[1].mask_output), second);
    EXPECT_NE(primary, second);
  }
}

}  // namespace
}  // namespace alcedo
