//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// #564 (RP2): the range bar moves a handle by the cursor delta from the press, Shift drags at
// 1/10 speed, and Left/Right keys and the wheel over a handle report steps. The bar holds no
// model state; the adapter tests (EditorMaskRangeAdapterTest) cover the commits.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QWheelEvent>
#include <filesystem>
#include <memory>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

/// The production RangeSelectorBar at a fixed width; values come from the test.
class RangeBarHarness {
 public:
  explicit RangeBarHarness(const QVariantMap& properties) {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    QQmlComponent component(
        &engine_, QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/RangeSelectorBar.qml")));
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(properties)));
    if (!root_) {
      for (const auto& error : component.errors()) {
        errors_.push_back(error.toString());
      }
      return;
    }
    root_->setParentItem(window_.contentItem());
    root_->setPosition({20.0, 20.0});
    root_->setWidth(266.0);
    root_->setHeight(root_->implicitHeight());
    window_.resize(320, 80);
    window_.show();
    (void)QTest::qWaitForWindowExposed(&window_);
    ProcessEvents(50);
  }

  [[nodiscard]] auto bar() const -> QQuickItem* { return root_.get(); }
  [[nodiscard]] auto window() -> QQuickWindow* { return &window_; }
  [[nodiscard]] auto errors() const -> std::string { return errors_.join('\n').toStdString(); }

  /// Window point of value @p value on the track, offset by @p dx pixels.
  [[nodiscard]] auto PointAt(double value, double dx = 0.0) const -> QPoint {
    const double from = root_->property("from").toDouble();
    const double to   = root_->property("to").toDouble();
    const double x    = root_->property("trackX").toDouble() + (value - from) / (to - from) * TrackWidth();
    return root_->mapToScene(QPointF(x + dx, root_->height() / 2.0)).toPoint();
  }
  [[nodiscard]] auto TrackWidth() const -> double {
    return root_->property("trackWidth").toDouble();
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QStringList                 errors_;
};

TEST(RangeSelectorBarQmlTest, HandleDragMovesByTheCursorDeltaFromThePress) {
  RangeBarHarness harness({{QStringLiteral("from"), 0.0},
                           {QStringLiteral("to"), 360.0},
                           {QStringLiteral("first"), 100.0},
                           {QStringLiteral("second"), 200.0},
                           {QStringLiteral("wraps"), true},
                           {QStringLiteral("minimumWidth"), 1.2}});
  ASSERT_NE(harness.bar(), nullptr) << harness.errors();
  QSignalSpy moved(harness.bar(), SIGNAL(moved(double, double)));
  QSignalSpy began(harness.bar(), SIGNAL(began()));
  QSignalSpy finished(harness.bar(), SIGNAL(finished()));
  const double degrees_per_pixel = 360.0 / harness.TrackWidth();

  // Press 6 px right of the first handle (inside the grab distance), release without a move.
  const auto press = harness.PointAt(100.0, 6.0);
  QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier, press);
  QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::NoModifier, press);
  ProcessEvents(20);
  EXPECT_EQ(began.count(), 1);
  EXPECT_EQ(finished.count(), 1);
  EXPECT_EQ(moved.count(), 0) << "a press alone reports no value";

  // Press 6 px off the handle again and move 10 px: the handle moves 10 px from its value.
  QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier, press);
  QTest::mouseMove(harness.window(), press + QPoint(10, 0));
  ProcessEvents(20);
  ASSERT_GE(moved.count(), 1);
  const auto last = moved.takeLast();
  EXPECT_NEAR(last.at(0).toDouble(), 100.0 + 10.0 * degrees_per_pixel, 1e-3);
  EXPECT_DOUBLE_EQ(last.at(1).toDouble(), 200.0);

  // Shift from here moves at 1/10 speed, from the value reached so far.
  QTest::mouseMove(harness.window(), press + QPoint(30, 0));
  ProcessEvents(20);
  const double before_shift = moved.takeLast().at(0).toDouble();
  EXPECT_NEAR(before_shift, 100.0 + 30.0 * degrees_per_pixel, 1e-3);
  QMouseEvent shift_move(QEvent::MouseMove, QPointF(press + QPoint(50, 0)),
                         QPointF(press + QPoint(50, 0)), Qt::NoButton, Qt::LeftButton,
                         Qt::ShiftModifier);
  QCoreApplication::sendEvent(harness.window(), &shift_move);
  ProcessEvents(20);
  ASSERT_GE(moved.count(), 1);
  EXPECT_NEAR(moved.takeLast().at(0).toDouble(), before_shift + 2.0 * degrees_per_pixel, 1e-3);
  QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::ShiftModifier, press + QPoint(50, 0));
  ProcessEvents(20);
  EXPECT_EQ(finished.count(), 2);
}

TEST(RangeSelectorBarQmlTest, KeysStepTheFocusedHandleAndTheWheelStepsTheHandleUnderIt) {
  RangeBarHarness harness({{QStringLiteral("from"), 0.0},
                           {QStringLiteral("to"), 1.0},
                           {QStringLiteral("first"), 0.3},
                           {QStringLiteral("second"), 0.7},
                           {QStringLiteral("minimumWidth"), 0.012}});
  ASSERT_NE(harness.bar(), nullptr) << harness.errors();
  QSignalSpy stepped(harness.bar(), SIGNAL(stepped(QString, int, bool)));

  // A press on the second handle focuses it; keys step it.
  const auto second = harness.PointAt(0.7);
  QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier, second);
  QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::NoModifier, second);
  ProcessEvents(20);
  ASSERT_TRUE(harness.bar()->hasActiveFocus());
  QTest::keyClick(harness.window(), Qt::Key_Right);
  QTest::keyClick(harness.window(), Qt::Key_Left, Qt::ShiftModifier);
  ASSERT_EQ(stepped.count(), 2);
  EXPECT_EQ(stepped.at(0).at(0).toString(), QStringLiteral("second"));
  EXPECT_EQ(stepped.at(0).at(1).toInt(), 1);
  EXPECT_FALSE(stepped.at(0).at(2).toBool());
  EXPECT_EQ(stepped.at(1).at(1).toInt(), -1);
  EXPECT_TRUE(stepped.at(1).at(2).toBool());

  // The wheel over the first handle steps it; the wheel over the track body is not consumed.
  stepped.clear();
  const auto first = harness.PointAt(0.3);
  QWheelEvent wheel(QPointF(first), QPointF(first), QPoint(), QPoint(0, 240), Qt::NoButton,
                    Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(harness.window(), &wheel);
  ProcessEvents(20);
  ASSERT_EQ(stepped.count(), 1);
  EXPECT_EQ(stepped.at(0).at(0).toString(), QStringLiteral("first"));
  EXPECT_EQ(stepped.at(0).at(1).toInt(), 2);
  const auto body = harness.PointAt(0.5);
  QWheelEvent body_wheel(QPointF(body), QPointF(body), QPoint(), QPoint(0, 120), Qt::NoButton,
                         Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(harness.window(), &body_wheel);
  ProcessEvents(20);
  EXPECT_EQ(stepped.count(), 1);
}

}  // namespace
}  // namespace alcedo::ui::test
