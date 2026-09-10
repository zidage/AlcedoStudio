//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/brush_placement.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/grade_mask_coverage.hpp"
#include "edit/mask/mask_id.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_mask_runtime_test_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaParameterizedGradeMixFixture : public ::testing::Test {
 protected:
  static const MaskId kMask;
  static constexpr std::uint32_t kWidth  = 16;
  static constexpr std::uint32_t kHeight = 12;

  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(kWidth, kHeight),
                                              gpu_dag_test::FullSensor(kWidth, kHeight));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
    grade_mask_test::AddParameterizedBrushMask(
        document_, kMask, {grade_mask_test::MakePaintStroke("stroke.1", 8.0f, 6.0f, 3.0f)});
    Compile();
  }

  void Compile() {
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), {});
    ASSERT_NE(plan_.FirstGrade(), nullptr);
    ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  }

  void ExecuteInteractive() {
    ASSERT_EQ(device_.Execute(plan_, prepared_, document_, nullptr, true,
                              TransientAllocationPolicy::SessionPacked),
              plan_.display_output);
    device_.WaitIdle();
  }

  void ExecuteQualityBase() {
    ASSERT_EQ(device_.Execute(plan_, prepared_, document_, nullptr, true,
                              TransientAllocationPolicy::SessionPacked, {},
                              ResultPersistenceScope::SensorDevelopOnly),
              plan_.display_output);
    device_.WaitIdle();
    device_.Workspace().Images().DiscardUnpublished();
  }

  [[nodiscard]] auto MixId() const -> GraphValueId { return plan_.FirstGrade()->mask_output; }

  auto DownloadMix() -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(MixId());
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

  void MoveBrush(Vector2 after) {
    auto* grade = document_.PrimaryGrade();
    ASSERT_NE(grade, nullptr);
    const auto before = grade->BrushPlacementTranslation(kMask);
    grade->SetBrushTranslation(MakeBrushTranslationCommand(
        grade->Id(), kMask, before, after, grade->MaskContentRevision(kMask)));
  }

  auto HostMixPixels() -> std::vector<std::uint8_t> {
    const auto full   = plan_.geometry.full_reference_extent;
    const auto raster = CanonicalBrushRasterExtent(full);
    GradeMaskCoverage coverage;
    coverage.SetGeometry(raster, full);
    const auto* mask = document_.PrimaryGrade()->FindMask(kMask);
    EXPECT_NE(mask, nullptr);
    if (mask == nullptr) {
      return {};
    }
    const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
    EXPECT_NE(brush, nullptr);
    if (brush == nullptr) {
      return {};
    }
    coverage.BindBrushSource(kMask, *brush);
    coverage.EvaluateFull(document_.PrimaryGrade()->Masks());
    const auto pixels = coverage.Pixels();
    return {pixels.begin(), pixels.end()};
  }

  void ExpectOneGradeMixSlot() {
    EXPECT_EQ(device_.Workspace().MaskTextures().EntryCount(), 0U);
    EXPECT_LE(device_.Workspace().ActiveRasterTextures().EntryCount(), 1U);
    EXPECT_NE(device_.Workspace().Images().Find(MixId()), nullptr);
    EXPECT_NE(device_.Workspace().Images().PublishedRevision(MixId()), 0U);
  }

  PreparedRawInput prepared_;
  PipelineDocument document_;
  ExecutionPlan    plan_;
  CudaRenderDevice device_;
};

const MaskId CudaParameterizedGradeMixFixture::kMask{"mask.brush"};

TEST_F(CudaParameterizedGradeMixFixture, CurrentGradeCoverageHasOneRetainedResult) {
  ExecuteInteractive();
  ExpectOneGradeMixSlot();
  const auto published_after_first = device_.Workspace().Images().PublishedCount();
  const auto first_mix             = DownloadMix();
  const auto first_revision        = device_.Workspace().Images().PublishedRevision(MixId());
  ASSERT_EQ(plan_.geometry.render_extent, CanonicalBrushRasterExtent(plan_.geometry.full_reference_extent));
  multi_mask_test::ExpectR8WithinTolerance(first_mix, HostMixPixels());

  MoveBrush({4.0f, 0.0f});
  ExecuteInteractive();
  ExpectOneGradeMixSlot();
  const auto second_mix = DownloadMix();
  EXPECT_NE(second_mix, first_mix);
  EXPECT_NE(device_.Workspace().Images().PublishedRevision(MixId()), first_revision);
  EXPECT_EQ(device_.Workspace().Images().PublishedCount(), published_after_first);
  multi_mask_test::ExpectR8WithinTolerance(second_mix, HostMixPixels());

  MoveBrush({8.0f, 2.0f});
  ExecuteInteractive();
  ExpectOneGradeMixSlot();
  EXPECT_NE(DownloadMix(), second_mix);
  EXPECT_EQ(device_.Workspace().Images().PublishedCount(), published_after_first);
  multi_mask_test::ExpectR8WithinTolerance(DownloadMix(), HostMixPixels());
}

TEST_F(CudaParameterizedGradeMixFixture, QualityMaskEvaluationDoesNotOverwriteInteractiveCache) {
  ExecuteInteractive();
  ExpectOneGradeMixSlot();
  const auto mix_revision = device_.Workspace().Images().PublishedRevision(MixId());
  const auto mix_resource = device_.Workspace().Images().Find(MixId())->Texture().ResourceId();
  const auto mix_pixels   = DownloadMix();
  const auto published    = device_.Workspace().Images().PublishedCount();

  device_.ResetPassStats();
  ExecuteQualityBase();
  EXPECT_GT(device_.PassStats().result_policy_bypass, 0U);
  EXPECT_EQ(device_.Workspace().Images().PublishedRevision(MixId()), mix_revision);
  EXPECT_EQ(device_.Workspace().Images().Find(MixId())->Texture().ResourceId(), mix_resource);
  EXPECT_EQ(device_.Workspace().Images().PublishedCount(), published);
  EXPECT_EQ(DownloadMix(), mix_pixels);
  ExpectOneGradeMixSlot();
}

TEST_F(CudaParameterizedGradeMixFixture, RebuiltMaskMatchesCacheHitPixels) {
  ExecuteInteractive();
  const auto first = DownloadMix();
  ASSERT_FALSE(first.empty());
  ASSERT_EQ(plan_.geometry.render_extent, CanonicalBrushRasterExtent(plan_.geometry.full_reference_extent));
  multi_mask_test::ExpectR8WithinTolerance(first, HostMixPixels());

  device_.ResetPassStats();
  ExecuteInteractive();
  EXPECT_GT(device_.PassStats().mask_union_skip, 0U);
  EXPECT_EQ(DownloadMix(), first);

  device_.Workspace().Images().DropPublished(MixId());
  device_.ResetPassStats();
  ExecuteInteractive();
  EXPECT_GT(device_.PassStats().mask_union_execute, 0U);
  EXPECT_EQ(DownloadMix(), first);
  multi_mask_test::ExpectR8WithinTolerance(DownloadMix(), HostMixPixels());
  ExpectOneGradeMixSlot();
}

}  // namespace
}  // namespace alcedo
