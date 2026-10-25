//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM4.2: the user places AI Mask prompts on the photo after crop and rotation; each prompt maps
// back to normalized coordinates of the analysis image (the whole source without crop and
// rotation) that the segmentation sidecar reads.

#include "ui/edit_viewer/ai_mask_prompt_geometry.hpp"

#include <gtest/gtest.h>

#include <QPointF>
#include <QVector2D>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo {
namespace {

constexpr Extent2D kFullReference{400, 300};

/// The photo as the viewport presents it: cropped and rotated by 7 degrees, zoomed, and panned.
[[nodiscard]] auto MakeCroppedRotatedMapping() -> MaskEditViewMapping {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.10f, 0.15f, 0.60f, 0.60f};
  image.rotation_degrees = 7.0f;
  const auto resolved =
      ResolveRenderGeometry(MakeSourceGeometry(kFullReference, kFullReference), image, {}, {}, {});
  MaskEditViewMapping mapping;
  mapping.widget     = {800, 600, 1.25f};
  mapping.photograph = {static_cast<int>(resolved.edit_extent.width),
                        static_cast<int>(resolved.edit_extent.height)};
  mapping.zoom       = 1.6f;
  mapping.pan        = QVector2D(0.05f, -0.03f);
  mapping.geometry   = resolved;
  return mapping;
}

/// The Geometry panel frame: the whole source rotated by 7 degrees in its bounding box.
[[nodiscard]] auto MakeGeometryPanelMapping() -> MaskEditViewMapping {
  ImageGeometryParams image;
  image.rotation_degrees = 7.0f;
  image.output_frame     = GeometryOutputFrame::RotatedSourceBounds;
  const auto resolved =
      ResolveRenderGeometry(MakeSourceGeometry(kFullReference, kFullReference), image, {}, {}, {});
  MaskEditViewMapping mapping;
  mapping.widget     = {800, 600, 1.0f};
  mapping.photograph = {static_cast<int>(resolved.edit_extent.width),
                        static_cast<int>(resolved.edit_extent.height)};
  mapping.geometry   = resolved;
  return mapping;
}

/// Item point of photograph UV @p uv, computed with the viewport mapper only.
[[nodiscard]] auto ItemOfPhotographUv(const MaskEditViewMapping& mapping, Vector2 uv) -> QPointF {
  const auto item = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(uv.x, uv.y), mapping.widget, mapping.photograph, mapping.zoom, mapping.pan, false);
  EXPECT_TRUE(item.has_value());
  return item.value_or(QPointF());
}

/// Normalized ReferenceSpace position of photograph UV @p uv, from the frame geometry only.
[[nodiscard]] auto SourceOfPhotographUv(const MaskEditViewMapping& mapping, Vector2 uv) -> Vector2 {
  const auto& geometry = mapping.geometry;
  const auto  reference =
      TransformPoint(geometry.render_to_reference,
                     Vector2{uv.x * static_cast<float>(geometry.render_extent.width),
                             uv.y * static_cast<float>(geometry.render_extent.height)});
  return Vector2{reference.x / static_cast<float>(geometry.full_reference_extent.width),
                 reference.y / static_cast<float>(geometry.full_reference_extent.height)};
}

void ExpectClose(Vector2 actual, Vector2 expected, const char* name) {
  EXPECT_NEAR(actual.x, expected.x, kMaskNormalizedMappingEpsilon) << name;
  EXPECT_NEAR(actual.y, expected.y, kMaskNormalizedMappingEpsilon) << name;
}

}  // namespace

