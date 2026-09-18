//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file main_qml_test_fixture.hpp
/// @brief Shared Main.qml loading helpers for workspace and interaction-policy UI tests.
///
/// Moves the LoadedMainWindow / MainQmlUrl / LoadMainWindow setup out of
/// workspace_shell_test without changing production startup. Does not implement
/// a second application bootstrap path.

#include "ui/album_backend_test_fixture.hpp"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QUrl>

#include <filesystem>
#include <memory>
#include <vector>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/language_manager.hpp"

class QQuickItem;

namespace alcedo::ui::test {

/// One loaded Main.qml window with a real ApplicationModuleHost.
struct LoadedMainWindow {
  ApplicationModuleHost      host;
  alcedo::ui::LanguageManager language_manager{QCoreApplication::instance()};
  QQmlApplicationEngine      engine;
  std::vector<QQmlError>     qml_warnings;
  QQuickWindow*              window = nullptr;

  LoadedMainWindow()                                  = default;
  LoadedMainWindow(const LoadedMainWindow&)            = delete;
  LoadedMainWindow& operator=(const LoadedMainWindow&) = delete;
};

/// Handles for the four Phase 6 interaction-policy guarded QML entry surfaces.
struct GuardedQmlEntrypoints {
  QQuickItem* switch_workspace_nav = nullptr;  ///< libraryNavButton / editorNavButton host
  QQuickItem* library_nav_button   = nullptr;
  QQuickItem* editor_nav_button    = nullptr;
  QQuickItem* filmstrip            = nullptr;  ///< canSelectEditorImage surface
  QQuickItem* versions_rail_button = nullptr;  ///< canCheckoutVersion surface
  QObject*    transfer_actions     = nullptr;  ///< canPaste surface
};

/// Returns the filesystem URL for production Main.qml under ALCEDO_TEST_SRC_DIR.
auto MainQmlUrl() -> QUrl;

/// GoogleTest fixture base that loads Main.qml with real host/policy controllers.
///
/// Preconditions: a QCoreApplication must exist (widget/ui test main).
class MainQmlTestFixture : public ApplicationModuleHostTestFixture {
 protected:
  /// Force reduced motion so workflow tests observe terminal geometry state.
  void ForceReducedMotionForWorkflowTests();

  /// Load Main.qml with an empty temp project (or no project when create_project is false).
  ///
  /// @param create_project  When true, CreateTestProject is called before load.
  /// @return Owned loaded window; window may be null when QML failed to load.
  auto LoadMainWindow(bool create_project = true) -> std::unique_ptr<LoadedMainWindow>;

  /// Load Main.qml after opening a packed project so serviceReady is true at init.
  auto LoadMainWindowWithPackedProject(const std::filesystem::path& packed_path)
      -> std::unique_ptr<LoadedMainWindow>;

  /// Resolve the four guarded editor entry surfaces under the loaded window.
  ///
  /// @param loaded  Window returned by LoadMainWindow*.
  /// @return Handles that may be null when the editor workspace is not mounted.
  auto FindGuardedEntrypoints(LoadedMainWindow& loaded) -> GuardedQmlEntrypoints;

  /// Access the real BackgroundTaskController owned by the host.
  [[nodiscard]] auto background_tasks(LoadedMainWindow& loaded) -> BackgroundTaskController*;

  /// Access the real InteractionPolicyController owned by the host.
  [[nodiscard]] auto interaction_policy(LoadedMainWindow& loaded) -> InteractionPolicyController*;

 private:
  auto FinishLoad(std::unique_ptr<LoadedMainWindow> loaded) -> std::unique_ptr<LoadedMainWindow>;
};

}  // namespace alcedo::ui::test
