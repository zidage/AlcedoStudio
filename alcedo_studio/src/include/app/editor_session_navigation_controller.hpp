//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_request_ids.hpp"
#include "edit/history/commit_types.hpp"
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
  CheckoutVersion,
  /// Phase 7A: create a new Version at the image root, then checkout.
  CreateRootVersionAndCheckout,
  /// Phase 7A: branch a new Version from a selected commit, then checkout.
  BranchFromCommitAndCheckout,
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
  /// Target Version for CheckoutVersion actions. Zero means unset.
  version_ref_id_t        version_id{};
  /// Phase 7A: display name for CreateRootVersion / BranchFromCommit.
  std::string             display_name;
  /// Phase 7A: selected commit for BranchFromCommitAndCheckout.
  commit_hash_t           branch_commit{};
  CheckpointTicket        ticket{};
};

/// Mutable navigation state owned by the session command reducer. The
/// navigation helper receives this state by reference so pending actions do
/// not live behind a second synchronization boundary.
struct EditorSessionNavigationState {
  std::optional<PendingEditorAction> pending_action;
  std::optional<PendingEditorAction> pending_recovery;
  /// Switch target queued behind a running save. A rapid second selection
  /// replaces this (never the running target); it is promoted to
  /// pending_action once the running save completes and the prior image is
  /// acquired. Only SwitchImage selections queue here.
  std::optional<PendingEditorAction> pending_next_target;
};

/// Outcome of a navigation request. The facade uses this to publish the
/// appropriate EditorSessionResult and decide whether to wait for the save
/// checkpoint or proceed immediately.
struct NavigationOutcome {
  /// The pending action was captured and a save checkpoint was started.
  bool             waiting_for_checkpoint  = false;
  /// The navigation completed synchronously (no prior image, same-image
  /// no-op, or a discard-and-continue path). Save-bounded navigations never
  /// set this: their completion is posted through the command queue.
  bool             completed_synchronously = false;
  /// The request was rejected (e.g. concurrent navigation or shutdown).
  bool             rejected                = false;
  /// The request failed (e.g. save seal failure).
  bool             failed                  = false;
  /// Same image already open: no-op.
  bool             same_image_noop         = false;
  /// The CheckpointTicket for correlation with OnCheckpointFinished.
  CheckpointTicket ticket;
  /// The image-load request id of the image being saved.
  ImageLoadRequestId sealed_image_load_request_id{};
  std::string      message;
};

/// Async completion outcome published by the navigation controller when an
/// asynchronous save checkpoint + navigation finishes (Phase 7A). The facade
/// uses this to publish the matching EditorSessionResult for the UI.
struct NavigationCompletion {
  bool             success        = false;
  bool             retained_image = false;  ///< True when the image survived a failure.
  std::string      message;
  CheckpointTicket ticket;
};

