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
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_history_types.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

class Hash128;
struct EditorMiniGitSaveCapture;
struct EditorAdjustmentPatch;
struct AdjustmentTransferPackage;
struct AdjustmentMergePreview;
struct AdjustmentMergeResolution;
struct AdjustmentMergeResult;
struct AdjustmentPasteResult;
struct EditorTransferCandidate;

/// Narrow ports used by EditorSessionService. Production implementations wrap
/// PipelineMgmtService, Mini-Git journal storage, thumbnail work, and background tasks.
/// Tests inject fakes. The service never exposes these ports to QML modules.

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
      -> EditorHistoryGuardHandle                             = 0;
  virtual void Release(const EditorHistoryGuardHandle& guard) = 0;
  /// Capture the committed operator state before the first interactive preview
  /// for one input sequence. Repeated preview samples for the same field must
  /// preserve the original captured state.
  virtual auto CaptureAdjustmentBeforePreview(const EditorHistoryGuardHandle& /*guard*/,
                                              const EditorAdjustmentPatch& /*patch*/,
                                              std::string* /*error*/) -> bool {
    return true;
  }
  /// Finalize one settled adjustment into the checked-out Version's working
  /// history. Production appends the mini-Git journal record before advancing
  /// the working head and transaction-chain hash.
  virtual auto CommitAdjustment(const EditorHistoryGuardHandle& /*guard*/,
                                const EditorAdjustmentPatch& /*patch*/, std::string* /*error*/)
      -> bool {
    return true;
  }
  virtual auto Undo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool = 0;
  virtual auto Redo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool = 0;
  /// Move the working head to an explicit commit in one operation. The target
  /// must be an ancestor of the working head (backward) or a member of the
  /// in-memory redo suffix (forward); otherwise the call fails without moving.
  /// On success the caller applies the returned traversed deltas and routes a
  /// single render. Fail closed: prior head/redo/snapshot remain on failure.
  virtual auto MoveHeadToCommit(const EditorHistoryGuardHandle& /*guard*/,
                                const commit_hash_t& /*commit_id*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Head move is not supported by this history port";
    }
    return false;
  }
  /// Read the adjustment state after a history operation. History remains the
  /// source of truth; the session service must not guess the resulting params.
  virtual auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle& guard,
                                      EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
      -> bool = 0;

  /// Switch the checked-out Version after a successful save checkpoint. Rebuilds
  /// the live pipeline from root + first-parent chain and refreshes the
  /// adjustment snapshot. Default rejects so fakes must opt in.
  /// Fail closed: prior Version and pipeline remain published on failure.
  /// `version_id` is a Version ref identity (Hash128 / version_ref_id_t).
  virtual auto CheckoutVersion(const EditorHistoryGuardHandle& /*guard*/,
                               const Hash128& /*version_id*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Version checkout is not supported by this history port";
    }
    return false;
  }

  /// Read named Version refs and the active Version's first-parent commit path.
  /// Journal frames are an internal recovery mechanism and are never returned
  /// as user-facing rows.
  virtual auto ReadHistorySnapshot(const EditorHistoryGuardHandle& /*guard*/,
                                   EditorHistorySnapshot* /*snapshot*/, std::string* error)
      -> bool {
    if (error != nullptr) {
      *error = "Editor history projection is not supported by this history port";
    }
    return false;
  }

  /// Return whether the active working head differs from the last materialized
  /// head. The editor uses this to gate the current-image discard action.
  /// Test ports that do not model Mini-Git state may keep the default false.
  virtual auto HasUnmaterializedChanges(const EditorHistoryGuardHandle& /*guard*/,
                                        std::string* /*error*/) -> bool {
    return false;
  }

  /// Restore the live working state to the last materialized head and clear
  /// its recovery journal. Test ports may keep the default no-op behavior.
  virtual auto DiscardUnmaterializedChanges(const EditorHistoryGuardHandle& /*guard*/,
                                            std::string* /*error*/) -> bool {
    return true;
  }

  /// Phase 7A: create a new Version at the image root (null head), set it
  /// active, rebuild the pipeline, clear redo, and publish the clean root
  /// snapshot. The new ref replaces the ambiguous active-head creation. Fail
  /// closed: prior ref/pipeline/snapshot remain published on failure.
  virtual auto CreateRootVersionAndCheckout(const EditorHistoryGuardHandle& /*guard*/,
                                            std::string /*display_name*/,
                                            version_ref_id_t* /*version_id*/, std::string* error)
      -> bool {
    if (error != nullptr) *error = "Root Version creation is not supported by this history port";
    return false;
  }
  /// Phase 7A: create a new Version at an explicit commit, set it active,
  /// rebuild the pipeline, clear redo, and publish the matching snapshot. Fail
  /// closed: prior ref/pipeline/snapshot remain published on failure.
  virtual auto BranchFromCommitAndCheckout(const EditorHistoryGuardHandle& /*guard*/,
                                           const commit_hash_t& /*commit_id*/,
                                           std::string /*display_name*/,
                                           version_ref_id_t* /*version_id*/, std::string* error)
      -> bool {
    if (error != nullptr) *error = "Branch creation is not supported by this history port";
    return false;
  }

  virtual auto RenameVersion(const EditorHistoryGuardHandle& /*guard*/,
                             const Hash128& /*version_id*/, std::string /*display_name*/,
                             std::string* error) -> bool {
    if (error != nullptr) *error = "Version rename is not supported by this history port";
    return false;
  }
  virtual auto RemoveVersion(const EditorHistoryGuardHandle& /*guard*/,
                             const Hash128& /*version_id*/, std::string* error) -> bool {
    if (error != nullptr) *error = "Version removal is not supported by this history port";
    return false;
  }

  virtual auto PasteAdjustments(const EditorHistoryGuardHandle& /*guard*/,
                                const AdjustmentTransferPackage& /*package*/,
                                std::string /*version_display_name*/,
                                AdjustmentPasteResult* /*result*/, std::string* error) -> bool {
    if (error != nullptr) *error = "Editor Paste is not supported by this history port";
    return false;
  }
  virtual auto BeginMerge(const EditorHistoryGuardHandle& /*guard*/,
                          const AdjustmentTransferPackage& /*package*/,
                          std::string /*incoming_version_display_name*/,
                          AdjustmentMergePreview* /*preview*/, std::string* error) -> bool {
    if (error != nullptr) *error = "Editor Merge is not supported by this history port";
    return false;
  }
  virtual auto CompleteMerge(const EditorHistoryGuardHandle& /*guard*/,
                             const AdjustmentMergePreview& /*preview*/,
                             const std::vector<AdjustmentMergeResolution>& /*resolutions*/,
                             AdjustmentMergeResult* /*result*/, std::string* error) -> bool {
    if (error != nullptr) *error = "Editor Merge is not supported by this history port";
    return false;
  }
  virtual auto CancelMerge(const EditorHistoryGuardHandle& /*guard*/,
                           const AdjustmentMergePreview& /*preview*/, std::string* error) -> bool {
    if (error != nullptr)
      *error = "Editor Merge cancellation is not supported by this history port";
    return false;
  }

  /// Build a Paste candidate without changing the published graph, Version
  /// selection, journal, or adjustment snapshot. The candidate is retained by
  /// the history port until CaptureTransferSaveCheckpoint and
  /// PublishTransferCandidate complete, or DiscardTransferCandidate is called.
  /// Default fakes retain the command inputs and defer the legacy mutation to
  /// PublishTransferCandidate.
  virtual auto PreparePaste(const EditorHistoryGuardHandle& /*guard*/,
                            const AdjustmentTransferPackage& package,
                            std::string version_display_name,
                            AdjustmentPasteResult* result,
                            EditorTransferCandidate* candidate, std::string* error) -> bool {
    if (candidate == nullptr) {
      if (error != nullptr) *error = "Paste candidate storage is required";
      return false;
    }
    candidate->package      = package;
    candidate->display_name = std::move(version_display_name);
    if (result != nullptr) *result = {};
    return true;
  }

  /// Build a Merge preview and staged candidate without changing published
  /// history. The preview records its source package and first-parent facts so
  /// stale resolutions can be rejected before any durable work begins.
  virtual auto PrepareMerge(const EditorHistoryGuardHandle& guard,
                            const AdjustmentTransferPackage& package,
                            std::string incoming_version_display_name,
                            AdjustmentMergePreview* preview,
                            EditorTransferCandidate* candidate, std::string* error) -> bool {
    if (candidate == nullptr) {
      if (error != nullptr) *error = "Merge candidate storage is required";
      return false;
    }
    if (!BeginMerge(guard, package, std::move(incoming_version_display_name), preview, error)) {
      return false;
    }
    candidate->package = package;
    return true;
  }

  /// Validate that the active history still matches the staged Merge preview.
  /// This is side-effect free and runs before conflict resolutions are applied.
  virtual auto ValidateMergeCandidate(const EditorHistoryGuardHandle& /*guard*/,
                                      const AdjustmentMergePreview& /*preview*/,
                                      const EditorTransferCandidate& /*candidate*/,
                                      std::string* /*error*/) -> bool {
    return true;
  }

  /// Apply Merge resolutions to the staged candidate graph only. Published
  /// history and the live adjustment snapshot remain unchanged until
  /// PublishTransferCandidate succeeds. Test adapters that retain only the
  /// legacy port inputs defer their observable mutation to publication.
  virtual auto CompleteMergeCandidate(
      const EditorHistoryGuardHandle& /*guard*/, const AdjustmentMergePreview& /*preview*/,
      const std::vector<AdjustmentMergeResolution>& /*resolutions*/,
      EditorTransferCandidate* candidate, AdjustmentMergeResult* result, std::string* error)
      -> bool {
    if (candidate == nullptr) {
      if (error != nullptr) *error = "Merge candidate storage is required";
      return false;
    }
    if (result != nullptr) *result = {};
    return true;
  }

  /// Capture one durable publication containing the complete candidate graph,
  /// final Version selection, final head, serialized pipeline state, and the
  /// current journal prefix. It must not mutate published history.
  virtual auto CaptureTransferSaveCheckpoint(
      const EditorHistoryGuardHandle& guard, const EditorTransferCandidate& /*candidate*/,
      std::string* error) -> std::shared_ptr<const EditorMiniGitSaveCapture> {
    return CaptureSaveCheckpoint(guard, error);
  }

  /// Publish a previously captured candidate after its one durable write has
  /// completed. The default adapter invokes the legacy synchronous port only
  /// for test doubles; production ports replace the live graph atomically.
  virtual auto PublishTransferCandidate(
      const EditorHistoryGuardHandle& guard, const EditorTransferCandidate& candidate,
      const AdjustmentMergePreview* preview,
      const std::vector<AdjustmentMergeResolution>& resolutions, AdjustmentPasteResult* paste,
      AdjustmentMergeResult* merge, std::string* error) -> bool {
    if (preview != nullptr) {
      return CompleteMerge(guard, *preview, resolutions, merge, error);
    }
    return PasteAdjustments(guard, candidate.package, candidate.display_name, paste, error);
  }

  /// Discard a staged candidate without changing published history.
  virtual auto DiscardTransferCandidate(const EditorHistoryGuardHandle& /*guard*/,
                                        const EditorTransferCandidate& /*candidate*/,
                                        std::string* /*error*/) -> bool {
    return true;
  }

  /// Capture the immutable live history prefix that a save checkpoint must
  /// persist. Production copies journal records and their inclusive sequence
  /// range under the journal mutex used by append/truncate, together with
  /// element/Version/root IDs, working head, chain hash, serialized pipeline
  /// state, and journal path. The caller owns the returned value and passes it
  /// directly to the checkpoint store; this port keeps no deferred capture side
  /// table. An empty journal yields nullopt sequence bounds — do not invent a
  /// second empty-flag.
  virtual auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle& /*guard*/,
                                     std::string* /*error*/)
      -> std::shared_ptr<const EditorMiniGitSaveCapture> {
    return nullptr;
  }

  /// Drop the live (and durable) journal prefix through last_sequence after a
  /// successful DuckDB materialize so the next same-session capture does not
  /// re-save already-materialized records. Materializer truncates by path; this
  /// keeps the in-memory MiniGitJournal that still owns append state in sync.
  /// Default is a no-op for fakes that do not hold a journal.
  virtual auto DiscardMaterializedJournalThrough(const EditorHistoryGuardHandle& /*guard*/,
                                                 std::uint64_t /*last_sequence*/,
                                                 std::string* /*error*/) -> bool {
    return true;
  }
  /// Reconcile the in-memory ImageEditState.materialized_* with DuckDB after a
  /// successful save checkpoint. The checkpoint writes the active Version's working
  /// head and first-parent chain hash to DuckDB but does not advance the in-memory
  /// materialized fields; without this call a subsequent PersistEditorHistoryState
  /// guard rejects the durable tuple as stale. Idempotent: a no-op when the in-memory
  /// state already agrees with the active head. Default is a no-op for fakes that do
  /// not hold a live commit graph.
  virtual auto SyncMaterializedStateAfterCheckpoint(const EditorHistoryGuardHandle& /*guard*/,
                                                    std::string* /*error*/) -> bool {
    return true;
  }
};

