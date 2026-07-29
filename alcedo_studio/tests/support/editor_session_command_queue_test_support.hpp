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
///   (a) gates the executor-owning operations behind a shared render lock so a
///   test can reproduce GUI-side render-lock blocking, and (b) records the
///   durable-publication order (save-started vs. version-created/merge-
///   committed) and models a dirty journal for the Paste/Merge ordering tests.
///
/// These types are test-only. They do not change production behavior and do
/// not introduce a real command queue; that is CQ1.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_session_ports.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "app/adjustment_transfer_types.hpp"
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
  mutable std::mutex                 mutex_;
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
  std::size_t                       change_notifications              = 0;
  std::size_t                       change_notifications_during_initiating = 0;
  std::size_t                       terminal_during_initiating        = 0;

  void mark_initiating() {
    std::scoped_lock lock(mutex_);
    initiating_ = true;
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
/// - `render_lock`: when non-null, executor-owning operations (Undo, Redo,
///   head move, version checkout, Paste, Merge, save-checkpoint capture)
///   acquire this mutex first. A test that holds the lock reproduces the
///   GUI-side render-lock wait; the command thread blocks until the lock is
///   released.
/// - `event_log` / `dirty_journal`: record the durable-publication order so
///   the Paste/Merge tests can assert save-before-create. Paste and Merge
///   succeed (the fake base rejects them) so the facade proceeds through the
///   real StartHistoryCheckpoint save path.
class ControllableEditorHistoryPort : public FakeEditorHistoryPort {
 public:
  /// When non-null, gated operations block on this mutex.
  std::mutex* render_lock = nullptr;
  /// When non-null, durable-publication events are appended here.
  std::vector<std::string>* event_log = nullptr;
  /// True when the active image has unflushed journal records (a dirty
  /// current image). The CQ4 target saves such a journal before creating a
  /// new Version or merge commit.
  bool dirty_journal = false;

  auto Undo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool override {
    auto ul = lock_render();
    return FakeEditorHistoryPort::Undo(guard, error);
  }
  auto Redo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool override {
    auto ul = lock_render();
    return FakeEditorHistoryPort::Redo(guard, error);
  }
  auto MoveHeadToCommit(const EditorHistoryGuardHandle& guard, const commit_hash_t& commit_id,
                        std::string* error) -> bool override {
    auto ul = lock_render();
    return FakeEditorHistoryPort::MoveHeadToCommit(guard, commit_id, error);
  }
  auto CheckoutVersion(const EditorHistoryGuardHandle& guard, const Hash128& version_id,
                       std::string* error) -> bool override {
    auto ul = lock_render();
    return FakeEditorHistoryPort::CheckoutVersion(guard, version_id, error);
  }

  auto PasteAdjustments(const EditorHistoryGuardHandle& guard,
                        const AdjustmentTransferPackage& /*package*/,
                        std::string /*version_display_name*/, AdjustmentPasteResult* result,
                        std::string* error) -> bool override {
    auto ul = lock_render();
    record("version_created");
    if (result != nullptr) {
      result->pasted = true;
    }
    (void)guard;
    (void)error;
    return true;
  }

  auto BeginMerge(const EditorHistoryGuardHandle& guard, const AdjustmentTransferPackage& /*package*/,
                  std::string /*incoming_version_display_name*/, AdjustmentMergePreview* preview,
                  std::string* error) -> bool override {
    (void)guard;
    (void)error;
    if (preview != nullptr) {
      preview->has_conflicts = false;
      preview->incoming_version_id = Hash128{0x11111111ULL, 0x22222222ULL};
    }
    return true;
  }

  auto CompleteMerge(const EditorHistoryGuardHandle& guard, const AdjustmentMergePreview& /*preview*/,
                     const std::vector<AdjustmentMergeResolution>& /*resolutions*/,
                     AdjustmentMergeResult* result, std::string* error) -> bool override {
    auto ul = lock_render();
    record("merge_committed");
    if (result != nullptr) {
      result->merged = true;
    }
    (void)guard;
    (void)error;
    return true;
  }

  auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle& guard, std::string* error)
      -> std::shared_ptr<const EditorMiniGitSaveCapture> override {
    auto ul = lock_render();
    return FakeEditorHistoryPort::CaptureSaveCheckpoint(guard, error);
  }

 private:
  [[nodiscard]] auto lock_render() -> std::unique_lock<std::mutex> {
    if (render_lock != nullptr) {
      return std::unique_lock<std::mutex>(*render_lock);
    }
    return std::unique_lock<std::mutex>();
  }

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