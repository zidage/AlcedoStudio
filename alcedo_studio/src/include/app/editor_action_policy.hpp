//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_session_command_queue.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Editor actions whose enabled state is projected to QML and checked at
/// command admission by the same pure evaluator.
enum class EditorAction : std::uint8_t {
  SelectImage = 0,
  PreviewAdjustment,
  CommitAdjustment,
  Undo,
  Redo,
  MoveHead,
  DiscardChanges,
  CheckoutVersion,
  CreateRootVersion,
  BranchVersion,
  RenameVersion,
  RemoveVersion,
  ApplyPaste,
  RetrySave,
  DiscardAndContinue,
  CancelPendingNavigation,
  CloseEditor,
  Shutdown,
  RequestViewChange,
  Count,
};

[[nodiscard]] inline auto EditorActionCount() -> std::size_t {
  return static_cast<std::size_t>(EditorAction::Count);
}

/// Result of one action evaluation. `reason` is empty when allowed.
struct EditorActionDecision {
  bool        allowed = false;
  std::string reason;

  [[nodiscard]] auto operator==(const EditorActionDecision&) const -> bool = default;
};

/// Kind of in-flight work owned by the command queue. Leases never mutate
/// individual canX flags; Evaluate recomputes decisions from leases + facts.
enum class EditorOperationLeaseKind : std::uint8_t {
  None = 0,
  ImageLoad,
  ImageSwitch,
  SaveCheckpoint,
  PasteMaterialization,
  FailureRecovery,
};

/// Queue-owned lease describing which actions an accepted operation blocks.
struct EditorOperationLease {
  EditorSessionOperationId operation{};
  EditorOperationLeaseKind kind = EditorOperationLeaseKind::None;
  sl_element_id_t          target_element_id = 0;
  image_id_t               target_image_id   = 0;
  ImageLoadRequestId       image_load_request{};
  EditorSaveTaskId         save_task{};
  std::vector<EditorAction> blocked_actions;
  std::string               blocking_reason;
};

/// Pure facts merged into the evaluator. Side-effect free; never reads live
/// executors or QML state.
struct EditorActionInputs {
  EditorSessionState session_state = EditorSessionState::NoImage;
  bool               has_image     = false;
  bool               can_undo      = false;
  bool               can_redo      = false;
  bool               has_unmaterialized_changes = false;
  bool               package_available          = false;
  bool               background_blocks_select_image = false;
  bool               background_blocks_paste        = false;
  bool               background_blocks_checkout     = false;
  bool               background_blocks_workspace    = false;
  bool               can_replace_unstarted_selection = false;
  bool               recovery_allows_retry           = false;
  bool               recovery_allows_discard_continue = false;
  bool               recovery_allows_cancel          = false;
};

/// Minimal queue-owned execution context visible to the evaluator. Not a
/// public session snapshot.
struct EditorCommandContext {
  std::vector<EditorOperationLease> leases;
};

/// Projected decisions for every EditorAction. Index = static_cast size.
using EditorActionDecisionTable =
    std::array<EditorActionDecision, static_cast<std::size_t>(EditorAction::Count)>;

struct EditorActionAvailability {
  EditorActionDecisionTable decisions{};

  [[nodiscard]] auto For(EditorAction action) const -> const EditorActionDecision& {
    return decisions[static_cast<std::size_t>(action)];
  }

  [[nodiscard]] auto operator==(const EditorActionAvailability&) const -> bool = default;
};

/// Pure action-admission policy shared by QML projection and command acceptance.
class EditorActionPolicy {
 public:
  /// Evaluate one action against the current leases and base facts.
  [[nodiscard]] static auto Evaluate(EditorAction action, const EditorCommandContext& context,
                                     const EditorActionInputs& inputs) -> EditorActionDecision;

  /// Evaluate every action once. Used for projection and change detection.
  [[nodiscard]] static auto EvaluateAll(const EditorCommandContext& context,
                                        const EditorActionInputs&   inputs)
      -> EditorActionAvailability;

  /// Map a command kind to the action it must pass before reduction.
  /// Returns nullopt for commands that are always queue-internal (e.g. reserved
  /// presentation setters that are not yet admitted through policy).
  [[nodiscard]] static auto ActionForCommand(EditorSessionCommandKind kind)
      -> std::optional<EditorAction>;

  /// Default blocked-action set for a newly acquired lease kind.
  [[nodiscard]] static auto DefaultBlockedActions(EditorOperationLeaseKind kind)
      -> std::vector<EditorAction>;
};

[[nodiscard]] auto EditorActionName(EditorAction action) -> const char*;

}  // namespace alcedo