class IEditorTaskPort {
 public:
  virtual ~IEditorTaskPort() = default;
  /// Register a logical background operation for UI progress (save, load, etc.).
  virtual auto BeginTask(const std::string& name, sl_element_id_t element_id) -> std::uint64_t = 0;
  virtual void EndTask(std::uint64_t task_id, bool success, const std::string& message)        = 0;
};

struct EditorJournalCommitOutcome {
  bool          accepted                      = false;
  bool          durable                       = false;
  bool          pending                       = false;
  std::uint64_t durable_batch_commit_sequence = 0;
  std::uint64_t durable_operation_sequence    = 0;
  std::string   error;
};

struct EditorMaterializeOutcome {
  bool          accepted                        = false;
  bool          materialized                    = false;
  std::uint64_t materialized_operation_sequence = 0;
  std::string   error;
};

using EditorJournalCommitCallback = std::function<void(EditorJournalCommitOutcome)>;
using EditorMaterializeCallback   = std::function<void(EditorMaterializeOutcome)>;

/// Typed journal-writer boundary for one image-scoped editor session. Database
/// materialization and recovery belong to IEditorCheckpointStore, so this
/// interface contains only journal append, durability, and discard operations.
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

  /// Async adapters invoke the callback after the journal durability operation
  /// reaches its terminal state. The default is synchronous for deterministic
  /// test ports.
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

  /// Legacy compatibility hook for Phase 5F test doubles. Runtime and new tests
  /// should override CommitJournal/CommitJournalAsync instead.
  virtual auto AppendBarrier(sl_element_id_t /*element_id*/, std::uint64_t /*session_generation*/,
                             std::string* /*error*/) -> bool {
    return true;
  }

  virtual auto DiscardUnflushed(sl_element_id_t element_id, std::string* error) -> bool = 0;
};

