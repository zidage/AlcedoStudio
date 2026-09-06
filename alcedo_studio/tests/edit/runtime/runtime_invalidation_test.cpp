//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/runtime_invalidation.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "multi_grade_runtime_test_support.hpp"

namespace alcedo {
namespace {

auto MakePrepared() -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                       gpu_dag_test::FullSensor(16, 12));
}

void ConsumeOperatorDirty(PipelineDocument& document) {
  if (auto* develop = document.Develop()) {
    (void)develop->Params().TakeDirtyPatch();
  }
  if (auto* drt = document.Drt()) {
    (void)drt->Params().TakeDirtyPatch();
    for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
      (void)drt->AdjustmentAt(index).TakeDirtyPatch();
    }
  }
  for (const auto& node : document.Graph().Nodes()) {
    auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node->Id()));
    if (grade == nullptr) {
      continue;
    }
    for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
      (void)grade->AdjustmentAt(index).TakeDirtyPatch();
    }
  }
}

struct ValidityHarness {
  PreparedRawInput           prepared = MakePrepared();
  PipelineDocument           document = CreateDefaultPipelineDocument();
  ExecutionPlan              plan{};
  RuntimeInvalidationState   invalidation{};

  ValidityHarness() {
    gpu_dag_test::EnsureTestCameraProfile(document);
    plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  }

  void Recompile(const RenderRequest& request = {}) {
    plan = GraphCompiler::Compile(document, prepared.CompileSource(), request);
    GraphCompiler::BindFrameGeometry(plan, document, request);
  }

  void Collect(std::span<const ActiveRasterMaskInput> rasters = {}) {
    invalidation.CollectAndPropagate(plan, document, prepared, rasters);
  }

  void Complete() {
    const ImageExtent sensor{plan.source.develop_output_extent.width,
                             plan.source.develop_output_extent.height};
    const ImageExtent geometry{plan.geometry.render_extent.width,
                               plan.geometry.render_extent.height};
    auto mark = [&](const GraphValueId& id, ImageExtent extent,
                    TextureFormat format = TextureFormat::Rgba32f) {
      invalidation.MarkCompleted(id, invalidation.MakeImageRepresentation(id, extent, format));
    };
    mark(plan.sensor_linear_output, sensor);
    mark(plan.geometry_output, geometry);
    mark(plan.develop_output, geometry);
    for (const auto& grade : plan.grade_nodes) {
      mark(LocalToneSourceId(grade.node_id), geometry, TextureFormat::R32f);
      mark(LocalToneResultId(grade.node_id), geometry, TextureFormat::R32f);
      mark(grade.scene_output, geometry);
      if (!grade.mask_stack.has_value()) {
        continue;
      }
      for (const auto& source : grade.mask_stack->sources) {
        mark(source.effective_output, geometry, TextureFormat::R8);
      }
      mark(grade.mask_output, geometry, TextureFormat::R8);
    }
    mark(plan.display_output, geometry);
  }

  void PublishCurrent() {
    Collect();
    ConsumeOperatorDirty(document);
    Complete();
  }

  [[nodiscard]] auto Required(const GraphValueId& id) const -> RuntimeRevision {
    return invalidation.RequiredRevision(id);
  }
  [[nodiscard]] auto Completed(const GraphValueId& id) const -> RuntimeRevision {
    return invalidation.CompletedRevision(id);
  }
  [[nodiscard]] auto Current(const GraphValueId& id) const -> bool {
    return Required(id) != 0 && Required(id) == Completed(id);
  }

  [[nodiscard]] auto Primary() const -> const CompiledGradeNode& { return *plan.FirstGrade(); }
};

TEST(RuntimeInvalidation, FirstCollectAssignsRequiredRevisionsAndDirtyConsumeLeavesThemAhead) {
  ValidityHarness harness;
  harness.Collect();
  EXPECT_GT(harness.Required(harness.plan.sensor_linear_output), 0U);
  EXPECT_EQ(harness.Completed(harness.plan.sensor_linear_output), 0U);
  ConsumeOperatorDirty(harness.document);
  EXPECT_GT(harness.Required(harness.plan.sensor_linear_output),
            harness.Completed(harness.plan.sensor_linear_output));
  EXPECT_FALSE(harness.document.Develop()->Params().IsDirty());
}

