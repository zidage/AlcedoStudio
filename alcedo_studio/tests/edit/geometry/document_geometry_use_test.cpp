//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/geometry/render_request.hpp"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

auto MakePlanWithSource() -> ExecutionPlan {
  ExecutionPlan plan;
  plan.source.develop_output_extent = {600, 400};
  plan.source.full_reference_extent = {600, 400};
  return plan;
}

auto BindGeometry(const PipelineDocument& document, DocumentGeometryUse use)
    -> ResolvedRenderGeometry {
  auto          plan = MakePlanWithSource();
  RenderRequest request;
  request.view.viewport_extent = {300, 200};
  request.resolution.max_edge  = 512;
  request.document_geometry    = use;
  GraphCompiler::BindFrameGeometry(plan, document, request);
  return plan.geometry;
}

void ExpectSameMatrix(const Matrix3x3& actual, const Matrix3x3& expected) {
  for (int i = 0; i < 9; ++i) {
    EXPECT_FLOAT_EQ(actual.m[i], expected.m[i]) << "matrix element " << i;
  }
}

}  // namespace

TEST(GpuDagGeometry, RotatedUncroppedSourceRequestIgnoresCropAndKeepsRotation) {
  auto cropped = CreateDefaultPipelineDocument();
  cropped.Geometry().SetCropRect({0.2f, 0.1f, 0.5f, 0.6f});
  cropped.Geometry().SetRotationDegrees(7.0f);
  const auto cropped_json_before = cropped.ToJson();

  auto       rotated_only        = CreateDefaultPipelineDocument();
  rotated_only.Geometry().SetRotationDegrees(7.0f);

  const auto preview  = BindGeometry(cropped, DocumentGeometryUse::RotatedUncroppedSource);
  const auto expected = BindGeometry(rotated_only, DocumentGeometryUse::RotatedUncroppedSource);

  // The crop does not change the preview frame; the rotation does.
  EXPECT_EQ(preview.edit_extent, expected.edit_extent);
  ExpectSameMatrix(preview.reference_to_edit, expected.reference_to_edit);
  ExpectSameMatrix(preview.render_to_decoded, expected.render_to_decoded);
  // The preview frames the whole rotated source: its bounding box is larger than the source.
  EXPECT_GT(preview.edit_extent.width, 600u);
  EXPECT_GT(preview.edit_extent.height, 400u);

  // Without rotation the preview is the plain source.
  const auto identity = CreateDefaultPipelineDocument();
  EXPECT_EQ(BindGeometry(identity, DocumentGeometryUse::RotatedUncroppedSource).edit_extent,
            (Extent2D{600, 400}));

  // The default request still applies the document crop, and binding never edits the document.
  const auto applied = BindGeometry(cropped, DocumentGeometryUse::ApplyCropAndRotation);
  EXPECT_LT(applied.edit_extent.width, 600u);
  EXPECT_EQ(cropped.ToJson(), cropped_json_before);
}

}  // namespace alcedo