/// Phase 6C-5: narrow checkpoint store for save/recovery. Accepts an immutable
/// capture and drives materialization through the Mini-Git materializer facade.
class IEditorCheckpointStore {
 public:
  virtual ~IEditorCheckpointStore() = default;

  /// Persist one immutable capture. The store may truncate the captured
  /// journal prefix only after the database write succeeds.
  virtual auto Materialize(std::shared_ptr<const EditorMiniGitSaveCapture> /*capture*/,
                           std::string* /*error*/) -> EditorMaterializeOutcome {
    return EditorMaterializeOutcome{true, true, 0, {}};
  }

  virtual auto MaterializeAsync(std::shared_ptr<const EditorMiniGitSaveCapture> capture,
                                EditorMaterializeCallback                       callback) -> bool {
    std::string error;
    auto        outcome = Materialize(std::move(capture), &error);
    if (outcome.error.empty()) outcome.error = std::move(error);
    if (callback) callback(std::move(outcome));
    return true;
  }

  virtual auto RecoverAndMaterialize(sl_element_id_t /*element_id*/,
                                     std::uint64_t /*session_generation*/, std::string* /*error*/)
      -> EditorMaterializeOutcome {
    return EditorMaterializeOutcome{true, true, 0, {}};
  }
};

/// Schedules a refresh for the currently focused thumbnail after a durable
/// checkpoint. Failed checkpoints must not call this port.
class IEditorThumbnailPort {
 public:
  virtual ~IEditorThumbnailPort()                                      = default;

