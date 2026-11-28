//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// MaskEvaluate, MaskUnion, and the AI Mask coverage on the Vulkan backend (#1036, phase VK11):
// the shared checks of the OpenCL, CUDA, and Metal mask tests against their independent
// references, and the recording, pipeline, memory priority, and failure rules of the passes. The
// comparisons with the reference backend are in vulkan_mask_test.cpp.

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai_mask_gpu_coverage_test_support.hpp"
#include "ai_mask_raster_test_support.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/vulkan/vulkan_mask_pass.hpp"
#include "mask_range_test_support.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_mask_test_support.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using mask_range_reference::ScenePixel;
using namespace vulkan_mask_test;

// ---- The shared checks against the independent references --------------------------------------

class VulkanMaskHarnessTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    device_ = std::make_unique<VulkanRenderDevice>();
  }

  void TearDown() override {
    device_.reset();
    VulkanWorkspaceFixture::TearDown();
  }

  void SetExtent(std::uint32_t width, std::uint32_t height) {
    width_    = width;
    height_   = height;
    prepared_ = MakeInput(width, height);
    document_ = MakeDocument();
  }

  void Compile(RenderRequest request = {}) {
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
  }

  void RenderAllMasks() {
    device_->BeginRender();
    try {
      (void)ExecuteVulkanMask(*device_, plan_, document_);
    } catch (...) {
      device_->CancelRender();
      throw;
    }
    device_->EndRender();
    device_->WaitIdle();
  }

  auto AiMaskHarness() -> ai_mask_test::AiMaskBackendHarness {
    SetExtent(ai_mask_test::kAnalysisExtent.width, ai_mask_test::kAnalysisExtent.height);
    ai_mask_test::AiMaskBackendHarness h;
    h.document      = [this]() -> PipelineDocument& { return document_; };
    h.compile       = [this](RenderRequest request) { Compile(request); };
    h.plan          = [this]() -> const ExecutionPlan& { return plan_; };
    h.render_mask   = [this] { RenderAllMasks(); };
    h.download_mask = [this] { return DownloadR8(*device_, plan_.FirstGrade()->mask_output); };
    h.set_reader    = [this](std::shared_ptr<const MaskAssetReader> reader) {
      device_->Workspace().SetMaskAssets(std::move(reader));
    };
    h.cache_entries   = [this] { return device_->Workspace().MaskCoverage().EntryCount(); };
    h.cache_uploads   = [this] { return device_->Workspace().MaskCoverage().UploadCount(); };
    h.release_session = [this] {
      device_->WaitIdle();
      device_->Workspace().ReleaseSessionResources();
    };
    h.candidate_coverage = [this](const AiMaskCoveragePixels& pixels, Extent2D analysis,
                                  AiMaskEdgeAdjustment edge) {
      return ai_mask_test::AcquireCandidateInCoverageWork(*device_, pixels, analysis, edge);
    };
    h.stored_coverage = [this](const AiSegmentationMaskSource& source) {
      return ai_mask_test::AcquireStoredInCoverageWork(*device_, source);
    };
    h.cache_edge_work  = [this] { return device_->Workspace().MaskCoverage().EdgeWorkCount(); };
    h.cache_used_bytes = [this] { return device_->Workspace().MaskCoverage().UsedBytes(); };
    h.fail_texture_allocation_after = [this](std::uint32_t allocations_to_pass) {
      device_->Workspace().Device().FailTextureAllocationAfter(allocations_to_pass);
    };
    return h;
  }

  auto MaskRangeHarness() -> mask_range_test::MaskRangeBackendHarness {
    SetExtent(mask_range_reference::kSceneExtent.width, mask_range_reference::kSceneExtent.height);
    mask_range_test::MaskRangeBackendHarness h;
    h.backend                = "vulkan";
    h.document               = [this]() -> PipelineDocument& { return document_; };
    h.compile                = [this] { Compile(); };
    h.plan                   = [this]() -> const ExecutionPlan& { return plan_; };
    h.render_mask_with_scene = [this](const MaskId& mask_id, std::span<const ScenePixel> scene) {
      const auto& grade  = *plan_.FirstGrade();
      const auto* source = grade.mask_stack->FindSource(mask_id);
      EXPECT_NE(source, nullptr);
      const auto binding = FrameSceneBinding::CachedImage(
          GraphValueId{NodeId{"test.range_scene"}, PortId{scene.empty() ? "missing" : "image"}});
      device_->BeginRender();
      try {
        if (!scene.empty()) {
          (void)UploadScene(*device_, scene, SceneStorage::kCachedImage, {width_, height_});
        }
        (void)ExecuteVulkanMask(*device_, plan_, document_, grade, *source, binding);
      } catch (...) {
        device_->CancelRender();
        throw;
      }
      device_->EndRender();
      device_->WaitIdle();
      return DownloadR8(*device_, source->effective_output);
    };
    h.set_reader = [this](std::shared_ptr<const MaskAssetReader> reader) {
      device_->Workspace().SetMaskAssets(std::move(reader));
    };
    h.download_r8 = [this](const GraphValueId& id) { return DownloadR8(*device_, id); };
    return h;
  }

  std::uint32_t                       width_  = 0;
  std::uint32_t                       height_ = 0;
  PreparedRawInput                    prepared_;
  PipelineDocument                    document_;
  ExecutionPlan                       plan_;
  std::unique_ptr<VulkanRenderDevice> device_;
};

