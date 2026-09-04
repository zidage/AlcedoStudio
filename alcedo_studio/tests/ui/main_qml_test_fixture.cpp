//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/main_qml_test_fixture.hpp"

#include <QQmlContext>
#include <QQmlExtensionPlugin>
#include <QQuickItem>
#include <QQuickStyle>
#include <QString>
#include <QuickQanava>
#include <sstream>

#include "ui/album_backend_seeded_project_fixture.hpp"
#include "ui/alcedo_main/app_theme.hpp"

Q_IMPORT_QML_PLUGIN(QuickQanavaPlugin)

namespace alcedo::ui::test {

auto MainQmlUrl() -> QUrl {
  const auto path =
      std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" / "Main.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

void MainQmlTestFixture::ForceReducedMotionForWorkflowTests() {
  alcedo::ui::AppTheme::Instance().setReduceMotion(true);
}

auto MainQmlTestFixture::FinishLoad(std::unique_ptr<LoadedMainWindow> loaded)
    -> std::unique_ptr<LoadedMainWindow> {
  alcedo::ui::AppTheme::SetEffectiveLanguageCode(loaded->language_manager.EffectiveLanguageCode());
  QQuickStyle::setStyle(QStringLiteral("Material"));

  loaded->engine.addImportPath(QStringLiteral("qrc:/"));
  QuickQanava::initialize(&loaded->engine);
  loaded->language_manager.AttachEngine(&loaded->engine);
  loaded->host.AttachQmlEngine(&loaded->engine);
  loaded->engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &loaded->host);
  loaded->engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                                   &alcedo::ui::AppTheme::Instance());
  loaded->engine.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                                   &loaded->language_manager);

  QObject::connect(&loaded->engine, &QQmlEngine::warnings,
                   [raw = loaded.get()](const QList<QQmlError>& warnings) {
                     raw->qml_warnings.insert(raw->qml_warnings.end(), warnings.begin(),
                                              warnings.end());
                   });

  loaded->engine.load(MainQmlUrl());
  if (loaded->engine.rootObjects().empty()) {
    std::ostringstream errors;
    for (const auto& warning : loaded->qml_warnings) {
      errors << warning.toString().toStdString() << '\n';
    }
    ADD_FAILURE() << errors.str();
    return loaded;
  }
  loaded->window = qobject_cast<QQuickWindow*>(loaded->engine.rootObjects().front());
  if (loaded->window != nullptr) {
    loaded->window->show();
    loaded->window->requestActivate();
  }
  return loaded;
}

auto MainQmlTestFixture::LoadMainWindow(bool create_project)
    -> std::unique_ptr<LoadedMainWindow> {
  auto loaded = std::make_unique<LoadedMainWindow>();
  alcedo::ui::AppTheme::RegisterFonts();
  ForceReducedMotionForWorkflowTests();
  if (create_project) {
    EXPECT_TRUE(CreateTestProject(loaded->host));
  }
  return FinishLoad(std::move(loaded));
}

auto MainQmlTestFixture::LoadMainWindowWithPackedProject(const std::filesystem::path& packed_path)
    -> std::unique_ptr<LoadedMainWindow> {
  auto loaded = std::make_unique<LoadedMainWindow>();
  alcedo::ui::AppTheme::RegisterFonts();
  ForceReducedMotionForWorkflowTests();
  EXPECT_TRUE(LoadPackedProject(loaded->host, packed_path));
  return FinishLoad(std::move(loaded));
}

auto MainQmlTestFixture::FindGuardedEntrypoints(LoadedMainWindow& loaded) -> GuardedQmlEntrypoints {
  GuardedQmlEntrypoints points;
  if (loaded.window == nullptr) {
    return points;
  }
  points.library_nav_button =
      loaded.window->findChild<QQuickItem*>(QStringLiteral("libraryNavButton"));
  points.editor_nav_button =
      loaded.window->findChild<QQuickItem*>(QStringLiteral("editorNavButton"));
  points.switch_workspace_nav =
      loaded.window->findChild<QQuickItem*>(QStringLiteral("editorWorkspaceNavigation"));
  if (points.switch_workspace_nav == nullptr) {
    points.switch_workspace_nav =
        loaded.window->findChild<QQuickItem*>(QStringLiteral("workspaceSwitch"));
  }
  points.filmstrip = loaded.window->findChild<QQuickItem*>(QStringLiteral("editorFilmstrip"));
  points.versions_rail_button =
      loaded.window->findChild<QQuickItem*>(QStringLiteral("editorVersionsRailButton"));
  points.transfer_actions =
      loaded.window->findChild<QObject*>(QStringLiteral("editorAdjustmentTransferActions"));
  return points;
}

auto MainQmlTestFixture::background_tasks(LoadedMainWindow& loaded) -> BackgroundTaskController* {
  return loaded.host.background_tasks();
}

auto MainQmlTestFixture::interaction_policy(LoadedMainWindow& loaded)
    -> InteractionPolicyController* {
  return loaded.host.interaction_policy();
}

}  // namespace alcedo::ui::test
