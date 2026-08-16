//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_workspace_navigation_qml_test.cpp
/// @brief Verifies the extracted Library/Editor navigation component at its QML boundary.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <filesystem>
#include <memory>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "editorWorkspaceNavigationHarness"
    width: 320
    height: 80
    visible: true

    property QtObject router: QtObject {
        objectName: "workspaceRouter"
        property string workspace: "library"
        property int libraryOpenCount: 0
        property int editorOpenCount: 0
        property int lastElementId: 0
        property int lastImageId: 0

        function openLibrary() {
            libraryOpenCount += 1
            workspace = "library"
        }

        function openEditor(elementId, imageId) {
            editorOpenCount += 1
            lastElementId = elementId
            lastImageId = imageId
            workspace = "editor"
        }
    }

    property QtObject policy: QtObject {
        objectName: "interactionPolicy"
        property bool canSwitchWorkspace: true
        property string switchWorkspaceReason: "Saving editor changes"
    }

    property QtObject editorSession: QtObject {
        objectName: "editorSession"
        property int lastElementId: 42
        property int lastImageId: 7
    }

    property QtObject firstImageProvider: QtObject {
        objectName: "firstImageProvider"
        property int elementId: 0
        property int imageId: 0
    }

    Loader {
        id: navigationLoader
        objectName: "navigationLoader"
        anchors.centerIn: parent
        width: 112
        height: 40
        source: navigationSourceUrl

        onLoaded: {
            if (!item) {
                return
            }
            item.workspaceRouter = router
            item.interactionPolicy = policy
            item.editorSession = editorSession
            item.navigationEnabled = true
            item.editorImageExists = function(elementId) { return elementId !== 99 }
            item.firstEditorImage = function() {
                if (firstImageProvider.elementId <= 0 || firstImageProvider.imageId <= 0) {
                    return null
                }
                return {
                    elementId: firstImageProvider.elementId,
                    imageId: firstImageProvider.imageId
                }
            }
        }
    }
}
)";

auto           QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto NavigationUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorWorkspaceNavigation.qml"));
}

void ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

void ClickButton(QObject* button) {
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "click", Qt::DirectConnection));
}

class EditorWorkspaceNavigationQmlHarness {
 public:
  QQmlApplicationEngine engine;
  QObject*              root_object = nullptr;
  QQuickWindow*         window      = nullptr;
  QStringList           warnings;

  EditorWorkspaceNavigationQmlHarness() {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) {
        warnings.push_back(warning.toString());
      }
    });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, [this](const QUrl& url) {
          warnings.push_back(QStringLiteral("Object creation failed: ") + url.toString());
        });
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(QmlDirectory());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("navigationSourceUrl"),
                                             NavigationUrl());
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///EditorWorkspaceNavigationHarness.qml")));
    if (!engine.rootObjects().empty()) {
      root_object = engine.rootObjects().front();
      window      = qobject_cast<QQuickWindow*>(root_object);
      if (window) {
        window->show();
        window->requestActivate();
      }
    }
    ProcessEvents(20);
  }

  auto navigation() const -> QObject* {
    return window ? window->findChild<QObject*>(QStringLiteral("editorWorkspaceNavigation"))
                  : nullptr;
  }

  auto router() const -> QObject* {
    return window ? window->findChild<QObject*>(QStringLiteral("workspaceRouter")) : nullptr;
  }

  auto policy() const -> QObject* {
    return window ? window->findChild<QObject*>(QStringLiteral("interactionPolicy")) : nullptr;
  }

  auto button(const QString& name) const -> QQuickItem* {
    return window ? window->findChild<QQuickItem*>(name) : nullptr;
  }
};

TEST(EditorWorkspaceNavigationQmlTest, AllowedButtonsRouteAndRestoreLastEditorImage) {
  EditorWorkspaceNavigationQmlHarness harness;
  ASSERT_NE(harness.window, nullptr)
      << "root=" << (harness.root_object ? harness.root_object->metaObject()->className() : "none")
      << " warnings=" << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* editor  = harness.button(QStringLiteral("editorNavButton"));
  auto* library = harness.button(QStringLiteral("libraryNavButton"));
  auto* router  = harness.router();
  ASSERT_NE(editor, nullptr);
  ASSERT_NE(library, nullptr);
  ASSERT_NE(router, nullptr);

  EXPECT_TRUE(editor->isEnabled());
  EXPECT_TRUE(library->isEnabled());
  ClickButton(editor);
  ProcessEvents(20);
  EXPECT_EQ(router->property("editorOpenCount").toInt(), 1);
  EXPECT_EQ(router->property("lastElementId").toInt(), 42);
  EXPECT_EQ(router->property("lastImageId").toInt(), 7);
  EXPECT_EQ(router->property("workspace").toString(), QStringLiteral("editor"));

  ClickButton(library);
  ProcessEvents(20);
  EXPECT_EQ(router->property("libraryOpenCount").toInt(), 1);
  EXPECT_EQ(router->property("workspace").toString(), QStringLiteral("library"));
}