TEST(RuntimeInvalidation, FreshStateAssignsRequiredWhenOperatorDirtyAlreadyConsumed) {
  ValidityHarness first;
  first.PublishCurrent();
  ASSERT_FALSE(first.document.Develop()->Params().IsDirty());
  ASSERT_GT(first.Required(first.plan.sensor_linear_output), 0U);

  RuntimeInvalidationState fresh;
  fresh.CollectAndPropagate(first.plan, first.document, first.prepared, {});
  EXPECT_GT(fresh.RequiredRevision(first.plan.sensor_linear_output), 0U);
  EXPECT_EQ(fresh.CompletedRevision(first.plan.sensor_linear_output), 0U);
  EXPECT_GT(fresh.RequiredRevision(first.plan.display_output), 0U);
}

TEST(RuntimeInvalidation, UnchangedSetValueAndSecondCollectKeepPublishedResultsValid) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto sensor = harness.Required(harness.plan.sensor_linear_output);
  const auto grade  = harness.Required(harness.Primary().scene_output);
  auto* exposure    = dynamic_cast<ExposureModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(exposure->Value());
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.plan.sensor_linear_output), sensor);
  EXPECT_EQ(harness.Required(harness.Primary().scene_output), grade);
  EXPECT_TRUE(harness.Current(harness.plan.sensor_linear_output));
  EXPECT_TRUE(harness.Current(harness.Primary().scene_output));
}

TEST(RuntimeInvalidation, PreLlfExposureInvalidatesLlfSourceResultAndDownstream) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto sensor_before = harness.Required(harness.plan.sensor_linear_output);
  const auto source_id     = LocalToneSourceId(harness.Primary().node_id);
  const auto result_id     = LocalToneResultId(harness.Primary().node_id);
  auto* exposure           = dynamic_cast<ExposureModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(exposure->Value() + 0.25f);
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.plan.sensor_linear_output), sensor_before);
  EXPECT_TRUE(harness.Current(harness.plan.sensor_linear_output));
  EXPECT_GT(harness.Required(source_id), harness.Completed(source_id));
  EXPECT_GT(harness.Required(result_id), harness.Completed(result_id));
  EXPECT_GT(harness.Required(harness.Primary().scene_output),
            harness.Completed(harness.Primary().scene_output));
  EXPECT_GT(harness.Required(harness.plan.display_output),
            harness.Completed(harness.plan.display_output));
}

TEST(RuntimeInvalidation, LlfSliderKeepsSourceAndInvalidatesResult) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto source_id = LocalToneSourceId(harness.Primary().node_id);
  const auto result_id = LocalToneResultId(harness.Primary().node_id);
  auto* shadows        = dynamic_cast<ShadowsModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(40.0f);
  harness.Collect();
  EXPECT_TRUE(harness.Current(source_id));
  EXPECT_GT(harness.Required(result_id), harness.Completed(result_id));
  EXPECT_GT(harness.Required(harness.Primary().scene_output),
            harness.Completed(harness.Primary().scene_output));
}

TEST(RuntimeInvalidation, PostLlfSaturationAndMixKeepLlfAndInvalidateGradeOutput) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto source_id = LocalToneSourceId(harness.Primary().node_id);
  const auto result_id = LocalToneResultId(harness.Primary().node_id);
  auto* saturation     = dynamic_cast<SaturationModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation()));
  ASSERT_NE(saturation, nullptr);
  saturation->SetValue(1.1f);
  harness.Collect();
  EXPECT_TRUE(harness.Current(source_id));
  EXPECT_TRUE(harness.Current(result_id));
  EXPECT_GT(harness.Required(harness.Primary().scene_output),
            harness.Completed(harness.Primary().scene_output));

  harness.PublishCurrent();
  harness.document.PrimaryGrade()->SetMix(0.4f);
  harness.Collect();
  EXPECT_TRUE(harness.Current(source_id));
  EXPECT_TRUE(harness.Current(result_id));
  EXPECT_GT(harness.Required(harness.Primary().scene_output),
            harness.Completed(harness.Primary().scene_output));
}

