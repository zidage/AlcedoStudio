//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"

#include <QSet>
#include <set>
#include <utility>

#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

namespace {
// Parse the {elementId, imageId,...} target-entry maps QML pushes into a set of
// element ids. Entries with elementId == 0 are skipped. Used by the target-list
// capabilities (RunImageAnalysis, DeleteImages).
auto TargetElementIds(const QVariantList& targets) -> std::set<quint64> {
  std::set<quint64> ids;
  for (const QVariant& entry : targets) {
    const quint64 id = entry.toMap().value(QStringLiteral("elementId")).toULongLong();
    if (id != 0) {
      ids.insert(id);
    }
  }
  return ids;
}
}  // namespace

InteractionPolicyController::InteractionPolicyController(BackgroundTaskController* registry,
                                                         QObject*                  parent)
    : QObject(parent), registry_(registry) {
  if (registry_) {
    connect(registry_, &BackgroundTaskController::TasksChanged, this,
            [this] { RebuildIndex(); });
    RebuildIndex();  // seed the initial lock set
  }
}

// ── Settable inputs ───────────────────────────────────────────────────────
void InteractionPolicyController::SetFocusedElementId(uint elementId) {
  if (focused_element_id_ == elementId) {
    return;
  }
  focused_element_id_ = elementId;
  emit PolicyChanged();
}

void InteractionPolicyController::SetPendingDeleteTargets(const QVariantList& targets) {
  if (pending_delete_targets_ == targets) {
    return;
  }
  pending_delete_targets_ = targets;
  emit PolicyChanged();
}

void InteractionPolicyController::SetPendingAnalysisTargets(const QVariantList& targets) {
  if (pending_analysis_targets_ == targets) {
    return;
  }
  pending_analysis_targets_ = targets;
  emit PolicyChanged();
}

void InteractionPolicyController::SetNaturalLanguageSearchEnabled(bool enabled) {
  if (natural_language_search_enabled_ == enabled) {
    return;
  }
  natural_language_search_enabled_ = enabled;
  emit PolicyChanged();
}

// ── Internal match logic ──────────────────────────────────────────────────
InteractionPolicyController::Answer InteractionPolicyController::Eval(
    InteractionCapability capability, quint64 elementId) const {
  Answer        out;
  QSet<QString> seen;
  for (const auto& active : locks_) {
    if (active.lock_.capability_ != capability) {
      continue;
    }
    // A global lock (element_id_ == 0) blocks every element; otherwise it must
    // match the queried element exactly.
    const bool matches =
        active.lock_.element_id_ == 0 || active.lock_.element_id_ == elementId;
    if (!matches) {
      continue;
    }
    out.allowed_ = false;
    if (out.reason_.isEmpty()) {
      out.reason_ = active.lock_.reason_;
    }
    if (!seen.contains(active.task_id_)) {
      seen.insert(active.task_id_);
      out.blocking_task_ids_.append(active.task_id_);
    }
  }
  return out;
}

InteractionPolicyController::Answer InteractionPolicyController::EvalGlobal(
    InteractionCapability capability) const {
  Answer        out;
  QSet<QString> seen;
  for (const auto& active : locks_) {
    if (active.lock_.capability_ != capability) {
      continue;
    }
    out.allowed_ = false;
    if (out.reason_.isEmpty()) {
      out.reason_ = active.lock_.reason_;
    }
    if (!seen.contains(active.task_id_)) {
      seen.insert(active.task_id_);
      out.blocking_task_ids_.append(active.task_id_);
    }
  }
  return out;
}

InteractionPolicyController::Answer InteractionPolicyController::EvalTargets(
    InteractionCapability  capability,
    const QVariantList&    targets) const {
  Answer out;
  if (targets.isEmpty()) {
    return out;  // nothing to block; the caller gates on non-empty targets too.
  }
  const auto    eids = TargetElementIds(targets);
  QSet<QString> seen;
  for (const auto& active : locks_) {
    if (active.lock_.capability_ != capability) {
      continue;
    }
    // A global lock blocks any target; a per-element lock blocks only its own.
    const bool matches =
        active.lock_.element_id_ == 0 || eids.count(active.lock_.element_id_) > 0;
    if (!matches) {
      continue;
    }
    out.allowed_ = false;
    if (out.reason_.isEmpty()) {
      out.reason_ = active.lock_.reason_;
    }
    if (!seen.contains(active.task_id_)) {
      seen.insert(active.task_id_);
      out.blocking_task_ids_.append(active.task_id_);
    }
  }
  return out;
}

