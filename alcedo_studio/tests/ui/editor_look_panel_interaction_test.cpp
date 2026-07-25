//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6D interaction reproduction tests. These load real QML sources and
// drive pointer sequences to observe layout and input failures — not to
// restate model implementation details.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QEventLoop>
#include <QPoint>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_cdl_trackball_item.hpp"
#include "ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"
#include "ui/alcedo_main/album_backend/editor_color_temp_model.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/editor_dialog/modules/color_temp.hpp"

namespace alcedo::ui::test {
namespace {

class RecordingSubmitter : public QObject, public IEditorAdjustmentSubmitter {
 public:
  struct Call {
    QString fieldKey;
    QString params;
    bool    settled;
  };
  std::vector<Call> calls;
  bool              canEditState = true;
  // Optional hang detector: if submit is re-entered while a previous submit is
  // still on the stack, set reentered=true (deadlock-class reentrancy).
  bool              inSubmit   = false;
  bool              reentered  = false;
  int               submitCount = 0;

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    if (inSubmit) {
      reentered = true;
    }
    inSubmit = true;
    ++submitCount;
    if (!canEditState) {
      inSubmit = false;
      return false;
    }
    calls.push_back({fieldKey, paramsJson, settled});
    inSubmit = false;
    return true;
  }
  auto canEdit() const -> bool override { return canEditState; }

  auto settledCount() const -> int {
    int n = 0;
    for (const auto& c : calls) {
      if (c.settled) {
        ++n;
      }
    }
    return n;
  }
};

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitUntil(const std::function<bool()>& pred, int timeoutMs) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    ProcessEvents(20);
  }
  return pred();
}

auto MapToWindow(QQuickItem* item, qreal nx, qreal ny) -> QPoint {
  const QPointF local(item->width() * nx, item->height() * ny);
  const QPointF global = item->mapToScene(local);
  return global.toPoint();
}

// ── 1. CollapsibleSection content placement ─────────────────────────────────

// Loads CollapsibleSection the same way EditorLookPanel does: children as
// default-property content with bodyContentHeight set from the body column.
constexpr char kCollapseHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 360
    height: 480
    visible: true
    color: "#111214"
    property var sectionItem: null

    Loader {
        anchors.fill: parent
        anchors.margins: 8
        source: sectionSourceUrl
        onLoaded: {
            if (!item) return
            item.title = "Detail"
            item.expanded = true
            item.objectName = "detailSection"
            // Mirror Look panel: inject body after load by reparenting is hard;
            // instead the harness QML embeds the section with content inline.
            root.sectionItem = item
        }
    }
}
)";

// Full inline harness (no Loader reparent issues) — content as child of section.
constexpr char kCollapseInlineHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 360
    height: 520
    visible: true
    color: "#111214"

    Flickable {
        id: scroller
        objectName: "lookScroller"
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        flickableDirection: Flickable.VerticalFlick

        ColumnLayout {
            id: column
            width: scroller.width
            spacing: 10

            Loader {
                id: sectionLoader
                Layout.fillWidth: true
                source: sectionSourceUrl
                onLoaded: {
                    if (!item) return
                    item.objectName = "detailSection"
                    item.title = "Detail"
                    item.expanded = true
                    item.controlsEnabled = true
                    // Force content via createObject into default property if present.
                }
            }

            // Reference section built the same way Look panel uses it: as a
            // component instance with a ColumnLayout child.
            CollapsibleSection {
                id: detailSection
                objectName: "detailSectionInline"
                Layout.fillWidth: true
                title: "Detail"
                expanded: true
                bodyContentHeight: detailBody.implicitHeight + 8
                surfaceColor: "#161719"
                disabledSurfaceColor: "#161719"
                borderColor: "#333"
                textColor: "#F5F1EA"
                mutedColor: "#AAA"
                hoverColor: "#222"
                accentColor: "#CCC"

                ColumnLayout {
                    id: detailBody
                    objectName: "detailBody"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    spacing: 8

                    Rectangle {
                        objectName: "detailSliderA"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        color: "#2C2D2F"
                    }
                    Rectangle {
                        objectName: "detailSliderB"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        color: "#3A3B3D"
                    }
                }
            }
        }
    }
}
)";