TEST(EditorWorkspaceNavigationQmlTest, PolicyBlockDisablesBothButtonsWithoutRouterCalls) {
  EditorWorkspaceNavigationQmlHarness harness;
  ASSERT_NE(harness.window, nullptr)
      << "root=" << (harness.root_object ? harness.root_object->metaObject()->className() : "none")
      << " warnings=" << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* editor     = harness.button(QStringLiteral("editorNavButton"));
  auto* library    = harness.button(QStringLiteral("libraryNavButton"));
  auto* router     = harness.router();
  auto* policy     = harness.policy();
  auto* navigation = harness.navigation();
  ASSERT_NE(editor, nullptr);
  ASSERT_NE(library, nullptr);
  ASSERT_NE(router, nullptr);
  ASSERT_NE(policy, nullptr);
  ASSERT_NE(navigation, nullptr);

  policy->setProperty("canSwitchWorkspace", false);
  ProcessEvents(20);
  EXPECT_FALSE(editor->isEnabled());
  EXPECT_FALSE(library->isEnabled());
  EXPECT_EQ(navigation->property("switchWorkspaceDisabledReason").toString(),
            QStringLiteral("Saving editor changes"));

  ClickButton(editor);
  ClickButton(library);
  ProcessEvents(20);
  EXPECT_EQ(router->property("editorOpenCount").toInt(), 0);
  EXPECT_EQ(router->property("libraryOpenCount").toInt(), 0);

  policy->setProperty("canSwitchWorkspace", true);
  ProcessEvents(20);
  EXPECT_TRUE(editor->isEnabled());
  ClickButton(editor);
  ProcessEvents(20);
  EXPECT_EQ(router->property("editorOpenCount").toInt(), 1);
}

TEST(EditorWorkspaceNavigationQmlTest, MissingLastImageFallsBackToEmptyEditor) {
  EditorWorkspaceNavigationQmlHarness harness;
  ASSERT_NE(harness.window, nullptr)
      << "root=" << (harness.root_object ? harness.root_object->metaObject()->className() : "none")
      << " warnings=" << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* editor  = harness.button(QStringLiteral("editorNavButton"));
  auto* session = harness.window->findChild<QObject*>(QStringLiteral("editorSession"));
  auto* router  = harness.router();
  ASSERT_NE(editor, nullptr);
  ASSERT_NE(session, nullptr);
  ASSERT_NE(router, nullptr);

  session->setProperty("lastElementId", 99);
  session->setProperty("lastImageId", 7);
  ClickButton(editor);
  ProcessEvents(20);
  EXPECT_EQ(router->property("lastElementId").toInt(), 0);
  EXPECT_EQ(router->property("lastImageId").toInt(), 0);
}

TEST(EditorWorkspaceNavigationQmlTest, MissingLastImageOpensFirstLibraryImage) {
  EditorWorkspaceNavigationQmlHarness harness;
  ASSERT_NE(harness.window, nullptr)
      << "root=" << (harness.root_object ? harness.root_object->metaObject()->className() : "none")
      << " warnings=" << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* editor = harness.button(QStringLiteral("editorNavButton"));
  auto* session = harness.window->findChild<QObject*>(QStringLiteral("editorSession"));
  auto* first   = harness.window->findChild<QObject*>(QStringLiteral("firstImageProvider"));
  auto* router  = harness.router();
  ASSERT_NE(editor, nullptr);
  ASSERT_NE(session, nullptr);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(router, nullptr);

  session->setProperty("lastElementId", 99);
  session->setProperty("lastImageId", 7);
  first->setProperty("elementId", 11);
  first->setProperty("imageId", 22);
  ClickButton(editor);
  ProcessEvents(20);
  EXPECT_EQ(router->property("lastElementId").toInt(), 11);
  EXPECT_EQ(router->property("lastImageId").toInt(), 22);
}

}  // namespace
}  // namespace alcedo::ui::test
