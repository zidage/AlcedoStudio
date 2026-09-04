//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file editor_session_test_ports.hpp
/// @brief Small reusable fake editor-session ports for focused module tests.
///
/// These types are not fixtures and do not own multi-module scenario state. Each
/// component fixture constructs only the fakes its module requires.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_session_ports.hpp"
#include "type/hash_type.hpp"

namespace alcedo::test {

/// Opaque non-null capture pointer used when tests do not need a real Mini-Git
/// capture payload. The deleter is a no-op; the static token is process-lifetime.
inline auto MakeOpaqueSaveCapture() -> std::shared_ptr<const EditorMiniGitSaveCapture> {
  static const int token = 0;
  return {reinterpret_cast<const EditorMiniGitSaveCapture*>(&token),
          [](const EditorMiniGitSaveCapture*) {}};
}

/// Fake pipeline port that records acquire/release counts and optional failures.
class FakeEditorPipelinePort final : public IEditorPipelinePort {
 public:
  bool fail_acquire  = false;
  int  acquire_count = 0;
  int  release_count = 0;
  std::vector<sl_element_id_t> acquired_ids;
  std::vector<sl_element_id_t> released_ids;

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorPipelineGuardHandle override {
    ++acquire_count;
    acquired_ids.push_back(element_id);
    if (fail_acquire) {
      if (error != nullptr) {
        *error = "pipeline acquire failed";
      }
      return {};
    }
    return EditorPipelineGuardHandle{element_id, true};
  }

  void Release(const EditorPipelineGuardHandle& guard) override {
    ++release_count;
    released_ids.push_back(guard.element_id);
  }
};

/// Fake history port covering acquire/release, edit commit, undo/redo, snapshot,
/// and immutable save-checkpoint capture.
class FakeEditorHistoryPort : public IEditorHistoryPort {
 public:
  bool fail_acquire   = false;
  bool fail_commit    = false;
  bool fail_undo      = false;
  bool fail_snapshot  = false;
  bool fail_capture   = false;
  int  acquire_count  = 0;
  int  release_count  = 0;
  int  capture_count  = 0;
  int  commit_count   = 0;
  int  undo_count     = 0;
  int  redo_count     = 0;
  int  checkpoint_capture_count = 0;
  int  checkout_count           = 0;
  bool fail_checkout            = false;
  Hash128 last_checkout_version{};
  bool   fail_root_version      = false;
  bool   fail_branch_version    = false;
  int    root_version_count     = 0;
  int    branch_version_count   = 0;
  Hash128 last_root_version{0xaaaaaaaaULL, 0xbbbbbbbbULL};
  Hash128 last_branch_version{0xccccccccULL, 0xddddddddULL};
  commit_hash_t last_branch_commit{};
  version_ref_id_t active_version_id{};
  version_ref_id_t last_commit_version{};
  EditorRenderAdjustmentSnapshot current_snapshot{};
  EditorAdjustmentPatch          last_captured_patch{};
  EditorAdjustmentPatch          last_committed_patch{};
  bool                           fail_node_command  = false;
  int                            add_grade_count    = 0;
  int                            remove_grade_count = 0;
  int                            rename_grade_count = 0;
  int                                             reconnect_grade_count = 0;
  NodeId                         last_node_id;
  NodeId                         last_before_node_id;
  NodeId                                          last_predecessor_id;
  NodeId                                          last_successor_id;
  std::string                    last_grade_name;
  std::optional<EditorRenderReason> last_render_reason = EditorRenderReason::UndoRedo;
  std::shared_ptr<const EditorMiniGitSaveCapture> next_capture = MakeOpaqueSaveCapture();

  auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorHistoryGuardHandle override {
    ++acquire_count;
    if (fail_acquire) {
      if (error != nullptr) {
        *error = "history acquire failed";
      }
      return {};
    }
    return EditorHistoryGuardHandle{element_id, true};
  }

  void Release(const EditorHistoryGuardHandle&) override { ++release_count; }

  auto CaptureAdjustmentBeforePreview(const EditorHistoryGuardHandle&,
                                      const EditorAdjustmentPatch& patch, std::string*)
      -> bool override {
    ++capture_count;
    last_captured_patch = patch;
    return true;
  }

