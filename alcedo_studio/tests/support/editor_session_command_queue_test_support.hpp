//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file editor_session_command_queue_test_support.hpp
/// @brief CQ0 test support for the editor-session command-queue plan.
///
/// Adds the deterministic harness the CQ0 failing-evidence tests drive the
/// CURRENT editor-session facade through:
/// - `ManualCommandExecutor`: a single-thread manual queue of closures. A
///   completion that is "posted" (immediate or delayed) is run only when the
///   test drains the executor, never on the service-start stack. No test
///   sleeps for ordering; the executor and the controllable ports own every
///   step.
/// - `SessionResultRecorder`: installed as the facade result observer and
///   change notifier. Records every published `EditorSessionResult` and
///   counts how many arrived while an initiating command was on the call
///   stack (the inline-completion sentinel).
/// - `ControllableEditorHistoryPort`: a `FakeEditorHistoryPort` subclass that
///   (a) exposes a shared worker gate that command operations deliberately do
///   not acquire, and (b) records the
///   durable-publication order (save-started vs. version-created)
///   and models a dirty journal for the Paste ordering tests.
///
/// These types are test-only. They do not change production behavior. CQ1
/// added the real `EditorSessionCommandQueue`; `SessionResultRecorder` and the
/// controllable ports remain the acceptance harness, and posted completions
/// now reduce through the service's `DrainCommandQueueForTests`.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "support/editor_session_test_ports.hpp"

namespace alcedo {
namespace test {

/// Manual single-thread executor. Posted closures queue until the test calls
/// `drain_one` / `drain_all`. Used to model posted worker completion (the CQ1
/// target) and to drive delayed completion without sleeps.
class ManualCommandExecutor {
 public:
  void post(std::function<void()> task) {
    std::scoped_lock lock(mutex_);
    queue_.push(std::move(task));
  }

  auto drain_one() -> bool {
    std::function<void()> task;
    {
      std::scoped_lock lock(mutex_);
      if (queue_.empty()) {
        return false;
      }
      task = std::move(queue_.front());
      queue_.pop();
    }
    task();
    return true;
  }

  void drain_all() {
    while (drain_one()) {
    }
  }

  [[nodiscard]] auto pending() const -> std::size_t {
    std::scoped_lock lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] auto idle() const -> bool { return pending() == 0; }

 private:
  mutable std::mutex                mutex_;
  std::queue<std::function<void()>> queue_;
};

/// Records published results and change notifications, with an inline-
/// completion sentinel. The test brackets an initiating call with
/// `mark_initiating` / `mark_returned`; results that arrive in between are
/// "during initiating" and prove a worker completion ran session code on the
/// service-start stack (the CQ1 invariant forbids this).
class SessionResultRecorder {
 public:
  std::vector<EditorSessionResult> results;
  std::vector<EditorSessionResult> results_during_initiating;
  std::size_t                      change_notifications                   = 0;
  std::size_t                      change_notifications_during_initiating = 0;
  std::size_t                      terminal_during_initiating             = 0;

  void                             mark_initiating() {
    std::scoped_lock lock(mutex_);
    initiating_ = true;
    results_during_initiating.clear();
    change_notifications_during_initiating = 0;
    terminal_during_initiating             = 0;
  }
  void mark_returned() {
    std::scoped_lock lock(mutex_);
    initiating_ = false;
  }

  auto result_observer() -> IEditorSessionBackend::ResultObserver {
    return [this](const EditorSessionResult& result) {
      std::scoped_lock lock(mutex_);
      results.push_back(result);
      if (initiating_) {
        results_during_initiating.push_back(result);
        if (EditorSessionResultIsTerminal(result.kind)) {
          ++terminal_during_initiating;
        }
      }
    };
  }

  auto change_notifier() -> IEditorSessionBackend::ChangeNotifier {
    return [this] {
      std::scoped_lock lock(mutex_);
      ++change_notifications;
      if (initiating_) {
        ++change_notifications_during_initiating;
      }
    };
  }

  /// Count terminal results published for one accepted command. A command
  /// that publishes more than one terminal result violates the CQ1/CQ3
  /// "one accepted command publishes at most one terminal result" invariant.
  [[nodiscard]] auto terminal_count() const -> std::size_t {
    std::scoped_lock lock(mutex_);
    std::size_t      count = 0;
    for (const auto& result : results) {
      if (EditorSessionResultIsTerminal(result.kind)) {
        ++count;
      }
    }
    return count;
  }

