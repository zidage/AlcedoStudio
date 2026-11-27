//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_geometry_test.cpp
/// @brief Comparison image placement from real renderer geometry, drawn by the production
///        EditorComparisonCanvas.qml.

#include <gtest/gtest.h>

#include <QPointF>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>

#include "editor_comparison_test_support.hpp"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"

namespace alcedo::ui::test {
namespace {

constexpr double kCanvasTolerance = 1e-3;

// The production canvas with one placement and no image: the image transform does not depend
// on loaded pixels.
class CanvasHarness {
 public:
  CanvasHarness(const ComparisonImagePlacement& placement, double width, double height) {
    AppTheme::RegisterFonts();
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    QQmlComponent component(
        &engine_, QUrl::fromLocalFile(ComparisonQmlDirectory() + "/EditorComparisonCanvas.qml"));
    QVariantMap initial;
    initial.insert(QStringLiteral("placement"), placement.ToVariantMap());
    canvas_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(initial)));
    if (!canvas_) {
      QStringList text;
      for (const auto& error : component.errors()) {
        text.push_back(error.toString());
      }
      errors_ = text.join('\n');
      return;
    }
    canvas_->setParentItem(window_.contentItem());
    canvas_->setSize(QSizeF(width, height));
    image_ = canvas_->findChild<QQuickItem*>(QStringLiteral("editorComparisonCanvasImage"));
  }

  auto canvas() const -> QQuickItem* { return canvas_.get(); }
  auto image() const -> QQuickItem* { return image_; }
  auto errors() const -> QString { return errors_; }

  /// Canvas position of the render-space point (x, y) as drawn by the Image item.
  auto MapRenderPoint(double x, double y) const -> QPointF {
    return image_->mapToItem(canvas_.get(), QPointF(x, y));
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> canvas_;
  QQuickItem*                 image_ = nullptr;
  QString                     errors_;
};

struct Fit {
  double scale = 0.0;
  double x     = 0.0;
  double y     = 0.0;
};

// Independent reference-to-canvas fit: aspect ratio kept, centered.
auto FitReference(Extent2D reference, double width, double height) -> Fit {
  const double scale = std::min(width / reference.width, height / reference.height);
  return Fit{scale, (width - reference.width * scale) / 2.0,
             (height - reference.height * scale) / 2.0};
}

auto ExpectNear(QPointF actual, QPointF expected, const char* what) {
  EXPECT_NEAR(actual.x(), expected.x(), kCanvasTolerance) << what;
  EXPECT_NEAR(actual.y(), expected.y(), kCanvasTolerance) << what;
}

}  // namespace

TEST(EditorComparisonGeometryTest, OrientationPreviewMasksComparisonAndExportShareMapping) {
  constexpr Extent2D        source_extent{5, 3};
  const auto                source = MakeSourceGeometry(source_extent, source_extent);
  const ImageGeometryParams image{.orientation = FrameOrientation::Rotate90Clockwise};
  const auto                preview  = ResolveRenderGeometry(source, image, {}, {}, {});
  const auto                exported = ResolveRenderGeometry(
      source, image, {}, ResolutionRequest{.quality = RenderQuality::Export}, {});
  ASSERT_EQ(preview.render_extent, (Extent2D{3, 5}));
  ASSERT_EQ(exported.render_extent, preview.render_extent);
  EXPECT_EQ(exported.filter, TextureFilter::Bicubic);

  MaskEditViewMapping mapping;
  mapping.widget     = {300, 500, 1.0f};
  mapping.photograph = {3, 5};
  mapping.geometry   = MaskEditGeometry::MakeDocumentPhotographGeometry(source_extent, image);
  ASSERT_TRUE(MaskEditGeometry::IsValid(mapping));
  ASSERT_EQ(mapping.geometry.render_extent, preview.render_extent);

  CanvasHarness canvas(ComparisonPlacementFromGeometry(preview), 500, 300);
  ASSERT_NE(canvas.canvas(), nullptr) << canvas.errors().toStdString();
  ASSERT_NE(canvas.image(), nullptr);
  for (std::uint32_t y = 0; y < 5; ++y) {
    for (std::uint32_t x = 0; x < 3; ++x) {
      const Vector2 render = PixelCenter(x, y);
      const Vector2 reference{render.y, 3.0f - render.x};
      EXPECT_EQ(TransformPoint(preview.render_to_reference, render), reference);
      EXPECT_EQ(TransformPoint(exported.render_to_reference, render), reference);
      ExpectNear(canvas.MapRenderPoint(render.x, render.y),
                 QPointF(reference.x * 100.0, reference.y * 100.0), "comparison pixel center");
      const QPointF item(render.x * 100.0, render.y * 100.0);
      const auto    mask_point = MaskEditGeometry::MapItemToReference(mapping, item, false);
      ASSERT_TRUE(mask_point.has_value());
      EXPECT_NEAR(mask_point->reference_pixels.x, reference.x, 1e-5);
      EXPECT_NEAR(mask_point->reference_pixels.y, reference.y, 1e-5);
      const auto mask_item = MaskEditGeometry::MapReferenceToItem(mapping, reference);
      ASSERT_TRUE(mask_item.has_value());
      ExpectNear(*mask_item, item, "mask pixel center");
    }
  }
}