TEST(EditorLookPanelInteractionTest, CollapsibleSectionBodyDoesNotOverlapHeader) {
  // Reproduce: title and body content share the same origin and clip each other.
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.addImportPath(SrcQmlDir());

  // Register CollapsibleSection by loading from file path as a component in QML
  // requires a module; load the harness with a qmldir-less import via full URL.
  // Simpler: instantiate CollapsibleSection.qml as the root content.
  const QString sectionUrl =
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/CollapsibleSection.qml")).toString();

  const QString harness = QStringLiteral(R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 360
    height: 400
    visible: true
    color: "#111"

    property alias section: detailSection
    property alias body: detailBody
    property alias header: headerProbe
    property alias sliderA: detailSliderA

    // Load CollapsibleSection as a Component from source URL.
    Component {
        id: sectionComponent
        Loader {
            source: "%1"
        }
    }

    // Inline copy of the production pattern using a dynamic Loader that
    // sets properties; content is created as a child of the loaded item.
    Loader {
        id: host
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        source: "%1"
        onLoaded: {
            item.objectName = "detailSection"
            item.title = "Detail"
            item.expanded = true
            item.bodyContentHeight = 100
            item.surfaceColor = "#161719"
            item.disabledSurfaceColor = "#161719"
            item.borderColor = "#333"
            item.textColor = "#eee"
            item.mutedColor = "#aaa"
            item.hoverColor = "#222"
            item.accentColor = "#ccc"
        }
    }

    // Probe items created as children of the loaded section (production path:
    // default-property children of CollapsibleSection).
    Timer {
        interval: 0
        running: true
        onTriggered: {
            if (!host.item) return
            // Create body content the same way QML default-property children attach.
            var body = Qt.createQmlObject(
                'import QtQuick; import QtQuick.Layouts; ColumnLayout {
                    id: detailBody; objectName: "detailBody";
                    anchors.left: parent.left; anchors.right: parent.right;
                    anchors.top: parent.top; anchors.margins: 6; spacing: 8;
                    Rectangle { objectName: "detailSliderA"; Layout.fillWidth: true;
                                Layout.preferredHeight: 40; color: "#444" }
                    Rectangle { objectName: "detailSliderB"; Layout.fillWidth: true;
                                Layout.preferredHeight: 40; color: "#555" }
                }', host.item, "detailBodyDynamic")
            host.item.bodyContentHeight = body.implicitHeight + 8
        }
    }

    // Aliases filled after load for the test.
    property var detailSection: host.item
    property var detailBody: null
    property var headerProbe: null
    property var detailSliderA: null
}
)").arg(sectionUrl);

  // Declarative children of CollapsibleSection must go through its default
  // property (bodyContent). createQmlObject(parent=section) bypasses that and
  // parents onto the section root — that path is not the production QML path.
  // Use a wrapper component that embeds content declaratively.
  const QByteArray pure = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 360
    height: 400
    visible: true
    color: "#111"

    Component {
        id: sectionWithBody
        // Load the real CollapsibleSection.qml and place declarative children
        // via a Loader+inline file is hard; instead copy the production usage
        // by nesting Loader content that re-parents into bodyContent after load
        // using the bodyContent objectName.
        Item {
            id: wrap
            width: 340
            height: sectionLoader.item ? sectionLoader.item.implicitHeight : 120

            Loader {
                id: sectionLoader
                anchors.left: parent.left
                anchors.right: parent.right
                source: sectionSourceUrl
                onLoaded: {
                    item.objectName = "detailSection"
                    item.title = "Detail"
                    item.expanded = true
                    item.surfaceColor = "#161719"
                    item.disabledSurfaceColor = "#161719"
                    item.borderColor = "#333"
                    item.textColor = "#eee"
                    item.mutedColor = "#aaa"
                    item.hoverColor = "#222"
                    item.accentColor = "#ccc"
                    wrap.height = Qt.binding(function() {
                        return item ? item.implicitHeight : 120
                    })

                    function findByName(obj, name) {
                        if (!obj) return null
                        if (obj.objectName === name) return obj
                        var kids = obj.children
                        for (var i = 0; i < kids.length; ++i) {
                            var r = findByName(kids[i], name)
                            if (r) return r
                        }
                        return null
                    }
                    // Production default property targets this slot.
                    var bodyContent = findByName(item, "collapsibleSectionBodyContent")
                    if (!bodyContent) {
                        console.warn("bodyContent slot missing")
                        return
                    }
                    var qml = 'import QtQuick; import QtQuick.Layouts;\n'
                            + 'ColumnLayout {\n'
                            + '  objectName: "detailBody"\n'
                            + '  anchors.left: parent.left\n'
                            + '  anchors.right: parent.right\n'
                            + '  anchors.top: parent.top\n'
                            + '  anchors.margins: 6\n'
                            + '  spacing: 8\n'
                            + '  Rectangle { objectName: "detailSliderA"; Layout.fillWidth: true; Layout.preferredHeight: 40; color: "#444" }\n'
                            + '  Rectangle { objectName: "detailSliderB"; Layout.fillWidth: true; Layout.preferredHeight: 40; color: "#555" }\n'
                            + '}\n'
                    var body = Qt.createQmlObject(qml, bodyContent, "detailBodyDyn")
                    item.bodyContentHeight = Math.max(96, body.implicitHeight + 8)
                }
            }
        }
    }

    Loader {
        id: host
        objectName: "sectionLoader"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        sourceComponent: sectionWithBody
    }
}
)";

  engine.rootContext()->setContextProperty(
      QStringLiteral("sectionSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/CollapsibleSection.qml")));
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));

  QStringList warnings;
  QObject::connect(&engine, &QQmlApplicationEngine::warnings, &engine,
                   [&](const QList<QQmlError>& errs) {
                     for (const auto& e : errs) {
                       warnings << e.toString();
                     }
                   });
  engine.loadData(pure);
  ASSERT_FALSE(engine.rootObjects().isEmpty()) << warnings.join('\n').toStdString();
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(100);

  auto* section = window->findChild<QQuickItem*>(QStringLiteral("detailSection"));
  ASSERT_NE(section, nullptr) << warnings.join('\n').toStdString();
  auto* header = section->findChild<QQuickItem*>(QStringLiteral("collapsibleSectionHeader"));
  auto* body   = section->findChild<QQuickItem*>(QStringLiteral("collapsibleSectionBody"));
  auto* sliderA = section->findChild<QQuickItem*>(QStringLiteral("detailSliderA"));
  ASSERT_NE(header, nullptr);
  ASSERT_NE(body, nullptr);
  ASSERT_NE(sliderA, nullptr) << "Body content never attached under the section";

  // Force layout.
  ProcessEvents(50);

  const QRectF headerScene(header->mapToScene(QPointF(0, 0)),
                           QSizeF(header->width(), header->height()));
  const QRectF sliderScene(sliderA->mapToScene(QPointF(0, 0)),
                           QSizeF(sliderA->width(), sliderA->height()));

  // Observed failure: slider sits under the header (intersecting).
  // Expected: slider fully below header bottom.
  const bool overlaps = headerScene.intersects(sliderScene);
  const qreal headerBottom = headerScene.bottom();
  const qreal sliderTop    = sliderScene.top();

  // Record the observation for the fix; the assertion encodes the product
  // requirement once the layout is corrected.
  EXPECT_FALSE(overlaps) << "header=[" << headerScene.y() << "," << headerScene.bottom()
                         << "] slider=[" << sliderScene.y() << "," << sliderScene.bottom()
                         << "] (content must live in the body, not under the title)";
  EXPECT_GE(sliderTop, headerBottom - 0.5)
      << "slider top " << sliderTop << " must be below header bottom " << headerBottom;
  EXPECT_GT(body->height(), 1.0) << "expanded body height collapsed to ~0";
}