TEST_F(VulkanMaskHarnessTest, AiMaskCoverageMatchesIndependentReferenceAtFullAndPreviewScale) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskMatchesReferenceAtFullAndPreviewScale(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskCoverageFollowsCropAndRotation) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskFollowsCropAndRotation(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskOutsideTextureBoundsIsZero) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskOutsideTextureBoundsIsZero(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskInvertAndOpacityApplyBeforeUnion) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskInvertAndOpacityApplyBeforeUnion(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskFeatherAndExpandChangeCoverage) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskFeatherAndExpandChangeCoverage(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskCoverageTextureUploadsOncePerAsset) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskCoverageTextureUploadsOncePerAsset(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskMissingAssetFailsRender) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckAiMaskMissingAssetFailsRender(h);
}

TEST_F(VulkanMaskHarnessTest, GpuExpandMatchesAlternatingMorphologyExactly) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckGpuExpandMatchesAlternatingMorphologyExactly(h);
}

TEST_F(VulkanMaskHarnessTest, GpuFeatherMatchesIndependentGaussianWithinOneCode) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckGpuFeatherMatchesIndependentGaussianWithinOneCode(h);
}

TEST_F(VulkanMaskHarnessTest, GpuCoverageIncludesFullMarginAtImageBorders) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckGpuCoverageIncludesFullMarginAtImageBorders(h);
}

TEST_F(VulkanMaskHarnessTest, EdgeChangeDoesNotUploadOriginalAgain) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckEdgeChangeDoesNotUploadOriginalAgain(h);
}

TEST_F(VulkanMaskHarnessTest, FailedEdgeWorkKeepsPublishedCoverage) {
  auto h = AiMaskHarness();
  ai_mask_test::CheckFailedEdgeWorkKeepsPublishedCoverage(h);
}

TEST_F(VulkanMaskHarnessTest, LuminanceAndHueRangesMatchTheDoublePrecisionReference) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckRangesMatchDoublePrecisionReference(h);
}

TEST_F(VulkanMaskHarnessTest, WrappingHueRangeSelectsHuesOnBothSidesOfZero) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckWrappingHueRangeSelectsHuesOnBothSidesOfZero(h);
}

TEST_F(VulkanMaskHarnessTest, FullySelectingRangeMatchesTheMaskWithoutRangesByteForByte) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckFullySelectingRangeMatchesTheMaskWithoutRangesByteForByte(h);
}

TEST_F(VulkanMaskHarnessTest, OnlyRangedMasksReadTheGradeInput) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckOnlyRangedMasksReadTheGradeInput(h);
}

TEST_F(VulkanMaskHarnessTest, DefaultSaturationThresholdKeepsTheFixedChromaGate) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckDefaultMinChromaKeepsTheFixedChromaGate(h);
}

TEST_F(VulkanMaskHarnessTest, RaisedSaturationThresholdDropsLowChromaPixels) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckRaisedMinChromaDropsLowChromaPixels(h);
}

TEST_F(VulkanMaskHarnessTest, AiMaskRangeMatchesReference) {
  auto h = MaskRangeHarness();
  mask_range_test::CheckAiMaskRangeMatchesReference(h);
}