auto InteractionPolicyController::ToVariantMap(const Answer& answer) -> QVariantMap {
  QVariantMap m;
  m.insert(QStringLiteral("allowed"), answer.allowed_);
  m.insert(QStringLiteral("reason"), answer.reason_);
  m.insert(QStringLiteral("blockingTaskIds"), QVariant::fromValue(answer.blocking_task_ids_));
  return m;
}

auto InteractionPolicyController::LocksEqual(const std::vector<ActiveLock>& a,
                                             const std::vector<ActiveLock>& b) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].task_id_ != b[i].task_id_ ||
        a[i].lock_.capability_ != b[i].lock_.capability_ ||
        a[i].lock_.element_id_ != b[i].lock_.element_id_ ||
        a[i].lock_.reason_ != b[i].lock_.reason_) {
      return false;
    }
  }
  return true;
}

void InteractionPolicyController::RebuildIndex() {
  if (!registry_) {
    return;
  }
  auto fresh = registry_->ActiveLocks();
  if (LocksEqual(locks_, fresh)) {
    return;
  }
  locks_ = std::move(fresh);
  emit PolicyChanged();
}

// ── Cached output Q_PROPERTYs ─────────────────────────────────────────────
bool InteractionPolicyController::CanEditFocusedDescription() const {
  return Eval(InteractionCapability::EditImageDescription, focused_element_id_).allowed_;
}

bool InteractionPolicyController::CanEditFocusedRating() const {
  return Eval(InteractionCapability::EditImageRating, focused_element_id_).allowed_;
}

bool InteractionPolicyController::CanEditFocusedRatingReason() const {
  return Eval(InteractionCapability::EditImageRatingReason, focused_element_id_).allowed_;
}

QString InteractionPolicyController::FocusedEditReason() const {
  // Surface the first blocked focused-edit reason so the inspector shows one
  // coherent "why is this disabled?" hint.
  auto a = Eval(InteractionCapability::EditImageDescription, focused_element_id_);
  if (!a.allowed_) {
    return a.reason_;
  }
  a = Eval(InteractionCapability::EditImageRating, focused_element_id_);
  if (!a.allowed_) {
    return a.reason_;
  }
  a = Eval(InteractionCapability::EditImageRatingReason, focused_element_id_);
  if (!a.allowed_) {
    return a.reason_;
  }
  return {};
}

bool InteractionPolicyController::CanDeletePendingTargets() const {
  return EvalTargets(InteractionCapability::DeleteImages, pending_delete_targets_).allowed_;
}

QString InteractionPolicyController::PendingDeleteReason() const {
  return EvalTargets(InteractionCapability::DeleteImages, pending_delete_targets_).reason_;
}

bool InteractionPolicyController::CanRunAnalysis() const {
  return EvalTargets(InteractionCapability::RunImageAnalysis, pending_analysis_targets_).allowed_;
}

QString InteractionPolicyController::RunAnalysisReason() const {
  return EvalTargets(InteractionCapability::RunImageAnalysis, pending_analysis_targets_).reason_;
}

bool InteractionPolicyController::CanChangeSemanticModel() const {
  return EvalGlobal(InteractionCapability::ChangeSemanticModel).allowed_;
}

bool InteractionPolicyController::CanRunSemanticGeneration() const {
  return EvalGlobal(InteractionCapability::RunSemanticGeneration).allowed_;
}

bool InteractionPolicyController::CanChangeModelDownloadSettings() const {
  return EvalGlobal(InteractionCapability::ChangeModelDownloadSettings).allowed_;
}

