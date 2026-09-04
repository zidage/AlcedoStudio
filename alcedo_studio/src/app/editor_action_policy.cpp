//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_action_policy.hpp"

#include <algorithm>

namespace alcedo {
namespace {

[[nodiscard]] auto LeaseBlocks(const EditorCommandContext& context, EditorAction action,
                               std::string* reason) -> bool {
  for (const auto& lease : context.leases) {
    if (lease.kind == EditorOperationLeaseKind::None) {
      continue;
    }
    const auto it =
        std::find(lease.blocked_actions.begin(), lease.blocked_actions.end(), action);
    if (it == lease.blocked_actions.end()) {
      continue;
    }
    if (reason != nullptr) {
      *reason = lease.blocking_reason.empty() ? "Blocked by an in-flight editor operation"
                                              : lease.blocking_reason;
    }
    return true;
  }
  return false;
}

[[nodiscard]] auto Deny(std::string reason) -> EditorActionDecision {
  return EditorActionDecision{false, std::move(reason)};
}

[[nodiscard]] auto Allow() -> EditorActionDecision { return EditorActionDecision{true, {}}; }

[[nodiscard]] auto IsRecoveryState(EditorSessionState state) -> bool {
  return state == EditorSessionState::RetainedImageFailure;
}

[[nodiscard]] auto IsBusyNavigationState(EditorSessionState state) -> bool {
  switch (state) {
    case EditorSessionState::Acquiring:
    case EditorSessionState::Loading:
    case EditorSessionState::Saving:
    case EditorSessionState::Switching:
      return true;
    default:
      return false;
  }
}

}  // namespace

auto EditorActionName(EditorAction action) -> const char* {
  switch (action) {
    case EditorAction::SelectImage:
      return "SelectImage";
    case EditorAction::PreviewAdjustment:
      return "PreviewAdjustment";
    case EditorAction::CommitAdjustment:
      return "CommitAdjustment";
    case EditorAction::Undo:
      return "Undo";
    case EditorAction::Redo:
      return "Redo";
    case EditorAction::MoveHead:
      return "MoveHead";
    case EditorAction::DiscardChanges:
      return "DiscardChanges";
    case EditorAction::CheckoutVersion:
      return "CheckoutVersion";
    case EditorAction::CreateRootVersion:
      return "CreateRootVersion";
    case EditorAction::BranchVersion:
      return "BranchVersion";
    case EditorAction::RenameVersion:
      return "RenameVersion";
    case EditorAction::RemoveVersion:
      return "RemoveVersion";
    case EditorAction::ApplyPaste:
      return "ApplyPaste";
    case EditorAction::RetrySave:
      return "RetrySave";
    case EditorAction::DiscardAndContinue:
      return "DiscardAndContinue";
    case EditorAction::CancelPendingNavigation:
      return "CancelPendingNavigation";
    case EditorAction::CloseEditor:
      return "CloseEditor";
    case EditorAction::Shutdown:
      return "Shutdown";
    case EditorAction::RequestViewChange:
      return "RequestViewChange";
    case EditorAction::Count:
      break;
  }
  return "Unknown";
}

auto EditorActionPolicy::DefaultBlockedActions(EditorOperationLeaseKind kind)
    -> std::vector<EditorAction> {
  switch (kind) {
    case EditorOperationLeaseKind::None:
      return {};
    case EditorOperationLeaseKind::ImageLoad:
    case EditorOperationLeaseKind::ImageSwitch:
      return {
          EditorAction::PreviewAdjustment, EditorAction::CommitAdjustment, EditorAction::Undo,
          EditorAction::Redo,              EditorAction::MoveHead,          EditorAction::DiscardChanges,
          EditorAction::CheckoutVersion,   EditorAction::CreateRootVersion, EditorAction::BranchVersion,
          EditorAction::RenameVersion,     EditorAction::RemoveVersion,     EditorAction::ApplyPaste,
          EditorAction::RequestViewChange,
      };
    case EditorOperationLeaseKind::SaveCheckpoint:
      return {
          EditorAction::PreviewAdjustment, EditorAction::CommitAdjustment, EditorAction::Undo,
          EditorAction::Redo,              EditorAction::MoveHead,          EditorAction::DiscardChanges,
          EditorAction::CheckoutVersion,   EditorAction::CreateRootVersion, EditorAction::BranchVersion,
          EditorAction::RenameVersion,     EditorAction::RemoveVersion,     EditorAction::ApplyPaste,
      };
    case EditorOperationLeaseKind::PasteMaterialization:
      return {
          EditorAction::SelectImage,       EditorAction::PreviewAdjustment,
          EditorAction::CommitAdjustment,  EditorAction::Undo,
          EditorAction::Redo,              EditorAction::MoveHead,
          EditorAction::DiscardChanges,    EditorAction::CheckoutVersion,
          EditorAction::CreateRootVersion, EditorAction::BranchVersion,
          EditorAction::RenameVersion,     EditorAction::RemoveVersion,
          EditorAction::ApplyPaste,        EditorAction::RequestViewChange,
      };
    case EditorOperationLeaseKind::FailureRecovery:
      return {
          EditorAction::SelectImage,       EditorAction::PreviewAdjustment,
          EditorAction::CommitAdjustment,  EditorAction::Undo,
          EditorAction::Redo,              EditorAction::MoveHead,
          EditorAction::DiscardChanges,    EditorAction::CheckoutVersion,
          EditorAction::CreateRootVersion, EditorAction::BranchVersion,
          EditorAction::RenameVersion,     EditorAction::RemoveVersion,
          EditorAction::ApplyPaste,        EditorAction::RequestViewChange,
      };
  }
  return {};
}

auto EditorActionPolicy::ActionForCommand(EditorSessionCommandKind kind)
    -> std::optional<EditorAction> {
  switch (kind) {
    case EditorSessionCommandKind::OpenImage:
    case EditorSessionCommandKind::SelectImage:
      return EditorAction::SelectImage;
    case EditorSessionCommandKind::CloseEditor:
      return EditorAction::CloseEditor;
    case EditorSessionCommandKind::Shutdown:
      return EditorAction::Shutdown;
    case EditorSessionCommandKind::PreviewAdjustment:
      return EditorAction::PreviewAdjustment;
    case EditorSessionCommandKind::CommitAdjustment:
    case EditorSessionCommandKind::AddColorGrade:
    case EditorSessionCommandKind::RemoveColorGrade:
    case EditorSessionCommandKind::RenameColorGrade:
    case EditorSessionCommandKind::ReconnectColorGrade:
    case EditorSessionCommandKind::EditNodeGraph:
      return EditorAction::CommitAdjustment;
    case EditorSessionCommandKind::Undo:
      return EditorAction::Undo;
    case EditorSessionCommandKind::Redo:
      return EditorAction::Redo;
    case EditorSessionCommandKind::MoveHead:
      return EditorAction::MoveHead;
    case EditorSessionCommandKind::DiscardChanges:
      return EditorAction::DiscardChanges;
    case EditorSessionCommandKind::CheckoutVersion:
      return EditorAction::CheckoutVersion;
    case EditorSessionCommandKind::CreateRootVersion:
      return EditorAction::CreateRootVersion;
    case EditorSessionCommandKind::BranchVersion:
      return EditorAction::BranchVersion;
    case EditorSessionCommandKind::RenameVersion:
      return EditorAction::RenameVersion;
    case EditorSessionCommandKind::RemoveVersion:
      return EditorAction::RemoveVersion;
    case EditorSessionCommandKind::ApplyPaste:
      return EditorAction::ApplyPaste;
    case EditorSessionCommandKind::RetrySave:
      return EditorAction::RetrySave;
    case EditorSessionCommandKind::DiscardAndContinue:
      return EditorAction::DiscardAndContinue;
    case EditorSessionCommandKind::CancelPendingNavigation:
      return EditorAction::CancelPendingNavigation;
    case EditorSessionCommandKind::RequestViewChange:
      return EditorAction::RequestViewChange;
    case EditorSessionCommandKind::SetPresentationTarget:
    case EditorSessionCommandKind::SetPresentationSize:
    case EditorSessionCommandKind::SetGeometryOverlay:
      return std::nullopt;
  }
  return std::nullopt;
}

auto EditorActionPolicy::Evaluate(EditorAction action, const EditorCommandContext& context,
                                  const EditorActionInputs& inputs) -> EditorActionDecision {
  if (inputs.session_state == EditorSessionState::ShuttingDown) {
    if (action == EditorAction::Shutdown) {
      return Allow();
    }
    return Deny("Editor is shutting down");
  }
  if (inputs.session_state == EditorSessionState::Failed &&
      action != EditorAction::CloseEditor && action != EditorAction::Shutdown &&
      action != EditorAction::SelectImage) {
    return Deny("Editor session failed");
  }

  std::string lease_reason;
  if (LeaseBlocks(context, action, &lease_reason)) {
    // SelectImage may replace an unstarted selection during load/switch.
    if (action == EditorAction::SelectImage && inputs.can_replace_unstarted_selection) {
      return Allow();
    }
    return Deny(std::move(lease_reason));
  }

  switch (action) {
    case EditorAction::SelectImage:
      if (inputs.background_blocks_select_image) {
        return Deny("A background task blocks editor image selection");
      }
      if (IsBusyNavigationState(inputs.session_state) &&
          !inputs.can_replace_unstarted_selection) {
        return Deny("Image selection is busy");
      }
      if (IsRecoveryState(inputs.session_state)) {
        return Deny("Resolve the save failure before selecting another image");
      }
      return Allow();

    case EditorAction::PreviewAdjustment:
    case EditorAction::CommitAdjustment:
    case EditorAction::RequestViewChange:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Adjustments require an interactive image");
      }
      return Allow();

    case EditorAction::Undo:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Undo requires an interactive image");
      }
      if (!inputs.can_undo) {
        return Deny("Nothing to undo");
      }
      return Allow();

