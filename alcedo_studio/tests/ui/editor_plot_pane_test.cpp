//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tone Curve TC4 (issue #889): view transform, layer geometry, hit test, input routing, and
// painting of the reusable plot pane.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "edit/scope/scope_analyzer.hpp"
#include "ui/alcedo_main/plot_pane/plot_grid_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_handle_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_histogram_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_pane.hpp"
#include "ui/alcedo_main/plot_pane/plot_polyline_layer.hpp"
#include "ui/alcedo_main/plot_pane/plot_view_transform.hpp"
#include "ui/qt_test_plugin_paths.hpp"

namespace alcedo::ui {
namespace {

// Pane of 200 x 200 with insets 20 left, 10 top, 10 right, 20 bottom: plot rectangle
// (20, 10, 170, 170). Limits [0, 1] x [0, 1].
constexpr qreal  kPaneSize    = 200.0;
constexpr qreal  kInsetLeft   = 20.0;
constexpr qreal  kInsetTop    = 10.0;
constexpr qreal  kInsetRight  = 10.0;
constexpr qreal  kInsetBottom = 20.0;
constexpr qreal  kPlotSide    = 170.0;
constexpr double kTolerance   = 1e-9;
const QRectF     kPlotRect(kInsetLeft, kInsetTop, kPlotSide, kPlotSide);

auto             UnitTransform() -> PlotViewTransform {
  PlotViewTransform view(PlotDataRect{}, PlotViewTransform::kDefaultMaximumZoom);
  view.SetItemRect(kPlotRect);
  return view;
}

void ExpectNear(const QPointF& actual, const QPointF& expected, double tolerance = kTolerance) {
  EXPECT_NEAR(actual.x(), expected.x(), tolerance);
  EXPECT_NEAR(actual.y(), expected.y(), tolerance);
}

void ExpectInsideLimits(const PlotViewTransform& view) {
  const auto& v = view.view();
  const auto& l = view.limits();
  EXPECT_GE(v.x.minimum, l.x.minimum - kTolerance);
  EXPECT_LE(v.x.maximum, l.x.maximum + kTolerance);
  EXPECT_GE(v.y.minimum, l.y.minimum - kTolerance);
  EXPECT_LE(v.y.maximum, l.y.maximum + kTolerance);
}

class TestHistogramSource final : public PlotHistogramSource {
 public:
  [[nodiscard]] auto histogram() const -> const ScopeHistogramRenderData& override {
    return histogram_;
  }
  void Publish(const std::vector<uint32_t>& counts, int bins) {
    histogram_ = BuildScopeHistogramRenderData(counts, bins);
    emit HistogramChanged();
  }