  auto CommitAdjustment(const EditorHistoryGuardHandle&, const EditorAdjustmentPatch& patch,
                        std::string* error) -> bool override {
    ++commit_count;
    last_committed_patch = patch;
    last_commit_version = active_version_id;
    if (fail_commit) {
      if (error != nullptr) {
        *error = "mini-Git journal append failed";
      }
      return false;
    }
    return true;
  }

  auto AddColorGrade(const EditorHistoryGuardHandle&, const NodeId& before_node_id,
                     const NodeId& new_id, std::string* error) -> bool override {
    ++add_grade_count;
    last_before_node_id = before_node_id;
    last_node_id        = new_id;
    last_render_reason  = EditorRenderReason::GraphTopologyChanged;
    if (fail_node_command) {
      if (error != nullptr) *error = "mini-Git journal append failed";
      return false;
    }
    return true;
  }

  auto RemoveColorGrade(const EditorHistoryGuardHandle&, const NodeId& node_id, std::string* error)
      -> bool override {
    ++remove_grade_count;
    last_node_id       = node_id;
    last_render_reason = EditorRenderReason::GraphTopologyChanged;
    if (fail_node_command) {
      if (error != nullptr) *error = "mini-Git journal append failed";
      return false;
    }
    return true;
  }

  auto RenameColorGrade(const EditorHistoryGuardHandle&, const NodeId& node_id,
                        std::string display_name, std::string* error) -> bool override {
    ++rename_grade_count;
    last_node_id       = node_id;
    last_grade_name    = std::move(display_name);
    last_render_reason = std::nullopt;
    if (fail_node_command) {
      if (error != nullptr) *error = "mini-Git journal append failed";
      return false;
    }
    return true;
  }

  auto ReconnectColorGrade(const EditorHistoryGuardHandle&, const NodeId& node_id,
                           const NodeId& new_predecessor_id, const NodeId& new_successor_id,
                           std::string* error) -> bool override {
    ++reconnect_grade_count;
    last_node_id        = node_id;
    last_predecessor_id = new_predecessor_id;
    last_successor_id   = new_successor_id;
    last_render_reason  = EditorRenderReason::GraphTopologyChanged;
    if (fail_node_command) {
      if (error != nullptr) *error = "mini-Git journal append failed";
      return false;
    }
    return true;
  }

  [[nodiscard]] auto LastPublishedRenderReason() const
      -> std::optional<EditorRenderReason> override {
    return last_render_reason;
  }

  auto Undo(const EditorHistoryGuardHandle&, std::string* error) -> bool override {
    ++undo_count;
    if (fail_undo) {
      if (error != nullptr) {
        *error = "undo failed";
      }
      return false;
    }
    return true;
  }

  auto Redo(const EditorHistoryGuardHandle&, std::string*) -> bool override {
    ++redo_count;
    return true;
  }

  auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle&,
                              EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool override {
    if (fail_snapshot) {
      if (error != nullptr) {
        *error = "snapshot read failed";
      }
      return false;
    }
    if (snapshot != nullptr) {
      *snapshot = current_snapshot;
    }
    return true;
  }

  auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle&, std::string* error)
      -> std::shared_ptr<const EditorMiniGitSaveCapture> override {
    ++checkpoint_capture_count;
    if (fail_capture) {
      if (error != nullptr) {
        *error = "history capture failed";
      }
      return nullptr;
    }
    return next_capture;
  }

  auto CheckoutVersion(const EditorHistoryGuardHandle&, const Hash128& version_id,
                       std::string* error) -> bool override {
    ++checkout_count;
    last_checkout_version = version_id;
    if (fail_checkout) {
      if (error != nullptr) {
        *error = "version checkout rebuild failed";
      }
      return false;
    }
    return true;
  }

  auto CreateRootVersionAndCheckout(const EditorHistoryGuardHandle&, std::string,
                                    version_ref_id_t* version_id, std::string* error)
      -> bool override {
    ++root_version_count;
    if (fail_root_version) {
      if (error != nullptr) *error = "root Version creation failed";
      return false;
    }
    active_version_id = last_root_version;
    if (version_id != nullptr) *version_id = last_root_version;
    return true;
  }

