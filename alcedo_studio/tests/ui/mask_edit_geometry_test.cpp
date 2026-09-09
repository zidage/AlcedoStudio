//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>

#include <QPointF>
#include <QVector2D>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"

namespace alcedo {
namespace {

struct MappingFixture {
  const char*        name;
  ViewportWidgetInfo widget;
  ViewportImageInfo  photograph;
  float              zoom;
  QVector2D          pan;
  Extent2D           photograph_extent;
};

const MappingFixture kRoundTripFixtures[] = {
    {"landscape_fit_dpr1", {800, 600, 1.0f}, {400, 300}, 1.0f, {0.0f, 0.0f}, {400, 300}},
    {"landscape_zoom_pan_dpr125",
     {800, 600, 1.25f},
     {400, 300},
     1.8f,
     {0.12f, -0.08f},
     {400, 300}},
    {"landscape_zoom_dpr15", {960, 540, 1.5f}, {400, 300}, 2.0f, {0.0f, 0.0f}, {400, 300}},
    {"portrait_dpr2", {540, 960, 2.0f}, {300, 400}, 1.4f, {-0.05f, 0.1f}, {300, 400}},
};

[[nodiscard]] auto MakeMapping(const MappingFixture& fixture) -> MaskEditViewMapping {
  MaskEditViewMapping mapping;
  mapping.widget     = fixture.widget;
  mapping.photograph = fixture.photograph;
  mapping.zoom       = fixture.zoom;
  mapping.pan        = fixture.pan;
  mapping.geometry = MaskEditGeometry::MakeIdentityPhotographGeometry(fixture.photograph_extent);
  return mapping;
}

[[nodiscard]] auto IndependentFromItem(const MaskEditViewMapping& mapping, const QPointF& item)
    -> MaskReferenceSample {
  const float     zoom = mapping.presentation == FramePresentationMode::RoiFrame ? 1.0f
                                                                                : mapping.zoom;
  const QVector2D pan  = mapping.presentation == FramePresentationMode::RoiFrame
                             ? QVector2D(0.0f, 0.0f)
                             : mapping.pan;
  const auto uv =
      ViewportMapper::WidgetPointToImageUv(item, mapping.widget, mapping.photograph, zoom, pan);
  EXPECT_TRUE(uv.has_value());
  Vector2 photograph_uv{static_cast<float>(uv->x()), static_cast<float>(uv->y())};
  if (mapping.presentation == FramePresentationMode::RoiFrame) {
    photograph_uv.x = mapping.displayed_roi.x + photograph_uv.x * mapping.displayed_roi.width;
    photograph_uv.y = mapping.displayed_roi.y + photograph_uv.y * mapping.displayed_roi.height;
  }
  const Vector2 render{photograph_uv.x * static_cast<float>(mapping.geometry.render_extent.width),
                       photograph_uv.y *
                           static_cast<float>(mapping.geometry.render_extent.height)};
  MaskReferenceSample sample;
  sample.photograph_uv    = photograph_uv;
  sample.reference_pixels  = TransformPoint(mapping.geometry.render_to_reference, render);
  sample.normalized        = MaskEditGeometry::NormalizedFromReferencePixels(
      sample.reference_pixels, mapping.geometry.full_reference_extent);
  sample.inside_photograph = photograph_uv.x >= 0.0f && photograph_uv.x <= 1.0f &&
                            photograph_uv.y >= 0.0f && photograph_uv.y <= 1.0f;
  return sample;
}

void ExpectNormalizedClose(Vector2 actual, Vector2 expected, const char* name) {
  EXPECT_NEAR(actual.x, expected.x, kMaskNormalizedMappingEpsilon) << name;
  EXPECT_NEAR(actual.y, expected.y, kMaskNormalizedMappingEpsilon) << name;
}

[[nodiscard]] auto LetterboxOutsideItem(const MaskEditViewMapping& mapping) -> QPointF {
  const auto top_left = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(0.0, 0.0), mapping.widget, mapping.photograph, mapping.zoom, mapping.pan);
  EXPECT_TRUE(top_left.has_value());
  return QPointF(top_left->x() - 8.0, top_left->y() - 8.0);
}

}  // namespace