  /// Invalidate cached pixels and schedule a new render only when the image
  /// remains focused in a thumbnail surface.
  virtual void RefreshAfterMaterialization(sl_element_id_t element_id) = 0;
};

/// Coordinator-facing diagnostics exposed to the session service for QML
/// spinner/progress/error display (Phase 5D) and production cutover inspection
/// (Phase 5E). QML never observes pipeline task objects — only this aggregate
/// busy/reason/rejection summary.
struct EditorRenderCoordinatorDiagnostics {
  bool                              has_inflight  = false;
  std::size_t                       pending_count = 0;
  std::optional<EditorRenderReason> inflight_reason{};
  std::size_t                       replaced_count  = 0;
  std::size_t                       cancelled_count = 0;
  std::string                       last_error;
  /// Phase 5E: stamps the coordinator currently accepts.
  std::uint64_t                     image_load_request_id = 0;
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

enum class EditorRenderSupersessionPolicy : std::uint8_t {
  CancelObsolete = 0,
  PreserveInflightFullFrame,
};

/// Immutable render command. Built by the facade or edit controller and passed
/// to the render controller; the render controller does not read adjustment
/// state from any other component.
struct EditorRenderCommand {
  EditorRenderReason                  reason       = EditorRenderReason::InitialFrame;
  std::uint64_t                       operation_id = 0;
  EditorRenderAdjustmentSnapshot      adjustment{};
  EditorRenderSupersessionPolicy      policy = EditorRenderSupersessionPolicy::CancelObsolete;
  std::optional<ViewportRenderRegion> view_region;
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
  virtual void SetActiveGenerations(
      std::uint64_t session_generation, std::uint64_t render_generation,
      std::uint64_t                  view_generation,
      EditorRenderSupersessionPolicy policy = EditorRenderSupersessionPolicy::CancelObsolete) = 0;
  /// Phase 5D diagnostics. Default impls report an idle coordinator so test
  /// fakes that do not override them stay QML-idle.
  [[nodiscard]] virtual auto diagnostics() const -> EditorRenderCoordinatorDiagnostics {
    return {};
  }
};

}  // namespace alcedo