  auto BranchFromCommitAndCheckout(const EditorHistoryGuardHandle&, const commit_hash_t& commit_id,
                                  std::string, version_ref_id_t* version_id, std::string* error)
      -> bool override {
    ++branch_version_count;
    last_branch_commit = commit_id;
    if (fail_branch_version) {
      if (error != nullptr) *error = "branch creation failed";
      return false;
    }
    active_version_id = last_branch_version;
    if (version_id != nullptr) *version_id = last_branch_version;
    return true;
  }
};

/// Fake task port that records begin/end outcomes and optional begin failure.
class FakeEditorTaskPort final : public IEditorTaskPort {
 public:
  bool                       fail_begin  = false;
  int                        begin_count = 0;
  int                        end_count   = 0;
  std::vector<std::uint64_t> begun_ids;
  std::vector<std::uint64_t> ended_ids;
  std::vector<bool>          ended_success;
  std::vector<std::string>   ended_messages;
  std::uint64_t              next_id = 1;

  auto BeginTask(const std::string& /*name*/, sl_element_id_t /*element_id*/)
      -> std::uint64_t override {
    ++begin_count;
    if (fail_begin) {
      return 0;
    }
    const auto id = next_id++;
    begun_ids.push_back(id);
    return id;
  }

  void EndTask(std::uint64_t task_id, bool success, const std::string& message) override {
    ++end_count;
    ended_ids.push_back(task_id);
    ended_success.push_back(success);
    ended_messages.push_back(message);
  }
};

/// Fake journal writer port with optional async commit and barrier failures.
class FakeEditorJournalPort : public IEditorJournalPort {
 public:
  bool                        fail_barrier      = false;
  bool                        fail_commit_start = false;
  bool                        async_commit      = false;
  bool                        finalize_succeeds = true;
  int                         barrier_count     = 0;
  int                         discard_count     = 0;
  EditorJournalCommitCallback pending_commit;

  auto FinalizeEdit(sl_element_id_t, std::uint64_t, std::string* error) -> bool override {
    if (!finalize_succeeds) {
      if (error != nullptr) {
        *error = "finalize failed";
      }
      return false;
    }
    return true;
  }

  auto AppendBarrier(sl_element_id_t, std::uint64_t, std::string* error) -> bool override {
    ++barrier_count;
    if (fail_barrier) {
      if (error != nullptr) {
        *error = "journal barrier failed";
      }
      return false;
    }
    return true;
  }

  auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                          EditorJournalCommitCallback callback) -> bool override {
    if (fail_commit_start) {
      return false;
    }
    if (!async_commit) {
      return IEditorJournalPort::CommitJournalAsync(element_id, session_generation,
                                                    std::move(callback));
    }
    pending_commit = std::move(callback);
    return true;
  }

  /// Completes a pending async journal commit. Durable true maps to a successful
  /// journal durability barrier that later materialization may truncate.
  void CompleteCommit(bool durable, std::string error = {}) {
    auto callback = std::move(pending_commit);
    if (!callback) {
      return;
    }
    callback(EditorJournalCommitOutcome{true, durable, !durable, durable ? 2u : 0u,
                                        durable ? 1u : 0u, std::move(error)});
  }

  auto DiscardUnflushed(sl_element_id_t, std::string*) -> bool override {
    ++discard_count;
    return true;
  }

};

/// Fake checkpoint store with optional async materialization and start failure.
class FakeEditorCheckpointStore final : public IEditorCheckpointStore {
 public:
  bool                      async_materialize      = false;
  bool                      fail_materialize_start = false;
  bool                      fail_materialize       = false;
  int                       materialize_count      = 0;
  /// Last capture pointer received by Materialize/MaterializeAsync (ownership
  /// is not taken beyond the call; the shared_ptr is retained for identity checks).
  std::shared_ptr<const EditorMiniGitSaveCapture> last_capture;
  EditorMaterializeCallback                       pending_materialize;