 private:
  mutable std::mutex mutex_;
  bool               initiating_ = false;
};

/// History port used by the CQ0 baseline tests. Extends the focused fake with
/// two controllable axes:
/// - `render_lock`: retained as a worker-owned gate for the tests. History
///   operations intentionally do not acquire it; a test that holds the lock
///   verifies the command path remains available.
/// - `event_log` / `dirty_journal`: record the durable-publication order so
///   the Paste tests can assert save-before-create. Paste succeeds (the fake
///   base rejects it) so the facade proceeds through the
///   real StartHistoryCheckpoint save path.
class ControllableEditorHistoryPort : public FakeEditorHistoryPort {
 public:
  /// Worker tests may hold this mutex while command operations are reduced.
  std::mutex*               render_lock   = nullptr;
  /// When non-null, durable-publication events are appended here.
  std::vector<std::string>* event_log     = nullptr;
  /// True when the active image has unflushed journal records (a dirty
  /// current image). The CQ1 target saves such a journal before creating a
  /// new Version or merge commit.
  bool                      dirty_journal = false;
  /// Legacy counter retained so baseline tests can assert the shadow-candidate
  /// publication path is never entered (always remains 0 after live paste/merge).
  int                       transfer_publication_count = 0;
  /// History facts projected into EditorActionInputs for CQ3 availability.
  bool                      force_can_undo = false;
  bool                      force_can_redo = false;

  auto ReadHistorySnapshot(const EditorHistoryGuardHandle& /*guard*/,
                           EditorHistorySnapshot* snapshot, std::string* /*error*/) -> bool override {
    if (snapshot == nullptr) {
      return false;
    }
    *snapshot           = EditorHistorySnapshot{};
    snapshot->can_undo  = force_can_undo;
    snapshot->can_redo  = force_can_redo;
    return true;
  }

  /// Report the dirty journal through the same query the facade uses to gate
  /// Paste and the discard action.
  auto HasUnmaterializedChanges(const EditorHistoryGuardHandle& /*guard*/, std::string* /*error*/)
      -> bool override {
    return dirty_journal;
  }

  /// A successful save checkpoint reconciles the in-memory materialized tuple
  /// with DuckDB, which clears the dirty-journal condition — the production
  /// Mini-Git state then reports HasUnmaterializedChanges == false.
  auto SyncMaterializedStateAfterCheckpoint(const EditorHistoryGuardHandle& guard,
                                            std::string* error) -> bool override {
    dirty_journal = false;
    return FakeEditorHistoryPort::SyncMaterializedStateAfterCheckpoint(guard, error);
  }

  auto Undo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool override {
    return FakeEditorHistoryPort::Undo(guard, error);
  }
  auto Redo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool override {
    return FakeEditorHistoryPort::Redo(guard, error);
  }
  auto MoveHeadToCommit(const EditorHistoryGuardHandle& guard, const commit_hash_t& commit_id,
                        std::string* error) -> bool override {
    return FakeEditorHistoryPort::MoveHeadToCommit(guard, commit_id, error);
  }
  auto CheckoutVersion(const EditorHistoryGuardHandle& guard, const Hash128& version_id,
                       std::string* error) -> bool override {
    return FakeEditorHistoryPort::CheckoutVersion(guard, version_id, error);
  }

  auto PasteLiveRootRelativeVersion(const EditorHistoryGuardHandle& guard,
                                    const AdjustmentTransferPackage& package,
                                    std::string /*version_display_name*/,
                                    AdjustmentPasteResult* result, std::string* error)
      -> bool override {
    (void)guard;
    if (package.Empty()) {
      if (error != nullptr) *error = "Adjustment transfer package is empty";
      return false;
    }
    record("version_created");
    dirty_journal = true;
    if (result != nullptr) {
      result->pasted = true;
      result->prior_version_id = Hash128{0x11111111ULL, 0x22222222ULL};
      result->new_version_id = Hash128{0x33333333ULL, 0x44444444ULL};
    }
    return true;
  }

  auto CancelLivePaste(const EditorHistoryGuardHandle& /*guard*/,
                       const version_ref_id_t& /*prior_version_id*/,
                       const version_ref_id_t& /*paste_version_id*/, std::string* /*error*/)
      -> bool override {
    record("paste_cancelled");
    return true;
  }

  auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const EditorMiniGitSaveCapture> override {
    return FakeEditorHistoryPort::CaptureSaveCheckpoint(guard, error);
  }

 private:
  void record(std::string event) {
    if (event_log != nullptr) {
      event_log->push_back(std::move(event));
    }
  }
};

/// Journal port that records when the save checkpoint starts, for the
/// durable-publication ordering tests. `CommitJournalAsync` logs
/// `save_started` before the (inline) save proceeds, so the test can compare
/// save-start vs. version-create/merge-commit order.
class OrderRecordingJournalPort : public FakeEditorJournalPort {
 public:
  std::vector<std::string>* event_log = nullptr;

  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          EditorJournalCommitCallback callback) -> bool override {
    record("save_started");
    return FakeEditorJournalPort::CommitJournalAsync(element_id, session_generation,
                                                     std::move(callback));
  }

 private:
  void record(std::string event) {
    if (event_log != nullptr) {
      event_log->push_back(std::move(event));
    }
  }
};

}  // namespace test
}  // namespace alcedo