TEST(MaskEditGeometryTest, PromptPointOnCroppedRotatedViewMapsToTheSourcePosition) {
  const auto mapping = MakeCroppedRotatedMapping();
  ASSERT_TRUE(MaskEditGeometry::IsValid(mapping));

  // The analysis image of the same source: no crop, no rotation, long edge at most 256.
  ResolutionRequest analysis_resolution;
  analysis_resolution.max_edge = 256;
  const auto analysis = ResolveRenderGeometry(MakeSourceGeometry(kFullReference, kFullReference),
                                              ImageGeometryParams{}, {}, analysis_resolution, {});
  ASSERT_EQ(analysis.render_extent, (Extent2D{256, 192}));

  for (const auto source : {Vector2{0.42f, 0.47f}, Vector2{0.20f, 0.30f}, Vector2{0.63f, 0.69f}}) {
    const auto reference = MaskEditGeometry::ReferencePixelsFromNormalized(source, kFullReference);
    const auto item      = MaskEditGeometry::MapReferenceToItem(mapping, reference);
    ASSERT_TRUE(item.has_value());
    const auto prompt = MapItemToAiMaskPromptPoint(mapping, *item);
    ASSERT_TRUE(prompt.has_value());
    ExpectClose(*prompt, source, "prompt equals the source position");

    // The prompt names the same pixel of the analysis image.
    const auto analysis_pixel = TransformPoint(analysis.reference_to_render, reference);
    ExpectClose(*prompt,
                Vector2{analysis_pixel.x / static_cast<float>(analysis.render_extent.width),
                        analysis_pixel.y / static_cast<float>(analysis.render_extent.height)},
                "prompt equals the analysis image position");
  }

  // The prompt is not the position on the displayed (cropped) photograph.
  const auto center        = ItemOfPhotographUv(mapping, {0.5f, 0.5f});
  const auto center_prompt = MapItemToAiMaskPromptPoint(mapping, center);
  ASSERT_TRUE(center_prompt.has_value());
  ExpectClose(*center_prompt, SourceOfPhotographUv(mapping, {0.5f, 0.5f}), "center");
  EXPECT_NEAR(center_prompt->x, 0.40f, 1.0e-4f) << "crop center x";
  EXPECT_NEAR(center_prompt->y, 0.45f, 1.0e-4f) << "crop center y";
}

TEST(MaskEditGeometryTest, PromptPointOutsideTheSourceIsRejected) {
  const auto panel = MakeGeometryPanelMapping();
  ASSERT_TRUE(MaskEditGeometry::IsValid(panel));
  // A border corner of the rotated source bounds is on the displayed photograph, not on the source.
  const Vector2 corner_uv{0.004f, 0.004f};
  const auto    corner_source = SourceOfPhotographUv(panel, corner_uv);
  ASSERT_TRUE(corner_source.x < 0.0f || corner_source.y < 0.0f);
  EXPECT_FALSE(MapItemToAiMaskPromptPoint(panel, ItemOfPhotographUv(panel, corner_uv)).has_value());
  const auto center = MapItemToAiMaskPromptPoint(panel, ItemOfPhotographUv(panel, {0.5f, 0.5f}));
  ASSERT_TRUE(center.has_value());
  ExpectClose(*center, Vector2{0.5f, 0.5f}, "panel center");

  // A point beside the photograph is not a prompt, even when it is inside the source.
  const auto    cropped = MakeCroppedRotatedMapping();
  const Vector2 beside_uv{-0.05f, 0.5f};
  const auto    beside_source = SourceOfPhotographUv(cropped, beside_uv);
  ASSERT_GT(beside_source.x, 0.0f);
  EXPECT_FALSE(
      MapItemToAiMaskPromptPoint(cropped, ItemOfPhotographUv(cropped, beside_uv)).has_value());

  MaskEditViewMapping invalid    = cropped;
  invalid.geometry.render_extent = {};
  EXPECT_FALSE(MapItemToAiMaskPromptPoint(invalid, QPointF(400.0, 300.0)).has_value());
}

