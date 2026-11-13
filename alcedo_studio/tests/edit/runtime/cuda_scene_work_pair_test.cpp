//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "edit/runtime/texture_format.hpp"
#include "multi_grade_runtime_test_support.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
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

class CudaSceneWorkPairFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
  }

  auto Compile(PipelineDocument& document) -> ExecutionPlan {
    return GraphCompiler::Compile(document, prepared_.CompileSource(), RenderRequest{});
  }

  auto Render(PipelineDocument& document, const ExecutionPlan& plan) -> GraphValueId {
    device_.ResetPassStats();
    const auto output = device_.Execute(plan, prepared_, document);
    device_.WaitIdle();
    return output;
  }

  PreparedRawInput prepared_;
  CudaRenderDevice device_;
};

TEST_F(CudaSceneWorkPairFixture, SceneWorkPairOwnsExactlyTwoRgba32fImages) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  const auto plan = Compile(document);
  Render(document, plan);
  auto& pair = device_.Workspace().SceneWork();
  EXPECT_EQ(pair.MemberCount(), 2U);
  EXPECT_EQ(pair.Format(), TextureFormat::Rgba32f);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member0).Width(), plan.geometry.render_extent.width);
  EXPECT_EQ(pair.Member(SceneWorkMember::Member1).Height(), plan.geometry.render_extent.height);
  EXPECT_NE(pair.Member(SceneWorkMember::Member0).ResourceId(),
            pair.Member(SceneWorkMember::Member1).ResourceId());
}

TEST_F(CudaSceneWorkPairFixture, SameExtentRendersReuseSceneWorkAllocations) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  const auto plan = Compile(document);
  Render(document, plan);
  const auto first = device_.Workspace().SceneWork().AllocationCount();
  Render(document, plan);
  Render(document, plan);
  EXPECT_EQ(device_.Workspace().SceneWork().AllocationCount(), first);
}

TEST_F(CudaSceneWorkPairFixture, SceneWorkBytesAreIncludedInResourceMeasurements) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  const auto plan = Compile(document);
  Render(document, plan);
  const auto snapshot = device_.Workspace().CaptureResourceSnapshot();
  const ImageExtent extent{plan.geometry.render_extent.width, plan.geometry.render_extent.height};
  EXPECT_EQ(snapshot.scene_work_member_count, 2U);
  EXPECT_EQ(snapshot.scene_work_used_bytes, SceneWorkImagePair<CudaBackend>::PairBytes(extent));
  EXPECT_GE(snapshot.scene_work_allocation_count, 2U);
}

TEST_F(CudaSceneWorkPairFixture, MultipleGradesAlternateOnlyTwoRgbaWorkImages) {
  auto render_count = [&](std::initializer_list<const char*> extra) {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    if (extra.size() != 0) {
      multi_grade_test::AddCleanGradesBeforeDrt(document, extra);
    }
    const auto plan = Compile(document);
    Render(document, plan);
    EXPECT_EQ(device_.Workspace().SceneWork().MemberCount(), 2U);
    EXPECT_EQ(device_.PassStats().primary_grade_execute, plan.grade_nodes.size());
  };
  render_count({});
  render_count({"grade.b"});
  render_count({"grade.b", "grade.c", "grade.d"});
  render_count({"g1", "g2", "g3", "g4", "g5", "g6", "g7"});
}

TEST_F(CudaSceneWorkPairFixture, RepeatedRenderDoesNotUsePreviousSceneWorkPixels) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(1.0f);
  auto plan = Compile(document);
  Render(document, plan);
  const auto first = DownloadTexture(device_, device_.Workspace().SceneWork().Member(
                                                  SceneWorkMember::Member0));
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(2.0f);
  plan = Compile(document);
  Render(document, plan);
  const auto second = DownloadTexture(device_, device_.Workspace().SceneWork().Member(
                                                   SceneWorkMember::Member0));
  ASSERT_FALSE(first.empty());
  ASSERT_EQ(first.size(), second.size());
  EXPECT_GT(std::abs(second.front().r - first.front().r), 1.0e-4f);
}

TEST_F(CudaSceneWorkPairFixture, DisabledAndZeroMixGradesAliasFrameInputWithoutCopy) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  ASSERT_TRUE(SetColorGradeEnabled(document, NodeId{"grade.primary"}, false).empty());
  const auto plan = Compile(document);
  Render(document, plan);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 1U);
  const auto develop = DownloadTexture(
      device_, device_.Workspace().Images().Find(plan.develop_output)->Texture());
  const auto display =
      DownloadTexture(device_, device_.Workspace().Images().Find(plan.display_output)->Texture());
  ASSERT_FALSE(develop.empty());
  ASSERT_FALSE(display.empty());
}

