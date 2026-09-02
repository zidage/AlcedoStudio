//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_task_port.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostInteractionPolicyTests = ApplicationModuleHostTestFixture;

auto Target(uint64_t elementId) -> QVariantMap {
  QVariantMap m;
  m.insert(QStringLiteral("elementId"), static_cast<qulonglong>(elementId));
  return m;
}

auto Targets(std::initializer_list<uint64_t> ids) -> QVariantList {
  QVariantList out;
  for (uint64_t id : ids) {
    out.append(Target(id));
  }
  return out;
}

auto Lock(InteractionCapability cap, uint64_t eid, const QString& reason) -> InteractionLock {
  return InteractionLock{cap, static_cast<quint64>(eid), reason};
}

// Register a task of `kind` holding `locks` and return its id.
auto RegisterLockedTask(BackgroundTaskController& registry, BackgroundTaskKind kind,
                        const std::vector<InteractionLock>& locks) -> QString {
  BackgroundTaskSnapshot s;
  s.kind_            = kind;
  s.state_           = BackgroundTaskState::Running;
  s.title_           = QStringLiteral("test");
  s.cancelable_      = false;
  s.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  s.locks_           = locks;
  return registry.RegisterTask(s);
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, NoTasks_AllCapabilitiesAllowed) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  EXPECT_TRUE(policy.EvaluateEditImageDescription(42).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateEditImageRating(42).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateEditImageRatingReason(42).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateRunImageAnalysis(Targets({42})).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateDeleteImages(Targets({42})).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateChangeSemanticModel().value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateRunSemanticGeneration().value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateChangeModelDownloadSettings().value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateChangeImageAnalysisProvider().value("allowed").toBool());
  // Cached Q_PROPERTYs likewise true.
  policy.SetFocusedElementId(42);
  EXPECT_TRUE(policy.CanEditFocusedDescription());
  EXPECT_TRUE(policy.CanEditFocusedRating());
  EXPECT_TRUE(policy.CanEditFocusedRatingReason());
  EXPECT_TRUE(policy.CanDeletePendingTargets());
  EXPECT_TRUE(policy.CanRunAnalysis());
  EXPECT_TRUE(policy.CanChangeSemanticModel());
  EXPECT_TRUE(policy.CanRunSemanticGeneration());
  EXPECT_TRUE(policy.CanChangeModelDownloadSettings());
  EXPECT_TRUE(policy.CanChangeImageAnalysisProvider());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, ImageAnalysisPerElementLocks_BlockAffectedOnly) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  const QString               id = RegisterLockedTask(
      registry, BackgroundTaskKind::ImageAnalysis,
      {
          Lock(InteractionCapability::EditImageDescription, 42, QStringLiteral("analyzing")),
          Lock(InteractionCapability::EditImageRating, 42, QStringLiteral("analyzing")),
          Lock(InteractionCapability::EditImageRatingReason, 42, QStringLiteral("analyzing")),
          Lock(InteractionCapability::RunImageAnalysis, 42, QStringLiteral("rerun")),
          Lock(InteractionCapability::DeleteImages, 42, QStringLiteral("no delete")),
          Lock(InteractionCapability::ChangeImageAnalysisProvider, 0,
                             QStringLiteral("provider locked")),
          Lock(InteractionCapability::ChangeSemanticModel, 0, QStringLiteral("model locked")),
          Lock(InteractionCapability::ChangeModelDownloadSettings, 0,
                             QStringLiteral("model files locked")),
      });

  // Affected image 42 is blocked across the image-edit + run + delete caps.
  const QVariantMap desc42 = policy.EvaluateEditImageDescription(42);
  EXPECT_FALSE(desc42.value("allowed").toBool());
  EXPECT_FALSE(desc42.value("reason").toString().isEmpty());
  const QStringList blocking = desc42.value("blockingTaskIds").toStringList();
  EXPECT_TRUE(blocking.contains(id));

  EXPECT_FALSE(policy.EvaluateEditImageRating(42).value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateEditImageRatingReason(42).value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateDeleteImages(Targets({42})).value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateRunImageAnalysis(Targets({42})).value("allowed").toBool());
  // Unrelated image 99 is unaffected for the per-element caps.
  EXPECT_TRUE(policy.EvaluateEditImageDescription(99).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateEditImageRating(99).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateDeleteImages(Targets({99})).value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateRunImageAnalysis(Targets({99})).value("allowed").toBool());
  // Snapshot-setting caps are global, so they are blocked regardless of element.
  EXPECT_FALSE(policy.EvaluateChangeImageAnalysisProvider().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeSemanticModel().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeModelDownloadSettings().value("allowed").toBool());
  // A semantic run may still reuse the already-running sidecar; it is not a
  // startup-snapshot mutation.
  EXPECT_TRUE(policy.EvaluateRunSemanticGeneration().value("allowed").toBool());

  // Cached focused-image gates track the focused element id.
  policy.SetFocusedElementId(42);
  EXPECT_FALSE(policy.CanEditFocusedDescription());
  EXPECT_FALSE(policy.CanEditFocusedRating());
  EXPECT_FALSE(policy.CanEditFocusedRatingReason());
  EXPECT_FALSE(policy.FocusedEditReason().isEmpty());
  policy.SetFocusedElementId(99);
  EXPECT_TRUE(policy.CanEditFocusedDescription());
  EXPECT_TRUE(policy.CanEditFocusedRating());
  EXPECT_TRUE(policy.CanEditFocusedRatingReason());
  EXPECT_TRUE(policy.FocusedEditReason().isEmpty());

  // Pending delete / analysis target gates.
  policy.SetPendingDeleteTargets(Targets({42}));
  EXPECT_FALSE(policy.CanDeletePendingTargets());
  EXPECT_FALSE(policy.PendingDeleteReason().isEmpty());
  policy.SetPendingDeleteTargets(Targets({99}));
  EXPECT_TRUE(policy.CanDeletePendingTargets());

  policy.SetPendingAnalysisTargets(Targets({42}));
  EXPECT_FALSE(policy.CanRunAnalysis());
  EXPECT_FALSE(policy.RunAnalysisReason().isEmpty());
  policy.SetPendingAnalysisTargets(Targets({99}));
  EXPECT_TRUE(policy.CanRunAnalysis());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, GlobalLock_BlocksEveryElement) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  RegisterLockedTask(
      registry, BackgroundTaskKind::ImageAnalysis,
      {
          Lock(InteractionCapability::DeleteImages, 0, QStringLiteral("album delete locked")),
          Lock(InteractionCapability::RunImageAnalysis, 0, QStringLiteral("album run locked")),
      });
  // A global (element_id == 0) lock blocks every element, not just 0.
  EXPECT_FALSE(policy.EvaluateDeleteImages(Targets({99})).value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateRunImageAnalysis(Targets({7})).value("allowed").toBool());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, SemanticGenerationLocks_BlockModelAndGeneration) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  RegisterLockedTask(
      registry, BackgroundTaskKind::SemanticGeneration,
      {
          Lock(InteractionCapability::ChangeSemanticModel, 0, QStringLiteral("model")),
          Lock(InteractionCapability::RunSemanticGeneration, 0, QStringLiteral("gen")),
          Lock(InteractionCapability::ChangeModelDownloadSettings, 0, QStringLiteral("dl")),
          Lock(InteractionCapability::ChangeImageAnalysisProvider, 0, QStringLiteral("provider")),
      });
  EXPECT_FALSE(policy.EvaluateChangeSemanticModel().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateRunSemanticGeneration().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeModelDownloadSettings().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeImageAnalysisProvider().value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateRunImageAnalysis(Targets({42})).value("allowed").toBool());
  EXPECT_FALSE(policy.CanChangeSemanticModel());
  EXPECT_FALSE(policy.CanRunSemanticGeneration());
  EXPECT_FALSE(policy.CanChangeModelDownloadSettings());
  // Image edits are unaffected by a semantic-generation run.
  EXPECT_TRUE(policy.EvaluateEditImageDescription(42).value("allowed").toBool());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, ModelDownloadLocks_BlockSettingsAndModelNotGeneration) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  RegisterLockedTask(
      registry, BackgroundTaskKind::ModelDownload,
      {
          Lock(InteractionCapability::ChangeModelDownloadSettings, 0, QStringLiteral("dl")),
          Lock(InteractionCapability::ChangeSemanticModel, 0, QStringLiteral("model")),
      });
  EXPECT_FALSE(policy.EvaluateChangeModelDownloadSettings().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeSemanticModel().value("allowed").toBool());
  // Download does NOT block generation.
  EXPECT_TRUE(policy.EvaluateRunSemanticGeneration().value("allowed").toBool());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, ModelActivationLocks_BlockAllThree) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  RegisterLockedTask(
      registry, BackgroundTaskKind::ModelActivation,
      {
          Lock(InteractionCapability::ChangeSemanticModel, 0, QStringLiteral("model")),
          Lock(InteractionCapability::RunSemanticGeneration, 0, QStringLiteral("gen")),
          Lock(InteractionCapability::ChangeModelDownloadSettings, 0, QStringLiteral("dl")),
          Lock(InteractionCapability::ChangeImageAnalysisProvider, 0, QStringLiteral("provider")),
      });
  EXPECT_FALSE(policy.EvaluateChangeSemanticModel().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateRunSemanticGeneration().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeModelDownloadSettings().value("allowed").toBool());
  EXPECT_FALSE(policy.EvaluateChangeImageAnalysisProvider().value("allowed").toBool());
  EXPECT_TRUE(policy.EvaluateRunImageAnalysis(Targets({42})).value("allowed").toBool());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, FinishTask_ClearsLocks) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  const QString               id = RegisterLockedTask(
      registry, BackgroundTaskKind::ImageAnalysis,
      {Lock(InteractionCapability::EditImageDescription, 42, QStringLiteral("analyzing"))});
  policy.SetFocusedElementId(42);
  EXPECT_FALSE(policy.CanEditFocusedDescription());
  registry.FinishTask(id, BackgroundTaskState::Succeeded);
  EXPECT_TRUE(policy.CanEditFocusedDescription());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests,
       EditorSaveLocksDisableFilmstripWorkspaceCheckoutAndPasteWithReason) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  const QString               reason = QStringLiteral("Saving editor changes");
  RegisterLockedTask(registry, BackgroundTaskKind::EditorSave,
                     {
                         Lock(InteractionCapability::SelectEditorImage, 0, reason),
                         Lock(InteractionCapability::SwitchWorkspace, 0, reason),
                         Lock(InteractionCapability::CheckoutVersion, 0, reason),
                         Lock(InteractionCapability::PasteAdjustments, 0, reason),
                     });

  EXPECT_FALSE(policy.CanSelectEditorImage());
  EXPECT_FALSE(policy.CanSwitchWorkspace());
  EXPECT_FALSE(policy.CanCheckoutVersion());
  EXPECT_FALSE(policy.CanPasteAdjustments());
  EXPECT_EQ(policy.SelectEditorImageReason(), reason);
  EXPECT_EQ(policy.SwitchWorkspaceReason(), reason);
  EXPECT_EQ(policy.CheckoutVersionReason(), reason);
  EXPECT_EQ(policy.PasteAdjustmentsReason(), reason);
  EXPECT_EQ(policy.EvaluateSelectEditorImage().value("reason").toString(), reason);
  EXPECT_EQ(policy.EvaluateSwitchWorkspace().value("reason").toString(), reason);
  EXPECT_EQ(policy.EvaluateCheckoutVersion().value("reason").toString(), reason);
  EXPECT_EQ(policy.EvaluatePasteAdjustments().value("reason").toString(), reason);
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, PolicyChanged_FiresOnlyOnLockSetChange) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  QSignalSpy                  spy(&policy, &InteractionPolicyController::PolicyChanged);
  const QString               id = RegisterLockedTask(
      registry, BackgroundTaskKind::ImageAnalysis,
      {Lock(InteractionCapability::EditImageDescription, 42, QStringLiteral("analyzing"))});
  const int after_register = spy.count();
  EXPECT_GE(after_register, 1);
  // A progress tick does not change the lock set, so PolicyChanged must NOT fire.
  registry.UpdateTask(id, QStringLiteral("working"), QStringLiteral("d"), 42);
  EXPECT_EQ(spy.count(), after_register);
  // Finishing the task clears the lock set → PolicyChanged fires.
  registry.FinishTask(id, BackgroundTaskState::Succeeded);
  EXPECT_GT(spy.count(), after_register);
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, BlockingTaskIds_AggregatesAcrossTasks) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  const QString               a = RegisterLockedTask(
      registry, BackgroundTaskKind::SemanticGeneration,
      {Lock(InteractionCapability::ChangeSemanticModel, 0, QStringLiteral("a"))});
  const QString b = RegisterLockedTask(
      registry, BackgroundTaskKind::ModelActivation,
      {Lock(InteractionCapability::ChangeSemanticModel, 0, QStringLiteral("b"))});
  const QStringList ids =
      policy.EvaluateChangeSemanticModel().value("blockingTaskIds").toStringList();
  EXPECT_TRUE(ids.contains(a));
  EXPECT_TRUE(ids.contains(b));
  EXPECT_EQ(ids.size(), 2);
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, NullRegistry_PolicyStaysOpen) {
  InteractionPolicyController policy(nullptr);
  // No registry → no locks → everything allowed, no PolicyChanged expected.
  EXPECT_TRUE(policy.CanEditFocusedDescription());
  EXPECT_TRUE(policy.CanChangeSemanticModel());
  policy.SetFocusedElementId(42);
  EXPECT_TRUE(policy.CanEditFocusedDescription());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests, NaturalLanguageSearchGate_DisablesFieldFilters) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  QSignalSpy                  spy(&policy, &InteractionPolicyController::PolicyChanged);
  // Default: NL off → field filters changeable, no reason surfaced.
  EXPECT_TRUE(policy.CanChangeSearchFieldFilters());
  EXPECT_TRUE(policy.SearchFieldFiltersReason().isEmpty());
  // Turn NL on → field filters disabled with a reason, PolicyChanged fires.
  // This gate is UI-only (no task lock); it is the documented exception to the
  // locks-only model — see the interaction_policy_controller header comment.
  policy.SetNaturalLanguageSearchEnabled(true);
  EXPECT_FALSE(policy.CanChangeSearchFieldFilters());
  EXPECT_FALSE(policy.SearchFieldFiltersReason().isEmpty());
  EXPECT_GT(spy.count(), 0);
  // Idempotent: setting the same value again does not notify.
  const int after_first = spy.count();
  policy.SetNaturalLanguageSearchEnabled(true);
  EXPECT_EQ(spy.count(), after_first);
  // Turn NL off → field filters changeable again, reason clears.
  policy.SetNaturalLanguageSearchEnabled(false);
  EXPECT_TRUE(policy.CanChangeSearchFieldFilters());
  EXPECT_TRUE(policy.SearchFieldFiltersReason().isEmpty());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests,
       ProductionEditorSaveTaskPublishesAndClearsFourCheckpointLocks) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  EditorSessionTaskPort       task_port(&registry);
  constexpr uint64_t          kElementA = 101;
  const QString               expected_reason = QStringLiteral("Saving editor changes");

  // ── Begin task publishes all four checkpoint locks ──
  const auto task_id = task_port.BeginTask("editor_save", kElementA);
  EXPECT_NE(task_id, 0u);

  EXPECT_FALSE(policy.CanSelectEditorImage());
  EXPECT_FALSE(policy.CanSwitchWorkspace());
  EXPECT_FALSE(policy.CanCheckoutVersion());
  EXPECT_FALSE(policy.CanPasteAdjustments());
  EXPECT_EQ(policy.SelectEditorImageReason(), expected_reason);
  EXPECT_EQ(policy.SwitchWorkspaceReason(), expected_reason);
  EXPECT_EQ(policy.CheckoutVersionReason(), expected_reason);
  EXPECT_EQ(policy.PasteAdjustmentsReason(), expected_reason);

  // Non-checkpoint capabilities remain enabled.
  EXPECT_TRUE(policy.CanChangeSemanticModel());
  EXPECT_TRUE(policy.CanRunSemanticGeneration());

  // ── End task as success clears all four locks ──
  task_port.EndTask(task_id, true, "");
  EXPECT_TRUE(policy.CanSelectEditorImage());
  EXPECT_TRUE(policy.CanSwitchWorkspace());
  EXPECT_TRUE(policy.CanCheckoutVersion());
  EXPECT_TRUE(policy.CanPasteAdjustments());

  // ── Failure also clears locks ──
  const auto task2 = task_port.BeginTask("editor_save", kElementA);
  EXPECT_FALSE(policy.CanSelectEditorImage());
  task_port.EndTask(task2, false, "save error");
  EXPECT_TRUE(policy.CanSelectEditorImage());
  EXPECT_TRUE(policy.CanSwitchWorkspace());
  EXPECT_TRUE(policy.CanCheckoutVersion());
  EXPECT_TRUE(policy.CanPasteAdjustments());

  // ── Cancellation (task removed from registry) clears locks ──
  task_port.BeginTask("editor_save", kElementA);
  EXPECT_FALSE(policy.CanSelectEditorImage());
  const auto ui_id = registry.ActiveLocks().front().task_id_;
  EXPECT_FALSE(ui_id.isEmpty());
  registry.FinishTask(ui_id, BackgroundTaskState::Canceled);
  EXPECT_TRUE(policy.CanSelectEditorImage());
}

