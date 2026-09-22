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

TEST(GpuDagGeometry, UncroppedSourceRequestIgnoresDocumentCropAndRotation) {
  auto cropped = CreateDefaultPipelineDocument();
  cropped.Geometry().SetCropRect({0.2f, 0.1f, 0.5f, 0.6f});
  cropped.Geometry().SetRotationDegrees(7.0f);
  const auto cropped_json_before = cropped.ToJson();

  const auto identity            = CreateDefaultPipelineDocument();

  const auto uncropped           = BindGeometry(cropped, DocumentGeometryUse::UncroppedSource);
  const auto expected = BindGeometry(identity, DocumentGeometryUse::ApplyCropAndRotation);

  EXPECT_EQ(uncropped.edit_extent, expected.edit_extent);
  EXPECT_EQ(uncropped.render_extent, expected.render_extent);
  EXPECT_EQ(uncropped.edit_extent, (Extent2D{600, 400}));
  ExpectSameMatrix(uncropped.reference_to_edit, expected.reference_to_edit);
  ExpectSameMatrix(uncropped.render_to_decoded, expected.render_to_decoded);
  EXPECT_EQ(uncropped.required_decoded_region, expected.required_decoded_region);

  // The default request still applies the document crop, and binding never edits the document.
  const auto applied = BindGeometry(cropped, DocumentGeometryUse::ApplyCropAndRotation);
  EXPECT_FALSE(applied.edit_extent == expected.edit_extent);
  EXPECT_EQ(cropped.ToJson(), cropped_json_before);
}

}  // namespace alcedo
