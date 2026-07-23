//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/editor_session_ports.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Opaque ticket identifying one in-flight save checkpoint. The navigation
/// controller uses this to correlate async completions with the save that
/// started them. A default-constructed ticket is invalid.
struct CheckpointTicket {
  std::uint64_t      request_id         = 0;
  std::uint64_t      session_generation = 0;
  sl_element_id_t    element_id         = 0;
  std::uint64_t      task_id            = 0;
  [[nodiscard]] auto valid() const -> bool { return request_id != 0; }
};

/// Request to start one editor save checkpoint. Built by the navigation
/// controller after finalizing the open edit command and capturing the live
/// serialized pipeline state.
struct SaveCheckpointRequest {
  sl_element_id_t                                 element_id         = 0;
  std::uint64_t                                   session_generation = 0;
  std::shared_ptr<const EditorMiniGitSaveCapture> capture;
};

/// Outcome of one save checkpoint. The completion callback receives this
/// exactly once. checkpoint_completed is true only when the journal commit was
/// durable and DuckDB materialization succeeded. The request_id matches the
/// CheckpointTicket returned by Start so the caller can correlate.
struct SaveCheckpointResult {
  std::uint64_t request_id           = 0;
  std::uint64_t session_generation   = 0;
  std::uint64_t task_id              = 0;
  bool          checkpoint_completed = false;
  std::string   error;
};

/// Invoked exactly once when the save checkpoint reaches its terminal state.
using SaveCheckpointCompletion = std::function<void(const SaveCheckpointResult&)>;

/// Owns the editor save checkpoint work: background task registration, journal
/// commit, and DuckDB materialization. Does not know about image B, pending
/// navigation, session guards, render generations, adjustment state, or the
/// facade. The completion callback is the sole channel back to the caller.
class EditorSaveCheckpointService final {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorJournalPort>     journal;
    std::shared_ptr<IEditorCheckpointStore> checkpoint_store;
    std::shared_ptr<IEditorThumbnailPort>   thumbnails;
    std::shared_ptr<IEditorTaskPort>        tasks;
  };

  explicit EditorSaveCheckpointService(Dependencies dependencies);
  ~EditorSaveCheckpointService();

  EditorSaveCheckpointService(const EditorSaveCheckpointService&)            = delete;
  EditorSaveCheckpointService& operator=(const EditorSaveCheckpointService&) = delete;

  /// Begin one save checkpoint: start the background task, then commit the
  /// journal and materialize. Returns a valid CheckpointTicket on success, or
  /// an invalid ticket with an error on failure. The completion callback is
  /// invoked exactly once, either synchronously (legacy synchronous journal
  /// ports) or asynchronously. On synchronous failure the ticket is invalid
  /// and completion was already invoked with checkpoint_completed=false.
  auto Start(SaveCheckpointRequest request, SaveCheckpointCompletion completion)
      -> CheckpointTicket;

  /// Stop accepting new callbacks and join/cancel outstanding work. After this
  /// returns, no completion callback will fire.
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
    std::uint64_t                                   request_id         = 0;
    std::uint64_t                                   session_generation = 0;
    sl_element_id_t                                 element_id         = 0;
    std::uint64_t                                   task_id            = 0;
    std::shared_ptr<const EditorMiniGitSaveCapture> capture;
    SaveCheckpointCompletion                        completion;
  };

  void HandleJournalCommit(std::uint64_t request_id, EditorJournalCommitOutcome outcome,
                           SaveCheckpointCompletion completion);
  void HandleMaterialization(std::uint64_t request_id, EditorMaterializeOutcome outcome,
                             SaveCheckpointCompletion completion);
  void FinishSave(std::uint64_t request_id, std::uint64_t session_generation, std::uint64_t task_id,
                  bool checkpoint_completed, std::string message,
                  SaveCheckpointCompletion completion);
  auto TakePendingSave(std::uint64_t request_id, std::uint64_t* task_id,
                       SaveCheckpointCompletion* completion) -> bool;

  Dependencies                       deps_;
  std::shared_ptr<AsyncCallbackGate> callback_gate_;
  mutable std::mutex                 mutex_;
  std::vector<PendingSave>           pending_saves_;
  std::uint64_t                      next_request_id_ = 1;
};

}  // namespace alcedo