TEST_F(ApplicationModuleHostInteractionPolicyTests,
       VersionCheckoutLockDoesNotDisableHistoryBrowsing) {
  BackgroundTaskController    registry;
  InteractionPolicyController policy(&registry);
  const QString               reason = QStringLiteral("Saving editor changes");
  RegisterLockedTask(registry, BackgroundTaskKind::EditorSave,
                     {Lock(InteractionCapability::CheckoutVersion, 0, reason)});

  // Only CheckoutVersion is locked; all other editor capabilities remain available.
  EXPECT_FALSE(policy.CanCheckoutVersion());
  EXPECT_EQ(policy.CheckoutVersionReason(), reason);

  EXPECT_TRUE(policy.CanSelectEditorImage());
  EXPECT_TRUE(policy.CanSwitchWorkspace());
  EXPECT_TRUE(policy.CanPasteAdjustments());

  // General editor capabilities are not affected by a CheckoutVersion-only lock.
  policy.SetFocusedElementId(42);
  EXPECT_TRUE(policy.CanEditFocusedDescription());
  EXPECT_TRUE(policy.CanEditFocusedRating());
  EXPECT_TRUE(policy.CanEditFocusedRatingReason());
  EXPECT_TRUE(policy.CanChangeSemanticModel());
  EXPECT_TRUE(policy.CanRunSemanticGeneration());
}

}  // namespace
}  // namespace alcedo::ui::test