TEST(EditorComparisonGeometryTest, SourceAlignmentPlacesHalfCropInHalfOfReferenceCanvas) {
  struct Case {
    Extent2D       source;
    NormalizedRect crop;
    std::uint32_t  max_edge;
    bool           left_half;
  };
  // A left and a right half crop, at full size and with a long-edge limit that resamples.
  for (const Case& c : {Case{{400, 300}, {0.0f, 0.0f, 0.5f, 1.0f}, 0, true},
                        Case{{400, 300}, {0.5f, 0.0f, 0.5f, 1.0f}, 0, false},
                        Case{{8000, 6000}, {0.0f, 0.0f, 0.5f, 1.0f}, 1024, true}}) {
    const auto geometry  = ResolveComparisonGeometry(c.source, c.crop, 0.0f, c.max_edge);
    const auto placement = ComparisonPlacementFromGeometry(geometry);
    ASSERT_EQ(placement.reference_extent, c.source);

    constexpr double kWidth  = 800.0;
    constexpr double kHeight = 300.0;
    CanvasHarness    harness(placement, kWidth, kHeight);
    ASSERT_NE(harness.canvas(), nullptr) << harness.errors().toStdString();
    ASSERT_NE(harness.image(), nullptr);

    const Fit    fit       = FitReference(c.source, kWidth, kHeight);
    const double ref_left  = fit.x;
    const double ref_mid   = fit.x + c.source.width * fit.scale / 2.0;
    const double ref_right = fit.x + c.source.width * fit.scale;
    const double ref_top   = fit.y;
    const double ref_bot   = fit.y + c.source.height * fit.scale;
    EXPECT_NEAR(harness.canvas()->property("fitScale").toDouble(), fit.scale, 1e-9);

    // The image keeps its render extent as local size; Qt applies the placement transform.
    EXPECT_DOUBLE_EQ(harness.image()->width(), placement.render_extent.width);
    EXPECT_DOUBLE_EQ(harness.image()->height(), placement.render_extent.height);

    const double w           = placement.render_extent.width;
    const double h           = placement.render_extent.height;
    const double image_left  = c.left_half ? ref_left : ref_mid;
    const double image_right = c.left_half ? ref_mid : ref_right;
    ExpectNear(harness.MapRenderPoint(0, 0), {image_left, ref_top}, "top-left");
    ExpectNear(harness.MapRenderPoint(w, 0), {image_right, ref_top}, "top-right");
    ExpectNear(harness.MapRenderPoint(w, h), {image_right, ref_bot}, "bottom-right");
    ExpectNear(harness.MapRenderPoint(0, h), {image_left, ref_bot}, "bottom-left");

    // The other half of the reference canvas has no image pixels: the image's canvas bounds
    // cover exactly one half.
    const QRectF bounds = harness.image()->mapRectToItem(harness.canvas(), QRectF(0, 0, w, h));
    EXPECT_NEAR(bounds.width(), (ref_right - ref_left) / 2.0, kCanvasTolerance);
    if (c.left_half) {
      EXPECT_LE(bounds.right(), ref_mid + kCanvasTolerance);
    } else {
      EXPECT_GE(bounds.left(), ref_mid - kCanvasTolerance);
    }
  }
}