TEST(RuntimeInvalidation, WhiteBalanceInvalidatesDevelopAndDownstreamLlf) {
  ValidityHarness harness;
  harness.PublishCurrent();
  auto payload        = harness.document.Develop()->Params().Params();
  payload.wb_mode     = "custom";
  payload.custom_cct  = 4800.0f;
  harness.document.Develop()->Params().ReplaceParams(payload);
  harness.Collect();
  EXPECT_TRUE(harness.Current(harness.plan.sensor_linear_output));
  EXPECT_TRUE(harness.Current(harness.plan.geometry_output));
  EXPECT_GT(harness.Required(harness.plan.develop_output),
            harness.Completed(harness.plan.develop_output));
  EXPECT_GT(harness.Required(LocalToneSourceId(harness.Primary().node_id)),
            harness.Completed(LocalToneSourceId(harness.Primary().node_id)));
}

TEST(RuntimeInvalidation, DemosaicInvalidatesSensorAndDownstreamLlf) {
  ValidityHarness harness;
  harness.PublishCurrent();
  auto payload             = harness.document.Develop()->Params().Params();
  payload.demosaic_method  = payload.demosaic_method == "rcd" ? "dht" : "rcd";
  harness.document.Develop()->Params().ReplaceParams(payload);
  harness.Collect();
  EXPECT_GT(harness.Required(harness.plan.sensor_linear_output),
            harness.Completed(harness.plan.sensor_linear_output));
  EXPECT_GT(harness.Required(LocalToneSourceId(harness.Primary().node_id)),
            harness.Completed(LocalToneSourceId(harness.Primary().node_id)));
}

TEST(RuntimeInvalidation, HighlightReconstructionInvalidatesSensorAndDownstreamLlf) {
  ValidityHarness harness;
  harness.PublishCurrent();
  auto payload                    = harness.document.Develop()->Params().Params();
  payload.highlights_reconstruct  = !payload.highlights_reconstruct;
  harness.document.Develop()->Params().ReplaceParams(payload);
  harness.Collect();
  EXPECT_GT(harness.Required(harness.plan.sensor_linear_output),
            harness.Completed(harness.plan.sensor_linear_output));
  EXPECT_GT(harness.Required(LocalToneSourceId(harness.Primary().node_id)),
            harness.Completed(LocalToneSourceId(harness.Primary().node_id)));
}

TEST(RuntimeInvalidation, LensCorrectionInvalidatesSensorAndDownstreamLlf) {
  ValidityHarness harness;
  harness.PublishCurrent();
  auto payload          = harness.document.Develop()->Params().Params();
  payload.lens_enabled  = !payload.lens_enabled;
  harness.document.Develop()->Params().ReplaceParams(payload);
  harness.Collect();
  EXPECT_GT(harness.Required(harness.plan.sensor_linear_output),
            harness.Completed(harness.plan.sensor_linear_output));
  EXPECT_GT(harness.Required(LocalToneSourceId(harness.Primary().node_id)),
            harness.Completed(LocalToneSourceId(harness.Primary().node_id)));
}

