//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>

#include "app/editor_action_policy.hpp"

namespace alcedo::ui {

/// QML projection of EditorActionPolicy decisions. Bind only these properties
/// for editor enablement; do not combine with history-model or package flags.
class EditorActionAvailabilityModel final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool canSelectImage READ can_select_image NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canEdit READ can_edit NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canUndo READ can_undo NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canRedo READ can_redo NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canMoveHead READ can_move_head NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canDiscardChanges READ can_discard_changes NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canCheckoutVersion READ can_checkout_version NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canCreateRootVersion READ can_create_root_version NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canBranchVersion READ can_branch_version NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canRenameVersion READ can_rename_version NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canRemoveVersion READ can_remove_version NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canPaste READ can_paste NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canRetrySave READ can_retry_save NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canDiscardAndContinue READ can_discard_and_continue NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canCancelPendingNavigation READ can_cancel_pending_navigation NOTIFY
                 AvailabilityChanged)
  Q_PROPERTY(bool canCloseEditor READ can_close_editor NOTIFY AvailabilityChanged)
  Q_PROPERTY(bool canSwitchWorkspace READ can_switch_workspace NOTIFY AvailabilityChanged)

 public:
  explicit EditorActionAvailabilityModel(QObject* parent = nullptr) : QObject(parent) {}

  void Apply(const alcedo::EditorActionAvailability& availability) {
    if (availability_ == availability) {
      return;
    }
    availability_ = availability;
    emit AvailabilityChanged();
  }

  [[nodiscard]] auto availability() const -> const alcedo::EditorActionAvailability& {
    return availability_;
  }

  [[nodiscard]] bool can_select_image() const {
    return Allowed(alcedo::EditorAction::SelectImage);
  }
  [[nodiscard]] bool can_edit() const {
    return Allowed(alcedo::EditorAction::PreviewAdjustment) &&
           Allowed(alcedo::EditorAction::CommitAdjustment);
  }
  [[nodiscard]] bool can_undo() const { return Allowed(alcedo::EditorAction::Undo); }
  [[nodiscard]] bool can_redo() const { return Allowed(alcedo::EditorAction::Redo); }
  [[nodiscard]] bool can_move_head() const { return Allowed(alcedo::EditorAction::MoveHead); }
  [[nodiscard]] bool can_discard_changes() const {
    return Allowed(alcedo::EditorAction::DiscardChanges);
  }
  [[nodiscard]] bool can_checkout_version() const {
    return Allowed(alcedo::EditorAction::CheckoutVersion);
  }
  [[nodiscard]] bool can_create_root_version() const {
    return Allowed(alcedo::EditorAction::CreateRootVersion);
  }
  [[nodiscard]] bool can_branch_version() const {
    return Allowed(alcedo::EditorAction::BranchVersion);
  }
  [[nodiscard]] bool can_rename_version() const {
    return Allowed(alcedo::EditorAction::RenameVersion);
  }
  [[nodiscard]] bool can_remove_version() const {
    return Allowed(alcedo::EditorAction::RemoveVersion);
  }
  [[nodiscard]] bool can_paste() const { return Allowed(alcedo::EditorAction::ApplyPaste); }
  [[nodiscard]] bool can_retry_save() const { return Allowed(alcedo::EditorAction::RetrySave); }
  [[nodiscard]] bool can_discard_and_continue() const {
    return Allowed(alcedo::EditorAction::DiscardAndContinue);
  }
  [[nodiscard]] bool can_cancel_pending_navigation() const {
    return Allowed(alcedo::EditorAction::CancelPendingNavigation);
  }
  [[nodiscard]] bool can_close_editor() const {
    return Allowed(alcedo::EditorAction::CloseEditor);
  }
  /// Workspace leave uses CloseEditor admission (same decision as closing).
  [[nodiscard]] bool can_switch_workspace() const { return can_close_editor(); }

 signals:
  void AvailabilityChanged();

 private:
  [[nodiscard]] bool Allowed(alcedo::EditorAction action) const {
    return availability_.For(action).allowed;
  }

  alcedo::EditorActionAvailability availability_{};
};

}  // namespace alcedo::ui
