//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

class EditorSessionLifecycle;
class EditorSessionRenderController;
class EditorSessionEditController;
class IEditorJournalPort;
class IEditorCheckpointStore;
class IEditorHistoryPort;

/// Named kind for a pending editor navigation action. The navigation
/// controller owns at most one pending action at a time.
enum class PendingEditorActionKind : std::uint8_t {
  SwitchImage = 0,
  CloseEditor,
};

/// A pending navigation action captured while a save checkpoint is in progress.
/// Image B does not begin loading until A's save checkpoint succeeds. The
/// checkpoint ticket correlates the pending action with the save completion.
struct PendingEditorAction {
  PendingEditorActionKind kind       = PendingEditorActionKind::SwitchImage;
  sl_element_id_t         element_id = 0;
  image_id_t              image_id   = 0;
  bool                    is_switch  = false;
  bool                    persist    = true;
  CheckpointTicket        ticket{};
};

/// Outcome of a navigation request. The facade uses this to publish the
/// appropriate EditorSessionResult and decide whether to wait for the save
/// checkpoint or proceed immediately.
struct NavigationOutcome {
  /// The pending action was captured and a save checkpoint was started.
  bool             waiting_for_checkpoint  = false;
  /// The navigation completed synchronously (sync save or no prior image).
  bool             completed_synchronously = false;
  /// The request was rejected (e.g. concurrent navigation or shutdown).
  bool             rejected                = false;
  /// The request failed (e.g. save seal failure).
  bool             failed                  = false;
  /// Same image already open: no-op.
  bool             same_image_noop         = false;
  /// The CheckpointTicket for correlation with OnCheckpointFinished.
  CheckpointTicket ticket;
  /// The session generation of the image being saved.
  std::uint64_t    sealed_session_generation = 0;
  std::string      message;
};

/// Owns the pending A-to-B/close navigation orchestration. The controller
/// calls EditorSessionLifecycle, EditorSaveCheckpointService,
/// EditorSessionRenderController, and EditorSessionEditController through their
/// public APIs; it never accesses their fields or mutexes. There are no lambda
/// backdoors to parent-class methods.
class EditorSessionNavigationController final {
 public:
  /// Sealing instructions for the current image. Built by the navigation
  /// controller and applied through the save service and lifecycle.
  struct SealContext {
    bool            persist_changes       = true;
    bool            start_background_save = true;
    sl_element_id_t element_id            = 0;
    std::uint64_t   session_generation    = 0;
  };

  explicit EditorSessionNavigationController(EditorSessionLifecycle&        lifecycle,
                                             EditorSaveCheckpointService&   save_service,
                                             EditorSessionRenderController& render,
                                             EditorSessionEditController&   edit,
                                             IEditorJournalPort*            journal,
                                             IEditorCheckpointStore*        checkpoint_store,
                                             IEditorHistoryPort*            history);

  /// Request to open or switch to a new image. If the current image has
  /// unsealed changes, starts a save checkpoint and captures the pending
  /// action. Returns a NavigationOutcome describing the immediate result.
  auto RequestOpenOrSwitch(sl_element_id_t element_id, image_id_t image_id, bool is_switch)
      -> NavigationOutcome;

  /// Request to close the editor. If the current image has unsealed changes,
  /// starts a save checkpoint and captures the pending close action.
  auto               RequestClose(bool persist_changes) -> NavigationOutcome;

  /// Resume the pending navigation after a save checkpoint completed. Called
  /// by the facade when the save service invokes its completion callback. On
  /// success, releases the prior image's guards, loads the adjustment snapshot
  /// for the target, and starts the first-frame render. On failure, keeps the
  /// prior image and discards the pending action.
  void               OnCheckpointFinished(const SaveCheckpointResult& result);

  /// True when a navigation action is pending (a save checkpoint is in
  /// progress). The facade uses this to reject concurrent navigation.
  [[nodiscard]] auto has_pending_action() const -> bool;

  /// Clear the pending action without resuming. Used when the facade needs to
  /// reset state after a synchronous completion or a rejection.
  void               ClearPendingAction();

 private:
  /// Seal the current image: finalize edit, capture checkpoint, start save.
  /// Returns a valid ticket on success.
  auto SealAndStartSave(bool persist_changes, bool start_background_save) -> CheckpointTicket;
  /// Continue to the target image after a successful save. Acquires guards,
  /// loads the adjustment snapshot, starts the first-frame render.
  void ContinueToTarget(sl_element_id_t element_id, image_id_t image_id, bool is_switch);
  /// Continue to a close after a successful save.
  void ContinueToClose(bool persist_changes);

  EditorSessionLifecycle&            lifecycle_;
  EditorSaveCheckpointService&       save_service_;
  EditorSessionRenderController&     render_;
  EditorSessionEditController&       edit_;
  IEditorJournalPort*                journal_;
  IEditorCheckpointStore*            checkpoint_store_;
  IEditorHistoryPort*                history_;
  mutable std::recursive_mutex       mutex_;
  std::optional<PendingEditorAction> pending_action_;
};

}  // namespace alcedo