TEST(RuntimeInvalidation, MiddleGradeEditLeavesUpstreamValid) {
  ValidityHarness harness;
  multi_grade_test::AddCleanGradesBeforeDrt(harness.document, {"grade.mid", "grade.tail"});
  harness.Recompile();
  harness.PublishCurrent();
  ASSERT_EQ(harness.plan.grade_nodes.size(), 3U);
  const auto& upstream = harness.plan.grade_nodes[0];
  const auto& middle   = harness.plan.grade_nodes[1];
  const auto& tail     = harness.plan.grade_nodes[2];
  auto* exposure       = dynamic_cast<ExposureModel*>(
      multi_grade_test::GradeNode(harness.document, middle.node_id.Value())
          ->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.5f);
  harness.Collect();
  EXPECT_TRUE(harness.Current(harness.plan.sensor_linear_output));
  EXPECT_TRUE(harness.Current(upstream.scene_output));
  EXPECT_TRUE(harness.Current(LocalToneSourceId(upstream.node_id)));
  EXPECT_GT(harness.Required(LocalToneSourceId(middle.node_id)),
            harness.Completed(LocalToneSourceId(middle.node_id)));
  EXPECT_GT(harness.Required(middle.scene_output), harness.Completed(middle.scene_output));
  EXPECT_GT(harness.Required(LocalToneSourceId(tail.node_id)),
            harness.Completed(LocalToneSourceId(tail.node_id)));
  EXPECT_GT(harness.Required(tail.scene_output), harness.Completed(tail.scene_output));
}

TEST(RuntimeInvalidation, SiblingMaskSourceStaysValidWhenOneMaskChanges) {
  ValidityHarness harness;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.a"});
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.b"});
  harness.Recompile();
  harness.PublishCurrent();
  ASSERT_TRUE(harness.Primary().mask_stack.has_value());
  ASSERT_EQ(harness.Primary().mask_stack->sources.size(), 2U);
  const auto first  = harness.Primary().mask_stack->sources[0].effective_output;
  const auto second = harness.Primary().mask_stack->sources[1].effective_output;
  harness.document.PrimaryGrade()->SetMaskOpacity(harness.Primary().mask_stack->sources[0].mask_id,
                                                  0.4f);
  harness.Collect();
  EXPECT_GT(harness.Required(first), harness.Completed(first));
  EXPECT_TRUE(harness.Current(second));
  EXPECT_GT(harness.Required(harness.Primary().mask_output),
            harness.Completed(harness.Primary().mask_output));
  EXPECT_TRUE(harness.Current(LocalToneSourceId(harness.Primary().node_id)));
}

TEST(RuntimeInvalidation, ActiveRasterRevisionInvalidatesOnlyThatMaskSource) {
  ValidityHarness harness;
  grade_mask_test::AddBrushMask(harness.document, MaskId{"mask.brush"}, MaskAssetKey{"asset.a"});
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"});
  harness.Recompile();
  ASSERT_TRUE(harness.Primary().mask_stack.has_value());
  MaskId brush_id;
  MaskId radial_id;
  GraphValueId brush_out{};
  GraphValueId radial_out{};
  for (const auto& source : harness.Primary().mask_stack->sources) {
    if (source.mask_id == MaskId{"mask.brush"}) {
      brush_id  = source.mask_id;
      brush_out = source.effective_output;
    } else {
      radial_id  = source.mask_id;
      radial_out = source.effective_output;
    }
  }
  ActiveRasterMaskInput raster;
  raster.owner_node_id     = harness.Primary().node_id;
  raster.mask_id           = brush_id;
  raster.session_generation = 1;
  raster.content_revision   = 1;
  harness.Collect(std::span<const ActiveRasterMaskInput>{&raster, 1});
  ConsumeOperatorDirty(harness.document);
  harness.Complete();
  raster.content_revision = 2;
  harness.Collect(std::span<const ActiveRasterMaskInput>{&raster, 1});
  EXPECT_GT(harness.Required(brush_out), harness.Completed(brush_out));
  EXPECT_TRUE(harness.Current(radial_out));
}

