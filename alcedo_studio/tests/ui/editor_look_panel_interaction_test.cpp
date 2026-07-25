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

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_cdl_trackball_item.hpp"
#include "ui/alcedo_main/album_backend/editor_cdl_trackball_model.hpp"
#include "ui/alcedo_main/album_backend/editor_color_temp_model.hpp"
#include "ui/alcedo_main/app_theme.hpp"

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

}  // namespace
}  // namespace alcedo::ui::test