bool InteractionPolicyController::CanChangeImageAnalysisProvider() const {
  return EvalGlobal(InteractionCapability::ChangeImageAnalysisProvider).allowed_;
}

bool InteractionPolicyController::CanChangeSearchFieldFilters() const {
  // UI-only mutual-exclusion gate: natural-language search and field filters
  // cannot be active together. This does NOT consult the task-lock model (no
  // InteractionCapability::ChangeSearchFieldFilters); the only input is the
  // QML-pushed natural-language toggle. See the header comment.
  return !natural_language_search_enabled_;
}

QString InteractionPolicyController::SearchFieldFiltersReason() const {
  if (natural_language_search_enabled_) {
    return Tr("Natural language search is enabled — field filters are disabled.");
  }
  return {};
}

bool InteractionPolicyController::CanSelectEditorImage() const {
  return EvalGlobal(InteractionCapability::SelectEditorImage).allowed_;
}

QString InteractionPolicyController::SelectEditorImageReason() const {
  return EvalGlobal(InteractionCapability::SelectEditorImage).reason_;
}

bool InteractionPolicyController::CanSwitchWorkspace() const {
  return EvalGlobal(InteractionCapability::SwitchWorkspace).allowed_;
}

QString InteractionPolicyController::SwitchWorkspaceReason() const {
  return EvalGlobal(InteractionCapability::SwitchWorkspace).reason_;
}

bool InteractionPolicyController::CanCheckoutVersion() const {
  return EvalGlobal(InteractionCapability::CheckoutVersion).allowed_;
}

QString InteractionPolicyController::CheckoutVersionReason() const {
  return EvalGlobal(InteractionCapability::CheckoutVersion).reason_;
}

bool InteractionPolicyController::CanPasteAdjustments() const {
  return EvalGlobal(InteractionCapability::PasteAdjustments).allowed_;
}

QString InteractionPolicyController::PasteAdjustmentsReason() const {
  return EvalGlobal(InteractionCapability::PasteAdjustments).reason_;
}

// ── Q_INVOKABLE one-shot queries (full {allowed, reason, blockingTaskIds} map) ─
QVariantMap InteractionPolicyController::EvaluateEditImageDescription(uint elementId) const {
  return ToVariantMap(Eval(InteractionCapability::EditImageDescription, elementId));
}

QVariantMap InteractionPolicyController::EvaluateEditImageRating(uint elementId) const {
  return ToVariantMap(Eval(InteractionCapability::EditImageRating, elementId));
}

QVariantMap InteractionPolicyController::EvaluateEditImageRatingReason(uint elementId) const {
  return ToVariantMap(Eval(InteractionCapability::EditImageRatingReason, elementId));
}

QVariantMap InteractionPolicyController::EvaluateRunImageAnalysis(
    const QVariantList& targets) const {
  return ToVariantMap(EvalTargets(InteractionCapability::RunImageAnalysis, targets));
}

QVariantMap InteractionPolicyController::EvaluateDeleteImages(const QVariantList& targets) const {
  return ToVariantMap(EvalTargets(InteractionCapability::DeleteImages, targets));
}

QVariantMap InteractionPolicyController::EvaluateChangeSemanticModel() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::ChangeSemanticModel));
}

QVariantMap InteractionPolicyController::EvaluateRunSemanticGeneration() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::RunSemanticGeneration));
}

QVariantMap InteractionPolicyController::EvaluateChangeModelDownloadSettings() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::ChangeModelDownloadSettings));
}

QVariantMap InteractionPolicyController::EvaluateChangeImageAnalysisProvider() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::ChangeImageAnalysisProvider));
}

QVariantMap InteractionPolicyController::EvaluateSelectEditorImage() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::SelectEditorImage));
}

QVariantMap InteractionPolicyController::EvaluateSwitchWorkspace() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::SwitchWorkspace));
}

QVariantMap InteractionPolicyController::EvaluateCheckoutVersion() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::CheckoutVersion));
}

QVariantMap InteractionPolicyController::EvaluatePasteAdjustments() const {
  return ToVariantMap(EvalGlobal(InteractionCapability::PasteAdjustments));
}

}  // namespace alcedo::ui