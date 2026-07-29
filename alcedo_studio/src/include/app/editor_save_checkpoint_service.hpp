//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/editor_session_command_queue.hpp"
#include "app/editor_session_ports.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Opaque ticket identifying one in-flight save checkpoint. The navigation
/// controller uses this to correlate async completions with the save that
/// started them. A default-constructed ticket is invalid.
struct CheckpointTicket {
  std::uint64_t      request_id         = 0;
  std::uint64_t      operation_id       = 0;
  std::uint64_t      session_generation = 0;
  sl_element_id_t    element_id         = 0;
  std::uint64_t      task_id            = 0;
  [[nodiscard]] auto valid() const -> bool { return request_id != 0; }
};

/// Request to start one editor save checkpoint. Built by the navigation
/// controller after acquiring the project-owned SaveCheckpointLock, finalizing
/// the open edit command, and capturing the live serialized pipeline state.
///
/// Call-chain ownership: Navigation acquires the lock via
/// EditorSaveCheckpointService::TryAcquireSaveLock before capture, moves that
/// lock into this request, and Start owns it through the durable save work.
struct SaveCheckpointRequest {
  sl_element_id_t                                     element_id         = 0;
  std::uint64_t                                       operation_id       = 0;
  std::uint64_t                                       session_generation = 0;
  std::shared_ptr<const EditorMiniGitSaveCapture>     capture;
  /// Inclusive last journal sequence from capture when the range is non-empty.
  /// Filled by the caller that built the capture so this service need not depend
  /// on the Mini-Git materializer type definition.
  std::optional<std::uint64_t>                        last_journal_sequence;
  /// Pre-acquired project-wide save lock (from TryAcquireSaveLock). When empty,
  /// Start attempts TryAcquire itself. Ownership moves into the service for the
  /// full journal / materialize / thumbnail / completion path.
  EditorSaveCheckpointCoordinator::SaveCheckpointLock save_lock;
};

/// Outcome of one save checkpoint. The completion callback receives this
/// exactly once. checkpoint_completed is true only when the journal commit was
/// durable and DuckDB materialization succeeded. The request_id matches the
/// CheckpointTicket returned by Start so the caller can correlate.
struct SaveCheckpointResult {
  std::uint64_t                request_id           = 0;
  std::uint64_t                operation_id         = 0;
  std::uint64_t                session_generation   = 0;
  std::uint64_t                task_id              = 0;
  bool                         checkpoint_completed = false;
  std::string                  error;
  /// Inclusive last journal sequence materialized by this checkpoint when the
  /// capture had a non-empty range. Callers (navigation) use this to drop the
  /// matching live journal prefix so same-session captures stay consistent with
  /// the on-disk truncate performed by the materializer.
  std::optional<std::uint64_t> last_journal_sequence;
};

/// Invoked exactly once when the save checkpoint reaches its terminal state.
using SaveCheckpointCompletion = std::function<void(const SaveCheckpointResult&)>;