 private:
  ScopeHistogramRenderData histogram_{};
};

// Gives the tests the protected event handlers of the pane.
class InputPlotPane final : public PlotPane {
 public:
  using PlotPane::hoverLeaveEvent;
  using PlotPane::keyPressEvent;
  using PlotPane::mouseDoubleClickEvent;
  using PlotPane::mouseMoveEvent;
  using PlotPane::mousePressEvent;
  using PlotPane::mouseReleaseEvent;
  using PlotPane::wheelEvent;
};

void ConfigurePane(PlotPane& pane) {
  pane.setSize(QSizeF(kPaneSize, kPaneSize));
  pane.set_inset_left(kInsetLeft);
  pane.set_inset_top(kInsetTop);
  pane.set_inset_right(kInsetRight);
  pane.set_inset_bottom(kInsetBottom);
}

auto Mouse(QEvent::Type type, const QPointF& position, Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) -> QMouseEvent {
  const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
  return QMouseEvent(type, position, position, button, buttons, modifiers);
}

auto Wheel(const QPointF& position, int angle, Qt::KeyboardModifiers modifiers) -> QWheelEvent {
  return QWheelEvent(position, position, QPoint(), QPoint(0, angle), Qt::NoButton, modifiers,
                     Qt::NoScrollPhase, false);
}

// ---- View transform ----------------------------------------------------------------------------

TEST(PlotViewTransformTest, DataToItemAndBackIsTheIdentity) {
  PlotViewTransform view = UnitTransform();
  ExpectNear(view.ToItem({0.0, 0.0}), {20.0, 180.0});
  ExpectNear(view.ToItem({1.0, 1.0}), {190.0, 10.0});

  const std::vector<QPointF> points = {
      {0.0, 0.0}, {1.0, 1.0}, {0.4135, 0.18}, {0.93, 0.07}, {-0.2, 1.3}};
  for (const auto& point : points) {
    ExpectNear(view.ToData(view.ToItem(point)), point);
  }
  ASSERT_TRUE(view.SetView({{0.2, 0.6}, {0.1, 0.5}}));
  for (const auto& point : points) {
    ExpectNear(view.ToData(view.ToItem(point)), point);
  }
  const std::vector<QPointF> items = {{20.0, 10.0}, {105.0, 95.0}, {3.0, 199.0}};
  for (const auto& item : items) {
    ExpectNear(view.ToItem(view.ToData(item)), item);
  }
}

TEST(PlotViewTransformTest, ZoomAtAPointKeepsThatDataPointAtItsItemPosition) {
  PlotViewTransform view = UnitTransform();
  const QPointF     anchor(0.4, 0.3);
  const QPointF     before = view.ToItem(anchor);
  ASSERT_TRUE(view.ZoomAt(anchor, 2.0));
  ExpectNear(view.ToItem(anchor), before);
  EXPECT_NEAR(view.view().x.span(), 0.5, kTolerance);
  EXPECT_NEAR(view.view().y.span(), 0.5, kTolerance);
  EXPECT_NEAR(view.zoom(), 2.0, kTolerance);
  EXPECT_TRUE(view.is_zoomed());

  const QPointF second(0.45, 0.35);
  const QPointF second_before = view.ToItem(second);
  ASSERT_TRUE(view.ZoomAt(second, 1.5));
  ExpectNear(view.ToItem(second), second_before);
  EXPECT_NEAR(view.zoom(), 3.0, kTolerance);

  // Zooming out at the same point keeps it as well while the view stays inside the limits.
  ASSERT_TRUE(view.ZoomAt(second, 1.0 / 1.2));
  ExpectNear(view.ToItem(second), second_before);
}

TEST(PlotViewTransformTest, PanAndZoomStayInsideTheLimits) {
  PlotViewTransform view = UnitTransform();
  // Zoom at a corner: the view moves back inside the limits.
  ASSERT_TRUE(view.ZoomAt({0.02, 0.98}, 4.0));
  ExpectInsideLimits(view);
  EXPECT_NEAR(view.view().x.span(), 0.25, kTolerance);

  ASSERT_TRUE(view.Pan({1000.0, -1000.0}));
  ExpectInsideLimits(view);
  EXPECT_NEAR(view.view().x.minimum, 0.0, kTolerance);
  EXPECT_NEAR(view.view().y.minimum, 0.0, kTolerance);
  EXPECT_FALSE(view.Pan({10.0, -10.0})) << "the view is at the corner of the limits";

  // A pan of 17 px moves the view by 17 / (170 * 4) data units.
  ASSERT_TRUE(view.Pan({-17.0, 0.0}));
  EXPECT_NEAR(view.view().x.minimum, 0.025, kTolerance);

  // The zoom stops at the maximum zoom, and a zoom out stops at the limits.
  view.ZoomAt({0.5, 0.5}, 1000.0);
  EXPECT_NEAR(view.zoom(), PlotViewTransform::kDefaultMaximumZoom, 1e-6);
  ExpectInsideLimits(view);
  view.ZoomAt({0.5, 0.5}, 1e-4);
  EXPECT_EQ(view.view(), view.limits());
  EXPECT_FALSE(view.is_zoomed());

  // A view outside the limits moves inside them; a view larger than the limits is the limits.
  ASSERT_TRUE(view.SetView({{0.8, 1.3}, {-0.4, 0.1}}));
  ExpectInsideLimits(view);
  EXPECT_NEAR(view.view().x.span(), 0.5, kTolerance);
  view.SetView({{-1.0, 2.0}, {-1.0, 2.0}});
  EXPECT_EQ(view.view(), view.limits());

  EXPECT_THROW(PlotViewTransform(PlotDataRect{{1.0, 1.0}, {0.0, 1.0}}, 32.0),
               std::invalid_argument);
  EXPECT_THROW(PlotViewTransform(PlotDataRect{}, 0.5), std::invalid_argument);
}

// ---- Layers ------------------------------------------------------------------------------------

TEST(PlotPolylineLayerTest, PolylineItemPointsFollowTheZoomedView) {
  PlotViewTransform view = UnitTransform();
  ASSERT_TRUE(view.ZoomAt({0.5, 0.5}, 2.0));  // view [0.25, 0.75] x [0.25, 0.75]

  PlotPolylineLayer layer;
  layer.set_points({{0.0, 0.0}, {0.5, 0.25}, {0.75, 0.6}, {1.0, 1.0}});
  const auto item = layer.ItemPoints(view);
  ASSERT_EQ(item.size(), 4U);
  // x = 20 + (x - 0.25) / 0.5 * 170, y = 180 - (y - 0.25) / 0.5 * 170.
  ExpectNear(item[0], {-65.0, 265.0});
  ExpectNear(item[1], {105.0, 180.0});
  ExpectNear(item[2], {190.0, 61.0});
  ExpectNear(item[3], {275.0, -75.0});

  // The drawing is clipped to the plot rectangle (with its antialiased edge).
  PlotLayerGeometry geometry;
  layer.Build(view, geometry);
  ASSERT_FALSE(geometry.triangles.empty());
  const QRectF edge = kPlotRect.adjusted(-3.0, -3.0, 3.0, 3.0);
  for (const auto& vertex : geometry.triangles) {
    EXPECT_TRUE(edge.contains(QPointF(vertex.x, vertex.y)))
        << "vertex " << vertex.x << "," << vertex.y << " outside the plot rectangle";
  }

  layer.set_visible(false);
  layer.Build(view, geometry);
  EXPECT_TRUE(geometry.triangles.empty());
}

TEST(PlotHandleLayerTest, PointerSevenPixelsFromAHandleHitsItAtRadiusEightAtEachZoom) {
  PlotHandleLayer layer;
  layer.set_hit_radius(8.0);
  layer.set_points({{0.5, 0.5}, {0.2, 0.9}});
  for (const double zoom : {1.0, 2.0, 8.0, 32.0}) {
    PlotViewTransform view = UnitTransform();
    view.ZoomAt({0.5, 0.5}, zoom);
    SCOPED_TRACE(testing::Message() << "zoom " << zoom);
    const QPointF center = view.ToItem({0.5, 0.5});
    for (const QPointF& offset :
         {QPointF(7.0, 0.0), QPointF(0.0, -7.0), QPointF(-4.95, 4.95), QPointF(7.9, 0.0)}) {
      EXPECT_EQ(layer.HitTest(view, center + offset), 0);
    }
    EXPECT_EQ(layer.HitTest(view, center + QPointF(9.0, 0.0)), -1);
    EXPECT_EQ(layer.HitTest(view, center + QPointF(0.0, 8.5)), -1);
  }
}

TEST(PlotHandleLayerTest, HandleOutsideThePlotRectangleIsNotHitOrDrawn) {
  PlotViewTransform view = UnitTransform();
  view.ZoomAt({0.5, 0.5}, 4.0);  // view [0.375, 0.625]
  PlotHandleLayer layer;
  layer.set_points({{0.1, 0.1}});
  EXPECT_EQ(layer.HitTest(view, view.ToItem({0.1, 0.1})), -1);
  PlotLayerGeometry geometry;
  layer.Build(view, geometry);
  EXPECT_TRUE(geometry.triangles.empty());
}

TEST(PlotGridLayerTest, TicksInsideTheViewDrawLinesAndLabels) {
  PlotViewTransform view = UnitTransform();
  PlotGridLayer     layer;
  layer.set_x_ticks_list({QVariantMap{{"value", 0.25}, {"label", "-2"}},
                          QVariantMap{{"value", 0.5}, {"label", "0"}, {"strong", true}},
                          QVariantMap{{"value", 1.5}, {"label", "far"}}});
  layer.set_y_ticks_list({QVariantMap{{"value", 0.5}, {"label", "mid"}}});
  ASSERT_EQ(layer.x_ticks().size(), 3U);
  EXPECT_TRUE(layer.x_ticks()[1].strong);

  PlotLayerGeometry geometry;
  layer.Build(view, geometry);
  EXPECT_FALSE(geometry.triangles.empty());
  ASSERT_EQ(geometry.labels.size(), 3U) << "the tick at 1.5 is outside the view";
  EXPECT_EQ(geometry.labels[0].text, QStringLiteral("-2"));
  ExpectNear(geometry.labels[0].anchor, {62.5, 183.0});
  EXPECT_EQ(geometry.labels[0].alignment, Qt::AlignTop | Qt::AlignHCenter);
  EXPECT_EQ(geometry.labels[2].text, QStringLiteral("mid"));
  ExpectNear(geometry.labels[2].anchor, {17.0, 95.0});

  // After a zoom the label follows its value.
  view.ZoomAt({0.25, 0.5}, 2.0);
  layer.Build(view, geometry);
  ExpectNear(geometry.labels[0].anchor, {62.5, 183.0});
}

// ---- Input -------------------------------------------------------------------------------------

TEST(PlotPaneInputTest, PressDragAndReleaseReachTheTopLayerInDataCoordinates) {
  InputPlotPane pane;
  ConfigurePane(pane);
  PlotHandleLayer bottom;
  PlotHandleLayer top;
  bottom.set_points({{0.5, 0.5}});
  top.set_points({{0.5, 0.5}});
  pane.AddLayer(&bottom);
  pane.AddLayer(&top);

  QSignalSpy    bottom_pressed(&bottom, &PlotHandleLayer::pressed);
  QSignalSpy    pressed(&top, &PlotHandleLayer::pressed);
  QSignalSpy    moved(&top, &PlotHandleLayer::moved);
  QSignalSpy    released(&top, &PlotHandleLayer::released);

  // The pointer is 3 px right of the handle: the handle keeps that offset while it moves.
  const QPointF handle = pane.mapFromData({0.5, 0.5});
  ExpectNear(handle, {105.0, 95.0});
  const QPointF press       = handle + QPointF(3.0, 0.0);
  auto          press_event = Mouse(QEvent::MouseButtonPress, press, Qt::LeftButton);
  pane.mousePressEvent(&press_event);
  EXPECT_TRUE(press_event.isAccepted());
  ASSERT_EQ(pressed.count(), 1);
  EXPECT_EQ(pressed.at(0).at(0).toInt(), 0);
  ExpectNear(pressed.at(0).at(1).toPointF(), {0.5, 0.5});
  EXPECT_EQ(top.pressed_index(), 0);
  EXPECT_EQ(bottom_pressed.count(), 0) << "only the top layer receives the press";

  auto move_event =
      Mouse(QEvent::MouseMove, press + QPointF(17.0, -34.0), Qt::LeftButton, Qt::ShiftModifier);
  pane.mouseMoveEvent(&move_event);
  ASSERT_EQ(moved.count(), 1);
  ExpectNear(moved.at(0).at(1).toPointF(), {0.6, 0.7}, 1e-9);
  EXPECT_EQ(moved.at(0).at(2).toInt(), static_cast<int>(Qt::ShiftModifier));

  auto release_event =
      Mouse(QEvent::MouseButtonRelease, press + QPointF(34.0, -17.0), Qt::NoButton);
  pane.mouseReleaseEvent(&release_event);
  ASSERT_EQ(released.count(), 1);
  EXPECT_EQ(released.at(0).at(0).toInt(), 0);
  ExpectNear(released.at(0).at(1).toPointF(), {0.7, 0.6}, 1e-9);
  EXPECT_EQ(top.pressed_index(), -1);

  // A move after the release reaches no layer.
  auto late_move = Mouse(QEvent::MouseMove, press, Qt::NoButton);
  pane.mouseMoveEvent(&late_move);
  EXPECT_EQ(moved.count(), 1);

  // The top layer, when not interactive, leaves the press to the layer under it.
  top.set_interactive(false);
  auto second_press = Mouse(QEvent::MouseButtonPress, press, Qt::LeftButton);
  pane.mousePressEvent(&second_press);
  EXPECT_EQ(bottom_pressed.count(), 1);
}

TEST(PlotPaneInputTest, PlainWheelStaysUnacceptedAndCtrlWheelZoomsAtThePointer) {
  InputPlotPane pane;
  ConfigurePane(pane);
  QSignalSpy    view_changed(&pane, &PlotPane::viewChanged);

  const QPointF pointer(62.0, 140.0);
  auto          plain = Wheel(pointer, 120, Qt::NoModifier);
  pane.wheelEvent(&plain);
  EXPECT_FALSE(plain.isAccepted());
  EXPECT_FALSE(pane.zoomed());
  EXPECT_EQ(view_changed.count(), 0);

  const QPointF data_under_pointer = pane.mapToData(pointer);
  auto          ctrl               = Wheel(pointer, 120, Qt::ControlModifier);
  pane.wheelEvent(&ctrl);
  EXPECT_TRUE(ctrl.isAccepted());
  EXPECT_TRUE(pane.zoomed());
  EXPECT_NEAR(pane.zoom(), pane.wheel_zoom_step(), 1e-9);
  ExpectNear(pane.mapFromData(data_under_pointer), pointer, 1e-9);
  EXPECT_EQ(view_changed.count(), 1);

  // Ctrl + wheel down zooms out again.
  auto ctrl_out = Wheel(pointer, -120, Qt::ControlModifier);
  pane.wheelEvent(&ctrl_out);
  EXPECT_FALSE(pane.zoomed());
}

TEST(PlotPaneInputTest, DragInAnEmptyAreaMovesAZoomedView) {
  InputPlotPane pane;
  ConfigurePane(pane);
  PlotHandleLayer handles;
  handles.set_points({{0.5, 0.5}});
  pane.AddLayer(&handles);

  // Not zoomed: a drag in an empty area leaves the view.
  const QPointF empty = pane.mapFromData({0.3, 0.3});
  auto          press = Mouse(QEvent::MouseButtonPress, empty, Qt::LeftButton);
  pane.mousePressEvent(&press);
  auto move = Mouse(QEvent::MouseMove, empty + QPointF(17.0, 0.0), Qt::LeftButton);
  pane.mouseMoveEvent(&move);
  auto release = Mouse(QEvent::MouseButtonRelease, empty + QPointF(17.0, 0.0), Qt::NoButton);
  pane.mouseReleaseEvent(&release);
  EXPECT_FALSE(pane.zoomed());

  pane.zoomAt({0.5, 0.5}, 2.0);  // view [0.25, 0.75]
  const QPointF zoomed_empty = pane.mapFromData({0.3, 0.3});
  auto          zoomed_press = Mouse(QEvent::MouseButtonPress, zoomed_empty, Qt::LeftButton);
  pane.mousePressEvent(&zoomed_press);
  auto zoomed_move = Mouse(QEvent::MouseMove, zoomed_empty + QPointF(17.0, -34.0), Qt::LeftButton);
  pane.mouseMoveEvent(&zoomed_move);
  // 340 px per data unit: the view moves left by 0.05 and down by 0.1, so the data follows
  // the pointer.
  EXPECT_NEAR(pane.view().x(), 0.20, 1e-9);
  EXPECT_NEAR(pane.view().y(), 0.15, 1e-9);
  // The data point under the pointer at the press stays under the pointer.
  ExpectNear(pane.mapFromData({0.3, 0.3}), zoomed_empty + QPointF(17.0, -34.0), 1e-9);
  auto zoomed_release =
      Mouse(QEvent::MouseButtonRelease, zoomed_empty + QPointF(17.0, -34.0), Qt::NoButton);
  pane.mouseReleaseEvent(&zoomed_release);
}

TEST(PlotPaneInputTest, KeysReachTheHandleLayerOnlyWithASelectedHandle) {
  InputPlotPane pane;
  ConfigurePane(pane);
  PlotHandleLayer handles;
  handles.set_points({{0.2, 0.2}, {0.5, 0.6}});
  pane.AddLayer(&handles);
  QSignalSpy key_pressed(&handles, &PlotHandleLayer::keyPressed);

  QKeyEvent  no_selection(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
  pane.keyPressEvent(&no_selection);
  EXPECT_FALSE(no_selection.isAccepted());
  EXPECT_EQ(key_pressed.count(), 0);

  handles.set_selected_index(1);
  QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::ShiftModifier);
  pane.keyPressEvent(&up);
  EXPECT_TRUE(up.isAccepted());
  ASSERT_EQ(key_pressed.count(), 1);
  EXPECT_EQ(key_pressed.at(0).at(0).toInt(), 1);
  EXPECT_EQ(key_pressed.at(0).at(1).toInt(), static_cast<int>(Qt::Key_Up));
  EXPECT_EQ(key_pressed.at(0).at(2).toInt(), static_cast<int>(Qt::ShiftModifier));

  QKeyEvent letter(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
  pane.keyPressEvent(&letter);
  EXPECT_FALSE(letter.isAccepted());

  QSignalSpy double_clicked(&handles, &PlotHandleLayer::doubleClicked);
  auto       double_click =
      Mouse(QEvent::MouseButtonDblClick, pane.mapFromData({0.2, 0.2}), Qt::LeftButton);
  pane.mouseDoubleClickEvent(&double_click);
  ASSERT_EQ(double_clicked.count(), 1);
  EXPECT_EQ(double_clicked.at(0).at(0).toInt(), 0);
}

TEST(PlotPaneInputTest, PressThatNoLayerTakesReportsTheBackgroundPress) {
  InputPlotPane pane;
  ConfigurePane(pane);
  PlotHandleLayer handles;
  handles.set_interactive(true);
  handles.set_points({{0.5, 0.5}});
  pane.AddLayer(&handles);
  QSignalSpy background(&pane, &PlotPane::backgroundPressed);

  auto on_handle = Mouse(QEvent::MouseButtonPress, pane.mapFromData({0.5, 0.5}), Qt::LeftButton);
  pane.mousePressEvent(&on_handle);
  auto on_handle_release =
      Mouse(QEvent::MouseButtonRelease, pane.mapFromData({0.5, 0.5}), Qt::NoButton);
  pane.mouseReleaseEvent(&on_handle_release);
  EXPECT_EQ(background.count(), 0) << "the handle layer took the press";

  auto empty = Mouse(QEvent::MouseButtonPress, pane.mapFromData({0.2, 0.7}), Qt::LeftButton,
                     Qt::ShiftModifier);
  pane.mousePressEvent(&empty);
  ASSERT_EQ(background.count(), 1);
  ExpectNear(background.at(0).at(0).toPointF(), {0.2, 0.7});
  EXPECT_EQ(background.at(0).at(1).toInt(), static_cast<int>(Qt::ShiftModifier));
}

TEST(PlotPaneInputTest, PointerPositionFollowsTheMovesAndLeavesWithTheHover) {
  InputPlotPane pane;
  ConfigurePane(pane);
  QSignalSpy pointer_changed(&pane, &PlotPane::pointerChanged);
  EXPECT_FALSE(pane.pointer_inside());

  auto press = Mouse(QEvent::MouseButtonPress, pane.mapFromData({0.3, 0.3}), Qt::LeftButton);
  pane.mousePressEvent(&press);
  auto move = Mouse(QEvent::MouseMove, pane.mapFromData({0.4, 0.6}), Qt::LeftButton);
  pane.mouseMoveEvent(&move);
  EXPECT_TRUE(pane.pointer_inside());
  ExpectNear(pane.pointer_position(), {0.4, 0.6});
  EXPECT_GE(pointer_changed.count(), 1);

  // The same item position shows another data position after a zoom.
  pane.zoomAt({0.5, 0.5}, 2.0);
  ExpectNear(pane.pointer_position(), {0.45, 0.55});

  // A move into the label inset is outside the plot rectangle.
  auto inset = Mouse(QEvent::MouseMove, QPointF(5.0, 100.0), Qt::LeftButton);
  pane.mouseMoveEvent(&inset);
  EXPECT_FALSE(pane.pointer_inside());
  auto release = Mouse(QEvent::MouseButtonRelease, QPointF(5.0, 100.0), Qt::NoButton);
  pane.mouseReleaseEvent(&release);

  auto back = Mouse(QEvent::MouseMove, pane.mapFromData({0.5, 0.5}), Qt::NoButton);
  pane.mouseMoveEvent(&back);
  ASSERT_TRUE(pane.pointer_inside());
  QHoverEvent leave(QEvent::HoverLeave, QPointF(-1.0, -1.0), QPointF(-1.0, -1.0),
                    QPointF(-1.0, -1.0));
  pane.hoverLeaveEvent(&leave);
  EXPECT_FALSE(pane.pointer_inside());
}

TEST(PlotPaneInputTest, PolylineDragKeepsThePressOffset) {
  InputPlotPane pane;
  ConfigurePane(pane);
  PlotPolylineLayer line;
  line.set_interactive(true);
  line.set_hit_radius(6.0);
  line.set_points({{0.0, 0.5}, {1.0, 0.5}});
  pane.AddLayer(&line);
  QSignalSpy    pressed(&line, &PlotPolylineLayer::pressed);
  QSignalSpy    moved(&line, &PlotPolylineLayer::moved);
  QSignalSpy    released(&line, &PlotPolylineLayer::released);

  // A press 4 px above the line grabs the nearest line point.
  const QPointF on_line = pane.mapFromData({0.4, 0.5});
  auto press = Mouse(QEvent::MouseButtonPress, on_line - QPointF(0.0, 4.0), Qt::LeftButton);
  pane.mousePressEvent(&press);
  ASSERT_EQ(pressed.count(), 1);
  ExpectNear(pressed.at(0).at(0).toPointF(), {0.4, 0.5});

  // The grabbed line point follows the pointer 1:1, 4 px below it.
  const QPointF pointer(120.0, 60.0);
  auto          move = Mouse(QEvent::MouseMove, pointer, Qt::LeftButton);
  pane.mouseMoveEvent(&move);
  ASSERT_EQ(moved.count(), 1);
  ExpectNear(moved.at(0).at(0).toPointF(), pane.mapToData(pointer + QPointF(0.0, 4.0)));
  auto release = Mouse(QEvent::MouseButtonRelease, pointer, Qt::NoButton);
  pane.mouseReleaseEvent(&release);
  ASSERT_EQ(released.count(), 1);
  ExpectNear(released.at(0).at(0).toPointF(), pane.mapToData(pointer + QPointF(0.0, 4.0)));

  // Farther than 6 px from the line, the layer does not take the press.
  auto far = Mouse(QEvent::MouseButtonPress, on_line - QPointF(0.0, 7.0), Qt::LeftButton);
  pane.mousePressEvent(&far);
  EXPECT_EQ(pressed.count(), 1);
}

// ---- Painting ----------------------------------------------------------------------------------

constexpr int kRedFirstBin = 20;
constexpr int kRedLastBin  = 59;

auto          RedOnlyCounts() -> std::vector<uint32_t> {
  std::vector<uint32_t> counts(256U * 3U, 0U);
  for (int bin = kRedFirstBin; bin <= kRedLastBin; ++bin) {
    counts[static_cast<size_t>(bin)] = 1000U;
  }
  return counts;
}

auto Brightness(const QColor& color) -> int { return color.red() + color.green() + color.blue(); }

// The offscreen platform gives the software scene graph, which draws no geometry nodes. This test
// therefore runs on the native platform (see main), and the window uses the RHI API of the
// platform and is never shown: grabWindow renders a window that is not exposed offscreen through
// that API (polish, sync, and render included). No window appears and no focus changes.
auto SelectRhiGraphicsApi() -> bool {
#if defined(Q_OS_WIN)
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
#elif defined(Q_OS_MACOS)
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#else
  QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
  return true;
}

struct PaneWindow {
  QQuickWindow window;

  PaneWindow() {
    window.setColor(QColor(0, 0, 0));
    window.resize(420, 220);
  }

  auto Present() -> QImage {
    QCoreApplication::processEvents();
    QImage image = window.grabWindow();
    EXPECT_NE(window.rendererInterface(), nullptr);
    if (window.rendererInterface()) {
      EXPECT_NE(window.rendererInterface()->graphicsApi(), QSGRendererInterface::Software);
    }
    if (const QString dump = qEnvironmentVariable("ALCEDO_PLOT_PANE_DUMP_DIR"); !dump.isEmpty()) {
      image.save(
          QStringLiteral("%1/%2.png")
              .arg(dump, QString::fromLatin1(
                             ::testing::UnitTest::GetInstance()->current_test_info()->name())));
    }
    return image;
  }
};

class PlotPanePaintTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    [[maybe_unused]] static const bool selected = SelectRhiGraphicsApi();
  }