TEST(RuntimeInvalidation, ViewportChangeKeepsCanonicalLlfAndMismatchesFrameResults) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto source_id = LocalToneSourceId(harness.Primary().node_id);
  const auto grade_id  = harness.Primary().scene_output;
  const ImageExtent geometry{harness.plan.geometry.render_extent.width,
                             harness.plan.geometry.render_extent.height};
  const auto llf_needed =
      harness.invalidation.MakeImageRepresentation(source_id, geometry, TextureFormat::R32f);
  const auto frame_needed =
      harness.invalidation.MakeImageRepresentation(grade_id, geometry, TextureFormat::Rgba32f);
  EXPECT_TRUE(harness.invalidation.IsSatisfied(source_id, llf_needed));
  EXPECT_TRUE(harness.invalidation.IsSatisfied(grade_id, frame_needed));

  RenderRequest viewport;
  viewport.view.visible_rect_in_edit_space = {0.2f, 0.2f, 0.6f, 0.6f};
  viewport.view.viewport_extent            = {8, 6};
  GraphCompiler::BindFrameGeometry(harness.plan, harness.document, viewport);
  harness.Collect();
  EXPECT_TRUE(harness.Current(source_id));
  EXPECT_TRUE(harness.Current(grade_id));
  const auto llf_after =
      harness.invalidation.MakeImageRepresentation(source_id, geometry, TextureFormat::R32f);
  const auto frame_after =
      harness.invalidation.MakeImageRepresentation(grade_id, geometry, TextureFormat::Rgba32f);
  EXPECT_TRUE(harness.invalidation.IsSatisfied(source_id, llf_after));
  EXPECT_FALSE(harness.invalidation.IsSatisfied(grade_id, frame_after));
}

TEST(RuntimeInvalidation, CropChangeMismatchesCanonicalLlfWithoutBumpingUnrelatedSensorRevision) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto sensor_before = harness.Required(harness.plan.sensor_linear_output);
  const auto source_id     = LocalToneSourceId(harness.Primary().node_id);
  const ImageExtent geometry{harness.plan.geometry.render_extent.width,
                             harness.plan.geometry.render_extent.height};
  harness.document.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  GraphCompiler::BindFrameGeometry(harness.plan, harness.document, RenderRequest{});
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.plan.sensor_linear_output), sensor_before);
  const auto llf_after =
      harness.invalidation.MakeImageRepresentation(source_id, geometry, TextureFormat::R32f);
  EXPECT_FALSE(harness.invalidation.IsSatisfied(source_id, llf_after));
}

TEST(RuntimeInvalidation, RenderScaleMismatchRejectsPriorFrameRepresentation) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto sensor_before = harness.Required(harness.plan.sensor_linear_output);
  const auto grade_id      = harness.Primary().scene_output;
  const ImageExtent geometry{harness.plan.geometry.render_extent.width,
                             harness.plan.geometry.render_extent.height};
  EXPECT_TRUE(harness.invalidation.IsSatisfied(
      grade_id,
      harness.invalidation.MakeImageRepresentation(grade_id, geometry, TextureFormat::Rgba32f)));

  RenderRequest scaled;
  scaled.resolution.render_scale = 0.5f;
  GraphCompiler::BindFrameGeometry(harness.plan, harness.document, scaled);
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.plan.sensor_linear_output), sensor_before);
  const ImageExtent scaled_extent{harness.plan.geometry.render_extent.width,
                                  harness.plan.geometry.render_extent.height};
  EXPECT_FALSE(harness.invalidation.IsSatisfied(
      grade_id, harness.invalidation.MakeImageRepresentation(grade_id, scaled_extent,
                                                            TextureFormat::Rgba32f)));
}

TEST(RuntimeInvalidation, ExportQualityMismatchRejectsPriorFrameRepresentation) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto grade_id = harness.Primary().scene_output;
  const ImageExtent geometry{harness.plan.geometry.render_extent.width,
                             harness.plan.geometry.render_extent.height};
  EXPECT_TRUE(harness.invalidation.IsSatisfied(
      grade_id,
      harness.invalidation.MakeImageRepresentation(grade_id, geometry, TextureFormat::Rgba32f)));

  RenderRequest export_quality;
  export_quality.resolution.quality = RenderQuality::Export;
  GraphCompiler::BindFrameGeometry(harness.plan, harness.document, export_quality);
  harness.Collect();
  EXPECT_FALSE(harness.invalidation.IsSatisfied(
      grade_id,
      harness.invalidation.MakeImageRepresentation(grade_id, geometry, TextureFormat::Rgba32f)));
}