TEST(MaskEditGeometryTest, PromptPointInRegionOfInterestPatchMapsLikeTheFullFrame) {
  const auto full   = MakeCroppedRotatedMapping();
  auto       roi    = full;
  roi.presentation  = FramePresentationMode::RoiFrame;
  roi.displayed_roi = FrameRoiRect{0.20f, 0.30f, 0.50f, 0.40f};

  const Vector2 photograph_uv{0.40f, 0.50f};
  const auto    full_prompt =
      MapItemToAiMaskPromptPoint(full, ItemOfPhotographUv(full, photograph_uv));
  ASSERT_TRUE(full_prompt.has_value());

  const Vector2 patch_uv{(photograph_uv.x - 0.20f) / 0.50f, (photograph_uv.y - 0.30f) / 0.40f};
  const auto    patch_item =
      ViewportMapper::ImageUvToWidgetPoint(QPointF(patch_uv.x, patch_uv.y), roi.widget,
                                           roi.photograph, 1.0f, QVector2D(0.0f, 0.0f), false);
  ASSERT_TRUE(patch_item.has_value());
  const auto roi_prompt = MapItemToAiMaskPromptPoint(roi, *patch_item);
  ASSERT_TRUE(roi_prompt.has_value());
  ExpectClose(*roi_prompt, *full_prompt, "region of interest");
}

TEST(MaskEditGeometryTest, PromptBoxOnRotatedViewIsTheBoundingBoxOfTheMappedCorners) {
  const auto mapping = MakeCroppedRotatedMapping();
  const auto a       = ItemOfPhotographUv(mapping, {0.30f, 0.25f});
  const auto b       = ItemOfPhotographUv(mapping, {0.65f, 0.70f});
  const auto box     = MapItemRectToAiMaskPromptBox(mapping, b, a);
  ASSERT_TRUE(box.has_value());

  // The screen rectangle is a rotated rectangle in the source; the box is its bounding box.
  const Vector2 corners[] = {
      SourceOfPhotographUv(mapping, {0.30f, 0.25f}), SourceOfPhotographUv(mapping, {0.65f, 0.25f}),
      SourceOfPhotographUv(mapping, {0.65f, 0.70f}), SourceOfPhotographUv(mapping, {0.30f, 0.70f})};
  float min_x = corners[0].x, max_x = corners[0].x, min_y = corners[0].y, max_y = corners[0].y;
  for (const auto& corner : corners) {
    min_x = std::fmin(min_x, corner.x);
    max_x = std::fmax(max_x, corner.x);
    min_y = std::fmin(min_y, corner.y);
    max_y = std::fmax(max_y, corner.y);
  }
  EXPECT_GT(std::fabs(corners[0].x - corners[3].x), 1.0e-3f) << "the source rectangle is rotated";
  EXPECT_NEAR(box->x, min_x, kMaskNormalizedMappingEpsilon);
  EXPECT_NEAR(box->y, min_y, kMaskNormalizedMappingEpsilon);
  EXPECT_NEAR(box->x + box->w, max_x, kMaskNormalizedMappingEpsilon);
  EXPECT_NEAR(box->y + box->h, max_y, kMaskNormalizedMappingEpsilon);

  // A box past the left edge of the Geometry panel frame is clipped to the source.
  const auto panel   = MakeGeometryPanelMapping();
  const auto clipped = MapItemRectToAiMaskPromptBox(
      panel, ItemOfPhotographUv(panel, {-0.10f, 0.40f}), ItemOfPhotographUv(panel, {0.30f, 0.60f}));
  ASSERT_TRUE(clipped.has_value());
  EXPECT_FLOAT_EQ(clipped->x, 0.0f);
  EXPECT_GT(clipped->w, 0.0f);
  EXPECT_LE(clipped->y + clipped->h, 1.0f);

  // A box beside an unrotated, uncropped photograph has no source pixels.
  MaskEditViewMapping identity;
  identity.widget     = {800, 600, 1.0f};
  identity.photograph = {400, 300};
  identity.geometry   = MaskEditGeometry::MakeIdentityPhotographGeometry(kFullReference);
  EXPECT_FALSE(MapItemRectToAiMaskPromptBox(identity, ItemOfPhotographUv(identity, {-0.20f, 0.20f}),
                                            ItemOfPhotographUv(identity, {-0.05f, 0.60f}))
                   .has_value());
  // Two equal corners give no box.
  EXPECT_FALSE(MapItemRectToAiMaskPromptBox(mapping, a, a).has_value());
}

