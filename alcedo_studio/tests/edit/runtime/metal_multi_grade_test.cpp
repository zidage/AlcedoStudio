//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "multi_grade_runtime_test_support.hpp"

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
  const auto&       texture = lease->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto DownloadWork(MetalRenderDevice& device, SceneWorkMember member) -> std::vector<Rgba> {
  auto&             texture = device.Workspace().SceneWork().Member(member);
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto LastGradeMember(std::size_t grade_count) -> SceneWorkMember {
  return grade_count % 2 == 0 ? SceneWorkMember::Member1 : SceneWorkMember::Member0;
}

auto ResourceIdOf(MetalRenderDevice& device, const GraphValueId& id) -> std::uint64_t {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  return lease == nullptr ? 0 : lease->Texture().ResourceId();
}

auto DownloadR8(MetalRenderDevice& device, const GraphValueId& id) -> std::vector<std::uint8_t> {
  auto* lease = device.Workspace().Images().Find(id);
  EXPECT_NE(lease, nullptr);
  if (lease == nullptr) {
    return {};
  }
  const auto& texture = lease->Texture();
  EXPECT_EQ(texture.Format(), TextureFormat::R8);
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture, std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
      device.CommandContext());
  return pixels;
}

class MetalMultiGradeFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
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

  PreparedRawInput  prepared_;
  MetalRenderDevice device_;
};

}  // namespace

TEST_F(MetalMultiGradeFixture, ZeroGradesFeedDevelopIntoDrtPost) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  const auto plan = Compile(document);
  ASSERT_TRUE(plan.grade_nodes.empty());
  const auto display = Render(document, plan);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 0U);
  EXPECT_EQ(device_.PassStats().drt_execute, 1U);
  EXPECT_EQ(display, plan.display_output);
  const auto develop = Download(device_, plan.develop_output);
  const auto pixels  = Download(device_, plan.display_output);
  ASSERT_FALSE(pixels.empty());
  EXPECT_TRUE(std::isfinite(pixels.front().r));
  EXPECT_NE(pixels.front().r, develop.front().r);
}

TEST_F(MetalMultiGradeFixture, EightGradesUseExactlyTwoNativeRgba32fSceneWorkResources) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(
      document, {"grade.b", "grade.c", "grade.d", "grade.e", "grade.f", "grade.g", "grade.h"});
  const auto plan = Compile(document);
  Render(document, plan);

  const auto& pair = device_.Workspace().SceneWork();
  EXPECT_EQ(plan.grade_nodes.size(), 8U);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 8U);
  EXPECT_EQ(pair.MemberCount(), 2U);
  EXPECT_EQ(pair.Format(), TextureFormat::Rgba32f);
  EXPECT_EQ(pair.AllocationCount(), 2U);
  EXPECT_NE(pair.Member(SceneWorkMember::Member0).ResourceId(),
            pair.Member(SceneWorkMember::Member1).ResourceId());
}

