//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

class EditTransaction;
class Hash128;

/// Narrow ports used by EditorSessionService. Production implementations wrap
/// PipelineMgmtService / EditHistoryMgmtService / BackgroundTaskController /
/// journal storage. Tests inject fakes. The service never exposes these ports
/// to QML modules.

struct EditorPipelineGuardHandle {
  sl_element_id_t element_id = 0;
  bool            valid      = false;
};

struct EditorHistoryGuardHandle {
  sl_element_id_t element_id = 0;
  bool            valid      = false;
};

class IEditorPipelinePort {
 public:
  virtual ~IEditorPipelinePort() = default;
  virtual auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorPipelineGuardHandle                             = 0;
  virtual void Release(const EditorPipelineGuardHandle& guard) = 0;
};

class IEditorHistoryPort {
 public:
  virtual ~IEditorHistoryPort() = default;
  virtual auto Acquire(sl_element_id_t element_id, std::string* error)
      -> EditorHistoryGuardHandle                                                      = 0;
  virtual void Release(const EditorHistoryGuardHandle& guard)                          = 0;
  virtual auto Undo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool = 0;
  virtual auto Redo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool = 0;
  /// Read the adjustment state after a history operation. History remains the
  /// source of truth; the session service must not guess the resulting params.
  virtual auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle& guard,
                                      EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool = 0;
};

class IEditorTaskPort {
 public:
  virtual ~IEditorTaskPort() = default;
  /// Register a logical background operation for UI progress (save, load, etc.).
  virtual auto BeginTask(const std::string& name, sl_element_id_t element_id) -> std::uint64_t = 0;
  virtual void EndTask(std::uint64_t task_id, bool success, const std::string& message)        = 0;
};

struct EditorJournalCommitOutcome {
  bool          accepted = false;
  bool          durable  = false;
  bool          pending  = false;
  std::uint64_t durable_batch_commit_sequence = 0;
  std::uint64_t durable_operation_sequence    = 0;
  std::string   error;
};

struct EditorMaterializeOutcome {
  bool          accepted = false;
  bool          materialized = false;
  std::uint64_t materialized_operation_sequence = 0;
  std::string   error;
};

using EditorJournalCommitCallback = std::function<void(EditorJournalCommitOutcome)>;
using EditorMaterializeCallback   = std::function<void(EditorMaterializeOutcome)>;

/// Typed persistence boundary for one image-scoped editor session. The
/// compatibility AppendBarrier shim is intentionally below the typed methods;
/// new callers cannot accidentally combine finalize, journal durability, and
/// materialization in one service call.
class IEditorJournalPort {
 public:
  virtual ~IEditorJournalPort() = default;

  /// Finalize the open edit command. This boundary is synchronous and must not
  /// perform file or database I/O.
  virtual auto FinalizeEdit(sl_element_id_t /*element_id*/, std::uint64_t /*session_generation*/,
                            std::string* /*error*/) -> bool {
    return true;
  }

  /// Commit queued journal records. The default implementation adapts old test
  /// ports that only override AppendBarrier.
  virtual auto CommitJournal(sl_element_id_t element_id, std::uint64_t session_generation,
                             std::string* error) -> EditorJournalCommitOutcome {
    EditorJournalCommitOutcome outcome;
    outcome.accepted = true;
    outcome.durable  = AppendBarrier(element_id, session_generation, error);
    if (!outcome.durable) {
      outcome.error = error != nullptr ? *error : "Journal commit failed";
    }
    return outcome;
  }

  /// Materialize journal-committed records into the durable history/pipeline
  /// projection. The default is a no-op for bootstrap ports.
  virtual auto Materialize(sl_element_id_t /*element_id*/, std::uint64_t /*session_generation*/,
                           std::string* /*error*/) -> EditorMaterializeOutcome {
    return EditorMaterializeOutcome{true, true, 0, {}};
  }

  /// Recover the durable journal head for an image and materialize any REDO'd
  /// journal-committed operations into DuckDB. Called when an image is opened so
  /// the editor starts from the recovered durable state. The default is a no-op
  /// for bootstrap ports that have no journal.
  virtual auto RecoverAndMaterialize(sl_element_id_t /*element_id*/,
                                     std::uint64_t /*session_generation*/, std::string* /*error*/)
      -> EditorMaterializeOutcome {
    return EditorMaterializeOutcome{true, true, 0, {}};
  }

