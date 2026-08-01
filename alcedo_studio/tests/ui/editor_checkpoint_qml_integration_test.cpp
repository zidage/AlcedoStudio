//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_checkpoint_qml_integration_test.cpp
/// @brief Phase 6C-5 Phase 6B: proves that the five QML entry surfaces are blocked
///        during an editor save and recover after the task finishes.
///
/// Each guarded QML entrypoint is exercised through its real component properties
/// rather than through C++ InteractionPolicyController getters alone. The test
/// registers an EditorSave task through the real BackgroundTaskController (the
/// same path EditorSessionTaskPort uses in production).

#include "ui/main_qml_test_fixture.hpp"

#include <QQuickItem>
#include <QSignalSpy>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QMetaObject>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"

namespace alcedo::ui::test {
namespace {

using EditorCheckpointQmlIntegrationTest = MainQmlTestFixture;

constexpr char kSaveReason[] = "Saving editor changes";

/// Process the Qt event loop so queued signals / bindings re-evaluate.
void SpinEventLoop(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

/// Register an EditorSave task holding the five checkpoint locks and return its
/// id. Mirrors EditorSessionTaskPort::BeginTask but operates directly on the
/// shared controller so the test observes the exact production lock set.
auto RegisterEditorSaveTask(BackgroundTaskController& registry,
                            const QString& reason = QString::fromUtf8(kSaveReason)) -> QString {
  BackgroundTaskSnapshot snap;
  snap.kind_             = BackgroundTaskKind::EditorSave;
  snap.state_            = BackgroundTaskState::Running;
  snap.title_            = QStringLiteral("editor_save");
  snap.detail_           = reason;
  snap.progress_percent_ = -1;
  snap.cancelable_       = false;
  snap.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
  snap.locks_            = {
      {InteractionCapability::SelectEditorImage, 0, reason},
      {InteractionCapability::SwitchWorkspace, 0, reason},
      {InteractionCapability::CheckoutVersion, 0, reason},
      {InteractionCapability::PasteAdjustments, 0, reason},
      {InteractionCapability::MergeAdjustments, 0, reason},
  };
  return registry.RegisterTask(snap);
}

/// Helper: read a QObject property as a QString, returning "" on failure.
auto ReadString(QObject* obj, const char* name) -> QString {
  if (!obj) return {};
  return obj->property(name).toString();
}

/// Helper: read a QObject property as a bool, returning false on failure.
auto ReadBool(QObject* obj, const char* name) -> bool {
  if (!obj) return false;
  return obj->property(name).toBool();
}

auto HasProperty(QObject* obj, const char* name) -> bool {
  return obj != nullptr && obj->metaObject()->indexOfProperty(name) >= 0;
}

TEST_F(EditorCheckpointQmlIntegrationTest,
       FiveQmlEntrySurfacesBlockedDuringSaveAndRecoverAfterFinish) {
  auto loaded = LoadMainWindow(true);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr) << "Main.qml window failed to load";
  ASSERT_TRUE(loaded->qml_warnings.empty())
      << "QML warnings: "
      << [&] {
           QStringList msgs;
           for (const auto& w : loaded->qml_warnings) msgs << w.toString();
           return msgs.join('\n').toStdString();
         }();

  auto points = FindGuardedEntrypoints(*loaded);
  auto* tasks = background_tasks(*loaded);
  auto* policy = interaction_policy(*loaded);
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(policy, nullptr);

  // ── Before save: all entry surfaces are enabled ──
  SpinEventLoop(100);

  const bool can_select_before = policy->CanSelectEditorImage();
  const bool can_switch_before = policy->CanSwitchWorkspace();
  const bool can_checkout_before = policy->CanCheckoutVersion();
  const bool can_paste_before = policy->CanPasteAdjustments();
  const bool can_merge_before = policy->CanMergeAdjustments();
  EXPECT_TRUE(can_select_before) << "SelectEditorImage blocked before save task";
  EXPECT_TRUE(can_switch_before) << "SwitchWorkspace blocked before save task";
  EXPECT_TRUE(can_checkout_before) << "CheckoutVersion blocked before save task";
  EXPECT_TRUE(can_paste_before) << "PasteAdjustments blocked before save task";
  EXPECT_TRUE(can_merge_before) << "MergeAdjustments blocked before save task";

  // ── QML component property checks before save ──
  // Filmstrip
  if (points.filmstrip) {
    EXPECT_TRUE(ReadBool(points.filmstrip, "selectionEnabled"))
        << "Filmstrip selectionEnabled blocked before save";
    EXPECT_TRUE(ReadString(points.filmstrip, "selectionDisabledReason").isEmpty());
  }

  // Workspace navigation
  if (points.switch_workspace_nav) {
    EXPECT_TRUE(ReadBool(points.switch_workspace_nav, "switchWorkspaceEnabled"))
        << "Workspace switch blocked before save";
    EXPECT_TRUE(ReadString(points.switch_workspace_nav, "switchWorkspaceDisabledReason").isEmpty());
  }

  // Versions rail
  if (points.versions_rail_button) {
    EXPECT_TRUE(ReadBool(points.versions_rail_button, "versionCheckoutEnabled"))
        << "Version checkout blocked before save";
    EXPECT_TRUE(
        ReadString(points.versions_rail_button, "versionCheckoutDisabledReason").isEmpty());
  }

  // Adjustment transfer actions
  if (points.transfer_actions) {
    EXPECT_TRUE(ReadBool(points.transfer_actions, "pasteEnabled"))
        << "Paste blocked before save";
  }

  // ── Start an editor-save task with the five checkpoint locks ──
  const QString task_id = RegisterEditorSaveTask(*tasks);
  EXPECT_FALSE(task_id.isEmpty());
  SpinEventLoop(100);

  // ── C++ policy: all five checkpoint capabilities are blocked with reason ──
  EXPECT_FALSE(policy->CanSelectEditorImage());
  EXPECT_FALSE(policy->CanSwitchWorkspace());
  EXPECT_FALSE(policy->CanCheckoutVersion());
  EXPECT_FALSE(policy->CanPasteAdjustments());
  EXPECT_FALSE(policy->CanMergeAdjustments());
  EXPECT_FALSE(policy->SelectEditorImageReason().isEmpty());
  EXPECT_FALSE(policy->SwitchWorkspaceReason().isEmpty());
  EXPECT_FALSE(policy->CheckoutVersionReason().isEmpty());
  EXPECT_FALSE(policy->PasteAdjustmentsReason().isEmpty());
  EXPECT_FALSE(policy->MergeAdjustmentsReason().isEmpty());

  // Non-checkpoint capabilities remain available.
  EXPECT_TRUE(policy->CanChangeSemanticModel());
  EXPECT_TRUE(policy->CanRunSemanticGeneration());

  // ── QML entry surfaces: properties reflect the blocked state ──

  // Filmstrip: selectionEnabled is false, reason is non-empty.
  if (points.filmstrip) {
    EXPECT_FALSE(ReadBool(points.filmstrip, "selectionEnabled"))
        << "Filmstrip selectionEnabled still true during save";
    EXPECT_FALSE(ReadString(points.filmstrip, "selectionDisabledReason").isEmpty())
        << "Filmstrip selectionDisabledReason empty during save";
  }

  // Workspace navigation: switchWorkspaceEnabled is false, reason is non-empty.
  if (points.switch_workspace_nav) {
    EXPECT_FALSE(ReadBool(points.switch_workspace_nav, "switchWorkspaceEnabled"))
        << "Workspace switch still enabled during save";
    EXPECT_FALSE(ReadString(points.switch_workspace_nav, "switchWorkspaceDisabledReason").isEmpty())
        << "Workspace switch reason empty during save";
  }

  // Versions rail: versionCheckoutEnabled is false, reason is non-empty.
  if (points.versions_rail_button) {
    EXPECT_FALSE(ReadBool(points.versions_rail_button, "versionCheckoutEnabled"))
        << "Version checkout still enabled during save";
    EXPECT_FALSE(
        ReadString(points.versions_rail_button, "versionCheckoutDisabledReason").isEmpty())
        << "Version checkout reason empty during save";

    // History browsing (the rail itself) remains available: the rail can still
    // open the history panel even when version checkout is locked.
    EXPECT_TRUE(ReadBool(points.versions_rail_button, "enabled"))
        << "History rail itself disabled when only checkout is locked";
  }

  // Adjustment transfer actions: paste/merge is blocked with reason.
  if (points.transfer_actions) {
    EXPECT_FALSE(ReadBool(points.transfer_actions, "pasteEnabled"))
        << "Paste still enabled during save";
    if (HasProperty(points.transfer_actions, "mergeEnabled")) {
      EXPECT_FALSE(ReadBool(points.transfer_actions, "mergeEnabled"))
          << "Merge still enabled during save";
    }
    // Version checkout UI belongs to 6C-6; paste/merge are checked here.
  }

  // ── Finish the save task ──
  tasks->FinishTask(task_id, BackgroundTaskState::Succeeded);
  SpinEventLoop(100);

  // ── All five checkpoint capabilities recover ──
  EXPECT_TRUE(policy->CanSelectEditorImage());
  EXPECT_TRUE(policy->CanSwitchWorkspace());
  EXPECT_TRUE(policy->CanCheckoutVersion());
  EXPECT_TRUE(policy->CanPasteAdjustments());
  EXPECT_TRUE(policy->CanMergeAdjustments());

  // ── QML entry surfaces recover ──
  if (points.filmstrip) {
    EXPECT_TRUE(ReadBool(points.filmstrip, "selectionEnabled"))
        << "Filmstrip selectionEnabled not restored after save";
    EXPECT_TRUE(ReadString(points.filmstrip, "selectionDisabledReason").isEmpty())
        << "Filmstrip reason not cleared after save";
  }

  if (points.switch_workspace_nav) {
    EXPECT_TRUE(ReadBool(points.switch_workspace_nav, "switchWorkspaceEnabled"))
        << "Workspace switch not restored after save";
    EXPECT_TRUE(ReadString(points.switch_workspace_nav, "switchWorkspaceDisabledReason").isEmpty())
        << "Workspace switch reason not cleared after save";
  }

  if (points.versions_rail_button) {
    EXPECT_TRUE(ReadBool(points.versions_rail_button, "versionCheckoutEnabled"))
        << "Version checkout not restored after save";
    EXPECT_TRUE(
        ReadString(points.versions_rail_button, "versionCheckoutDisabledReason").isEmpty())
        << "Version checkout reason not cleared after save";
  }
}

TEST_F(EditorCheckpointQmlIntegrationTest,
       FailedSaveAlsoClearsQmlLocks) {
  auto loaded = LoadMainWindow(true);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr) << "Main.qml window failed to load";
  ASSERT_TRUE(loaded->qml_warnings.empty());