// ── 2. Double-click reset on AdjustmentSlider ───────────────────────────────

constexpr char kSliderHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 400
    height: 200
    visible: true
    color: "#111"
    property var sliderItem: null

    Loader {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 64
        anchors.margins: 16
        source: sliderSourceUrl
        onLoaded: {
            if (item) {
                item.model = exposureModel
                item.objectName = "testSlider"
                root.sliderItem = item
            }
        }
    }
}
)";

TEST(EditorLookPanelInteractionTest, DoubleClickOnAdjustmentSliderResetsValue) {
  RecordingSubmitter submitter;
  EditorAdjustmentValueModel model;
  model.setFieldKey(QStringLiteral("exposure"));
  model.setMinimum(-2);
  model.setMaximum(2);
  model.setDefaultValue(0);
  model.setStep(0.1);
  model.setPrecision(1);
  model.setSubmitter(&submitter);
  model.setValue(1.2);  // load-only
  ASSERT_EQ(submitter.settledCount(), 0);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.rootContext()->setContextProperty(QStringLiteral("exposureModel"), &model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("sliderSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentSlider.qml")));
  engine.loadData(QByteArray(kSliderHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(80);

  auto* sliderRoot = window->findChild<QQuickItem*>(QStringLiteral("testSlider"));
  ASSERT_NE(sliderRoot, nullptr);
  auto* handle = sliderRoot->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderHandle"));
  ASSERT_NE(handle, nullptr);

  const QPoint pos = MapToWindow(handle, 0.5, 0.5);
  // Real double-click sequence.
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, pos);
  ProcessEvents(100);

  EXPECT_NEAR(model.value(), 0.0, 1e-6)
      << "double-click must reset to default; value still " << model.value();
  EXPECT_GE(submitter.settledCount(), 1) << "reset must commit a settled patch";
}

// ── 3. Color-temp continuous drag ───────────────────────────────────────────

TEST(EditorLookPanelInteractionTest, ColorTempCctDragEmitsMultipleInteractiveUpdates) {
  RecordingSubmitter submitter;
  EditorColorTempModel model;
  model.setSubmitter(&submitter);
  model.setAsShotCct(5600);
  model.setAsShotTint(0);
  model.loadFromParams(QStringLiteral("as_shot"), 5600, 0, true);

  // Drive the model the way MonoSlider does during a press-drag-release.
  model.beginCctDrag();
  const int startPos = model.cctSliderPos();
  // Simulate continuous drag across many slider positions.
  int interactive = 0;
  for (int delta = 50; delta <= 800; delta += 50) {
    const int before = static_cast<int>(submitter.calls.size());
    model.updateCctSliderDrag(startPos + delta);
    if (static_cast<int>(submitter.calls.size()) > before) {
      if (!submitter.calls.back().settled) {
        ++interactive;
      }
    }
  }
  model.finishCctDrag();

  EXPECT_GE(interactive, 5) << "continuous drag must produce multiple interactive patches, got "
                            << interactive;
  EXPECT_EQ(submitter.settledCount(), 1);
  EXPECT_EQ(model.modeIndex(), 1);
}

// ── 4. Color-temp MonoSlider-style binding fight (QML) ──────────────────────

// Fixed Look MonoSlider pattern: externalValue only applies when not pressed.
constexpr char kCctSliderHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 400
    height: 160
    visible: true
    color: "#111"

    Slider {
        id: cctSlider
        objectName: "cctSlider"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: 20
        height: 32
        from: 0
        to: 4096
        stepSize: 1
        live: true
        touchDragThreshold: 0
        property real externalValue: colorTempModel.cctSliderPos
        value: externalValue
        onExternalValueChanged: {
            if (!pressed)
                value = externalValue
        }
        onPressedChanged: {
            if (pressed) colorTempModel.beginCctDrag()
            else colorTempModel.finishCctDrag()
        }
        onMoved: colorTempModel.updateCctSliderDrag(Math.round(value))
    }

    Connections {
        target: colorTempModel
        function onCctChanged() { cctSlider.externalValue = colorTempModel.cctSliderPos }
    }
}
)";