TEST(EditorComparisonGeometryTest, RotatedCropCornersMatchRendererReferenceGeometry) {
  constexpr Extent2D kSource{200, 100};
  const auto         geometry =
      ResolveComparisonGeometry(kSource, NormalizedRect{0.30f, 0.25f, 0.40f, 0.50f}, 10.0f);
  const auto       placement = ComparisonPlacementFromGeometry(geometry);

  constexpr double kWidth    = 500.0;
  constexpr double kHeight   = 400.0;
  CanvasHarness    harness(placement, kWidth, kHeight);
  ASSERT_NE(harness.canvas(), nullptr) << harness.errors().toStdString();
  const Fit                    fit = FitReference(kSource, kWidth, kHeight);

  const float                  w   = static_cast<float>(geometry.render_extent.width);
  const float                  h   = static_cast<float>(geometry.render_extent.height);
  const std::array<Vector2, 4> corners{Vector2{0, 0}, Vector2{w, 0}, Vector2{w, h}, Vector2{0, h}};
  std::array<Vector2, 4>       reference_corners{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    // The renderer's own inverse maps the reference corner back to the render corner.
    reference_corners[i] = TransformPoint(geometry.render_to_reference, corners[i]);
    const auto back      = TransformPoint(geometry.reference_to_render, reference_corners[i]);
    EXPECT_NEAR(back.x, corners[i].x, 1e-3f);
    EXPECT_NEAR(back.y, corners[i].y, 1e-3f);

    // Every corner lies in the crop region of the source.
    EXPECT_GT(reference_corners[i].x, 0.0f);
    EXPECT_LT(reference_corners[i].x, static_cast<float>(kSource.width));
    EXPECT_GT(reference_corners[i].y, 0.0f);
    EXPECT_LT(reference_corners[i].y, static_cast<float>(kSource.height));

    const QPointF expected(fit.x + reference_corners[i].x * fit.scale,
                           fit.y + reference_corners[i].y * fit.scale);
    ExpectNear(harness.MapRenderPoint(corners[i].x, corners[i].y), expected, "rotated corner");
  }

  // The footprint is the rotated quadrilateral, not an axis-aligned rectangle: the top edge
  // rises or falls by the 10 degree rotation.
  const double top_rise = reference_corners[1].y - reference_corners[0].y;
  const double top_run  = reference_corners[1].x - reference_corners[0].x;
  EXPECT_NEAR(std::fabs(std::atan2(top_rise, top_run)) * 180.0 / 3.14159265358979, 10.0, 0.05);

  // The crop center stays at the crop rectangle center in the source.
  const auto center = TransformPoint(geometry.render_to_reference, Vector2{w / 2, h / 2});
  EXPECT_NEAR(center.x, (0.30f + 0.20f) * kSource.width, 0.5f);
  EXPECT_NEAR(center.y, (0.25f + 0.25f) * kSource.height, 0.5f);
}

TEST(EditorComparisonGeometryTest, PlacementRejectsEmptyOrNonInvertibleGeometry) {
  const auto valid = ResolveComparisonGeometry(Extent2D{64, 48});
  EXPECT_NO_THROW((void)ComparisonPlacementFromGeometry(valid));

  auto empty_render          = valid;
  empty_render.render_extent = Extent2D{0, 48};
  EXPECT_THROW((void)ComparisonPlacementFromGeometry(empty_render), std::invalid_argument);

  auto singular                = valid;
  singular.render_to_reference = Matrix3x3::Scale(1.0f, 0.0f);
  EXPECT_THROW((void)ComparisonPlacementFromGeometry(singular), std::invalid_argument);

  auto projective                     = valid;
  projective.render_to_reference.m[6] = 0.01f;
  EXPECT_THROW((void)ComparisonPlacementFromGeometry(projective), std::invalid_argument);

  auto not_finite                     = valid;
  not_finite.render_to_reference.m[2] = std::numeric_limits<float>::infinity();
  EXPECT_THROW((void)ComparisonPlacementFromGeometry(not_finite), std::invalid_argument);
}

}  // namespace alcedo::ui::test
