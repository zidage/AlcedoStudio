//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_brush_interaction_qml_test.cpp
/// @brief First left-press+drag while Mask owns the left button must forward
///        the whole pointer path (press, moves, one release) without a prior
///        click. Handler routing matches EditorWorkspace.qml.

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace alcedo::ui::test {
namespace {

struct PointerSample {
  qreal x       = 0.0;
  qreal y       = 0.0;
  int   buttons = 0;
};

class RecordingMaskCreation final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool ownsLeftButton READ owns_left_button CONSTANT)
 public:
  [[nodiscard]] auto owns_left_button() const -> bool { return true; }

  Q_INVOKABLE bool handlePress(qreal x, qreal y, int button) {
    presses.push_back({x, y, button});
    return button == static_cast<int>(Qt::LeftButton);
  }
  Q_INVOKABLE bool handleMove(qreal x, qreal y, int buttons) {
    moves.push_back({x, y, buttons});
    return true;
  }
  Q_INVOKABLE bool handleRelease(qreal x, qreal y, int button) {
    releases.push_back({x, y, button});
    return true;
  }
  Q_INVOKABLE void handleHover(qreal, qreal) {}
  Q_INVOKABLE void cancelOpenPointerInput() { ++cancels; }

  std::vector<PointerSample> presses;
  std::vector<PointerSample> moves;
  std::vector<PointerSample> releases;
  int                        cancels = 0;
};

class RecordingInteraction final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool hasCustomCursor READ has_custom_cursor CONSTANT)
  Q_PROPERTY(int cursorShape READ cursor_shape CONSTANT)
 public:
  [[nodiscard]] auto has_custom_cursor() const -> bool { return false; }
  [[nodiscard]] auto cursor_shape() const -> int { return static_cast<int>(Qt::ArrowCursor); }

  Q_INVOKABLE void handleHoverMove(qreal, qreal) {}
  Q_INVOKABLE void handlePress(qreal x, qreal y, int button) { presses.push_back({x, y, button}); }
  Q_INVOKABLE void handleMove(qreal x, qreal y, int buttons) { moves.push_back({x, y, buttons}); }
  Q_INVOKABLE void handleRelease(qreal x, qreal y, int button) {
    releases.push_back({x, y, button});
  }
  Q_INVOKABLE void handleDoubleTap(qreal, qreal) { ++double_taps; }

  std::vector<PointerSample> presses;
  std::vector<PointerSample> moves;
  std::vector<PointerSample> releases;
  int                        double_taps = 0;
};

// Keep this handler block aligned with EditorWorkspace.qml: one PointHandler
// writer, TapHandler off while Mask owns the left button, last valid point on
// release, and pointChanged may start the Mask stream before activeChanged.
constexpr char kFirstDragHarnessQml[] = R"(
import QtQuick
import QtQuick.Window