// ---- Recording and failure rules ----------------------------------------------------------------

TEST_F(VulkanMaskHarnessTest, MaskPassesRecordIntoTheOpenRecordingWithoutASubmission) {
  SetExtent(ai_mask_test::kAnalysisExtent.width, ai_mask_test::kAnalysisExtent.height);
  const auto asset =
      ai_mask_test::MakeEllipseAsset(ai_mask_test::kAnalysisExtent, ai_mask_test::kAssetRect);
  auto reader = std::make_shared<ai_mask_test::InMemoryMaskAssetReader>();
  reader->Put(asset);
  device_->Workspace().SetMaskAssets(reader);
  AddMask(document_, ai_mask_test::MakeAiMask(asset, ai_mask_test::kAnalysisExtent, "mask.ai",
                                              AiMaskEdgeAdjustment{2.5F, 2}));
  AddMask(document_, MakeRotatedRadial("mask.rotated"));
  AddMask(document_, MakeDiagonalGradient("mask.gradient"));
  Compile();
  auto&      backend = device_->Workspace().Device();
  const auto submits = backend.SubmitCount();
  const auto waits   = backend.WaitCount();
  const auto before  = vulkan::ReadVulkanApiCounters();
  device_->BeginRender();
  (void)ExecuteVulkanMask(*device_, plan_, document_);
  EXPECT_TRUE(device_->CommandContext().IsRecording());
  EXPECT_EQ(backend.SubmitCount(), submits);
  EXPECT_EQ(backend.WaitCount(), waits);
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  // Three MaskEvaluate dispatches, two Expand steps, two Feather passes, two Union maxima.
  EXPECT_EQ(delta.dispatches, 9U);
  EXPECT_EQ(delta.submits, 0U);
  device_->EndRender();
  device_->WaitIdle();
  device_->PublishResults();
}

TEST_F(VulkanMaskHarnessTest, RepeatedEdgeWorkCreatesNoPipeline) {
  SetExtent(ai_mask_test::kAnalysisExtent.width, ai_mask_test::kAnalysisExtent.height);
  const auto analysis = ai_mask_test::kEdgeAnalysisExtent;
  const auto pixels   = ai_mask_test::MakeEdgeTestCandidate(analysis);
  // The first edge work with Expand and Feather creates the three pipelines.
  (void)ai_mask_test::AcquireCandidateInCoverageWork(*device_, pixels, analysis,
                                                     AiMaskEdgeAdjustment{2.0F, 2});
  const auto before = vulkan::ReadVulkanApiCounters();
  const auto work   = device_->Workspace().MaskCoverage().EdgeWorkCount();
  for (int update = 1; update <= 12; ++update) {
    const AiMaskEdgeAdjustment edge{0.5F * static_cast<float>(update), (update % 5) - 2};
    (void)ai_mask_test::AcquireCandidateInCoverageWork(*device_, pixels, analysis, edge);
  }
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(device_->Workspace().MaskCoverage().EdgeWorkCount(), work + 12);
  EXPECT_EQ(delta.pipeline_creations, 0U);
  EXPECT_EQ(delta.module_loads, 0U);
  EXPECT_GT(delta.pipeline_cache_hits, 0U);
}

TEST_F(VulkanMaskHarnessTest, CoverageEntriesHaveTheCachePriorityAndRenderImagesTheWorkingOne) {
  SetExtent(ai_mask_test::kAnalysisExtent.width, ai_mask_test::kAnalysisExtent.height);
  const auto analysis = ai_mask_test::kAnalysisExtent;
  const auto pixels   = ai_mask_test::MakeEdgeTestCandidate(analysis);
  device_->BeginCoverageWork();
  const auto original = device_->Workspace().AcquireAiMaskCandidateCoverage(
      pixels, analysis, AiMaskEdgeAdjustment{}, device_->CommandContext());
  const auto adjusted = device_->Workspace().AcquireAiMaskCandidateCoverage(
      pixels, analysis, AiMaskEdgeAdjustment{2.0F, 3}, device_->CommandContext());
  device_->EndCoverageWork();
  device_->WaitIdle();
  // The uploaded coverage and the Feather and Expand result are cache entries (plan section 4.2).
  EXPECT_EQ(original->texture.MemoryPriority(), VulkanBackend::kCachePriority);
  EXPECT_EQ(adjusted->texture.MemoryPriority(), VulkanBackend::kCachePriority);
  EXPECT_GT(device_->Workspace().MaskCoverage().WorkTextureCount(), 0U);

  AddMask(document_, MakeRotatedRadial("mask.rotated"));
  Compile();
  RenderAllMasks();
  const auto* mask = device_->Workspace().Images().Find(plan_.FirstGrade()->mask_output);
  ASSERT_NE(mask, nullptr);
  EXPECT_EQ(mask->Texture().MemoryPriority(), VulkanBackend::kWorkingSetPriority);
  device_->PublishResults();
}