  /// Queue one finalized edit-history operation in the image journal. These
  /// methods perform no file I/O; CommitJournal owns the durability barrier.
  virtual auto RecordEdit(sl_element_id_t /*element_id*/, std::uint64_t /*session_generation*/,
                          const EditTransaction& /*transaction*/, std::string* /*error*/) -> bool {
    return true;
  }
  virtual auto RecordCursorMove(sl_element_id_t /*element_id*/,
                                std::uint64_t /*session_generation*/, std::uint64_t /*from_cursor*/,
                                std::uint64_t /*to_cursor*/, std::string* /*error*/) -> bool {
    return true;
  }
  virtual auto RecordRewriteTimeline(sl_element_id_t /*element_id*/,
                                     std::uint64_t /*session_generation*/,
                                     const Hash128& /*expected_timeline_hash*/,
                                     const Hash128& /*discarded_tail_hash*/,
                                     std::uint64_t /*retained_cursor*/,
                                     const EditTransaction& /*replacement*/, std::string* /*error*/)
      -> bool {
    return true;
  }

  /// Async adapters own their worker and invoke the callback after the typed
  /// operation reaches its terminal state. Defaults are synchronous so legacy
  /// fakes remain deterministic.
  virtual auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                                  EditorJournalCommitCallback callback) -> bool {
    std::string error;
    auto        outcome = CommitJournal(element_id, session_generation, &error);
    if (outcome.error.empty()) {
      outcome.error = std::move(error);
    }
    if (callback) {
      callback(std::move(outcome));
    }
    return true;
  }

  virtual auto MaterializeAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                                EditorMaterializeCallback callback) -> bool {
    std::string error;
    auto        outcome = Materialize(element_id, session_generation, &error);
    if (outcome.error.empty()) {
      outcome.error = std::move(error);
    }
    if (callback) {
      callback(std::move(outcome));
    }
    return true;
  }

  /// Legacy compatibility hook for Phase 5F test doubles. Production and new
  /// tests should override CommitJournal/CommitJournalAsync instead.
  virtual auto AppendBarrier(sl_element_id_t /*element_id*/, std::uint64_t /*session_generation*/,
                             std::string* /*error*/) -> bool {
    return true;
  }

  virtual auto DiscardUnflushed(sl_element_id_t element_id, std::string* error) -> bool = 0;
};

/// Coordinator-facing diagnostics exposed to the session service for QML
/// spinner/progress/error display (Phase 5D) and production cutover inspection
/// (Phase 5E). QML never observes pipeline task objects — only this aggregate
/// busy/reason/rejection summary.
struct EditorRenderCoordinatorDiagnostics {
  bool                              has_inflight    = false;
  std::size_t                       pending_count   = 0;
  std::optional<EditorRenderReason> inflight_reason{};
  std::size_t                       replaced_count  = 0;
  std::size_t                       cancelled_count = 0;
  std::string                       last_error;
  /// Phase 5E: generations the coordinator currently accepts.
  std::uint64_t                     session_generation = 0;
  std::uint64_t                     render_generation  = 0;
  std::uint64_t                     view_generation    = 0;
  /// Last request that was rejected at Submit (generation/token/scheduler).
  std::string                       last_rejection_reason;
  std::optional<EditorRenderReason> last_rejected_render_reason{};
  /// Last intent that reached FrameSubmitted (native slot ready for composition).
  std::optional<FrameRole>          last_submitted_frame_role{};
  std::optional<EditorRenderReason> last_submitted_render_reason{};
  /// Monotonic counters of terminal outcomes for tests/diagnostics.
  std::size_t                       accepted_count  = 0;
  std::size_t                       failed_count    = 0;
  std::size_t                       presented_count = 0;
};

/// Sole path from the session service into pipeline work. Production wraps
/// EditorRenderCoordinator; tests may inject a recording stub.
class IEditorRenderSubmitPort {
 public:
  virtual ~IEditorRenderSubmitPort()                                          = default;
  virtual auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult = 0;
  virtual void CancelSession(std::uint64_t session_generation)                = 0;
  /// Cancel a session and wait until production workers no longer use its
  /// presentation sink. Test/fake ports keep the historical synchronous
  /// behavior through this default implementation.
  virtual void CancelSessionAndWait(std::uint64_t session_generation) {
    CancelSession(session_generation);
  }
  virtual void SetActiveGenerations(std::uint64_t session_generation,
                                    std::uint64_t render_generation,
                                    std::uint64_t view_generation)            = 0;
  /// Phase 5D diagnostics. Default impls report an idle coordinator so test
  /// fakes that do not override them stay QML-idle.
  [[nodiscard]] virtual auto diagnostics() const -> EditorRenderCoordinatorDiagnostics {
    return {};
  }
};

}  // namespace alcedo