TEST(MaskEditGeometryTest, MaskReferenceMappingRoundTripsAcrossZoomPanAndDpr) {
  const Vector2 known_q{0.35f, 0.55f};
  for (const auto& fixture : kRoundTripFixtures) {
    const auto mapping = MakeMapping(fixture);
    ASSERT_TRUE(MaskEditGeometry::IsValid(mapping)) << fixture.name;
    const auto reference = MaskEditGeometry::ReferencePixelsFromNormalized(
        known_q, mapping.geometry.full_reference_extent);
    const auto item = MaskEditGeometry::MapReferenceToItem(mapping, reference);
    ASSERT_TRUE(item.has_value()) << fixture.name;

    const auto mapped = MaskEditGeometry::MapItemToReference(mapping, *item, false);
    ASSERT_TRUE(mapped.has_value()) << fixture.name;
    ExpectNormalizedClose(mapped->normalized, known_q, fixture.name);

    const auto independent = IndependentFromItem(mapping, *item);
    ExpectNormalizedClose(mapped->normalized, independent.normalized, fixture.name);

    const auto item_back =
        MaskEditGeometry::MapReferenceToItem(mapping, mapped->reference_pixels);
    ASSERT_TRUE(item_back.has_value()) << fixture.name;
    const float dx = static_cast<float>(item_back->x() - item->x());
    const float dy = static_cast<float>(item_back->y() - item->y());
    EXPECT_LE(std::hypot(dx, dy), kMaskItemRoundTripLogicalPx) << fixture.name;
  }

  const auto fit = MakeMapping(kRoundTripFixtures[0]);
  const auto center =
      MaskEditGeometry::MapItemToReference(fit, QPointF(400.0, 300.0), false);
  ASSERT_TRUE(center.has_value());
  ExpectNormalizedClose(center->normalized, Vector2{0.5f, 0.5f}, "fit_center");
  EXPECT_NEAR(center->reference_pixels.x, 200.0f, 1.0e-3f);
  EXPECT_NEAR(center->reference_pixels.y, 150.0f, 1.0e-3f);

  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.10f, 0.15f, 0.70f, 0.60f};
  image.rotation_degrees = 23.0f;
  image.expand_to_fit    = true;
  const auto cropped =
      ResolveRenderGeometry(MakeSourceGeometry({400, 300}, {400, 300}), image, {}, {}, {});
  MaskEditViewMapping crop_mapping = fit;
  crop_mapping.photograph           = {static_cast<int>(cropped.edit_extent.width),
                               static_cast<int>(cropped.edit_extent.height)};
  crop_mapping.geometry            = cropped;
  ASSERT_TRUE(MaskEditGeometry::IsValid(crop_mapping));
  const Vector2 crop_render{
      0.5f * static_cast<float>(cropped.render_extent.width),
      0.5f * static_cast<float>(cropped.render_extent.height)};
  const auto crop_reference = TransformPoint(cropped.render_to_reference, crop_render);
  const auto crop_item = MaskEditGeometry::MapReferenceToItem(crop_mapping, crop_reference);
  ASSERT_TRUE(crop_item.has_value());
  const auto crop_mapped = MaskEditGeometry::MapItemToReference(crop_mapping, *crop_item, false);
  ASSERT_TRUE(crop_mapped.has_value());
  const auto expected_q = MaskEditGeometry::NormalizedFromReferencePixels(
      crop_reference, cropped.full_reference_extent);
  ExpectNormalizedClose(crop_mapped->normalized, expected_q, "cropped_rotated");
  const auto crop_independent = IndependentFromItem(crop_mapping, *crop_item);
  ExpectNormalizedClose(crop_mapped->normalized, crop_independent.normalized, "cropped_rotated");
}