TEST_F(CudaSceneWorkPairFixture, PointwiseGradeMixPreservesOriginalGradeInput) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(1.0f);
  document.PrimaryGrade()->SetMix(0.5f);
  const auto plan = Compile(document);
  Render(document, plan);
  const auto develop = DownloadTexture(
      device_, device_.Workspace().Images().Find(plan.develop_output)->Texture());
  const auto grade = DownloadTexture(device_, device_.Workspace().SceneWork().Member(
                                                  SceneWorkMember::Member0));
  const float adjusted = multi_grade_test::ApplyExposureAcescc(develop.front().r, 1.0f);
  const float mixed    = develop.front().r + (adjusted - develop.front().r) * 0.5f;
  EXPECT_NEAR(grade.front().r, mixed, 1.0e-5f);
}

TEST_F(CudaSceneWorkPairFixture, GradeOutputsAreNeverPublishedToPersistentCache) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  const auto plan = Compile(document);
  Render(document, plan);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.grade_nodes[0].scene_output), nullptr);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.grade_nodes[1].scene_output), nullptr);
  EXPECT_NE(device_.Workspace().Images().Find(plan.display_output), nullptr);
  EXPECT_NE(device_.Workspace().Images().Find(plan.develop_output), nullptr);
}

TEST_F(CudaSceneWorkPairFixture, FailedFrameDoesNotCreateReusableSceneContent) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(1.0f);
  auto plan = Compile(document);
  Render(document, plan);
  device_.Workspace().Device().FailNextUpload();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(2.0f);
  plan = Compile(document);
  EXPECT_THROW((void)device_.Execute(plan, prepared_, document), std::runtime_error);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.grade_nodes[0].scene_output), nullptr);
}

TEST_F(CudaSceneWorkPairFixture, ExtentChangeRecreatesBothSceneWorkImagesAfterGpuCompletion) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  auto plan     = Compile(document);
  Render(document, plan);
  const auto first_alloc = device_.Workspace().SceneWork().AllocationCount();
  const auto first_id =
      device_.Workspace().SceneWork().Member(SceneWorkMember::Member0).ResourceId();
  prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(7, 5),
                                            gpu_dag_test::FullSensor(7, 5));
  plan      = Compile(document);
  Render(document, plan);
  EXPECT_EQ(device_.Workspace().SceneWork().AllocationCount(), first_alloc + 2U);
  EXPECT_NE(device_.Workspace().SceneWork().Member(SceneWorkMember::Member0).ResourceId(),
            first_id);
  EXPECT_EQ(device_.Workspace().SceneWork().Extent().width, plan.geometry.render_extent.width);
  EXPECT_EQ(device_.Workspace().SceneWork().Extent().height, plan.geometry.render_extent.height);
}

TEST_F(CudaSceneWorkPairFixture, FinalMixReadsAdjustedPixelBeforeOverwritingIt) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(1.0f);
  document.PrimaryGrade()->SetMix(0.25f);
  const auto plan = Compile(document);
  Render(document, plan);
  const auto develop = DownloadTexture(
      device_, device_.Workspace().Images().Find(plan.develop_output)->Texture());
  const auto grade = DownloadTexture(
      device_, device_.Workspace().SceneWork().Member(SceneWorkMember::Member0));
  const float adjusted = multi_grade_test::ApplyExposureAcescc(develop.front().r, 1.0f);
  const float mixed    = develop.front().r + (adjusted - develop.front().r) * 0.25f;
  EXPECT_NEAR(grade.front().r, mixed, 1.0e-5f);
  EXPECT_GT(std::abs(grade.front().r - adjusted), 1.0e-4f);
  EXPECT_GT(std::abs(grade.front().r - develop.front().r), 1.0e-5f);
}

