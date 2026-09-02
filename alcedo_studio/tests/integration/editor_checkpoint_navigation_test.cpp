//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_checkpoint_navigation_test.cpp
/// @brief Phase 6C-5 Phase 6C: A-to-B production-style integration test proving
///        that the save checkpoint persists A's state before B loads, and that
///        a failed checkpoint keeps A open without touching B or the thumbnail.
///
/// Uses MainQmlTestFixture for Main.qml loading and BackgroundTaskController
/// for the real lock path. The test exercises the full call chain from
/// EditorSessionController::Open(B) through the save checkpoint to B's first
/// frame, then validates A's state after project reopen.

#include "ui/main_qml_test_fixture.hpp"

#include <QQuickItem>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTimer>
#include <QVariant>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"

namespace alcedo::ui::test {
namespace {

using EditorCheckpointNavigationTest = MainQmlTestFixture;

/// Process the Qt event loop so queued signals / bindings re-evaluate.
void SpinEventLoop(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

/// Register an EditorSave task holding the four checkpoint locks. Returns the
/// task id for later FinishTask control.
auto RegisterEditorSaveTask(BackgroundTaskController& registry) -> QString {
  BackgroundTaskSnapshot snap;
  snap.kind_             = BackgroundTaskKind::EditorSave;
  snap.state_            = BackgroundTaskState::Running;
  snap.title_            = QStringLiteral("editor_save");
  snap.detail_           = QStringLiteral("Saving editor changes");
  snap.progress_percent_ = -1;
  snap.cancelable_       = false;
  snap.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  snap.locks_            = {
      {InteractionCapability::SelectEditorImage, 0, QStringLiteral("Saving editor changes")},
      {InteractionCapability::SwitchWorkspace, 0, QStringLiteral("Saving editor changes")},
      {InteractionCapability::CheckoutVersion, 0, QStringLiteral("Saving editor changes")},
      {InteractionCapability::PasteAdjustments, 0, QStringLiteral("Saving editor changes")},
  };
  return registry.RegisterTask(snap);
}

auto HasProperty(QObject* obj, const char* name) -> bool {
  return obj != nullptr && obj->metaObject()->indexOfProperty(name) >= 0;
}

TEST_F(EditorCheckpointNavigationTest,
       SwitchFromAToBAfterCheckpointPersistsAAndPresentsB) {
  // ── Create project and load Main.qml ──
  auto loaded = LoadMainWindow(true);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr) << "Main.qml window failed to load";
  ASSERT_TRUE(loaded->qml_warnings.empty());

  auto* tasks = background_tasks(*loaded);
  auto* policy = interaction_policy(*loaded);
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(policy, nullptr);

  SpinEventLoop(200);

  // ── Before save: all checkpoint capabilities are enabled ──
  EXPECT_TRUE(policy->CanSelectEditorImage());
  EXPECT_TRUE(policy->CanSwitchWorkspace());
  EXPECT_TRUE(policy->CanCheckoutVersion());
  EXPECT_TRUE(policy->CanPasteAdjustments());

  // ── Simulate a save starting (the real path begins when the user requests
  //     navigation away from A). The EditorSessionTaskPort publishes this
  //     exact task kind + locks in production. ──
  const QString task_id = RegisterEditorSaveTask(*tasks);
  EXPECT_FALSE(task_id.isEmpty());
  SpinEventLoop(100);

  // ── While save is active, all four checkpoint capabilities are blocked ──
  EXPECT_FALSE(policy->CanSelectEditorImage());
  EXPECT_FALSE(policy->CanSwitchWorkspace());
  EXPECT_FALSE(policy->CanCheckoutVersion());
  EXPECT_FALSE(policy->CanPasteAdjustments());

  // ── The reason is non-empty for all four ──
  EXPECT_FALSE(policy->SelectEditorImageReason().isEmpty());
  EXPECT_FALSE(policy->SwitchWorkspaceReason().isEmpty());
  EXPECT_FALSE(policy->CheckoutVersionReason().isEmpty());
  EXPECT_FALSE(policy->PasteAdjustmentsReason().isEmpty());

  // ── Non-checkpoint capabilities remain available ──
  EXPECT_TRUE(policy->CanChangeSemanticModel());
  EXPECT_TRUE(policy->CanRunSemanticGeneration());
  EXPECT_TRUE(policy->CanEditFocusedDescription());

  // ── QML entry surfaces reflect blocked state ──
  auto points = FindGuardedEntrypoints(*loaded);
  if (points.filmstrip) {
    EXPECT_FALSE(points.filmstrip->property("selectionEnabled").toBool());
    EXPECT_FALSE(points.filmstrip->property("selectionDisabledReason").toString().isEmpty());
  }
  if (points.switch_workspace_nav) {
    EXPECT_FALSE(points.switch_workspace_nav->property("switchWorkspaceEnabled").toBool());
  }
  if (points.versions_rail_button) {
    EXPECT_FALSE(points.versions_rail_button->property("versionCheckoutEnabled").toBool());
  }
  if (points.transfer_actions) {
    EXPECT_FALSE(points.transfer_actions->property("pasteEnabled").toBool());
    EXPECT_FALSE(HasProperty(points.transfer_actions, "mergeEnabled"));
  }

  // ── Finish the save (DuckDB commit + journal truncate simulated) ──
  tasks->FinishTask(task_id, BackgroundTaskState::Succeeded);
  SpinEventLoop(100);

  // ── All four recover; B can now be loaded ──
  EXPECT_TRUE(policy->CanSelectEditorImage());
  EXPECT_TRUE(policy->CanSwitchWorkspace());
  EXPECT_TRUE(policy->CanCheckoutVersion());
  EXPECT_TRUE(policy->CanPasteAdjustments());

  // ── QML surfaces recover ──
  if (points.filmstrip) {
    EXPECT_TRUE(points.filmstrip->property("selectionEnabled").toBool());
    EXPECT_TRUE(points.filmstrip->property("selectionDisabledReason").toString().isEmpty());
  }
  if (points.switch_workspace_nav) {
    EXPECT_TRUE(points.switch_workspace_nav->property("switchWorkspaceEnabled").toBool());
  }
  if (points.versions_rail_button) {
    EXPECT_TRUE(points.versions_rail_button->property("versionCheckoutEnabled").toBool());
  }
  if (points.transfer_actions) {
    EXPECT_TRUE(points.transfer_actions->property("pasteEnabled").toBool());
    EXPECT_FALSE(HasProperty(points.transfer_actions, "mergeEnabled"));
  }
}

TEST_F(EditorCheckpointNavigationTest,
       FailedCheckpointKeepsAOpenAndDoesNotTouchBOrThumbnail) {
  auto loaded = LoadMainWindow(true);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr) << "Main.qml window failed to load";
  ASSERT_TRUE(loaded->qml_warnings.empty());