TEST(MaskEditGeometryTest, AnalysisToItemTransformInvertsThePromptMapping) {
  auto roi          = MakeCroppedRotatedMapping();
  roi.presentation  = FramePresentationMode::RoiFrame;
  roi.displayed_roi = FrameRoiRect{0.20f, 0.30f, 0.50f, 0.40f};
  for (const auto& mapping : {MakeCroppedRotatedMapping(), MakeGeometryPanelMapping(), roi}) {
    const auto transform = AiMaskAnalysisToItemTransform(mapping);
    ASSERT_TRUE(transform.has_value());
    for (const auto uv : {Vector2{0.45f, 0.50f}, Vector2{0.30f, 0.62f}, Vector2{0.55f, 0.41f}}) {
      // Photograph UV inside the region of interest, placed through ReferenceSpace so that the
      // region-of-interest patch maps it as well.
      const auto source = SourceOfPhotographUv(mapping, uv);
      const auto item   = MaskEditGeometry::MapReferenceToItem(
          mapping, MaskEditGeometry::ReferencePixelsFromNormalized(source, kFullReference));
      ASSERT_TRUE(item.has_value());
      const auto prompt = MapItemToAiMaskPromptPoint(mapping, *item);
      ASSERT_TRUE(prompt.has_value());
      ExpectClose(*prompt, source, "prompt");
      const auto back = transform->map(QPointF(prompt->x, prompt->y));
      EXPECT_LE(std::hypot(back.x() - item->x(), back.y() - item->y()),
                kMaskItemRoundTripLogicalPx);
    }
  }
  MaskEditViewMapping invalid;
  EXPECT_FALSE(AiMaskAnalysisToItemTransform(invalid).has_value());
}

TEST(MaskEditGeometryTest, PathStrokeSamplesEqualArcLengthUpToTheLimit) {
  const auto mapping   = MakeCroppedRotatedMapping();
  const auto transform = AiMaskAnalysisToItemTransform(mapping);
  ASSERT_TRUE(transform.has_value());
  // A straight stroke on the source from (0.30, 0.40) to (0.50, 0.40), with an extra pointer
  // sample near the start: the samples must not follow the pointer sample spacing.
  const std::vector<QPointF> path = {transform->map(QPointF(0.30, 0.40)),
                                     transform->map(QPointF(0.31, 0.40)),
                                     transform->map(QPointF(0.50, 0.40))};
  const auto                 length =
      std::hypot(path.back().x() - path.front().x(), path.back().y() - path.front().y());
  const auto expected_count = std::min<std::size_t>(
      static_cast<std::size_t>(std::lround(length / kAiMaskStrokePromptSpacingItemPx)) + 1,
      kAiMaskMaximumStrokePromptPoints);
  ASSERT_GE(expected_count, 3U);

  const auto points =
      MapItemStrokeToAiMaskPromptPoints(mapping, path, kAiMaskMaximumStrokePromptPoints);
  ASSERT_EQ(points.size(), expected_count);
  ExpectClose(points.front(), Vector2{0.30f, 0.40f}, "first sample");
  ExpectClose(points.back(), Vector2{0.50f, 0.40f}, "last sample");
  const float step = 0.20f / static_cast<float>(expected_count - 1);
  for (std::size_t index = 0; index < points.size(); ++index) {
    ExpectClose(points[index], Vector2{0.30f + step * static_cast<float>(index), 0.40f},
                "equal arc length");
  }

  // The limit of a stroke caps the count; the samples still span the stroke.
  const auto capped = MapItemStrokeToAiMaskPromptPoints(mapping, path, 2);
  ASSERT_EQ(capped.size(), 2U);
  ExpectClose(capped.back(), Vector2{0.50f, 0.40f}, "capped last sample");

  // A stroke without length is one point; a stroke off the source has none.
  const auto click = MapItemStrokeToAiMaskPromptPoints(mapping, {path.front(), path.front()},
                                                       kAiMaskMaximumStrokePromptPoints);
  ASSERT_EQ(click.size(), 1U);
  ExpectClose(click.front(), Vector2{0.30f, 0.40f}, "click");
  EXPECT_TRUE(MapItemStrokeToAiMaskPromptPoints(mapping, {QPointF(-500.0, -500.0)},
                                                kAiMaskMaximumStrokePromptPoints)
                  .empty());
}

}  // namespace alcedo