  auto Materialize(std::shared_ptr<const EditorMiniGitSaveCapture> capture, std::string* error)
      -> EditorMaterializeOutcome override {
    ++materialize_count;
    last_capture = capture;
    if (!capture) {
      if (error != nullptr) {
        *error = "Save capture is required";
      }
      return {false, false, 0, "Save capture is required"};
    }
    if (fail_materialize) {
      if (error != nullptr) {
        *error = "materialization failed";
      }
      return {true, false, 0, "materialization failed"};
    }
    return {true, true, 1, {}};
  }

  auto MaterializeAsync(std::shared_ptr<const EditorMiniGitSaveCapture> capture,
                        EditorMaterializeCallback                       callback) -> bool override {
    if (fail_materialize_start) {
      ++materialize_count;
      last_capture = capture;
      return false;
    }
    if (!async_materialize) {
      std::string error;
      auto        outcome = Materialize(std::move(capture), &error);
      if (outcome.error.empty()) {
        outcome.error = std::move(error);
      }
      if (callback) {
        callback(std::move(outcome));
      }
      return true;
    }
    ++materialize_count;
    last_capture        = capture;
    pending_materialize = std::move(callback);
    return true;
  }

  /// Completes a pending async materialization. Materialized true means DuckDB
  /// write and subsequent journal-prefix truncate both succeeded.
  void CompleteMaterialization(bool materialized, std::string error = {}) {
    auto callback = std::move(pending_materialize);
    if (!callback) {
      return;
    }
    callback(
        EditorMaterializeOutcome{true, materialized, materialized ? 1u : 0u, std::move(error)});
  }
};

/// Fake focused-thumbnail refresh port that records element ids.
class FakeEditorThumbnailPort final : public IEditorThumbnailPort {
 public:
  int                         refresh_count = 0;
  std::vector<sl_element_id_t> refreshed_ids;

  void RefreshAfterMaterialization(sl_element_id_t element_id) override {
    ++refresh_count;
    refreshed_ids.push_back(element_id);
  }
};

/// Fake render submit port used only when a constructor requires a render peer.
/// It does not model GPU work; navigation fixtures keep this private.
class FakeEditorRenderSubmitPort final : public IEditorRenderSubmitPort {
 public:
  int cancel_count = 0;
  int submit_count = 0;
  EditorRenderReason last_reason = EditorRenderReason::InitialFrame;
  bool defer_idle_completion = false;
  SessionIdleCallback pending_idle_completion;
  std::uint64_t pending_idle_epoch = 0;

  void CancelSessionAndWait(std::uint64_t) override { ++cancel_count; }
  void CancelSession(std::uint64_t) override { ++cancel_count; }
  void CancelSession(std::uint64_t epoch, SessionIdleCallback on_idle) override {
    ++cancel_count;
    if (!defer_idle_completion) {
      if (on_idle) {
        on_idle(epoch);
      }
      return;
    }
    pending_idle_epoch      = epoch;
    pending_idle_completion = std::move(on_idle);
  }
  void CompleteSessionIdle() {
    auto callback = std::move(pending_idle_completion);
    if (callback) {
      callback(pending_idle_epoch);
    }
  }
  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override {
    ++submit_count;
    last_reason = intent.reason;
    EditorRenderResult result;
    result.kind       = EditorRenderResultKind::RequestAccepted;
    result.request_id = static_cast<std::uint64_t>(submit_count);
    return result;
  }
  void SetActiveImageLoadRequest(std::uint64_t) override {}
};

/// Lightweight coordinator test double for save-lock diagnostics in fixtures.
/// Production lock ownership remains EditorSaveCheckpointCoordinator; this type
/// only records acquire/release attempts for unit scenarios that need a spy.
class FakeSaveCheckpointCoordinator final {
 public:
  int             acquire_count = 0;
  int             release_count = 0;
  sl_element_id_t active_element_id = 0;
  bool            saving = false;

  /// Records an acquire attempt and returns whether the fake currently allows it.
  auto TryAcquire(sl_element_id_t element_id) -> bool {
    ++acquire_count;
    if (saving) {
      return false;
    }
    saving = true;
    active_element_id = element_id;
    return true;
  }

  /// Records a release for the active element.
  void Release(sl_element_id_t /*element_id*/) {
    ++release_count;
    saving = false;
    active_element_id = 0;
  }
};

}  // namespace alcedo::test