Window {
    id: root
    objectName: "brushFirstDragHarness"
    width: 640
    height: 480
    visible: true
    color: "#202020"
    title: "brush-first-drag"

    property bool editorControlsEnabled: true
    property bool maskOwnsLeftButton: true
    property var maskCreation: null
    property var editorInteraction: null

    Item {
        id: viewportSlot
        objectName: "viewportSlot"
        anchors.fill: parent

        PointHandler {
            id: viewportPointer
            objectName: "editorViewportPointer"
            enabled: root.editorControlsEnabled
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.TouchScreen | PointerDevice.Stylus
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            property int _activeButton: Qt.LeftButton
            property bool _pressed: false
            property bool _maskStream: false
            property bool _canceled: false
            property real _lastX: 0
            property real _lastY: 0
            property bool _haveLastItemPos: false

            function rememberItemPos(x, y) {
                _lastX = x
                _lastY = y
                _haveLastItemPos = true
            }
            function finishX() {
                if (_haveLastItemPos && Math.abs(point.position.x) < 1e-6
                        && Math.abs(point.position.y) < 1e-6)
                    return _lastX
                return point.position.x
            }
            function finishY() {
                if (_haveLastItemPos && Math.abs(point.position.x) < 1e-6
                        && Math.abs(point.position.y) < 1e-6)
                    return _lastY
                return point.position.y
            }
            function beginMaskOrPan() {
                _activeButton = (point.pressedButtons & Qt.MiddleButton)
                        ? Qt.MiddleButton : Qt.LeftButton
                rememberItemPos(point.position.x, point.position.y)
                if (_activeButton === Qt.LeftButton
                        && root.maskOwnsLeftButton
                        && root.maskCreation
                        && root.maskCreation.handlePress(
                               point.position.x, point.position.y, _activeButton)) {
                    _maskStream = true
                } else {
                    _maskStream = false
                    editorInteraction.handlePress(
                                point.position.x, point.position.y, _activeButton)
                }
            }
            function finishMaskOrPan() {
                var x = finishX()
                var y = finishY()
                if (_canceled && _maskStream && root.maskCreation
                        && typeof root.maskCreation.cancelOpenPointerInput === "function") {
                    root.maskCreation.cancelOpenPointerInput()
                } else if (_maskStream && root.maskCreation) {
                    root.maskCreation.handleRelease(x, y, _activeButton)
                } else {
                    editorInteraction.handleRelease(x, y, _activeButton)
                }
                _pressed = false
                _maskStream = false
                _canceled = false
                _haveLastItemPos = false
            }

            onCanceled: { _canceled = true }
            onActiveChanged: {
                if (active) {
                    _pressed = true
                    _canceled = false
                    beginMaskOrPan()
                } else if (_pressed) {
                    finishMaskOrPan()
                }
            }
            onPointChanged: {
                if (!active) {
                    return
                }
                if (!_pressed) {
                    _pressed = true
                    _canceled = false
                    beginMaskOrPan()
                }
                if (point.pressedButtons !== 0) {
                    rememberItemPos(point.position.x, point.position.y)
                }
                if (_maskStream && root.maskCreation) {
                    root.maskCreation.handleMove(
                                _haveLastItemPos ? _lastX : point.position.x,
                                _haveLastItemPos ? _lastY : point.position.y,
                                point.pressedButtons)
                } else {
                    editorInteraction.handleMove(
                                point.position.x, point.position.y,
                                point.pressedButtons)
                }
            }
        }

        DragHandler {
            id: viewportPanDrag
            objectName: "editorViewportPanDrag"
            enabled: root.editorControlsEnabled
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.TouchScreen | PointerDevice.Stylus
            acceptedButtons: root.maskOwnsLeftButton
                             ? Qt.MiddleButton
                             : (Qt.LeftButton | Qt.MiddleButton)
            target: null
            onActiveChanged: {
                if (active) {
                    editorInteraction.handlePress(
                                centroid.pressPosition.x, centroid.pressPosition.y,
                                (centroid.pressedButtons & Qt.MiddleButton)
                                        ? Qt.MiddleButton : Qt.LeftButton)
                    editorInteraction.handleMove(
                                centroid.position.x, centroid.position.y,
                                centroid.pressedButtons)
                } else {
                    editorInteraction.handleRelease(
                                centroid.position.x, centroid.position.y, Qt.LeftButton)
                }
            }
            onTranslationChanged: {
                if (active) {
                    editorInteraction.handleMove(
                                centroid.position.x, centroid.position.y,
                                centroid.pressedButtons)
                }
            }
        }

        TapHandler {
            id: viewportDoubleTap
            objectName: "editorViewportDoubleTap"
            enabled: root.editorControlsEnabled && !root.maskOwnsLeftButton
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onDoubleTapped: function (eventPoint) {
                editorInteraction.handleDoubleTap(
                            eventPoint.position.x, eventPoint.position.y)
            }
        }
    }
}
)";

void ProcessEventsFor(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

void DragBentPath(QQuickWindow* window, const QPoint& origin) {
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, origin);
  ProcessEventsFor(16);
  const QPoint offsets[] = {{24, 4}, {48, 18}, {72, 10}, {104, 36}, {128, 28}};
  for (const auto& offset : offsets) {
    QTest::mouseMove(window, origin + offset);
    ProcessEventsFor(16);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, origin + offsets[4]);
  ProcessEventsFor(30);
}

}  // namespace

TEST(EditorBrushInteractionQmlTest, FirstBrushDragWithoutPriorClickPaintsWholePath) {
  ASSERT_TRUE(QCoreApplication::instance());

  RecordingMaskCreation mask;
  RecordingInteraction  interaction;
  QQmlApplicationEngine engine;
  QStringList           warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& emitted) {
    for (const auto& warning : emitted) {
      warnings.push_back(warning.toString());
    }
  });
  engine.rootContext()->setContextProperty(QStringLiteral("maskCreation"), &mask);
  engine.rootContext()->setContextProperty(QStringLiteral("editorInteraction"), &interaction);
  engine.loadData(QByteArray{kFirstDragHarnessQml},
                  QUrl(QStringLiteral("file:///brushFirstDragHarness.qml")));
  ASSERT_FALSE(engine.rootObjects().empty()) << warnings.join('\n').toStdString();
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
  ASSERT_NE(window, nullptr);
  window->setProperty("maskCreation", QVariant::fromValue(static_cast<QObject*>(&mask)));
  window->setProperty("editorInteraction",
                      QVariant::fromValue(static_cast<QObject*>(&interaction)));
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEventsFor(20);

  auto* tap = window->findChild<QObject*>(QStringLiteral("editorViewportDoubleTap"));
  ASSERT_NE(tap, nullptr);
  EXPECT_FALSE(tap->property("enabled").toBool());

  const QPoint origin(180, 140);
  DragBentPath(window, origin);

  EXPECT_EQ(mask.cancels, 0);
  ASSERT_EQ(mask.presses.size(), 1u);
  EXPECT_EQ(mask.presses.front().buttons, static_cast<int>(Qt::LeftButton));
  EXPECT_GE(mask.moves.size(), 4u) << "first left-drag must forward the path, not a single dab";
  ASSERT_EQ(mask.releases.size(), 1u);
  EXPECT_GT(std::hypot(mask.releases.front().x - mask.presses.front().x,
                       mask.releases.front().y - mask.presses.front().y),
            40.0);
  EXPECT_TRUE(interaction.presses.empty()) << "Mask-owned left drag must not start pan";
  EXPECT_EQ(interaction.double_taps, 0);
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}

}  // namespace alcedo::ui::test

#include "editor_brush_interaction_qml_test.moc"