TEST_F(MetalMultiGradeFixture, GradeWithoutPrimaryIdRendersItsParameters) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.primary"}).empty());
  ASSERT_EQ(document.Graph().FindNode("grade.primary"), nullptr);
  ASSERT_EQ(document.PrimaryGrade()->Id(), NodeId{"grade.b"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.b"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  const auto plan = Compile(document);
  ASSERT_EQ(plan.grade_nodes.size(), 1U);
  EXPECT_EQ(plan.grade_nodes.front().node_id, NodeId{"grade.b"});
  Render(document, plan);
  const auto input  = Download(device_, plan.develop_output);
  const auto output = DownloadWork(device_, LastGradeMember(plan.grade_nodes.size()));
  ASSERT_FALSE(output.empty());
  EXPECT_NEAR(output.front().r, multi_grade_test::ApplyExposureAcescc(input.front().r, 1.0f),
              1.0e-5f);
}

TEST_F(MetalMultiGradeFixture, ThreeGradesComposeInEdgeOrder) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                   type_ids::Contrast())
      .SetValue(100.0f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.c"},
                                                   type_ids::Exposure())
      .SetValue(2.0f);
  const auto plan = Compile(document);
  ASSERT_EQ(plan.grade_nodes.size(), 3U);
  Render(document, plan);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 3U);
  const auto develop = Download(device_, plan.develop_output);
  const auto b       = DownloadWork(device_, SceneWorkMember::Member1);
  const auto c       = DownloadWork(device_, SceneWorkMember::Member0);
  ASSERT_FALSE(c.empty());
  const auto after_a =
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f);
  const auto after_b = multi_grade_test::ApplyContrastAcescc(after_a, 100.0f);
  const auto after_c = multi_grade_test::ApplyExposureAcescc(after_b, 2.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(b.front(), after_b, 1.0e-5f));
  EXPECT_TRUE(multi_grade_test::RgbNear(c.front(), after_c, 1.0e-5f));
  const auto swapped = multi_grade_test::ApplyExposureAcescc(
      multi_grade_test::ApplyContrastAcescc(
          multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 2.0f),
          100.0f),
      1.0f);
  EXPECT_GT(std::abs(c.front().r - swapped[0]), 1.0e-4f);
}

TEST_F(MetalMultiGradeFixture, ReconnectChangesNoncommutingGradeResult) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                   type_ids::Contrast())
      .SetValue(100.0f);
  auto plan = Compile(document);
  Render(document, plan);
  const auto  first_order = DownloadWork(device_, LastGradeMember(plan.grade_nodes.size()));
  const auto  develop     = Download(device_, plan.develop_output);
  const auto  expected_ab = multi_grade_test::ApplyContrastAcescc(
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f), 100.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(first_order.front(), expected_ab, 1.0e-5f));

  ASSERT_TRUE(
      ReconnectColorGrade(document, NodeId{"grade.b"}, NodeId{"develop"}, NodeId{"grade.primary"})
          .empty());
  plan = Compile(document);
  EXPECT_EQ(plan.grade_nodes[0].node_id, NodeId{"grade.b"});
  Render(document, plan);
  EXPECT_GE(device_.PassStats().camera_color_skip, 1U);
  const auto  reconnected = DownloadWork(device_, LastGradeMember(plan.grade_nodes.size()));
  const auto  expected_ba = multi_grade_test::ApplyExposureAcescc(
      multi_grade_test::ApplyContrastAcescc(multi_grade_test::Rgb(develop.front()), 100.0f), 1.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(reconnected.front(), expected_ba, 1.0e-5f));
  EXPECT_GT(std::abs(reconnected.front().r - first_order.front().r), 1.0e-4f);
}

TEST_F(MetalMultiGradeFixture, SameAdjustmentTypeUsesDistinctNodeSlots) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.b"},
                                                   type_ids::Exposure())
      .SetValue(2.0f);
  auto plan = Compile(document);
  Render(document, plan);
  const auto first_a = DownloadWork(device_, SceneWorkMember::Member0);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.b"},
                                                   type_ids::Exposure())
      .SetValue(4.0f);
  plan = Compile(document);
  Render(document, plan);
  EXPECT_EQ(device_.PassStats().primary_grade_skip, 0U);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 2U);
  const auto second_a = DownloadWork(device_, SceneWorkMember::Member0);
  const auto second_b = DownloadWork(device_, SceneWorkMember::Member1);
  const auto develop  = Download(device_, plan.develop_output);
  EXPECT_NEAR(second_a.front().r, first_a.front().r, 1.0e-6f);
  EXPECT_NEAR(second_b.front().r,
              multi_grade_test::ApplyExposureAcescc(
                  multi_grade_test::ApplyExposureAcescc(develop.front().r, 1.0f), 4.0f),
              1.0e-5f);
}