TEST(RuntimeInvalidation, DocumentEpochPreventsReuseOfSameNodeIds) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto grade_id = harness.Primary().scene_output;
  EXPECT_TRUE(harness.Current(grade_id));
  harness.invalidation.AdvanceDocumentEpoch();
  EXPECT_GT(harness.Required(grade_id), harness.Completed(grade_id));
  EXPECT_GT(harness.Required(harness.plan.sensor_linear_output),
            harness.Completed(harness.plan.sensor_linear_output));
}

TEST(RuntimeInvalidation, RepeatedEditsDoNotGrowTrackedValueCount) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto tracked = harness.invalidation.TrackedValueCount();
  auto* exposure     = dynamic_cast<ExposureModel*>(
      harness.document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  for (int step = 0; step < 12; ++step) {
    exposure->SetValue(0.1f * static_cast<float>(step));
    harness.PublishCurrent();
  }
  EXPECT_EQ(harness.invalidation.TrackedValueCount(), tracked);
}

TEST(RuntimeInvalidation, DisplayNameDoesNotInvalidateResults) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto grade = harness.Required(harness.Primary().scene_output);
  harness.document.PrimaryGrade()->SetDisplayName("Look A");
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.Primary().scene_output), grade);
}

TEST(RuntimeInvalidation, AddedGradeLeavesUpstreamValidAndInvalidatesDisplay) {
  ValidityHarness harness;
  harness.PublishCurrent();
  const auto sensor  = harness.Required(harness.plan.sensor_linear_output);
  const auto primary = harness.Required(harness.Primary().scene_output);
  multi_grade_test::AddCleanGradesBeforeDrt(harness.document, {"grade.tail"});
  harness.Recompile();
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.plan.sensor_linear_output), sensor);
  EXPECT_TRUE(harness.Current(harness.plan.sensor_linear_output));
  EXPECT_EQ(harness.Required(harness.plan.grade_nodes.front().scene_output), primary);
  EXPECT_TRUE(harness.Current(harness.plan.grade_nodes.front().scene_output));
  EXPECT_GT(harness.Required(harness.plan.grade_nodes.back().scene_output),
            harness.Completed(harness.plan.grade_nodes.back().scene_output));
  EXPECT_GT(harness.Required(harness.plan.display_output),
            harness.Completed(harness.plan.display_output));
}

TEST(RuntimeInvalidation, RemovingMaskInvalidatesGradeOutputNotDevelop) {
  ValidityHarness harness;
  grade_mask_test::AddRadialMask(harness.document, MaskId{"mask.radial"});
  harness.Recompile();
  harness.PublishCurrent();
  const auto develop = harness.Required(harness.plan.develop_output);
  const auto grade   = harness.Required(harness.Primary().scene_output);
  harness.document.PrimaryGrade()->RemoveMask(MaskId{"mask.radial"});
  harness.Recompile();
  harness.Collect();
  EXPECT_EQ(harness.Required(harness.plan.develop_output), develop);
  EXPECT_TRUE(harness.Current(harness.plan.develop_output));
  EXPECT_GT(harness.Required(harness.Primary().scene_output), grade);
}

TEST(RuntimeInvalidation, CanonicalIdentityIgnoresViewportAndFollowsCrop) {
  auto       prepared = MakePrepared();
  auto       document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base_canonical = HashCanonicalReferenceIdentity(plan, prepared);
  const auto base_frame     = HashFrameImageIdentity(plan, prepared, 1);

  RenderRequest viewport;
  viewport.view.visible_rect_in_edit_space = {0.25f, 0.25f, 0.5f, 0.5f};
  viewport.view.viewport_extent            = {8, 6};
  GraphCompiler::BindFrameGeometry(plan, document, viewport);
  EXPECT_EQ(HashCanonicalReferenceIdentity(plan, prepared), base_canonical);
  EXPECT_NE(HashFrameImageIdentity(plan, prepared, 1), base_frame);

  document.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  GraphCompiler::BindFrameGeometry(plan, document, viewport);
  EXPECT_NE(HashCanonicalReferenceIdentity(plan, prepared), base_canonical);
}

}  // namespace
}  // namespace alcedo