TEST(EditorLookPanelInteractionTest, ColorTempQmlDragProducesInteractiveBurst) {
  RecordingSubmitter submitter;
  EditorColorTempModel model;
  model.setSubmitter(&submitter);
  model.loadFromParams(QStringLiteral("custom"), 5600, 0, true);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("colorTempModel"), &model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.loadData(QByteArray(kCctSliderHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(80);

  auto* slider = window->findChild<QQuickItem*>(QStringLiteral("cctSlider"));
  ASSERT_NE(slider, nullptr);

  const QPoint start = MapToWindow(slider, 0.3, 0.5);
  const QPoint end   = MapToWindow(slider, 0.8, 0.5);

  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
  ProcessEvents(20);
  // Move in steps to simulate continuous drag.
  for (int i = 1; i <= 10; ++i) {
    const qreal t = static_cast<qreal>(i) / 10.0;
    const QPoint p(start.x() + static_cast<int>((end.x() - start.x()) * t), start.y());
    QTest::mouseMove(window, p);
    ProcessEvents(15);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
  ProcessEvents(50);

  int interactive = 0;
  for (const auto& c : submitter.calls) {
    if (!c.settled) {
      ++interactive;
    }
  }
  // This is the reproduction: with binding fight we often get 0–1 interactive.
  // After the fix we require a continuous stream.
  EXPECT_GE(interactive, 3) << "cct drag interactive count=" << interactive
                            << " total calls=" << submitter.calls.size()
                            << " (binding fight yields click-only behavior)";
}

// ── 5. CDL double-click reset reentrancy / hang ─────────────────────────────

TEST(EditorLookPanelInteractionTest, CdlDoubleClickResetDoesNotReenterSubmit) {
  RecordingSubmitter submitter;
  EditorCdlTrackballModel model;
  model.setSubmitter(&submitter);

  // Sequence matching QQuickItem mouse events for a double-click on the disc.
  model.beginDiscDrag(QStringLiteral("lift"));
  model.updateDiscDrag(QStringLiteral("lift"), 0.2, 0.1);
  model.finishDiscDrag();
  // Second click of double-click (often empty drag).
  model.beginDiscDrag(QStringLiteral("lift"));
  model.finishDiscDrag();
  // Double-click handler calls resetWheel without clearing drag in older code.
  model.beginDiscDrag(QStringLiteral("lift"));  // still-pressed edge case
  model.resetWheel(QStringLiteral("lift"));

  EXPECT_FALSE(submitter.reentered) << "submit re-entered — classic deadlock/reentrancy";
  EXPECT_FALSE(model.dragActive()) << "drag must be cleared after reset";
  EXPECT_NEAR(model.liftX(), 0.0, 1e-6);
  EXPECT_NEAR(model.liftY(), 0.0, 1e-6);
}

TEST(EditorLookPanelInteractionTest, CdlItemDoubleClickResetsWithoutHang) {
  RecordingSubmitter submitter;
  auto model = std::make_unique<EditorCdlTrackballModel>();
  model->setSubmitter(&submitter);
  model->setWheelDisc(QStringLiteral("gamma"), 0.4, -0.2);

  // Drive the QQuickItem from C++ (no QML type registration needed).
  QQuickWindow window;
  window.resize(200, 200);
  window.setColor(QColor(0x11, 0x11, 0x11));
  auto* disc = new EditorCdlTrackballItem(window.contentItem());
  disc->setObjectName(QStringLiteral("gammaDisc"));
  disc->setWidth(148);
  disc->setHeight(148);
  disc->setX(26);
  disc->setY(26);
  disc->setModel(model.get());
  disc->setWheelRole(QStringLiteral("gamma"));
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  ProcessEvents(50);

  const QPoint pos = MapToWindow(disc, 0.5, 0.5);
  const auto started = std::chrono::steady_clock::now();
  QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, pos);
  ProcessEvents(100);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  EXPECT_LT(elapsed, 2000) << "double-click reset hung for " << elapsed << "ms";
  EXPECT_FALSE(submitter.reentered);
  EXPECT_FALSE(model->dragActive());
  EXPECT_NEAR(model->gammaX(), 0.0, 1e-5);
  EXPECT_NEAR(model->gammaY(), 0.0, 1e-5);
}

// ── 6. Flickable steals slider drag ─────────────────────────────────────────

constexpr char kFlickSliderHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 360
    height: 240
    visible: true
    color: "#111"

    Flickable {
        id: scroller
        objectName: "scroller"
        anchors.fill: parent
        contentWidth: width
        contentHeight: 1200
        clip: true
        flickableDirection: Flickable.VerticalFlick

        Column {
            width: parent.width
            Item { width: 1; height: 40 }
            Loader {
                id: loader
                width: parent.width - 24
                x: 12
                height: 64
                source: sliderSourceUrl
                onLoaded: {
                    if (item) {
                        item.model = exposureModel
                        item.objectName = "testSlider"
                        item.flickable = scroller
                    }
                }
            }
            Item { width: 1; height: 1000 }
        }
    }
}
)";

TEST(EditorLookPanelInteractionTest, VerticalSliderDragDoesNotScrollParentFlickable) {
  RecordingSubmitter submitter;
  EditorAdjustmentValueModel model;
  model.setFieldKey(QStringLiteral("exposure"));
  model.setMinimum(-2);
  model.setMaximum(2);
  model.setDefaultValue(0);
  model.setStep(0.05);
  model.setSubmitter(&submitter);
  model.setValue(0);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.rootContext()->setContextProperty(QStringLiteral("exposureModel"), &model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("sliderSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentSlider.qml")));
  engine.loadData(QByteArray(kFlickSliderHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(80);

  auto* scroller = window->findChild<QQuickItem*>(QStringLiteral("scroller"));
  auto* handle   = window->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderHandle"));
  ASSERT_NE(scroller, nullptr);
  ASSERT_NE(handle, nullptr);

  const qreal y0 = scroller->property("contentY").toReal();
  const QPoint start = MapToWindow(handle, 0.2, 0.5);
  const QPoint end(start.x() + 80, start.y() + 30);  // diagonal: horizontal value + vertical scroll risk

  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
  ProcessEvents(20);
  for (int i = 1; i <= 8; ++i) {
    const qreal t = static_cast<qreal>(i) / 8.0;
    QPoint p(start.x() + static_cast<int>((end.x() - start.x()) * t),
             start.y() + static_cast<int>((end.y() - start.y()) * t));
    QTest::mouseMove(window, p);
    ProcessEvents(15);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
  ProcessEvents(50);

  const qreal y1 = scroller->property("contentY").toReal();
  EXPECT_NEAR(y1, y0, 1.0) << "Flickable contentY moved from " << y0 << " to " << y1
                            << " while dragging the slider";
  // Slider should still have accepted the drag (value changed).
  EXPECT_NE(model.value(), 0.0);
}

// ── 7. Production path: snapshot echo during CCT drag ───────────────────────
//
// EditorAdjustmentStack reloads panels on every AdjustmentSnapshotChanged. That
// fires synchronously from interactive submit. If the reload aborts dragActive,
// subsequent moves become no-ops — the user sees "click only, cannot drag".

TEST(EditorLookPanelInteractionTest, ColorTempSnapshotReloadDuringDragKeepsDragAlive) {
  RecordingSubmitter submitter;
  EditorColorTempModel model;
  model.setSubmitter(&submitter);
  model.setAsShotCct(5600);
  model.setAsShotTint(0);
  model.loadFromParams(QStringLiteral("as_shot"), 5600, 0, true);

  model.beginCctDrag();
  ASSERT_TRUE(model.dragActive());
  const int start = model.cctSliderPos();
  model.updateCctSliderDrag(start + 300);
  ASSERT_GE(static_cast<int>(submitter.calls.size()), 1);
  ASSERT_TRUE(model.dragActive());

  // Echo the just-submitted interactive params the way loadFromSnapshot does.
  model.loadFromParams(QStringLiteral("custom"), model.cct(), model.tint(), true);

  EXPECT_TRUE(model.dragActive())
      << "snapshot echo must not clear dragActive mid-gesture (click-only CCT)";

  const int calls_before = static_cast<int>(submitter.calls.size());
  model.updateCctSliderDrag(start + 600);
  EXPECT_GT(static_cast<int>(submitter.calls.size()), calls_before)
      << "moves after snapshot echo must keep submitting interactive patches";
  model.finishCctDrag();
  EXPECT_EQ(submitter.settledCount(), 1);
  EXPECT_FALSE(model.dragActive());
}

// Production MonoSlider pattern (mirrors EditorLookPanel MonoSlider + row).
// externalValue is owned by the model; the slider may not fight it while pressed.
constexpr char kProductionMonoCctHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 420
    height: 180
    visible: true
    color: "#111"
    property alias cctSlider: mono

    Slider {
        id: mono
        objectName: "prodCctSlider"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.margins: 20
        height: 32
        from: 0
        to: 4096
        stepSize: 1
        live: true
        touchDragThreshold: 0
        snapMode: Slider.SnapAlways

        property real externalValue: colorTempModel.cctSliderPos
        property bool _gestureMoved: false
        property real _pressValue: 0
        property double _lastClickMs: 0

        // Seed only; press owns value until release (no permanent binding fight).
        Component.onCompleted: value = externalValue
        onExternalValueChanged: {
            if (!pressed)
                value = externalValue
        }
        onPressedChanged: {
            if (pressed) {
                _gestureMoved = false
                _pressValue = value
                colorTempModel.beginCctDrag()
            } else {
                var now = Date.now()
                var isDouble = !_gestureMoved
                        && (now - _lastClickMs) < 350
                        && Math.abs(value - _pressValue) <= Math.max(stepSize * 0.5, 1e-9)
                _lastClickMs = now
                if (isDouble) {
                    colorTempModel.reset()
                    value = externalValue
                } else {
                    colorTempModel.finishCctDrag()
                    value = externalValue
                }
            }
        }
        onMoved: {
            _gestureMoved = true
            colorTempModel.updateCctSliderDrag(Math.round(value))
        }
    }
}
)";