TEST_F(MetalMultiGradeFixture, RepeatedAdjustmentInstancesKeepTheirOrder) {
  auto  document = multi_grade_test::MakeIdentityGradeDocument();
  auto* grade    = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, grade->Id(), type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, grade->Id(), type_ids::Contrast())
      .SetValue(100.0f);
  auto extra = BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Exposure());
  ASSERT_NE(extra, nullptr);
  document.InsertAdjustment(grade->Id(), grade->AdjustmentCount(),
                            AdjustmentInstanceId{"grade.primary.exposure.2"}, std::move(extra));
  auto* second_exposure =
      dynamic_cast<ExposureModel*>(&grade->AdjustmentAt(grade->AdjustmentCount() - 1));
  ASSERT_NE(second_exposure, nullptr);
  second_exposure->SetValue(2.0f);
  const auto plan = Compile(document);
  Render(document, plan);
  const auto  develop  = Download(device_, plan.develop_output);
  const auto  output   = DownloadWork(device_, LastGradeMember(plan.grade_nodes.size()));
  // The fixed compile order groups same-type instances at their rank, so the
  // second Exposure applies before Contrast regardless of stored position.
  const auto  expected = multi_grade_test::ApplyContrastAcescc(
      multi_grade_test::ApplyExposureAcescc(
          multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f),
          2.0f),
      100.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(output.front(), expected, 1.0e-5f));
}

TEST_F(MetalMultiGradeFixture, EachGradeMixesAgainstItsOwnInput) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  auto* grade_a = document.PrimaryGrade();
  auto* grade_b = multi_grade_test::GradeNode(document, "grade.b");
  ASSERT_NE(grade_a, nullptr);
  ASSERT_NE(grade_b, nullptr);
  grade_a->SetMix(0.5f);
  grade_b->SetMix(0.25f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, grade_a->Id(), type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, grade_b->Id(), type_ids::Contrast())
      .SetValue(100.0f);

  auto make_fill = [&](float fill, const char* grade_id, const char* mask_id) {
    LinearGradientMaskSource flat;
    flat.start_value         = fill;
    flat.end_value           = fill;
    flat.transition_distance = 1.0f;
    auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{grade_id}));
    ASSERT_NE(grade, nullptr);
    grade_mask_test::AddMask(*grade,
                             grade_mask_test::MakeLinearGradientMask(MaskId{mask_id}, flat));
  };
  make_fill(1.0f, "grade.primary", "mask.a");
  make_fill(128.0f / 255.0f, "grade.b", "mask.b");
  document.MarkTopologyDirty();

  const auto plan = Compile(document);
  ASSERT_TRUE(plan.grade_nodes[0].mask_stack.has_value());
  ASSERT_TRUE(plan.grade_nodes[1].mask_stack.has_value());
  EXPECT_EQ(plan.grade_nodes[0].mask_stack->owner_node_id, NodeId{"grade.primary"});
  EXPECT_EQ(plan.grade_nodes[0].mask_stack->sources.front().mask_id, MaskId{"mask.a"});
  EXPECT_EQ(plan.grade_nodes[1].mask_stack->owner_node_id, NodeId{"grade.b"});
  EXPECT_EQ(plan.grade_nodes[1].mask_stack->sources.front().mask_id, MaskId{"mask.b"});
  device_.ResetPassStats();
  const auto output = device_.Execute(plan, prepared_, document);
  (void)output;
  device_.WaitIdle();
  EXPECT_EQ(device_.PassStats().mask_execute, 2U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 2U);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 2U);
  EXPECT_NE(plan.grade_nodes[0].mask_output, plan.grade_nodes[1].mask_output);
  EXPECT_NE(ResourceIdOf(device_, plan.grade_nodes[0].mask_output),
            ResourceIdOf(device_, plan.grade_nodes[1].mask_output));
  const auto mask_a = DownloadR8(device_, plan.grade_nodes[0].mask_output);
  const auto mask_b = DownloadR8(device_, plan.grade_nodes[1].mask_output);
  ASSERT_FALSE(mask_a.empty());
  ASSERT_FALSE(mask_b.empty());
  EXPECT_EQ(mask_a.front(), 255);
  EXPECT_EQ(mask_b.front(), 128);
  EXPECT_FLOAT_EQ(grade_a->Mix(), 0.5f);
  EXPECT_FLOAT_EQ(grade_b->Mix(), 0.25f);
  const auto  develop = Download(device_, plan.develop_output);
  const auto  a       = DownloadWork(device_, SceneWorkMember::Member0);
  const auto  b       = DownloadWork(device_, SceneWorkMember::Member1);
  const auto  adj_a =
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f);
  const auto out_a =
      multi_grade_test::MixToward(multi_grade_test::Rgb(develop.front()), adj_a, 0.5f, 1.0f);
  const auto adj_b = multi_grade_test::ApplyContrastAcescc(out_a, 100.0f);
  const auto out_b = multi_grade_test::MixToward(out_a, adj_b, 0.25f, 128.0f / 255.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(a.front(), out_a, 1.0e-5f));
  EXPECT_TRUE(multi_grade_test::RgbNear(b.front(), out_b, 2.0e-5f));
  const auto wrong_mix = multi_grade_test::MixToward(multi_grade_test::Rgb(develop.front()), adj_b,
                                                     0.25f, 128.0f / 255.0f);
  EXPECT_GT(std::abs(b.front().r - wrong_mix[0]), 1.0e-4f);
}

