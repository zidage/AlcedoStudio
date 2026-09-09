//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QPointF>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QVector2D>

#include "edit/geometry/types.hpp"
#include "edit/mask/mask_model.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/edit_viewer/mask_edit_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_geometry.hpp"
#include "ui/edit_viewer/mask_overlay_layout.hpp"
#include "ui/editor_rhi/editor_overlay_item.hpp"

namespace alcedo {
namespace {

[[nodiscard]] auto MakeMapping(int widget_w, int widget_h, int image_w, int image_h, float zoom,
                               QVector2D pan, float dpr) -> MaskEditViewMapping {
  MaskEditViewMapping mapping;
  mapping.widget      = {widget_w, widget_h, dpr};
  mapping.photograph  = {image_w, image_h};
  mapping.zoom        = zoom;
  mapping.pan         = pan;
  mapping.geometry    = MaskEditGeometry::MakeIdentityPhotographGeometry(
      Extent2D{static_cast<std::uint32_t>(image_w), static_cast<std::uint32_t>(image_h)});
  return mapping;
}

[[nodiscard]] auto SampleRadial() -> RadialMaskSource {
  RadialMaskSource source;
  source.center_x      = 0.50f;
  source.center_y      = 0.50f;
  source.major_radius  = 0.28f;
  source.minor_radius  = 0.18f;
  source.rotation      = 0.35f;
  source.inner_feather = 0.25f;
  source.outer_feather = 0.20f;
  return source;
}

[[nodiscard]] auto SampleLinear() -> LinearGradientMaskSource {
  LinearGradientMaskSource source;
  source.origin_x            = 0.50f;
  source.origin_y            = 0.50f;
  source.normal_x            = 0.0f;
  source.normal_y            = 1.0f;
  source.transition_distance = 0.30f;
  return source;
}

// Independent inverse of the native Radial evaluator (not the overlay layout helper).
[[nodiscard]] auto IndependentRadialNormalized(const RadialMaskSource& source, float rho,
                                               float theta) -> Vector2 {
  const float c  = std::cos(source.rotation);
  const float s  = std::sin(source.rotation);
  const float u  = rho * std::cos(theta);
  const float v  = rho * std::sin(theta);
  const float dx = c * (u * source.major_radius) - s * (v * source.minor_radius);
  const float dy = s * (u * source.major_radius) + c * (v * source.minor_radius);
  return Vector2{source.center_x + dx, source.center_y + dy};
}

[[nodiscard]] auto IndependentRadialTheta(const RadialMaskSource& source, Vector2 normalized)
    -> float {
  const float c  = std::cos(source.rotation);
  const float s  = std::sin(source.rotation);
  const float dx = normalized.x - source.center_x;
  const float dy = normalized.y - source.center_y;
  const float u  = (c * dx + s * dy) / std::max(source.major_radius, 1.0e-6f);
  const float v  = (-s * dx + c * dy) / std::max(source.minor_radius, 1.0e-6f);
  return std::atan2(v, u);
}

[[nodiscard]] auto IndependentItemToNormalized(const MaskEditViewMapping& mapping,
                                               const QPointF& item) -> Vector2 {
  const auto sample = MaskEditGeometry::MapItemToReference(mapping, item, true);
  EXPECT_TRUE(sample.has_value());
  return sample ? sample->normalized : Vector2{};
}

[[nodiscard]] auto IndependentMapNormalized(const MaskEditViewMapping& mapping, Vector2 normalized)
    -> QPointF {
  const Vector2 reference = MaskEditGeometry::ReferencePixelsFromNormalized(
      normalized, mapping.geometry.full_reference_extent);
  const auto item = MaskEditGeometry::MapReferenceToItem(mapping, reference);
  EXPECT_TRUE(item.has_value());
  return item.value_or(QPointF());
}

[[nodiscard]] auto PointInTriangle(const QPointF& p, const QPointF& a, const QPointF& b,
                                   const QPointF& c) -> bool {
  const auto cross = [](const QPointF& o, const QPointF& u, const QPointF& v) {
    return (u.x() - o.x()) * (v.y() - o.y()) - (u.y() - o.y()) * (v.x() - o.x());
  };
  const double c1      = cross(a, b, p);
  const double c2      = cross(b, c, p);
  const double c3      = cross(c, a, p);
  const bool   has_neg = (c1 < 0) || (c2 < 0) || (c3 < 0);
  const bool   has_pos = (c1 > 0) || (c2 > 0) || (c3 > 0);
  return !(has_neg && has_pos);
}

[[nodiscard]] auto VerticesCoverPoint(const std::vector<MaskOverlayVertex>& vertices,
                                      const QPointF& point) -> bool {
  for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
    if (PointInTriangle(point, QPointF(vertices[i].x, vertices[i].y),
                        QPointF(vertices[i + 1].x, vertices[i + 1].y),
                        QPointF(vertices[i + 2].x, vertices[i + 2].y))) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto OverlayCoversPoint(const MaskOverlaySceneGeometry& scene, const QPointF& point)
    -> bool {
  return VerticesCoverPoint(scene.handle_fill, point) ||
         VerticesCoverPoint(scene.handle_outline, point) ||
         VerticesCoverPoint(scene.connectors, point) || VerticesCoverPoint(scene.cursor, point) ||
         VerticesCoverPoint(scene.creation_guides, point);
}

[[nodiscard]] auto MaxDistanceFrom(const std::vector<MaskOverlayVertex>& vertices,
                                   const QPointF& origin) -> float {
  float best = 0.0f;
  for (const auto& vertex : vertices) {
    best = std::max(best, static_cast<float>(std::hypot(vertex.x - origin.x(), vertex.y - origin.y())));
  }
  return best;
}

[[nodiscard]] auto HandleById(const MaskOverlayDisplay& display, MaskOverlayHandleId id)
    -> const MaskOverlayHandle* {
  for (const auto& handle : display.handles) {
    if (handle.id == id) {
      return &handle;
    }
  }
  return nullptr;
}

struct OverlayWindow {
  QQuickWindow                    window;
  editor_rhi::EditorOverlayItem*  overlay = nullptr;

  OverlayWindow() {
    window.setColor(QColor(32, 48, 64));
    window.resize(400, 300);
    overlay = new editor_rhi::EditorOverlayItem(window.contentItem());
    overlay->setWidth(400);
    overlay->setHeight(300);
    overlay->setVisible(true);
    window.show();
    const bool exposed = QTest::qWaitForWindowExposed(&window);
    EXPECT_TRUE(exposed);
  }

  void Present() {
    overlay->update();
    window.update();
    QSignalSpy spy(&window, &QQuickWindow::frameSwapped);
    if (!spy.wait(1500)) {
      QTest::qWait(80);
    }
    QCoreApplication::processEvents();
  }
};

}  // namespace

TEST(MaskOverlayControlTest, ExistingMaskEditHasControlsAndNoCoverageFill) {
  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  const auto style   = DefaultMaskOverlayStyle();
  const auto radial  = MakeRadialExistingOverlayDisplay(mapping, SampleRadial(), style, {});
  const auto linear  = MakeLinearExistingOverlayDisplay(mapping, SampleLinear(), style, {});
  const auto brush =
      MakeBrushExistingOverlayDisplay(mapping, Vector2{200.0f, 150.0f}, {});

  EXPECT_EQ(radial.mode, MaskOverlayMode::Existing);
  EXPECT_EQ(linear.mode, MaskOverlayMode::Existing);
  EXPECT_EQ(brush.mode, MaskOverlayMode::Existing);
  EXPECT_GE(radial.handles.size(), 3u);
  EXPECT_NE(HandleById(radial, MaskOverlayHandleId::RadialCenter), nullptr);
  EXPECT_NE(HandleById(linear, MaskOverlayHandleId::LinearOrigin), nullptr);
  EXPECT_NE(HandleById(brush, MaskOverlayHandleId::BrushMove), nullptr);
  EXPECT_TRUE(radial.creation_path.empty());
  EXPECT_TRUE(radial.creation_outline.empty());
  EXPECT_TRUE(brush.creation_path.empty());

  const auto radial_scene = BuildMaskOverlaySceneGeometry(radial, style);
  const auto linear_scene = BuildMaskOverlaySceneGeometry(linear, style);
  const auto brush_scene  = BuildMaskOverlaySceneGeometry(brush, style);
  EXPECT_EQ(radial_scene.coverage_fill_vertex_count, 0);
  EXPECT_EQ(radial_scene.settled_stroke_path_vertex_count, 0);
  EXPECT_TRUE(radial_scene.creation_guides.empty());
  EXPECT_GT(radial_scene.handle_count, 0);
  EXPECT_FALSE(radial_scene.handle_fill.empty());
  EXPECT_TRUE(brush_scene.creation_guides.empty());
  EXPECT_EQ(linear_scene.coverage_fill_vertex_count, 0);

  const auto interior = IndependentMapNormalized(
      mapping, IndependentRadialNormalized(SampleRadial(), 0.40f, 1.05f));
  EXPECT_FALSE(OverlayCoversPoint(radial_scene, interior))
      << "existing Radial overlay filled coverage at " << interior.x() << "," << interior.y();

  OverlayWindow host;
  host.overlay->setMaskOverlayDisplay(radial);
  host.Present();
  const QImage grab = host.window.grabWindow();
  ASSERT_FALSE(grab.isNull());
  ASSERT_GT(grab.width(), 0);
  const qreal dpr = host.window.devicePixelRatio();
  const int px    = std::clamp(static_cast<int>(std::lround(interior.x() * dpr)), 0, grab.width() - 1);
  const int py    = std::clamp(static_cast<int>(std::lround(interior.y() * dpr)), 0, grab.height() - 1);
  const QColor sample = grab.pixelColor(px, py);
  const QColor clear  = host.window.color();
  EXPECT_LT(std::abs(sample.red() - clear.red()), 40);
  EXPECT_LT(std::abs(sample.green() - clear.green()), 40);
  EXPECT_LT(std::abs(sample.blue() - clear.blue()), 40)
      << "grabbed (" << sample.red() << "," << sample.green() << "," << sample.blue()
      << ") at interior; expected window color without Mask coverage tint";

  const auto* center = HandleById(radial, MaskOverlayHandleId::RadialCenter);
  ASSERT_NE(center, nullptr);
  const int hx = std::clamp(static_cast<int>(std::lround(center->item.x() * dpr)), 0,
                            grab.width() - 1);
  const int hy = std::clamp(static_cast<int>(std::lround(center->item.y() * dpr)), 0,
                            grab.height() - 1);
  const QColor handle_pixel = grab.pixelColor(hx, hy);
  EXPECT_GT(handle_pixel.red() + handle_pixel.green() + handle_pixel.blue(), 80);
}

TEST(MaskOverlayControlTest, MaskControlsUpdateWhileRenderIsHeld) {
  std::atomic<bool> render_busy{true};
  std::thread renderer([&] {
    while (render_busy.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  const auto style   = DefaultMaskOverlayStyle();
  auto display       = MakeRadialExistingOverlayDisplay(mapping, SampleRadial(), style, {});
  ASSERT_FALSE(display.handles.empty());
  const QPointF before = display.handles.front().item;
  for (auto& handle : display.handles) {
    handle.item += QPointF(18.0, 0.0);
  }

  editor_rhi::EditorOverlayItem overlay;
  overlay.setMaskOverlayDisplay(display);
  const auto& scene = overlay.lastMaskSceneGeometry();
  EXPECT_GT(scene.handle_count, 0);
  EXPECT_EQ(scene.coverage_fill_vertex_count, 0);
  bool moved = false;
  for (const auto& vertex : scene.handle_fill) {
    if (std::abs(vertex.x - static_cast<float>(before.x() + 18.0)) < 8.0f) {
      moved = true;
      break;
    }
  }
  EXPECT_TRUE(moved);

  render_busy.store(false, std::memory_order_relaxed);
  renderer.join();
}

TEST(MaskOverlayControlTest, MaskOverlayReusesNodesForMovement) {
  OverlayWindow host;
  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  auto display =
      MakeRadialExistingOverlayDisplay(mapping, SampleRadial(), DefaultMaskOverlayStyle(), {});
  host.overlay->setMaskOverlayDisplay(display);
  host.Present();
  const int creates_after_first = host.overlay->maskPaintNodeCreateCount();
  ASSERT_GT(creates_after_first, 0);

  for (auto& handle : display.handles) {
    handle.item += QPointF(10.0, 6.0);
  }
  host.overlay->setMaskOverlayDisplay(display);
  host.Present();
  EXPECT_EQ(host.overlay->maskPaintNodeCreateCount(), creates_after_first);
  EXPECT_GT(host.overlay->lastMaskSceneGeometry().handle_count, 0);
}

TEST(MaskOverlayControlTest, MaskControlsRecreateAfterSceneInvalidation) {
  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  const auto display =
      MakeLinearExistingOverlayDisplay(mapping, SampleLinear(), DefaultMaskOverlayStyle(), {});
  int handles_before = 0;
  int vertices_before = 0;
  {
    OverlayWindow first;
    first.overlay->setMaskOverlayDisplay(display);
    first.Present();
    handles_before  = first.overlay->lastMaskSceneGeometry().handle_count;
    vertices_before = static_cast<int>(first.overlay->lastMaskSceneGeometry().handle_fill.size());
    ASSERT_GT(first.overlay->maskPaintNodeCreateCount(), 0);
    ASSERT_GT(handles_before, 0);
  }

  OverlayWindow second;
  second.overlay->setMaskOverlayDisplay(display);
  second.Present();
  EXPECT_GT(second.overlay->maskPaintNodeCreateCount(), 0);
  EXPECT_EQ(second.overlay->lastMaskSceneGeometry().handle_count, handles_before);
  EXPECT_EQ(static_cast<int>(second.overlay->lastMaskSceneGeometry().handle_fill.size()),
            vertices_before);
  EXPECT_EQ(second.overlay->lastMaskSceneGeometry().coverage_fill_vertex_count, 0);
}

TEST(MaskOverlayControlTest, MaskControlsKeepLogicalSizeAndThemeColors) {
  auto& theme = ui::AppTheme::Instance();
  theme.setCurrentThemeIndex(0);
  EXPECT_EQ(theme.maskOverlayHandleRadius(),
            static_cast<int>(kMaskOverlayHandleRadiusLogicalPx));
  EXPECT_NEAR(theme.maskOverlayHandleOutlineWidth(),
              static_cast<qreal>(kMaskOverlayHandleOutlineWidthLogicalPx), 1.0e-6);
  EXPECT_EQ(theme.maskOverlayHandleHitRadius(),
            static_cast<int>(kMaskHandleHitRadiusLogicalPx));
  EXPECT_EQ(theme.maskOverlayControlColor(), theme.textColor());
  EXPECT_EQ(theme.maskOverlayControlOutlineColor(), theme.bgCanvasColor());
  EXPECT_EQ(theme.maskOverlayInactiveColor(), theme.textMutedColor());

  MaskOverlayStyle style = DefaultMaskOverlayStyle();
  style.control_fill     = theme.maskOverlayControlColor();
  style.control_outline  = theme.maskOverlayControlOutlineColor();
  style.inactive         = theme.maskOverlayInactiveColor();

  const auto fit = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  const auto zoomed =
      MakeMapping(400, 300, 400, 300, 2.0f, QVector2D(0, 0), 1.25f);
  const auto fit_display =
      MakeRadialExistingOverlayDisplay(fit, SampleRadial(), style, {});
  const auto zoom_display =
      MakeRadialExistingOverlayDisplay(zoomed, SampleRadial(), style, {});
  const auto* fit_center  = HandleById(fit_display, MaskOverlayHandleId::RadialCenter);
  const auto* zoom_center = HandleById(zoom_display, MaskOverlayHandleId::RadialCenter);
  ASSERT_NE(fit_center, nullptr);
  ASSERT_NE(zoom_center, nullptr);

  MaskOverlayDisplay fit_handle_only;
  fit_handle_only.mode        = MaskOverlayMode::Existing;
  fit_handle_only.source_kind = MaskOverlaySourceKind::Radial;
  fit_handle_only.handles.push_back(*fit_center);
  MaskOverlayDisplay zoom_handle_only = fit_handle_only;
  zoom_handle_only.handles.front().item = zoom_center->item;

  const auto fit_scene  = BuildMaskOverlaySceneGeometry(fit_handle_only, style);
  const auto zoom_scene = BuildMaskOverlaySceneGeometry(zoom_handle_only, style);
  const float fit_span  = MaxDistanceFrom(fit_scene.handle_fill, fit_center->item);
  const float zoom_span = MaxDistanceFrom(zoom_scene.handle_fill, zoom_center->item);
  EXPECT_NEAR(fit_span, zoom_span, 0.75f);
  EXPECT_NEAR(fit_span, style.handle_radius_logical_px, 0.75f);

  ASSERT_FALSE(fit_scene.handle_fill.empty());
  const float alpha = static_cast<float>(style.control_fill.alphaF());
  EXPECT_NEAR(fit_scene.handle_fill.front().r, style.control_fill.red() * alpha, 2.0);
  EXPECT_NEAR(fit_scene.handle_fill.front().g, style.control_fill.green() * alpha, 2.0);
  EXPECT_NEAR(fit_scene.handle_fill.front().b, style.control_fill.blue() * alpha, 2.0);

  const auto cursor_fit = MapReferenceRadiusToItem(fit, Vector2{200.0f, 150.0f}, 40.0f);
  const auto cursor_zoom = MapReferenceRadiusToItem(zoomed, Vector2{200.0f, 150.0f}, 40.0f);
  ASSERT_TRUE(cursor_fit.has_value());
  ASSERT_TRUE(cursor_zoom.has_value());
  EXPECT_GT(*cursor_zoom, *cursor_fit);
}

TEST(MaskOverlayControlTest, DegenerateRadialProducesEmptyGeometry) {
  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  RadialMaskSource source;
  source.major_radius = 0.0f;
  source.minor_radius = 0.0f;
  const auto display =
      MakeRadialExistingOverlayDisplay(mapping, source, DefaultMaskOverlayStyle(), {});
  EXPECT_EQ(display.mode, MaskOverlayMode::Hidden);
  const auto scene = BuildMaskOverlaySceneGeometry(display, DefaultMaskOverlayStyle());
  EXPECT_EQ(scene.handle_count, 0);
  EXPECT_TRUE(scene.handle_fill.empty());
}

TEST(MaskOverlayControlTest, RadialCreationOutlineStaysWithinChordTolerance) {
  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  const auto source  = SampleRadial();
  const auto outline = TessellateRadialBoundaryItemPolyline(mapping, source, 1.0f, {});
  ASSERT_GE(outline.size(), 3u);
  float max_error = 0.0f;
  constexpr float kPi = 3.14159265358979323846f;
  for (std::size_t i = 0; i < outline.size(); ++i) {
    const QPointF a = outline[i];
    const QPointF b = outline[(i + 1) % outline.size()];
    const QPointF chord(0.5 * (a.x() + b.x()), 0.5 * (a.y() + b.y()));
    const float t0 = IndependentRadialTheta(source, IndependentItemToNormalized(mapping, a));
    const float t1 = IndependentRadialTheta(source, IndependentItemToNormalized(mapping, b));
    float dt       = t1 - t0;
    while (dt > kPi) {
      dt -= 2.0f * kPi;
    }
    while (dt < -kPi) {
      dt += 2.0f * kPi;
    }
    const QPointF truth = IndependentMapNormalized(
        mapping, IndependentRadialNormalized(source, 1.0f, t0 + 0.5f * dt));
    max_error = std::max(
        max_error, static_cast<float>(std::hypot(truth.x() - chord.x(), truth.y() - chord.y())));
  }
  EXPECT_LE(max_error, kMaskOverlayMaxChordDeviationLogicalPx + 0.05f);

  const auto creating =
      MakeRadialCreatingOverlayDisplay(mapping, source, DefaultMaskOverlayStyle(), {});
  EXPECT_EQ(creating.mode, MaskOverlayMode::Creating);
  EXPECT_FALSE(creating.creation_outline.empty());
  const auto scene = BuildMaskOverlaySceneGeometry(creating, DefaultMaskOverlayStyle());
  EXPECT_EQ(scene.coverage_fill_vertex_count, 0);
  EXPECT_FALSE(scene.creation_guides.empty());
}

TEST(MaskOverlayControlTest, HiddenDisplayClearsMaskOverlayNodes) {
  OverlayWindow host;
  const auto mapping = MakeMapping(400, 300, 400, 300, 1.0f, QVector2D(0, 0), 1.0f);
  host.overlay->setMaskOverlayDisplay(
      MakeBrushExistingOverlayDisplay(mapping, Vector2{200.0f, 150.0f}, {}));
  host.Present();
  EXPECT_GT(host.overlay->lastMaskSceneGeometry().handle_count, 0);
  host.overlay->setMaskOverlayDisplay(MaskOverlayDisplay{});
  host.Present();
  EXPECT_EQ(host.overlay->lastMaskSceneGeometry().handle_count, 0);
  EXPECT_TRUE(host.overlay->lastMaskSceneGeometry().handle_fill.empty());
}

}  // namespace alcedo