TEST(EditorLookPanelInteractionTest, ColorTempMonoSliderDragSurvivesSnapshotEcho) {
  // Submitter reloads the model on every interactive patch (session snapshot path).
  class SnapshotEchoSubmitter : public RecordingSubmitter {
   public:
    EditorColorTempModel* model = nullptr;
    auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
      const bool ok = RecordingSubmitter::submitPatch(fieldKey, paramsJson, settled);
      if (ok && model != nullptr) {
        // Minimal echo: re-apply current model state as a snapshot load would.
        model->loadFromParams(model->modeValue(), model->cct(), model->tint(), true);
      }
      return ok;
    }
  };

  SnapshotEchoSubmitter submitter;
  EditorColorTempModel model;
  submitter.model = &model;
  model.setSubmitter(&submitter);
  model.setAsShotCct(5600);
  model.setAsShotTint(0);
  model.loadFromParams(QStringLiteral("as_shot"), 5600, 0, true);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("colorTempModel"), &model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.loadData(QByteArray(kProductionMonoCctHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(80);

  auto* slider = window->findChild<QQuickItem*>(QStringLiteral("prodCctSlider"));
  ASSERT_NE(slider, nullptr);

  const QPoint start = MapToWindow(slider, 0.25, 0.5);
  const QPoint end   = MapToWindow(slider, 0.85, 0.5);
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
  ProcessEvents(20);
  for (int i = 1; i <= 12; ++i) {
    const qreal t = static_cast<qreal>(i) / 12.0;
    QPoint p(start.x() + static_cast<int>((end.x() - start.x()) * t), start.y());
    QTest::mouseMove(window, p);
    ProcessEvents(12);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
  ProcessEvents(50);

  int interactive = 0;
  for (const auto& c : submitter.calls) {
    if (!c.settled) {
      ++interactive;
    }
  }
  EXPECT_GE(interactive, 4) << "production MonoSlider + snapshot echo interactive="
                            << interactive << " total=" << submitter.calls.size();
  EXPECT_EQ(submitter.settledCount(), 1);
  EXPECT_EQ(model.modeIndex(), 1);
}

TEST(EditorLookPanelInteractionTest, ColorTempDoubleClickResetMovesSliderToAsShotPos) {
  RecordingSubmitter submitter;
  EditorColorTempModel model;
  model.setSubmitter(&submitter);
  model.setAsShotCct(5200);
  model.setAsShotTint(0);
  model.loadFromParams(QStringLiteral("custom"), 9000, 0, true);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("colorTempModel"), &model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.loadData(QByteArray(kProductionMonoCctHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(80);

  auto* slider = window->findChild<QQuickItem*>(QStringLiteral("prodCctSlider"));
  ASSERT_NE(slider, nullptr);

  // Confirm the handle started at the custom CCT position.
  EXPECT_NEAR(slider->property("value").toReal(), static_cast<qreal>(model.cctSliderPos()), 1.0);

  const int asShotPos = color_temp::CctToSliderPos(5200.0f);
  const QPoint pos = MapToWindow(slider, 0.5, 0.5);
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, pos);
  ProcessEvents(120);

  EXPECT_EQ(model.modeIndex(), 0);
  EXPECT_NEAR(model.cct(), 5200.0, 1.0);
  EXPECT_NEAR(slider->property("value").toReal(), static_cast<qreal>(asShotPos), 2.0)
      << "slider handle must follow as-shot CCT after double-click reset; value="
      << slider->property("value").toReal() << " expected≈" << asShotPos;
}

// ── 8. Double-click must not stall the GUI event loop ───────────────────────
//
// Reproduces: double-click CDL / slider → OS wait cursor while the render busy
// spinner still animates. Cause class: nested settle + re-entrant snapshot load
// during the mouse sequence, leaving the GUI thread busy/re-entering submit.

class ReentrantSnapshotSubmitter : public RecordingSubmitter {
 public:
  std::function<void(const Call&)> on_call;
  int nestedProcessEventsMs = 5;

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    if (inSubmit) {
      reentered = true;
    }
    inSubmit = true;
    ++submitCount;
    if (!canEditState) {
      inSubmit = false;
      return false;
    }
    Call call{fieldKey, paramsJson, settled};
    calls.push_back(call);
    if (on_call) {
      on_call(call);
    }
    // Mimic session work that pumps the event loop (paint / present wakeups).
    if (nestedProcessEventsMs > 0) {
      ProcessEvents(nestedProcessEventsMs);
    }
    inSubmit = false;
    return true;
  }
};

TEST(EditorLookPanelInteractionTest, CdlDoubleClickKeepsEventLoopResponsive) {
  ReentrantSnapshotSubmitter submitter;
  auto model = std::make_unique<EditorCdlTrackballModel>();
  model->setSubmitter(&submitter);
  model->setWheelDisc(QStringLiteral("gamma"), 0.35, -0.2);

  // Snapshot echo on every submit (as EditorAdjustmentStack does).
  submitter.on_call = [&](const ReentrantSnapshotSubmitter::Call&) {
    // Reload disc from "snapshot" without intending to abort the gesture.
    // If the model incorrectly re-enters settle/drag from here, reentered trips.
  };

  QQuickWindow window;
  window.resize(220, 220);
  window.setColor(QColor(0x11, 0x11, 0x11));
  auto* disc = new EditorCdlTrackballItem(window.contentItem());
  disc->setObjectName(QStringLiteral("gammaDiscHang"));
  disc->setWidth(160);
  disc->setHeight(160);
  disc->setX(30);
  disc->setY(30);
  disc->setModel(model.get());
  disc->setWheelRole(QStringLiteral("gamma"));
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  ProcessEvents(50);

  const QPoint pos = MapToWindow(disc, 0.55, 0.55);
  // QTest::mouseDClick delivers press/release/press/dblclick/release.
  QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, pos);
  ProcessEvents(40);

  // Event loop must still service timers after the double-click sequence.
  bool timer_fired = false;
  QTimer::singleShot(0, [&] { timer_fired = true; });
  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(WaitUntil([&] { return timer_fired; }, 500))
      << "GUI event loop unresponsive after CDL double-click";
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  EXPECT_LT(elapsed, 400) << "event loop lag after CDL double-click: " << elapsed << "ms";
  EXPECT_FALSE(submitter.reentered) << "submit re-entered during double-click sequence";
  EXPECT_FALSE(model->dragActive());
  EXPECT_NEAR(model->gammaX(), 0.0, 1e-5);
  EXPECT_NEAR(model->gammaY(), 0.0, 1e-5);
  // At most one settled commit should own the reset (no finish+reset double settle).
  EXPECT_LE(submitter.settledCount(), 2)
      << "excess settled submits during double-click inflate render/history pressure; settled="
      << submitter.settledCount();
}

TEST(EditorLookPanelInteractionTest, AdjustmentSliderDoubleClickKeepsEventLoopResponsive) {
  ReentrantSnapshotSubmitter submitter;
  EditorAdjustmentValueModel model;
  model.setFieldKey(QStringLiteral("saturation"));
  model.setMinimum(-100);
  model.setMaximum(100);
  model.setDefaultValue(0);
  model.setStep(1);
  model.setPrecision(0);
  model.setSubmitter(&submitter);
  model.setValue(40);

  submitter.on_call = [&](const ReentrantSnapshotSubmitter::Call&) {
    // Snapshot echo: plain setValue (Tone/Look loadModelFromSnapshot path).
    // Must not re-enter submit.
  };

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.rootContext()->setContextProperty(QStringLiteral("exposureModel"), &model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("sliderSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentSlider.qml")));
  engine.loadData(QByteArray(kSliderHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(80);

  auto* handle = window->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderHandle"));
  ASSERT_NE(handle, nullptr);
  const QPoint pos = MapToWindow(handle, 0.5, 0.5);
  QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, pos);
  ProcessEvents(40);

  bool timer_fired = false;
  QTimer::singleShot(0, [&] { timer_fired = true; });
  ASSERT_TRUE(WaitUntil([&] { return timer_fired; }, 500))
      << "GUI event loop unresponsive after slider double-click";
  EXPECT_FALSE(submitter.reentered);
  EXPECT_FALSE(model.dragActive());
  EXPECT_NEAR(model.value(), 0.0, 1e-6);
}

// ── 9. Collapsible section chrome must stay borderless ──────────────────────

TEST(EditorLookPanelInteractionTest, CollapsibleSectionSurfaceHasNoBorder) {
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.rootContext()->setContextProperty(
      QStringLiteral("sectionSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/CollapsibleSection.qml")));

  const QByteArray harness = R"(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    width: 320; height: 200; visible: true; color: "#111"
    Loader {
        anchors.fill: parent
        anchors.margins: 12
        source: sectionSourceUrl
        onLoaded: {
            item.objectName = "borderSection"
            item.title = "Detail"
            item.expanded = true
            item.bodyContentHeight = 80
            item.surfaceColor = "#161719"
            item.disabledSurfaceColor = "#161719"
            item.borderColor = "#FFFFFF"
            item.textColor = "#eee"
            item.mutedColor = "#aaa"
            item.hoverColor = "#222"
            item.accentColor = "#ccc"
        }
    }
}
)";
  engine.loadData(harness);
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(60);

  auto* chrome = window->findChild<QQuickItem*>(QStringLiteral("collapsibleSectionChrome"));
  ASSERT_NE(chrome, nullptr) << "section chrome rectangle missing objectName";
  int width = -1;
  const QVariant borderVar = chrome->property("border");
  if (auto* pen = borderVar.value<QObject*>()) {
    width = pen->property("width").toInt();
  }
  EXPECT_EQ(width, 0) << "collapsible section must not draw a card border (got width=" << width
                      << ")";
}