  PlotPanePaintTest() {
    pane_ = new PlotPane(host_.window.contentItem());
    ConfigurePane(*pane_);
    pane_->set_background_color(kBackground);
    pane_->set_surface_color(kSurface);

    grid_.set_color(QColor(0, 0, 230));
    grid_.set_strong_color(QColor(0, 230, 0));
    grid_.set_label_color(QColor(255, 255, 255));
    QFont font;
    font.setPixelSize(12);
    grid_.set_label_font(font);
    grid_.SetXTicks({{0.25, QStringLiteral("88"), false}, {0.5, QString(), true}});

    source_.Publish(RedOnlyCounts(), 256);
    histogram_.set_source(&source_);
    histogram_.set_cut_color(kSurface);

    line_.set_color(QColor(255, 255, 255));
    line_.set_points({{0.0, 0.0}, {1.0, 1.0}});

    handles_.set_color(QColor(255, 255, 255));
    handles_.set_selected_color(QColor(255, 255, 255));
    handles_.set_points({{0.75, 0.4}, {0.9, 0.2}});
    handles_.set_selected_index(1);

    pane_->AddLayer(&grid_);
    pane_->AddLayer(&histogram_);
    pane_->AddLayer(&line_);
    pane_->AddLayer(&handles_);
  }

  auto        Present() -> QImage { return host_.Present(); }

