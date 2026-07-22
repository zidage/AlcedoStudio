//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <vector>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {

/// Phase 2 — the QML-facing computed-state controller that answers "can this
/// control run right now, and if not, why?". It consumes the interaction locks
/// published by active background tasks (via `BackgroundTaskController::ActiveLocks`)
/// plus a small set of QML-pushed inputs (the currently focused image element id
/// and the pending delete / analysis target lists) and exposes both Q_INVOKABLE
/// one-shot queries and cached Q_PROPERTY booleans for `enabled:` bindings.
///
/// Like `BackgroundTaskController`, this is a standalone QObject with no host
/// dependency so it is unit-testable with just a `BackgroundTaskController`
/// (tests register snapshots with `locks_` directly).
///
/// Threading: all register/query activity happens on the UI thread
/// (`BackgroundTaskController::TasksChanged` is emitted there and the QML inputs
/// are set from QML bindings), so the cached lock vector needs no mutex.
///
/// Churn control: `BackgroundTaskController` emits `TasksChanged` on every
/// progress tick, but progress ticks never change the lock set (`UpdateTask` does
/// not touch `locks_`). `RebuildIndex()` therefore only emits `PolicyChanged`
/// when the active lock set actually changes, so `enabled:` bindings do not
/// re-evaluate on every 250 ms download tick.
class InteractionPolicyController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(uint focusedElementId READ FocusedElementId WRITE SetFocusedElementId
                 NOTIFY PolicyChanged)
  Q_PROPERTY(QVariantList pendingDeleteTargets READ PendingDeleteTargets WRITE
                 SetPendingDeleteTargets NOTIFY PolicyChanged)
  Q_PROPERTY(QVariantList pendingAnalysisTargets READ PendingAnalysisTargets WRITE
                 SetPendingAnalysisTargets NOTIFY PolicyChanged)
  // Search-settings natural-language toggle (UI mutual-exclusion input). When
  // true the search field-filter checkboxes are disabled (see
  // CanChangeSearchFieldFilters). This is a UI-only gate — it does NOT go
  // through the task-lock model (no InteractionCapability, no lock); it is the
  // documented exception to the locks-only design because the
  // natural-language / field-filter exclusion is a pure UI concern, not a
  // background-task conflict.
  Q_PROPERTY(bool naturalLanguageSearchEnabled READ NaturalLanguageSearchEnabled WRITE
                 SetNaturalLanguageSearchEnabled NOTIFY PolicyChanged)

  // Focused-image edit gates (depend on focusedElementId + locks).
  Q_PROPERTY(bool canEditFocusedDescription READ CanEditFocusedDescription NOTIFY PolicyChanged)
  Q_PROPERTY(bool canEditFocusedRating READ CanEditFocusedRating NOTIFY PolicyChanged)
  Q_PROPERTY(bool canEditFocusedRatingReason READ CanEditFocusedRatingReason NOTIFY PolicyChanged)
  Q_PROPERTY(QString focusedEditReason READ FocusedEditReason NOTIFY PolicyChanged)
  // Pending delete gate (depends on pendingDeleteTargets + locks).
  Q_PROPERTY(bool canDeletePendingTargets READ CanDeletePendingTargets NOTIFY PolicyChanged)
  Q_PROPERTY(QString pendingDeleteReason READ PendingDeleteReason NOTIFY PolicyChanged)
  // Pending analysis-run gate (depends on pendingAnalysisTargets + locks).
  Q_PROPERTY(bool canRunAnalysis READ CanRunAnalysis NOTIFY PolicyChanged)
  Q_PROPERTY(QString runAnalysisReason READ RunAnalysisReason NOTIFY PolicyChanged)
  // Global task gates.
  Q_PROPERTY(bool canChangeSemanticModel READ CanChangeSemanticModel NOTIFY PolicyChanged)
  Q_PROPERTY(bool canRunSemanticGeneration READ CanRunSemanticGeneration NOTIFY PolicyChanged)
  Q_PROPERTY(bool canChangeModelDownloadSettings READ CanChangeModelDownloadSettings NOTIFY
                 PolicyChanged)
  Q_PROPERTY(bool canChangeImageAnalysisProvider READ CanChangeImageAnalysisProvider NOTIFY
                 PolicyChanged)
  // Search field-filter gate (depends on naturalLanguageSearchEnabled only).
  Q_PROPERTY(bool canChangeSearchFieldFilters READ CanChangeSearchFieldFilters NOTIFY
                 PolicyChanged)
  Q_PROPERTY(QString searchFieldFiltersReason READ SearchFieldFiltersReason NOTIFY PolicyChanged)
  // Phase 6C-5: editor navigation gates while a global save checkpoint holds locks.
  Q_PROPERTY(bool canSelectEditorImage READ CanSelectEditorImage NOTIFY PolicyChanged)
  Q_PROPERTY(QString selectEditorImageReason READ SelectEditorImageReason NOTIFY PolicyChanged)
  Q_PROPERTY(bool canSwitchWorkspace READ CanSwitchWorkspace NOTIFY PolicyChanged)
  Q_PROPERTY(QString switchWorkspaceReason READ SwitchWorkspaceReason NOTIFY PolicyChanged)
  Q_PROPERTY(bool canCheckoutVersion READ CanCheckoutVersion NOTIFY PolicyChanged)
  Q_PROPERTY(QString checkoutVersionReason READ CheckoutVersionReason NOTIFY PolicyChanged)
  Q_PROPERTY(bool canPasteAdjustments READ CanPasteAdjustments NOTIFY PolicyChanged)
  Q_PROPERTY(QString pasteAdjustmentsReason READ PasteAdjustmentsReason NOTIFY PolicyChanged)
  Q_PROPERTY(bool canMergeAdjustments READ CanMergeAdjustments NOTIFY PolicyChanged)
  Q_PROPERTY(QString mergeAdjustmentsReason READ MergeAdjustmentsReason NOTIFY PolicyChanged)

 public:
  explicit InteractionPolicyController(BackgroundTaskController* registry = nullptr,
                                       QObject*                  parent   = nullptr);

  // ── Settable inputs ────────────────────────────────────────────────────
  uint             FocusedElementId() const { return focused_element_id_; }
  void             SetFocusedElementId(uint elementId);
  QVariantList     PendingDeleteTargets() const { return pending_delete_targets_; }
  void             SetPendingDeleteTargets(const QVariantList& targets);
  QVariantList     PendingAnalysisTargets() const { return pending_analysis_targets_; }
  void             SetPendingAnalysisTargets(const QVariantList& targets);
  bool             NaturalLanguageSearchEnabled() const { return natural_language_search_enabled_; }
  void             SetNaturalLanguageSearchEnabled(bool enabled);

  // ── Cached output Q_PROPERTYs (for `enabled:` bindings) ───────────────
  bool    CanEditFocusedDescription() const;
  bool    CanEditFocusedRating() const;
  bool    CanEditFocusedRatingReason() const;
  QString FocusedEditReason() const;
  bool    CanDeletePendingTargets() const;
  QString PendingDeleteReason() const;
  bool    CanRunAnalysis() const;
  QString RunAnalysisReason() const;
  bool    CanChangeSemanticModel() const;
  bool    CanRunSemanticGeneration() const;
  bool    CanChangeModelDownloadSettings() const;
  bool    CanChangeImageAnalysisProvider() const;
  bool    CanChangeSearchFieldFilters() const;
  QString SearchFieldFiltersReason() const;
  bool    CanSelectEditorImage() const;
  QString SelectEditorImageReason() const;
  bool    CanSwitchWorkspace() const;
  QString SwitchWorkspaceReason() const;
  bool    CanCheckoutVersion() const;
  QString CheckoutVersionReason() const;
  bool    CanPasteAdjustments() const;
  QString PasteAdjustmentsReason() const;
  bool    CanMergeAdjustments() const;
  QString MergeAdjustmentsReason() const;

  // ── Q_INVOKABLE one-shot queries (return {allowed, reason, blockingTaskIds}) ─
  // Named `Evaluate*` (not `Can*`) so they do not collide with the no-arg
  // Q_PROPERTY bool getters (`CanChangeSemanticModel()` etc.) — same name with
  // no parameters and a different return type would be an illegal overload.
  Q_INVOKABLE QVariantMap EvaluateEditImageDescription(uint elementId) const;
  Q_INVOKABLE QVariantMap EvaluateEditImageRating(uint elementId) const;
  Q_INVOKABLE QVariantMap EvaluateEditImageRatingReason(uint elementId) const;
  Q_INVOKABLE QVariantMap EvaluateRunImageAnalysis(const QVariantList& targets) const;
  Q_INVOKABLE QVariantMap EvaluateDeleteImages(const QVariantList& targets) const;
  Q_INVOKABLE QVariantMap EvaluateChangeSemanticModel() const;
  Q_INVOKABLE QVariantMap EvaluateRunSemanticGeneration() const;
  Q_INVOKABLE QVariantMap EvaluateChangeModelDownloadSettings() const;
  Q_INVOKABLE QVariantMap EvaluateChangeImageAnalysisProvider() const;
  Q_INVOKABLE QVariantMap EvaluateSelectEditorImage() const;
  Q_INVOKABLE QVariantMap EvaluateSwitchWorkspace() const;
  Q_INVOKABLE QVariantMap EvaluateCheckoutVersion() const;
  Q_INVOKABLE QVariantMap EvaluatePasteAdjustments() const;
  Q_INVOKABLE QVariantMap EvaluateMergeAdjustments() const;

 signals:
  void PolicyChanged();

 private:
  // Internal answer: allowed + the first matching lock's reason + the task ids
  // holding matching locks. Trailing-underscore members per the project's
  // clang-tidy `PublicMemberSuffix:"_"` rule.
  struct Answer {
    bool        allowed_             = true;
    QString     reason_;
    QStringList blocking_task_ids_;
  };
  // Single element-scoped capability (e.g. EditImageDescription for one image).
  Answer Eval(InteractionCapability capability, quint64 elementId) const;
  // Global capability (blocks if any lock with this capability is held).
  Answer EvalGlobal(InteractionCapability capability) const;
  // Target-list capability (RunImageAnalysis / DeleteImages): blocked if ANY
  // target elementId is covered by a matching lock.
  Answer EvalTargets(InteractionCapability  capability,
                    const QVariantList&    targets) const;
  static auto ToVariantMap(const Answer& answer) -> QVariantMap;
  static auto LocksEqual(const std::vector<ActiveLock>& a,
                         const std::vector<ActiveLock>& b) -> bool;
  void RebuildIndex();

  BackgroundTaskController*      registry_ = nullptr;
  std::vector<ActiveLock>        locks_;
  uint                           focused_element_id_      = 0;
  QVariantList                   pending_delete_targets_  = {};
  QVariantList                   pending_analysis_targets_ = {};
  bool                           natural_language_search_enabled_ = false;
};

}  // namespace alcedo::ui