/// Owns the editor save checkpoint work: background task registration, journal
/// commit, and DuckDB materialization. Does not know about image B, pending
/// navigation, session guards, render generations, adjustment state, or the
/// facade. The completion callback is the sole channel back to the caller.
///
/// Global save lock: the project-owned EditorSaveCheckpointCoordinator is
/// injected at construction. A SaveCheckpointLock is held from Start through
/// durable journal commit, materialization, and thumbnail invalidation. It is
/// released before the terminal callback so navigation may recover another
/// image through the same coordinator.
class EditorSaveCheckpointService final {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorJournalPort>              journal;
    std::shared_ptr<IEditorCheckpointStore>          checkpoint_store;
    std::shared_ptr<IEditorThumbnailPort>            thumbnails;
    std::shared_ptr<IEditorTaskPort>                 tasks;
    std::shared_ptr<IEditorSessionCommandExecutor>   command_executor;
    /// Project-owned global save lock. Shared with EditorMiniGitMaterializer.
    std::shared_ptr<EditorSaveCheckpointCoordinator> save_coordinator;
  };

  explicit EditorSaveCheckpointService(Dependencies dependencies);
  ~EditorSaveCheckpointService();

  EditorSaveCheckpointService(const EditorSaveCheckpointService&)            = delete;
  EditorSaveCheckpointService& operator=(const EditorSaveCheckpointService&) = delete;

  /// Non-blocking attempt to take the project-owned save lock for element_id.
  /// Call on the GUI thread before CaptureSaveCheckpoint so capture and the
  /// subsequent Start share one ownership interval. Returns a lock whose
  /// owns_lock() is false when another checkpoint owns the lock or Shutdown
  /// has begun. The move-only lock is the ownership authority.
  [[nodiscard]] auto           TryAcquireSaveLock(sl_element_id_t element_id)
      -> EditorSaveCheckpointCoordinator::SaveCheckpointLock;

  /// Begin one save checkpoint: start the background task, then commit the
  /// journal and materialize. Holds the project-owned SaveCheckpointLock (from
  /// the request or acquired here) through durable save work, then releases it
  /// before the terminal completion callback.
  /// Returns a valid CheckpointTicket on success, or an invalid ticket with an
  /// error on failure. The completion callback is invoked exactly once, either
  /// synchronously (legacy synchronous journal ports) or asynchronously. On
  /// synchronous failure the ticket is invalid and completion was already
  /// invoked with checkpoint_completed=false.
  auto Start(SaveCheckpointRequest request, SaveCheckpointCompletion completion)
      -> CheckpointTicket;

  /// Stop accepting new callbacks, publish one terminal cancellation result for
  /// each abandoned in-flight save (checkpoint_completed=false), release those
  /// save locks, and join outstanding gate work. After this returns, a later
  /// storage/journal completion cannot finish the same task again.
  void               CancelAndWait();

  /// Invoked by the navigation controller when a checkpoint completes. The
  /// service ends the task and invokes the stored completion callback. Stale
  /// completions (no matching ticket) are silently ignored.
  void               OnCheckpointFinished(const SaveCheckpointResult& result);

  /// True while at least one save checkpoint is outstanding. Diagnostics only.
  [[nodiscard]] auto active() const -> bool;

 private:
  struct AsyncCallbackGate {
    auto                    Enter() -> bool;
    void                    Leave();
    void                    StopAndWait();
    std::mutex              mutex;
    std::condition_variable condition;
    std::size_t             active_callbacks = 0;
    bool                    stopping         = false;
  };

  struct PendingSave {
    std::uint64_t                                       request_id         = 0;
    std::uint64_t                                       operation_id       = 0;
    std::uint64_t                                       session_generation = 0;
    sl_element_id_t                                     element_id         = 0;
    std::uint64_t                                       task_id            = 0;
    std::shared_ptr<const EditorMiniGitSaveCapture>     capture;
    std::optional<std::uint64_t>                        last_journal_sequence;
    /// Held through durable save work; released before terminal completion.
    EditorSaveCheckpointCoordinator::SaveCheckpointLock save_lock;
    SaveCheckpointCompletion                            completion;
  };

  void         HandleJournalCommit(std::uint64_t request_id, EditorJournalCommitOutcome outcome,
                                   SaveCheckpointCompletion completion);
  void         HandleMaterialization(std::uint64_t request_id, EditorMaterializeOutcome outcome,
                                     SaveCheckpointCompletion completion);
  void         DeliverCompletion(SaveCheckpointCompletion completion, SaveCheckpointResult result);
  void         FinishSave(std::uint64_t request_id, std::uint64_t operation_id,
                          std::uint64_t session_generation, std::uint64_t task_id,
                          bool checkpoint_completed, std::string message,
                          SaveCheckpointCompletion                              completion,
                          EditorSaveCheckpointCoordinator::SaveCheckpointLock&& save_lock,
                          std::optional<std::uint64_t> last_journal_sequence = std::nullopt);
  auto         TakePendingSave(std::uint64_t request_id, std::uint64_t* task_id,
                               SaveCheckpointCompletion*                            completion,
                               EditorSaveCheckpointCoordinator::SaveCheckpointLock* save_lock) -> bool;

  Dependencies deps_;
  std::shared_ptr<AsyncCallbackGate> callback_gate_;
  mutable std::mutex                 mutex_;
  std::vector<PendingSave>           pending_saves_;
  std::uint64_t                      next_request_id_ = 1;
};

}  // namespace alcedo