TEST_F(CudaSceneWorkPairFixture, NeighborhoodApplyReadsScratchBeforeInPlaceWrite) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  prepared_ = RawInputLoader::FromDirectRgb(
      multi_grade_test::MakeNeighborhoodRgbaPlane(width, height, 0.18f, 0.55f),
      gpu_dag_test::FullSensor(width, height));
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(0.05f);
  auto* sharpen =
      dynamic_cast<SharpenModel*>(document.Drt()->FindAdjustmentByType(type_ids::Sharpen()));
  ASSERT_NE(sharpen, nullptr);
  sharpen->SetAmount(100.0f);
  sharpen->SetRadius(3.0f);
  sharpen->SetThreshold(0.0f);
  const auto plan = Compile(document);
  Render(document, plan);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.display_output) != nullptr, true);
  const auto display = DownloadTexture(
      device_, device_.Workspace().Images().Find(plan.display_output)->Texture());
  const auto base = DownloadTexture(
      device_, device_.Workspace().SceneWork().Member(SceneWorkMember::Member1));
  const auto center         = static_cast<std::size_t>(height / 2) * width + width / 2;
  const auto neighbor_index = center - 1;
  ASSERT_EQ(display.size(), base.size());
  EXPECT_GT(display[center].r, base[center].r);
  EXPECT_LT(display[neighbor_index].r, base[neighbor_index].r);
}

TEST_F(CudaSceneWorkPairFixture, PostProcessingUsesFreeSceneWorkMemberAndEndsOnDisplay) {
  constexpr std::uint32_t width  = 64;
  constexpr std::uint32_t height = 64;
  prepared_ = RawInputLoader::FromDirectRgb(
      multi_grade_test::MakeNeighborhoodRgbaPlane(width, height, 0.18f, 0.55f),
      gpu_dag_test::FullSensor(width, height));
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(0.05f);
  auto* sharpen =
      dynamic_cast<SharpenModel*>(document.Drt()->FindAdjustmentByType(type_ids::Sharpen()));
  ASSERT_NE(sharpen, nullptr);
  sharpen->SetAmount(80.0f);
  sharpen->SetRadius(2.0f);
  const auto plan = Compile(document);
  Render(document, plan);
  EXPECT_NE(device_.Workspace().Images().Find(plan.display_output), nullptr);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.drt.scene_output), nullptr);
  EXPECT_EQ(device_.Workspace().SceneWork().MemberCount(), 2U);
  const auto display = DownloadTexture(
      device_, device_.Workspace().Images().Find(plan.display_output)->Texture());
  const auto free_member = DownloadTexture(
      device_, device_.Workspace().SceneWork().Member(SceneWorkMember::Member1));
  ASSERT_EQ(display.size(), free_member.size());
  const auto center = static_cast<std::size_t>(height / 2) * width + width / 2;
  EXPECT_GT(display[center].r, free_member[center].r);
}

TEST_F(CudaSceneWorkPairFixture, SceneWorkImagesAreNeverPassedToFrameSink) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  const auto plan = Compile(document);
  const auto display = Render(document, plan);
  EXPECT_EQ(display, plan.display_output);
  EXPECT_NE(device_.Workspace().Images().Find(plan.display_output), nullptr);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.grade_nodes[0].scene_output), nullptr);
  EXPECT_EQ(device_.Workspace().SceneWork().MemberCount(), 2U);
}

TEST_F(CudaSceneWorkPairFixture, FailedFrameDoesNotPublishLocalToneOrDisplayResults) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ShadowsModel>(document, NodeId{"grade.primary"},
                                                 type_ids::Shadows())
      .SetValue(40.0f);
  auto plan = Compile(document);
  Render(document, plan);
  const auto display_rev =
      device_.Workspace().Images().PublishedRevision(plan.display_output);
  const auto llf_rev = device_.Workspace().Images().PublishedRevision(
      LocalToneResultId(document.PrimaryGrade()->Id()));
  ASSERT_NE(display_rev, 0U);
  device_.Workspace().Device().FailNextUpload();
  multi_grade_test::GradeAdjustment<ShadowsModel>(document, NodeId{"grade.primary"},
                                                 type_ids::Shadows())
      .SetValue(80.0f);
  plan = Compile(document);
  EXPECT_THROW((void)device_.Execute(plan, prepared_, document), std::runtime_error);
  EXPECT_EQ(device_.Workspace().Images().PublishedRevision(plan.display_output), display_rev);
  EXPECT_EQ(device_.Workspace().Images().PublishedRevision(
                LocalToneResultId(document.PrimaryGrade()->Id())),
            llf_rev);
  EXPECT_EQ(device_.Workspace().Images().Find(plan.grade_nodes[0].scene_output), nullptr);
}

}  // namespace
}  // namespace alcedo