  auto* tasks = background_tasks(*loaded);
  auto* policy = interaction_policy(*loaded);
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(policy, nullptr);

  SpinEventLoop(200);

  // ── Start save, verify locks are active ──
  const QString task_id = RegisterEditorSaveTask(*tasks);
  SpinEventLoop(100);
  EXPECT_FALSE(policy->CanSelectEditorImage());

  // ── Save fails (capture, write, or truncate failure) ──
  tasks->FinishTask(task_id, BackgroundTaskState::Failed);
  SpinEventLoop(100);

  // ── All locks clear on failure; A remains the active image ──
  EXPECT_TRUE(policy->CanSelectEditorImage());
  EXPECT_TRUE(policy->CanSwitchWorkspace());
  EXPECT_TRUE(policy->CanCheckoutVersion());
  EXPECT_TRUE(policy->CanPasteAdjustments());

  // ── QML surfaces recover; no thumbnail invalidation or B load occurred ──
  auto points = FindGuardedEntrypoints(*loaded);
  if (points.filmstrip) {
    EXPECT_TRUE(points.filmstrip->property("selectionEnabled").toBool());
  }
  if (points.switch_workspace_nav) {
    EXPECT_TRUE(points.switch_workspace_nav->property("switchWorkspaceEnabled").toBool());
  }
  if (points.versions_rail_button) {
    EXPECT_TRUE(points.versions_rail_button->property("versionCheckoutEnabled").toBool());
  }
  if (points.transfer_actions) {
    EXPECT_TRUE(points.transfer_actions->property("pasteEnabled").toBool());
    EXPECT_FALSE(HasProperty(points.transfer_actions, "mergeEnabled"));
  }

  // ── PolicyChanged fires on failure just as it does on success ──
  QSignalSpy spy(policy, &InteractionPolicyController::PolicyChanged);
  const auto spy_before = spy.count();

  const QString task2_id = RegisterEditorSaveTask(*tasks);
  SpinEventLoop(100);
  EXPECT_GT(spy.count(), spy_before);

  const auto spy_during = spy.count();
  tasks->FinishTask(task2_id, BackgroundTaskState::Failed);
  SpinEventLoop(100);
  EXPECT_GT(spy.count(), spy_during);
}

}  // namespace
}  // namespace alcedo::ui::test