TEST(MaskEditGeometryTest, DetailPatchDoesNotChangeMaskReferenceSpace) {
  const auto full = MakeMapping(kRoundTripFixtures[0]);
  const auto item =
      MaskEditGeometry::MapReferenceToItem(full, MaskEditGeometry::ReferencePixelsFromNormalized(
                                                     {0.40f, 0.35f}, {400, 300}));
  ASSERT_TRUE(item.has_value());
  const auto full_sample = MaskEditGeometry::MapItemToReference(full, *item, false);
  ASSERT_TRUE(full_sample.has_value());

  MaskEditViewMapping overlay = full;
  overlay.displayed_roi       = FrameRoiRect{0.25f, 0.20f, 0.50f, 0.40f};
  const auto overlay_sample = MaskEditGeometry::MapItemToReference(overlay, *item, false);
  ASSERT_TRUE(overlay_sample.has_value());
  ExpectNormalizedClose(overlay_sample->normalized, full_sample->normalized,
                         "fullframe_ignores_roi");

  MaskEditViewMapping roi_frame = full;
  roi_frame.presentation           = FramePresentationMode::RoiFrame;
  roi_frame.displayed_roi         = FrameRoiRect{0.25f, 0.20f, 0.50f, 0.40f};
  roi_frame.zoom                  = 4.0f;
  roi_frame.pan                   = QVector2D(0.3f, -0.2f);
  const Vector2 patch_uv{(0.40f - 0.25f) / 0.50f, (0.35f - 0.20f) / 0.40f};
  const auto patch_item = ViewportMapper::ImageUvToWidgetPoint(
      QPointF(patch_uv.x, patch_uv.y), roi_frame.widget, roi_frame.photograph, 1.0f,
      QVector2D(0.0f, 0.0f), false);
  ASSERT_TRUE(patch_item.has_value());
  const auto roi_sample = MaskEditGeometry::MapItemToReference(roi_frame, *patch_item, false);
  ASSERT_TRUE(roi_sample.has_value());
  ExpectNormalizedClose(roi_sample->normalized, full_sample->normalized,
                         "roi_frame_full_reference");
  EXPECT_NEAR(roi_sample->reference_pixels.x, full_sample->reference_pixels.x,
              kMaskNormalizedMappingEpsilon);
  EXPECT_NEAR(roi_sample->reference_pixels.y, full_sample->reference_pixels.y,
              kMaskNormalizedMappingEpsilon);
  EXPECT_EQ(full.geometry.full_reference_extent, (Extent2D{400, 300}));

  ResolutionRequest capped;
  capped.max_edge = 256;
  const auto capped_geometry = ResolveRenderGeometry(MakeSourceGeometry({400, 300}, {400, 300}),
                                                    {}, {}, capped, {});
  EXPECT_NE(capped_geometry.render_extent, full.geometry.render_extent);
  EXPECT_EQ(capped_geometry.full_reference_extent, full.geometry.full_reference_extent);
  MaskEditViewMapping capped_mapping = full;
  capped_mapping.geometry            = capped_geometry;
  const auto capped_sample = MaskEditGeometry::MapItemToReference(capped_mapping, *item, false);
  ASSERT_TRUE(capped_sample.has_value());
  ExpectNormalizedClose(capped_sample->normalized, full_sample->normalized,
                         "interactive_output_size");
}

TEST(MaskEditGeometryTest, InvalidGeometryRejectsMaskPress) {
  auto mapping = MakeMapping(kRoundTripFixtures[0]);
  const auto valid_item =
      MaskEditGeometry::MapReferenceToItem(mapping, Vector2{200.0f, 150.0f});
  ASSERT_TRUE(valid_item.has_value());
  ASSERT_TRUE(MaskEditGeometry::MapItemToReference(mapping, *valid_item, false).has_value());

  auto empty = mapping;
  empty.geometry = {};
  EXPECT_FALSE(MaskEditGeometry::IsValid(empty));
  EXPECT_FALSE(MaskEditGeometry::MapItemToReference(empty, *valid_item, false).has_value());

  auto singular = mapping;
  singular.geometry.render_to_reference.m[0] = 0.0f;
  singular.geometry.render_to_reference.m[1] = 0.0f;
  singular.geometry.render_to_reference.m[3] = 0.0f;
  singular.geometry.render_to_reference.m[4] = 0.0f;
  EXPECT_FALSE(MaskEditGeometry::IsValid(singular));
  EXPECT_FALSE(MaskEditGeometry::MapItemToReference(singular, *valid_item, false).has_value());

  auto zero_widget = mapping;
  zero_widget.widget.widget_width = 0;
  EXPECT_FALSE(MaskEditGeometry::MapItemToReference(zero_widget, *valid_item, false).has_value());

  const auto outside = LetterboxOutsideItem(mapping);
  EXPECT_FALSE(MaskEditGeometry::MapItemToReference(mapping, outside, false).has_value());
  const auto outside_drag = MaskEditGeometry::MapItemToReference(mapping, outside, true);
  ASSERT_TRUE(outside_drag.has_value());
  EXPECT_FALSE(outside_drag->inside_photograph);

  editor_rhi::EditorInteractionController controller;
  controller.setViewportMetrics(800, 600, 1.0);
  controller.setImageSize(400, 300);
  EXPECT_FALSE(controller.MapItemToMaskReference(400.0, 300.0, false).has_value());
  controller.setDisplayedMaskGeometry(
      MaskEditGeometry::MakeIdentityPhotographGeometry({400, 300}));
  const auto via_controller = controller.MapItemToMaskReference(400.0, 300.0, false);
  ASSERT_TRUE(via_controller.has_value());
  ExpectNormalizedClose(via_controller->normalized, Vector2{0.5f, 0.5f}, "controller_center");

  const auto before = controller.maskEditMappingIdentity();
  controller.setViewportMetrics(640, 480, 1.5);
  EXPECT_TRUE(MaskEditGeometry::MappingChanged(before, controller.maskEditMappingIdentity()));
  controller.resetPresentationStateForNewImage();
  EXPECT_FALSE(controller.MapItemToMaskReference(400.0, 300.0, false).has_value());
}