    case EditorAction::Redo:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Redo requires an interactive image");
      }
      if (!inputs.can_redo) {
        return Deny("Nothing to redo");
      }
      return Allow();

    case EditorAction::MoveHead:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Head move requires an interactive image");
      }
      return Allow();

    case EditorAction::DiscardChanges:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Discard requires an interactive image");
      }
      if (!inputs.has_unmaterialized_changes) {
        return Deny("No unmaterialized changes to discard");
      }
      return Allow();

    case EditorAction::CheckoutVersion:
    case EditorAction::CreateRootVersion:
    case EditorAction::BranchVersion:
    case EditorAction::RenameVersion:
    case EditorAction::RemoveVersion:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Version actions require an interactive image");
      }
      if (inputs.background_blocks_checkout) {
        return Deny("A background task blocks Version actions");
      }
      return Allow();

    case EditorAction::ApplyPaste:
      if (inputs.session_state != EditorSessionState::Interactive || !inputs.has_image) {
        return Deny("Paste requires an interactive image");
      }
      if (!inputs.package_available) {
        return Deny("No copied adjustments are available");
      }
      if (inputs.background_blocks_paste) {
        return Deny("A background task blocks Paste");
      }
      return Allow();

    case EditorAction::RetrySave:
      if (!inputs.recovery_allows_retry) {
        return Deny("Retry Save is not available");
      }
      return Allow();

    case EditorAction::DiscardAndContinue:
      if (!inputs.recovery_allows_discard_continue) {
        return Deny("Discard and Continue is not available");
      }
      return Allow();

    case EditorAction::CancelPendingNavigation:
      if (!inputs.recovery_allows_cancel) {
        return Deny("Cancel pending navigation is not available");
      }
      return Allow();

    case EditorAction::CloseEditor:
      if (inputs.background_blocks_workspace &&
          inputs.session_state == EditorSessionState::Interactive) {
        // Close remains available during recovery and shutdown paths; only a
        // global workspace lock in Interactive blocks leaving the editor.
        return Deny("A background task blocks leaving the editor");
      }
      return Allow();

    case EditorAction::Shutdown:
      return Allow();

    case EditorAction::Count:
      break;
  }
  return Deny("Unknown editor action");
}

auto EditorActionPolicy::EvaluateAll(const EditorCommandContext& context,
                                     const EditorActionInputs&   inputs)
    -> EditorActionAvailability {
  EditorActionAvailability availability;
  for (std::size_t i = 0; i < EditorActionCount(); ++i) {
    availability.decisions[i] =
        Evaluate(static_cast<EditorAction>(i), context, inputs);
  }
  return availability;
}

}  // namespace alcedo