using NavigationCompletionNotifier = std::function<void(const NavigationCompletion&)>;

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
    ImageLoadRequestId image_load_request_id{};
  };

  explicit EditorSessionNavigationController(
      EditorSessionLifecycle& lifecycle, EditorSaveCheckpointService& save_service,
      EditorSessionRenderController& render, EditorSessionEditController& edit,
      IEditorJournalPort* journal, IEditorCheckpointStore* checkpoint_store,
      IEditorHistoryPort* history, EditorSessionNavigationState* state = nullptr);

  /// Stamp the operation currently being reduced onto saves and render work.
  void SetOperationId(std::uint64_t operation_id);

  /// Request to open or switch to a new image. If the current image has
  /// unsealed changes, starts a save checkpoint and captures the pending
  /// action. Returns a NavigationOutcome describing the immediate result.
  auto RequestOpenOrSwitch(sl_element_id_t element_id, image_id_t image_id, bool is_switch)
      -> NavigationOutcome;

  /// Request to close the editor. If the current image has unsealed changes,
  /// starts a save checkpoint and captures the pending close action.
  auto RequestClose(bool persist_changes) -> NavigationOutcome;

  /// Request to check out another Version on the currently open image. Always
  /// completes a save checkpoint first (even when the journal is empty) so the
  /// working head is durable before reconstruction. Rebuilds the pipeline from
  /// root + first-parent chain only after the checkpoint succeeds.
  auto RequestCheckoutVersion(const version_ref_id_t& version_id) -> NavigationOutcome;

  /// Phase 7A: request a new Version at the image root and check it out. Saves
  /// the current Version first, then creates the root ref, rebuilds, and
  /// publishes the clean root snapshot/frame.
  auto RequestCreateRootVersion(std::string display_name) -> NavigationOutcome;

  /// Phase 7A: request a new Version at an explicit commit and check it out.
  /// Saves the current Version first, then creates the ref at the selected
  /// commit, rebuilds, and publishes the matching snapshot/frame.
  auto RequestBranchFromCommit(const commit_hash_t& commit_id, std::string display_name)
      -> NavigationOutcome;

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
  /// reset state after a rejected command.
  void               ClearPendingAction();

  /// Phase 7A: register a notifier called when an async save checkpoint +
  /// navigation finishes (success or failure). The facade uses this to publish
  /// the matching EditorSessionResult for the UI.
  void               SetCompletionNotifier(NavigationCompletionNotifier notifier);

  /// Phase 7A: true when the session is in the RetainedImageFailure state and
  /// a pending recovery target exists (save failure with a pending navigation).
  [[nodiscard]] auto has_pending_recovery() const -> bool;

  /// Phase 7A: retry the save checkpoint for the pending recovery target.
  /// Returns a NavigationOutcome for the re-attempted save.
  auto               RetrySaveAfterFailure() -> NavigationOutcome;

  /// Phase 7A: discard unflushed changes and continue the pending navigation.
  /// Clears the journal, releases the current guards, and proceeds to the
  /// pending target. Returns a NavigationOutcome.
  auto               DiscardAndContinueAfterFailure() -> NavigationOutcome;

  /// Phase 7A: cancel the pending navigation and resume Interactive on the
  /// retained image. No data loss: the image stays published.
  void               CancelPendingNavigation();

 private:
  /// Seal the current image: finalize edit, capture checkpoint, start save.
  /// Returns a valid ticket on success.
  auto SealAndStartSave(bool persist_changes, bool start_background_save) -> CheckpointTicket;
  /// Continue to the target image after a successful save. Acquires guards,
  /// loads the adjustment snapshot, starts the first-frame render.
  void ContinueToTarget(sl_element_id_t element_id, image_id_t image_id, bool is_switch);
  /// Continue to a close after a successful save.
  void ContinueToClose(bool persist_changes);
  /// Continue Version checkout after a successful save on the current image.
  /// Returns false and sets error on checkout rebuild failure; the prior
  /// Version remains active (history port fails closed).
  auto ContinueCheckoutVersion(const version_ref_id_t& version_id, std::string* error) -> bool;
  /// Phase 7A: continue root Version creation + checkout after a successful
  /// save on the current image. Returns false and sets error on failure; the
  /// provisional ref is removed and the prior ref/pipeline restored.
  auto ContinueCreateRootVersion(std::string display_name, std::string* error) -> bool;
  /// Phase 7A: continue branch-from-commit + checkout after a successful save.
  /// Returns false and sets error on failure; provisional ref removed.
  auto ContinueBranchFromCommit(const commit_hash_t& commit_id, std::string display_name,
                                std::string* error) -> bool;
  /// Promote a rapid SwitchImage selection queued behind a completed switch.
  /// Seals the just-acquired image and starts its save so the queued target
  /// becomes the next running navigation. Called on the owner thread after the
  /// running switch's OnCheckpointFinished publishes its completion.
  void PromoteQueuedSwitchTarget();
  /// Publish the async completion to the registered notifier (if any).
  void NotifyCompletion(bool success, bool retained_image, std::string message,
                        const CheckpointTicket& ticket);
  /// Retain the current image and remember the pending target after a save or
  /// checkpoint failure. Callers are already on the session owner thread.
  void RetainPendingFailure(PendingEditorAction pending, std::string message);

  void AssertOwnerThread() const { assert(std::this_thread::get_id() == owner_thread_); }

  EditorSessionLifecycle&        lifecycle_;
  EditorSaveCheckpointService&   save_service_;
  EditorSessionRenderController& render_;
  EditorSessionEditController&   edit_;
  IEditorJournalPort*            journal_;
  IEditorCheckpointStore*        checkpoint_store_;
  IEditorHistoryPort*            history_;
  EditorSessionNavigationState   owned_state_;
  EditorSessionNavigationState*  state_;
  std::thread::id                owner_thread_;
  std::uint64_t                  operation_id_ = 0;
  NavigationCompletionNotifier   completion_notifier_;
};

}  // namespace alcedo