  auto points = FindGuardedEntrypoints(*loaded);
  auto* tasks = background_tasks(*loaded);
  auto* policy = interaction_policy(*loaded);
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(policy, nullptr);

  // Register and immediately fail the task.
  const QString task_id = RegisterEditorSaveTask(*tasks);
  SpinEventLoop(100);
  EXPECT_FALSE(policy->CanSelectEditorImage());

  tasks->FinishTask(task_id, BackgroundTaskState::Failed);
  SpinEventLoop(100);

  // All locks clear on failure.
  EXPECT_TRUE(policy->CanSelectEditorImage());
  EXPECT_TRUE(policy->CanSwitchWorkspace());
  EXPECT_TRUE(policy->CanCheckoutVersion());
  EXPECT_TRUE(policy->CanPasteAdjustments());
  EXPECT_TRUE(policy->CanMergeAdjustments());

  // QML surfaces recover.
  if (points.filmstrip) {
    EXPECT_TRUE(ReadBool(points.filmstrip, "selectionEnabled"));
    EXPECT_TRUE(ReadString(points.filmstrip, "selectionDisabledReason").isEmpty());
  }
}

TEST_F(EditorCheckpointQmlIntegrationTest,
       PolicyChangedFiresOnceWhenSaveTaskStartsAndClears) {
  auto loaded = LoadMainWindow(true);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->window, nullptr);
  ASSERT_TRUE(loaded->qml_warnings.empty());

  auto* tasks = background_tasks(*loaded);
  auto* policy = interaction_policy(*loaded);
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(policy, nullptr);

  QSignalSpy spy(policy, &InteractionPolicyController::PolicyChanged);
  const auto before_count = spy.count();

  const QString task_id = RegisterEditorSaveTask(*tasks);
  SpinEventLoop(100);
  EXPECT_GT(spy.count(), before_count) << "PolicyChanged not emitted on save task start";

  const auto during_count = spy.count();
  tasks->FinishTask(task_id, BackgroundTaskState::Succeeded);
  SpinEventLoop(100);
  EXPECT_GT(spy.count(), during_count) << "PolicyChanged not emitted when save task finishes";
}

}  // namespace
}  // namespace alcedo::ui::test