TEST(MaskEditGeometryTest, HandleHitRadiusStaysLogicalAcrossZoom) {
  auto mapping = MakeMapping(kRoundTripFixtures[0]);
  const auto handle =
      MaskEditGeometry::MapReferenceToItem(mapping, Vector2{200.0f, 150.0f});
  ASSERT_TRUE(handle.has_value());
  EXPECT_TRUE(MaskEditGeometry::HitsHandle(QPointF(handle->x() + 11.0, handle->y()), *handle));
  EXPECT_FALSE(MaskEditGeometry::HitsHandle(QPointF(handle->x() + 13.0, handle->y()), *handle));

  mapping.zoom = 4.0f;
  const auto zoomed_handle =
      MaskEditGeometry::MapReferenceToItem(mapping, Vector2{200.0f, 150.0f});
  ASSERT_TRUE(zoomed_handle.has_value());
  EXPECT_TRUE(
      MaskEditGeometry::HitsHandle(QPointF(zoomed_handle->x() + 11.0, zoomed_handle->y()),
                                   *zoomed_handle));
  EXPECT_FALSE(
      MaskEditGeometry::HitsHandle(QPointF(zoomed_handle->x() + 13.0, zoomed_handle->y()),
                                   *zoomed_handle));
}

TEST(MaskEditGeometryTest, CroppedRotatedPhotographUsesResolvedGeometryNotIdentity) {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.10f, 0.15f, 0.55f, 0.60f};
  image.rotation_degrees = 18.0f;
  image.expand_to_fit    = true;
  const Extent2D full{400, 300};
  const auto    resolved = MaskEditGeometry::MakeDocumentPhotographGeometry(full, image);
  const auto    identity = MaskEditGeometry::MakeIdentityPhotographGeometry(full);
  EXPECT_NE(resolved.render_extent.width, identity.render_extent.width);
  EXPECT_NE(resolved.render_to_reference.m[0], identity.render_to_reference.m[0]);

  MaskEditViewMapping mapping;
  mapping.widget     = {400, 300, 1.0f};
  mapping.photograph = {static_cast<int>(resolved.edit_extent.width),
                         static_cast<int>(resolved.edit_extent.height)};
  mapping.zoom       = 1.0f;
  mapping.geometry   = resolved;
  ASSERT_TRUE(MaskEditGeometry::IsValid(mapping));
  auto identity_mapping     = mapping;
  identity_mapping.geometry = identity;
  identity_mapping.photograph = {400, 300};
  const QPointF item(200.0, 150.0);
  const auto    cropped = MaskEditGeometry::MapItemToReference(mapping, item, true);
  const auto    uncropped = MaskEditGeometry::MapItemToReference(identity_mapping, item, true);
  ASSERT_TRUE(cropped.has_value());
  ASSERT_TRUE(uncropped.has_value());
  EXPECT_GT(std::hypot(cropped->normalized.x - uncropped->normalized.x,
                         cropped->normalized.y - uncropped->normalized.y),
            1.0e-3f);

  editor_rhi::EditorInteractionController controller;
  controller.setViewportMetrics(400, 300, 1.0);
  controller.setImageSize(400, 300);
  controller.setDisplayedMaskGeometry(resolved);
  controller.setRenderReferenceSize(static_cast<int>(resolved.edit_extent.width),
                                     static_cast<int>(resolved.edit_extent.height));
  const auto published = controller.maskEditViewMapping();
  EXPECT_EQ(published.photograph.image_width, static_cast<int>(resolved.edit_extent.width));
  EXPECT_EQ(published.photograph.image_height, static_cast<int>(resolved.edit_extent.height));
  EXPECT_NE(published.geometry.render_to_reference.m[0], identity.render_to_reference.m[0]);
}

}  // namespace alcedo