TEST_F(VulkanMaskHarnessTest, InvalidRequestsFailAndRecordNothing) {
  SetExtent(64, 48);
  AddMask(document_, MakeRotatedRadial("mask.rotated"));
  auto ranged        = mask_range_test::MakeFullCoverageMask("mask.ranged");
  ranged.color_range = ColorRangeModel{20.0F, 120.0F, 12.0F};
  AddMask(document_, ranged);
  Compile();
  const auto& grade = *plan_.FirstGrade();

  // Outside BeginRender.
  EXPECT_THROW((void)ExecuteVulkanMask(*device_, plan_, document_), std::runtime_error);
  EXPECT_THROW((void)ExecuteVulkanMaskUnion(*device_, plan_, document_, grade), std::runtime_error);
  EXPECT_FALSE(device_->CommandContext().IsRecording());

  device_->BeginRender();
  const auto  before        = vulkan::ReadVulkanApiCounters();
  // A ranged Mask without its Grade input, and with a Grade input of another size.
  const auto* ranged_source = grade.mask_stack->FindSource(MaskId{"mask.ranged"});
  ASSERT_NE(ranged_source, nullptr);
  try {
    (void)ExecuteVulkanMask(*device_, plan_, document_, grade, *ranged_source,
                            FrameSceneBinding::CachedImage(kRangeSceneId));
    ADD_FAILURE() << "a ranged Mask without its Grade input succeeded";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ExecuteVulkanMask: the Grade input of a Mask range"),
              std::string::npos)
        << error.what();
  }
  const std::vector<ScenePixel> small_scene(32 * 24);
  (void)UploadScene(*device_, small_scene, SceneStorage::kCachedImage, {32, 24});
  EXPECT_THROW((void)ExecuteVulkanMask(*device_, plan_, document_, grade, *ranged_source,
                                       FrameSceneBinding::CachedImage(kRangeSceneId)),
               std::runtime_error);
  // The displays with an output of the wrong format or size.
  auto& r8 = device_->Workspace()
                 .AcquireImageForWrite(GraphValueId{NodeId{"test"}, PortId{"r8"}},
                                       {64, 48, TextureFormat::R8})
                 .Texture();
  auto& small_rgba = device_->Workspace()
                         .AcquireImageForWrite(GraphValueId{NodeId{"test"}, PortId{"rgba"}},
                                               {16, 16, TextureFormat::Rgba32f})
                         .Texture();
  EXPECT_THROW(RecordMaskValueDisplay(*device_, r8, small_rgba), std::runtime_error);
  EXPECT_THROW(RecordMaskValueDisplay(*device_, small_rgba, small_rgba), std::runtime_error);
  // An edge dispatch without work and one with missing textures.
  auto& backend = device_->Workspace().Device();
  EXPECT_THROW(backend.RecordAiMaskEdge({}, {}, device_->CommandContext()), std::runtime_error);
  AiMaskEdgeDispatch expand;
  expand.output_extent = {8, 8};
  expand.source_extent = {8, 8};
  expand.expand_steps  = 1;
  EXPECT_THROW(backend.RecordAiMaskEdge(expand, {}, device_->CommandContext()), std::runtime_error);
  AiMaskEdgeDispatch too_wide = expand;
  too_wide.feather_weights.assign(kAiMaskFeatherMaximumRadius + 2, 0.0F);
  EXPECT_THROW(backend.RecordAiMaskEdge(too_wide, {}, device_->CommandContext()),
               std::runtime_error);
  // No failure dispatched a kernel or acquired the output of the failed MaskEvaluate.
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(delta.submits, 0U);
  EXPECT_EQ(device_->Workspace().Images().Find(ranged_source->effective_output), nullptr);
  device_->CancelRender();
}

}  // namespace
}  // namespace alcedo