TEST_F(MetalMultiGradeFixture, DisabledGradeAliasesInputUntilFinalReader) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.b"},
                                                   type_ids::Exposure())
      .SetValue(4.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.c"},
                                                   type_ids::Contrast())
      .SetValue(100.0f);
  ASSERT_TRUE(SetColorGradeEnabled(document, NodeId{"grade.b"}, false).empty());
  const auto plan = Compile(document);
  Render(document, plan);
  const auto  develop  = Download(device_, plan.develop_output);
  const auto  a        = DownloadWork(device_, SceneWorkMember::Member0);
  const auto  c        = DownloadWork(device_, SceneWorkMember::Member1);
  const auto  expected = multi_grade_test::ApplyContrastAcescc(
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f), 100.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(c.front(), expected, 1.0e-5f));
  EXPECT_TRUE(multi_grade_test::RgbNear(
      a.front(),
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f),
      1.0e-5f));
}

TEST_F(MetalMultiGradeFixture, ZeroMixGradeAliasesInputUntilFinalReader) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.b"},
                                                   type_ids::Exposure())
      .SetValue(4.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.c"},
                                                   type_ids::Contrast())
      .SetValue(100.0f);
  auto* grade_b = multi_grade_test::GradeNode(document, "grade.b");
  ASSERT_NE(grade_b, nullptr);
  grade_b->SetMix(0.0f);
  const auto plan = Compile(document);
  Render(document, plan);
  const auto  develop  = Download(device_, plan.develop_output);
  const auto  a        = DownloadWork(device_, SceneWorkMember::Member0);
  const auto  c        = DownloadWork(device_, SceneWorkMember::Member1);
  const auto  expected = multi_grade_test::ApplyContrastAcescc(
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f), 100.0f);
  EXPECT_TRUE(multi_grade_test::RgbNear(c.front(), expected, 1.0e-5f));
  EXPECT_TRUE(multi_grade_test::RgbNear(
      a.front(),
      multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f),
      1.0e-5f));
}