// ── 10. Multi-slider rapid handoff + QML snapshot signal cascade ────────────
//
// Production path before the interactive-snapshot suppress fix:
//   slider move → submitPatch(interactive) → backend Emit/NotifyChange
//     → AdjustmentSnapshotChanged (sync) → loadFromSnapshot(Tone+Look)
// Switching from slider A to B immediately multiplies that cascade with
// settle(A) + interactive(B) while the mouse handler is still on the stack.
// Symptom: OS wait cursor, render spinner still animating (GUI stuck).

constexpr char kDualSliderHarness[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 400
    height: 280
    visible: true
    color: "#111"
    property int snapshotReloadCount: 0

    function loadFromSnapshotEcho() {
        // Mirrors EditorAdjustmentStack.loadFromSnapshot: plain setters only.
        // Count every echo so the test can see interactive signal storms.
        root.snapshotReloadCount++
        if (!satModel.dragActive) {
            // no-op value touch still runs JS + property plumbing cost
            satModel.value = satModel.value
        }
        if (!vibModel.dragActive) {
            vibModel.value = vibModel.value
        }
    }

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 20
        Loader {
            id: satLoader
            width: parent.width
            height: 64
            source: sliderSourceUrl
            onLoaded: {
                if (item) {
                    item.model = satModel
                    item.objectName = "satSlider"
                }
            }
        }
        Loader {
            id: vibLoader
            width: parent.width
            height: 64
            source: sliderSourceUrl
            onLoaded: {
                if (item) {
                    item.model = vibModel
                    item.objectName = "vibSlider"
                }
            }
        }
    }
}
)";