  // The grab has device pixels: the item point scales by the device pixel ratio of the window.
  static auto Pixel(const QImage& image, const QPointF& item) -> QColor {
    const qreal dpr = image.devicePixelRatio();
    return image.pixelColor(static_cast<int>(std::floor(item.x() * dpr)),
                            static_cast<int>(std::floor(item.y() * dpr)));
  }

  static auto Region(const QImage& image, const QRectF& item) -> QImage {
    const qreal dpr = image.devicePixelRatio();
    return image.copy(QRectF(item.topLeft() * dpr, item.size() * dpr).toAlignedRect());
  }

  const QColor        kBackground = QColor(40, 40, 40);
  const QColor        kSurface    = QColor(22, 22, 22);
  PaneWindow          host_;
  PlotPane*           pane_ = nullptr;
  PlotGridLayer       grid_;
  TestHistogramSource source_;
  PlotHistogramLayer  histogram_;
  PlotPolylineLayer   line_;
  PlotHandleLayer     handles_;
};

TEST_F(PlotPanePaintTest, PaneDrawsEachLayer) {
  const QImage image = Present();
  ASSERT_FALSE(image.isNull());

  // Background in the insets, surface in the plot rectangle.
  EXPECT_EQ(Pixel(image, {5.0, 5.0}), kBackground);
  EXPECT_EQ(Pixel(image, pane_->mapFromData({0.95, 0.9})), kSurface);

  // Grid: an ordinary line at x 0.25 (item 62.5) and a strong line at x 0.5 (item 105).
  const QColor ordinary = Pixel(image, pane_->mapFromData({0.25, 0.9}));
  EXPECT_GT(ordinary.blue(), 150);
  EXPECT_LT(ordinary.green(), 60);
  const QPointF strong = pane_->mapFromData({0.5, 0.9});
  const int     strong_green =
      std::max(Pixel(image, strong).green(), Pixel(image, strong - QPointF(1.0, 0.0)).green());
  EXPECT_GT(strong_green, 90);

  // Histogram: red only in bins 20 to 59 (x 0.08 to 0.23), near the baseline.
  const QColor red = Pixel(image, pane_->mapFromData({0.15, 0.03}));
  EXPECT_GT(red.red(), red.green() + 30);
  EXPECT_GT(red.red(), red.blue() + 30);
  EXPECT_EQ(Pixel(image, pane_->mapFromData({0.4, 0.03})), kSurface);

  // Line: the diagonal.
  // A 1.25 px line covers most of the sampled pixel; the surface is about 66.
  EXPECT_GT(Brightness(Pixel(image, pane_->mapFromData({0.6, 0.6}))), 400);

  // Handles: a ring with a dark center, and the selected handle filled.
  const QPointF ring = pane_->mapFromData({0.75, 0.4});
  EXPECT_LT(Brightness(Pixel(image, ring)), 150) << "the ring of an unselected handle is open";
  EXPECT_GT(Brightness(Pixel(image, ring + QPointF(3.0, 0.0))), 400);
  EXPECT_GT(Brightness(Pixel(image, pane_->mapFromData({0.9, 0.2}))), 600);

  // Label "88" under the plot at x 62.5.
  int          label_pixels = 0;
  const QImage label        = Region(image, QRectF(52.0, 183.0, 22.0, 17.0));
  for (int y = 0; y < label.height(); ++y) {
    for (int x = 0; x < label.width(); ++x) {
      label_pixels += Brightness(label.pixelColor(x, y)) > 300 ? 1 : 0;
    }
  }
  EXPECT_GT(label_pixels, 4);
}

TEST_F(PlotPanePaintTest, CurveLineIsOnePointTwoFivePixelsWideAtItsPosition) {
  // The Tone Curve section draws the curve with the plotLineWidth token: 1.25 px.
  constexpr double kCurveLineWidth = 1.25;
  line_.set_width(kCurveLineWidth);
  line_.set_points({{0.0, 0.6}, {1.0, 0.6}});
  const QImage image = Present();
  ASSERT_FALSE(image.isNull());

  // A column at x 0.35 crosses no grid line, no histogram bin, and no handle. The coverage of
  // each device pixel is its brightness above the surface; the sum across the line is its width
  // and the weighted mean of the rows is its center.
  const qreal   dpr       = image.devicePixelRatio();
  const QPointF center    = pane_->mapFromData({0.35, 0.6});
  const int     column    = static_cast<int>(std::floor(center.x() * dpr));
  const int     first_row = static_cast<int>(std::floor((center.y() - 6.0) * dpr));
  const int     last_row  = static_cast<int>(std::ceil((center.y() + 6.0) * dpr));
  const double  surface   = Brightness(kSurface);
  const double  full      = Brightness(QColor(255, 255, 255)) - surface;
  double        coverage  = 0.0;
  double        moment    = 0.0;
  for (int row = first_row; row <= last_row; ++row) {
    const double pixel =
        std::clamp((Brightness(image.pixelColor(column, row)) - surface) / full, 0.0, 1.0);
    coverage += pixel;
    moment += pixel * (row + 0.5);
  }
  ASSERT_GT(coverage, 0.0);
  EXPECT_NEAR(coverage / dpr, kCurveLineWidth, 0.25) << "line width in item pixels";
  EXPECT_NEAR(moment / coverage / dpr, center.y(), 0.25) << "line center in item pixels";
  // Nothing of the line two pixels away from it.
  EXPECT_EQ(Pixel(image, center + QPointF(0.0, 2.5)), kSurface);
  EXPECT_EQ(Pixel(image, center - QPointF(0.0, 2.5)), kSurface);
}

TEST_F(PlotPanePaintTest, OnlyTheChangedLayerIsBuiltAgain) {
  Present();
  const int grid      = pane_->LayerBuildCount(&grid_);
  const int histogram = pane_->LayerBuildCount(&histogram_);
  const int line      = pane_->LayerBuildCount(&line_);
  const int handles   = pane_->LayerBuildCount(&handles_);
  ASSERT_GE(grid, 1);

  line_.set_points({{0.0, 0.2}, {1.0, 0.8}});
  const QImage image = Present();
  EXPECT_EQ(pane_->LayerBuildCount(&line_), line + 1);
  EXPECT_EQ(pane_->LayerBuildCount(&grid_), grid);
  EXPECT_EQ(pane_->LayerBuildCount(&histogram_), histogram);
  EXPECT_EQ(pane_->LayerBuildCount(&handles_), handles);
  EXPECT_GT(Brightness(Pixel(image, pane_->mapFromData({0.5, 0.5}))), 400);
  EXPECT_LT(Brightness(Pixel(image, pane_->mapFromData({0.6, 0.6}))), 300);

  // A new histogram builds only the histogram layer.
  source_.Publish(std::vector<uint32_t>(256U * 3U, 10U), 256);
  Present();
  EXPECT_EQ(pane_->LayerBuildCount(&histogram_), histogram + 1);
  EXPECT_EQ(pane_->LayerBuildCount(&grid_), grid);

  // A view change builds every layer.
  pane_->zoomAt({0.5, 0.5}, 2.0);
  Present();
  EXPECT_EQ(pane_->LayerBuildCount(&grid_), grid + 1);
  EXPECT_EQ(pane_->LayerBuildCount(&line_), line + 2);
  EXPECT_EQ(pane_->LayerBuildCount(&handles_), handles + 1);
}

TEST_F(PlotPanePaintTest, TwoPanesInOneWindowKeepTheirOwnViews) {
  auto* second = new PlotPane(host_.window.contentItem());
  ConfigurePane(*second);
  second->setX(210.0);
  second->set_surface_color(kSurface);
  PlotPolylineLayer second_line;
  second_line.set_color(QColor(255, 255, 255));
  second_line.set_points({{0.0, 0.0}, {1.0, 1.0}});
  second->AddLayer(&second_line);

  const QImage before        = Present();
  const int    second_builds = second->LayerBuildCount(&second_line);

  pane_->zoomAt({0.2, 0.2}, 4.0);
  const QImage after = Present();
  EXPECT_TRUE(pane_->zoomed());
  EXPECT_FALSE(second->zoomed());
  EXPECT_EQ(second->LayerBuildCount(&second_line), second_builds);
  EXPECT_EQ(Region(before, {210.0, 0.0, 200.0, 200.0}), Region(after, {210.0, 0.0, 200.0, 200.0}))
      << "the second pane did not change";
  EXPECT_NE(Region(before, {0.0, 0.0, 200.0, 200.0}), Region(after, {0.0, 0.0, 200.0, 200.0}));

  second->zoomAt({0.8, 0.8}, 2.0);
  EXPECT_NEAR(pane_->zoom(), 4.0, 1e-9);
  EXPECT_NEAR(second->zoom(), 2.0, 1e-9);
  ExpectNear(pane_->mapFromData({0.2, 0.2}), {54.0, 146.0}, 1e-9);
  delete second;
}

}  // namespace
}  // namespace alcedo::ui

// The native platform plugin instead of the offscreen one: see SelectRhiGraphicsApi.
int main(int argc, char** argv) {
  alcedo::ui::test::ConfigureQtPluginPaths(argc > 0 ? argv[0] : nullptr,
                                           /*force_offscreen=*/false);
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