TEST_F(MetalMultiGradeFixture, TwoLocalToneGradesUseTheirOwnSources) {
  constexpr std::uint32_t width    = 64;
  constexpr std::uint32_t height   = 64;
  auto                    prepared = RawInputLoader::FromDirectRgb(
      multi_grade_test::MakeNeighborhoodRgbaPlane(width, height, 0.02f, 0.08f),
      gpu_dag_test::FullSensor(width, height));
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  multi_grade_test::GradeAdjustment<ShadowsModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Shadows())
      .SetValue(80.0f);
  multi_grade_test::GradeAdjustment<ShadowsModel>(document, NodeId{"grade.b"}, type_ids::Shadows())
      .SetValue(40.0f);
  const auto chained = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  device_.ResetPassStats();
  (void)device_.Execute(chained, prepared, document);
  device_.WaitIdle();
  const auto chained_a = DownloadWork(device_, SceneWorkMember::Member0);
  const auto chained_b = DownloadWork(device_, SceneWorkMember::Member1);
  const auto develop   = Download(device_, chained.develop_output);
  ASSERT_EQ(chained_b.size(), static_cast<std::size_t>(width) * height);
  const std::size_t center = static_cast<std::size_t>(height / 2) * width + width / 2;
  EXPECT_GT(std::abs(chained_a[center].r - develop[center].r), 1.0e-4f);
  EXPECT_GT(std::abs(chained_b[center].r - chained_a[center].r), 1.0e-4f);

  auto isolated = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ShadowsModel>(isolated, NodeId{"grade.primary"},
                                                  type_ids::Shadows())
      .SetValue(40.0f);
  MetalRenderDevice isolated_device;
  const auto        isolated_plan =
      GraphCompiler::Compile(isolated, prepared.CompileSource(), RenderRequest{});
  (void)isolated_device.Execute(isolated_plan, prepared, isolated);
  isolated_device.WaitIdle();
  const auto isolated_pixels = DownloadWork(isolated_device, SceneWorkMember::Member0);
  ASSERT_EQ(isolated_pixels.size(), chained_b.size());
  EXPECT_GT(std::abs(chained_b[center].r - isolated_pixels[center].r), 1.0e-4f);
}

TEST_F(MetalMultiGradeFixture, TwoLutGradesKeepIndependentCubeState) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  const auto red_path  = std::filesystem::absolute("build/tmp/nm2/metal_multi_grade_lut/red.cube");
  const auto blue_path = std::filesystem::absolute("build/tmp/nm2/metal_multi_grade_lut/blue.cube");
  multi_grade_test::WriteConstantRgbCube(red_path, 1.0f, 0.0f, 0.0f);
  multi_grade_test::WriteConstantRgbCube(blue_path, 0.0f, 0.0f, 1.0f);
  multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.primary"}, type_ids::Lmt())
      .SetCubePath(red_path.string());
  multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{"grade.b"}, type_ids::Lmt())
      .SetCubePath(blue_path.string());
  const auto plan = Compile(document);
  Render(document, plan);
  const auto a = DownloadWork(device_, SceneWorkMember::Member0);
  const auto b = DownloadWork(device_, SceneWorkMember::Member1);
  EXPECT_NEAR(a.front().r, 1.0f, 1.0e-4f);
  EXPECT_NEAR(a.front().b, 0.0f, 1.0e-4f);
  EXPECT_NEAR(b.front().r, 0.0f, 1.0e-4f);
  EXPECT_NEAR(b.front().b, 1.0f, 1.0e-4f);
}

TEST_F(MetalMultiGradeFixture, MiddleGradeEditReusesKeyStagesAndReexecutesEveryGrade) {
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                   type_ids::Contrast())
      .SetValue(50.0f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.c"},
                                                   type_ids::Exposure())
      .SetValue(0.5f);
  auto plan = Compile(document);
  Render(document, plan);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                   type_ids::Contrast())
      .SetValue(100.0f);
  plan = Compile(document);
  Render(document, plan);
  EXPECT_EQ(device_.PassStats().camera_color_skip, 1U);
  EXPECT_EQ(device_.PassStats().primary_grade_skip, 0U);
  EXPECT_EQ(device_.PassStats().primary_grade_execute, 3U);
  const auto  develop  = Download(device_, plan.develop_output);
  const auto  output   = DownloadWork(device_, LastGradeMember(plan.grade_nodes.size()));
  const auto  expected = multi_grade_test::ApplyExposureAcescc(
      multi_grade_test::ApplyContrastAcescc(
          multi_grade_test::ApplyExposureAcescc(multi_grade_test::Rgb(develop.front()), 1.0f),
          100.0f),
      0.5f);
  EXPECT_TRUE(multi_grade_test::RgbNear(output.front(), expected, 1.0e-5f));
}

}  // namespace alcedo