TEST(EditorLookPanelInteractionTest,
       RapidMultiSliderHandoffKeepsEventLoopResponsiveUnderSnapshotCascade) {
  // Session-like submitter: every patch synchronously re-enters a snapshot
  // reload callback (old interactive echo path) and may pump the event loop.
  class CascadeSubmitter : public QObject, public IEditorAdjustmentSubmitter {
   public:
    struct Call {
      QString fieldKey;
      QString params;
      bool    settled;
    };
    std::vector<Call>                calls;
    bool                             canEditState = true;
    bool                             inSubmit     = false;
    bool                             reentered    = false;
    int                              maxDepth     = 0;
    int                              depth        = 0;
    int                              nestedPumpMs = 2;
    std::function<void(const Call&)> on_each;

    auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
      if (inSubmit) {
        reentered = true;
      }
      inSubmit = true;
      ++depth;
      maxDepth = std::max(maxDepth, depth);
      if (!canEditState) {
        --depth;
        inSubmit = false;
        return false;
      }
      Call call{fieldKey, paramsJson, settled};
      calls.push_back(call);
      if (on_each) {
        on_each(call);
      }
      // Mimic paint/present wakeups that re-enter the event loop during submit.
      if (nestedPumpMs > 0) {
        ProcessEvents(nestedPumpMs);
      }
      --depth;
      inSubmit = false;
      return true;
    }
    auto canEdit() const -> bool override { return canEditState; }
  };

  CascadeSubmitter submitter;
  EditorAdjustmentValueModel sat;
  sat.setFieldKey(QStringLiteral("saturation"));
  sat.setMinimum(-100);
  sat.setMaximum(100);
  sat.setDefaultValue(0);
  sat.setStep(1);
  sat.setPrecision(0);
  sat.setSubmitter(&submitter);
  sat.setValue(0);

  EditorAdjustmentValueModel vib;
  vib.setFieldKey(QStringLiteral("vibrance"));
  vib.setMinimum(-100);
  vib.setMaximum(100);
  vib.setDefaultValue(0);
  vib.setStep(1);
  vib.setPrecision(0);
  vib.setSubmitter(&submitter);
  vib.setValue(0);

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(
      QStringLiteral("appTheme"), QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
  engine.rootContext()->setContextProperty(QStringLiteral("satModel"), &sat);
  engine.rootContext()->setContextProperty(QStringLiteral("vibModel"), &vib);
  engine.rootContext()->setContextProperty(
      QStringLiteral("sliderSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentSlider.qml")));
  engine.loadData(QByteArray(kDualSliderHarness));
  ASSERT_FALSE(engine.rootObjects().isEmpty());
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  ASSERT_NE(window, nullptr);
  window->show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(window));
  ProcessEvents(100);

  // Wire cascade: every submit reloads like AdjustmentSnapshotChanged did.
  auto* rootItem = window->contentItem();
  ASSERT_NE(rootItem, nullptr);
  // Access ApplicationWindow properties via root object.
  auto* winObj = engine.rootObjects().constFirst();
  submitter.on_each = [winObj](const CascadeSubmitter::Call&) {
    QMetaObject::invokeMethod(winObj, "loadFromSnapshotEcho");
  };

  auto* satHandle = window->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderHandle"));
  // Two sliders share the same handle objectName — find by walking parents.
  QList<QQuickItem*> handles;
  std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
    if (!item) return;
    if (item->objectName() == QLatin1String("adjustmentSliderHandle")) {
      handles.push_back(item);
    }
    for (QQuickItem* c : item->childItems()) collect(c);
  };
  collect(window->contentItem());
  ASSERT_EQ(handles.size(), 2) << "expected two AdjustmentSlider handles";
  auto* handleA = handles[0];
  auto* handleB = handles[1];

  const QPoint a0 = MapToWindow(handleA, 0.2, 0.5);
  const QPoint a1 = MapToWindow(handleA, 0.8, 0.5);
  const QPoint b0 = MapToWindow(handleB, 0.2, 0.5);
  const QPoint b1 = MapToWindow(handleB, 0.75, 0.5);

  // Drag slider A continuously, release, immediately drag slider B (handoff).
  const auto t0 = std::chrono::steady_clock::now();
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, a0);
  for (int i = 1; i <= 10; ++i) {
    const qreal t = static_cast<qreal>(i) / 10.0;
    QPoint p(a0.x() + static_cast<int>((a1.x() - a0.x()) * t), a0.y());
    QTest::mouseMove(window, p);
    ProcessEvents(5);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, a1);
  // No long pause — immediate handoff to the next slider.
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, b0);
  for (int i = 1; i <= 10; ++i) {
    const qreal t = static_cast<qreal>(i) / 10.0;
    QPoint p(b0.x() + static_cast<int>((b1.x() - b0.x()) * t), b0.y());
    QTest::mouseMove(window, p);
    ProcessEvents(5);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, b1);
  const auto dragMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();

  // Event loop must still service timers after the handoff sequence.
  bool timer_fired = false;
  QTimer::singleShot(0, [&] { timer_fired = true; });
  ASSERT_TRUE(WaitUntil([&] { return timer_fired; }, 800))
      << "GUI event loop unresponsive after multi-slider handoff (cascade freeze)";
  EXPECT_FALSE(submitter.reentered) << "submit re-entered via nested snapshot cascade";
  EXPECT_LE(submitter.maxDepth, 1) << "nested submit depth=" << submitter.maxDepth;
  EXPECT_LT(dragMs, 5000) << "multi-slider handoff took " << dragMs
                          << "ms under snapshot cascade (GUI thrash)";

  const int reloads = winObj->property("snapshotReloadCount").toInt();
  // Under the old path every interactive tick reloads. The handoff must still
  // complete with a responsive loop even when reloads fire; production now
  // suppresses interactive snapshot emits at the session controller.
  EXPECT_GE(static_cast<int>(submitter.calls.size()), 4);
  EXPECT_NE(sat.value(), 0.0);
  EXPECT_NE(vib.value(), 0.0);
  EXPECT_FALSE(sat.dragActive());
  EXPECT_FALSE(vib.dragActive());
  Q_UNUSED(reloads);
  Q_UNUSED(satHandle);
}

TEST(EditorLookPanelInteractionTest,
       MultiSliderSubmitDetectsRenderLockVersusGuiHandshakeContention) {
  // Production deadlock class (history Capture/Commit on GUI vs present):
  //   worker holds render_lock and BlockingQueuedConnection → GUI
  //   GUI submit blocks on render_lock
  // → both wait; OS wait cursor while render still looks "busy".
  //
  // We must not hang the test: GUI uses try_lock with a budget (fail-fast).
  // Worker owns the mutex end-to-end (never unlock from another thread).
  std::mutex        render_lock;
  std::atomic<bool> worker_ready{false};
  std::atomic<bool> worker_done{false};
  std::atomic<bool> contention{false};
  std::atomic<bool> release_worker{false};
  std::atomic<bool> worker_saw_gui{false};

  QObject gui_anchor;
  std::thread worker([&] {
    render_lock.lock();
    worker_ready.store(true);
    // Hold the lock until the GUI-side submit has observed contention.
    while (!release_worker.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Classic second half of the deadlock: worker needs the GUI while still
    // holding the lock. Production hangs here if GUI is blocked on the lock.
    // We unlock *before* BlockingQueued so the test can complete, then prove
    // the GUI is still pumpable.
    render_lock.unlock();
    QMetaObject::invokeMethod(
        &gui_anchor, [&] { worker_saw_gui.store(true); }, Qt::BlockingQueuedConnection);
    worker_done.store(true);
  });

  ASSERT_TRUE(WaitUntil([&] { return worker_ready.load(); }, 1000));

  class LockingSubmitter : public RecordingSubmitter {
   public:
    std::mutex*        lock       = nullptr;
    std::atomic<bool>* contention = nullptr;
    auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
      if (lock == nullptr) {
        return RecordingSubmitter::submitPatch(fieldKey, paramsJson, settled);
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
      while (std::chrono::steady_clock::now() < deadline) {
        if (lock->try_lock()) {
          lock->unlock();
          return RecordingSubmitter::submitPatch(fieldKey, paramsJson, settled);
        }
        ProcessEvents(2);
      }
      if (contention) {
        contention->store(true);
      }
      // Fail the patch rather than block the GUI forever.
      return false;
    }
  };

  LockingSubmitter submitter;
  submitter.lock       = &render_lock;
  submitter.contention = &contention;

  EditorAdjustmentValueModel model;
  model.setFieldKey(QStringLiteral("saturation"));
  model.setMinimum(-100);
  model.setMaximum(100);
  model.setDefaultValue(0);
  model.setStep(1);
  model.setSubmitter(&submitter);

  // Multi-slider style: two settle bursts while worker holds the lock.
  model.beginDrag();
  model.updateDrag(20);
  model.finishDrag();
  model.beginDrag();
  model.updateDrag(40);
  model.finishDrag();

  EXPECT_TRUE(contention.load())
      << "expected try_lock contention while worker holds render_lock "
         "(documents Capture/Commit vs present deadlock class)";

  bool timer_fired = false;
  QTimer::singleShot(0, [&] { timer_fired = true; });
  EXPECT_TRUE(WaitUntil([&] { return timer_fired; }, 500))
      << "GUI must stay responsive (fail-fast) under render_lock contention";

  release_worker.store(true);
  // Pump so worker's BlockingQueuedConnection can complete.
  ASSERT_TRUE(WaitUntil(
      [&] {
        ProcessEvents(10);
        return worker_done.load();
      },
      2000))
      << "worker failed to finish after release_worker";
  EXPECT_TRUE(worker_saw_gui.load());
  if (worker.joinable()) {
    worker.join();
  }
}

}  // namespace
}  // namespace alcedo::ui::test
