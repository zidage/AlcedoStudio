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
#include "app/editor_comparison_types.hpp"
#include "app/editor_history_types.hpp"
#include "app/editor_panel_projection.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_session_types.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "type/type.hpp"

namespace alcedo {

class CommitGraph;
class Hash128;
class MiniGitWorkingHistory;
class PipelineDocument;
struct EditorMiniGitSaveCapture;
struct EditorAdjustmentPatch;
struct AdjustmentTransferPackage;
struct AdjustmentMergePreview;
struct AdjustmentMergeResolution;
struct AdjustmentMergeResult;
struct AdjustmentPasteResult;

/// Narrow ports used by EditorSessionService. Production implementations wrap
/// PipelineMgmtService, Mini-Git journal storage, thumbnail work, and background tasks.
/// Tests inject fakes. The service never exposes these ports to QML modules.

struct EditorHistoryGuardHandle {
  sl_element_id_t element_id = 0;
  bool            valid      = false;
};

/**
 * @brief Read side of the editor's working document.
 *
 * The history port takes the editor lease and owns the working document; this port only exposes
 * the preview snapshot the history publishes after each write, for readers off the session owner
 * thread (the GUI projections and typed write targets).
 */
class IEditorPipelinePort {
 public:
  virtual ~IEditorPipelinePort() = default;
  /**
   * @brief Last preview snapshot published from the working document of @p element_id.
   *
   * Immutable; safe on any thread. Null when the editor holds no lease for the image. Default
   * fakes return null.
   */
  [[nodiscard]] virtual auto CurrentPreview(sl_element_id_t /*element_id*/) const
      -> std::shared_ptr<const PipelineGraphSnapshot> {
    return nullptr;
  }
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
  /**
   * @brief Restore applied provisional fields without committing history.
   *
   * Clears the current input-sequence before-values and writes them back to the
   * live document and executor. Does not move the working head. Default is a
   * no-op success so fakes can opt in.
   *
   * @return false on restore failure. @p live_changed is true when any
   *         provisional field was present before the restore.
   */
  virtual auto RestoreUnsettledPreview(const EditorHistoryGuardHandle& /*guard*/,
                                       bool* live_changed, std::string* /*error*/) -> bool {
    if (live_changed != nullptr) {
      *live_changed = false;
    }
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
  /// Apply one net node-graph topology delta in place. Default rejects.
  virtual auto EditNodeGraph(const EditorHistoryGuardHandle& /*guard*/,
                             NodeGraphTopologyChange /*change*/, std::string* error) -> bool {
    if (error != nullptr) *error = "Node graph topology edit is not supported by this history port";
    return false;
  }
  /// Rename one Color Grade without requesting a pixel render.
  virtual auto RenameColorGrade(const EditorHistoryGuardHandle& /*guard*/,
                                const NodeId& /*node_id*/, std::string /*display_name*/,
                                std::string* error) -> bool {
    if (error != nullptr) *error = "Color Grade rename is not supported by this history port";
    return false;
  }
  /// Commit deletion-only metadata without rendering. Equal values succeed without a commit.
  /// @p changed reports an effective commit, or false on no-op/failure. Default rejects.
  virtual auto SetColorGradeDeletionProtected(const EditorHistoryGuardHandle& /*guard*/,
                                              const NodeId& /*node_id*/, bool /*deletion_protected*/,
                                              std::string* error, bool* changed = nullptr) -> bool {
    if (changed) *changed = false;
    if (error) *error = "Color Grade deletion protection is not supported by this history port";
    return false;
  }
  /// Insert one clean Color Grade at the top of the Mask Groups stack,
  /// directly before DRT/Post, as one typed history commit. The current
  /// predecessor of DRT/Post must equal @p expected_predecessor_id; a mismatch
  /// rejects the stale request without document, counter, or history changes.
  /// Default rejects.
  virtual auto InsertColorGradeAtTop(const EditorHistoryGuardHandle& /*guard*/,
                                     const NodeId& /*new_id*/,
                                     const NodeId& /*expected_predecessor_id*/, std::string* error)
      -> bool {
    if (error != nullptr) {
      *error = "Color Grade top insertion is not supported by this history port";
    }
    return false;
  }
  /// Remove one Color Grade and bridge its scene-image neighbors as one typed
  /// history commit. Develop and DRT/Post are never removable. Default rejects.
  virtual auto RemoveColorGradeAndBridge(const EditorHistoryGuardHandle& /*guard*/,
                                         const NodeId& /*node_id*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Color Grade bridge removal is not supported by this history port";
    }
    return false;
  }

  using MaskSettle = std::function<bool(const PipelineEditBatch& batch, std::string* error)>;
  /**
   * @brief Operation on the working document.
   *
   * Before returning (also on failure) the operation sets @p input_open to whether its input
   * sequence is still open, that is, whether the working document now holds values that are not
   * committed. The history treats those values exactly like a pending slider sequence: nothing
   * is saved and no committed snapshot is published until the sequence settles or is cancelled.
   */
  using MaskDocumentOp =
      std::function<bool(PipelineDocument& document, MiniGitWorkingHistory& history,
                         const MaskSettle& settle, bool* input_open, std::string* error)>;

  /**
   * @brief Run @p op on the working document of the session owner thread.
   *
   * @p settle publishes a typed batch whose working document already holds the after values.
   * The history publishes the resulting preview (and, when the sequence is closed, committed)
   * snapshot after @p op returns. Takes no render lock. Default fakes reject.
   */
  virtual auto WithWorkingDocument(const EditorHistoryGuardHandle& /*guard*/,
                                   const MaskDocumentOp& /*op*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Working document access is not supported by this history port";
    }
    return false;
  }

  virtual auto Undo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool = 0;
  virtual auto Redo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool = 0;
  /// Render reason published by the last successful history mutation. Default
  /// UndoRedo keeps fakes rendering. Production returns nullopt for rename-only
  /// batches so the session can skip a pipeline render while still bumping the
  /// history revision.
  [[nodiscard]] virtual auto LastPublishedRenderReason() const
      -> std::optional<EditorRenderReason> {
    return EditorRenderReason::UndoRedo;
  }
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
  /**
   * @brief Copy load-only panel values for the GUI.
   *
   * Default is an empty projection so fakes that do not own a document can skip
   * this path. Production reads the owner-copied fields, not Model JSON.
   */
  virtual auto ReadPanelProjection(const EditorHistoryGuardHandle& /*guard*/,
                                   EditorPanelProjection* projection, std::string* /*error*/)
      -> bool {
    if (projection != nullptr) {
      *projection = {};
    }
    return true;
  }

  /**
   * @brief Replace load-only panel values with the selected node's fields.
   *
   * Does not mutate parameters, commit history, or request a photo render.
   * Must not wait on an inflight present handshake.
   * Default fakes succeed without storing a node.
   */
  virtual auto SetPanelProjectionNode(const EditorHistoryGuardHandle& /*guard*/,
                                      const NodeId& /*node_id*/,
                                      std::uint64_t /*session_generation*/,
                                      std::string* /*error*/) -> bool {
    return true;
  }

  /// Switch the checked-out Version after a successful save checkpoint. Rebuilds
  /// the live document from root + first-parent chain and refreshes the panel
  /// projection. Default rejects so fakes must opt in.
  /// Fail closed: prior Version and pipeline remain published on failure.
  /// `version_id` is a Version ref identity (Hash128 / version_ref_id_t).
  virtual auto CheckoutVersion(const EditorHistoryGuardHandle& /*guard*/,
                               const Hash128& /*version_id*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Version checkout is not supported by this history port";
    }
    return false;
  }

  /// Copy the owned image's CommitGraph and immutable root document for a
  /// read-only consumer (Copy Adjustments). The copies are detached from the
  /// live history, so the consumer never loads or observes the editor state.
  virtual auto SnapshotHistorySource(const EditorHistoryGuardHandle& /*guard*/,
                                     std::shared_ptr<const CommitGraph>* /*graph*/,
                                     std::shared_ptr<const PipelineDocument>* /*root_document*/,
                                     std::string* error) -> bool {
    if (error != nullptr) {
      *error = "History source snapshot is not supported by this history port";
    }
    return false;
  }

  /**
   * @brief Build both comparison inputs of the held image without changing its history.
   *
   * Resolves @p a and @p b against the held CommitGraph and immutable root, and gives every
   * replayed state the sensor settings of @p captured_current (BuildEditorComparisonInputs).
   * Moves no head, writes no commit, WAL record, or checkpoint, and does not touch the working
   * document, its dirty state, or the published previews. Session owner thread.
   *
   * @param captured_current Preview of the working values captured when the comparison opened.
   *        It must belong to the held image and to the lineage of its working document.
   * @return true with @p pair set to both inputs; false with @p error set and @p pair unchanged.
   *         Default ports reject.
   */
  virtual auto BuildComparisonInputs(
      const EditorHistoryGuardHandle& /*guard*/,
      const std::shared_ptr<const PipelineGraphSnapshot>& /*captured_current*/,
      const EditorComparisonSource& /*a*/, const EditorComparisonSource& /*b*/,
      EditorComparisonInputPair* /*pair*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Comparison inputs are not supported by this history port";
    }
    return false;
  }

  /// Read only the active Version identity. This avoids constructing the
  /// Versions/commits projection when a consumer only needs a layout key or
  /// stale-session check.
  virtual auto ReadActiveVersionId(const EditorHistoryGuardHandle& /*guard*/,
                                   version_ref_id_t* /*version_id*/, std::string* error) -> bool {
    if (error != nullptr) {
      *error = "Active Version identity is not supported by this history port";
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

  /// Return whether the working document of the image holds a value that no commit records: an
  /// open field input sequence or an open Mask edit. Test ports that do not model input
  /// sequences may keep the default false.
  [[nodiscard]] virtual auto HasUncommittedLiveValues(
      const EditorHistoryGuardHandle& /*guard*/) const -> bool {
    return false;
  }

  /// Return whether the active working head differs from the last materialized
  /// head. The editor uses this to enable the current-image discard action.
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


  /// Paste onto the live CommitGraph and WAL, then apply package operators to
  /// the live pipeline. Default fake records success without mutating state.
  virtual auto PasteLiveRootRelativeVersion(const EditorHistoryGuardHandle& /*guard*/,
                                            const AdjustmentTransferPackage& package,
                                            std::string version_display_name,
                                            AdjustmentPasteResult* result, std::string* error)
      -> bool {
    if (result == nullptr) {
      if (error != nullptr) *error = "Paste result storage is required";
      return false;
    }
    if (package.Empty()) {
      if (error != nullptr) *error = "Adjustment transfer package is empty";
      return false;
    }
    result->pasted = true;
    result->prior_version_id = {};
    result->new_version_id = Hash128{0xA57E000000000001ULL, 0xA57E000000000002ULL};
    (void)version_display_name;
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

struct EditorMaterializeOutcome {
  bool          accepted                        = false;
  bool          materialized                    = false;
  std::uint64_t materialized_operation_sequence = 0;
  std::string   error;
};

using EditorMaterializeCallback = std::function<void(EditorMaterializeOutcome)>;

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

  /// Optional asynchronous recovery entry point. Normal editor Open remains on
  /// the synchronous path until pipeline installation and presentation no
  /// longer form a cross-thread wait cycle.
  virtual auto RecoverAndMaterializeAsync(sl_element_id_t element_id,
                                          std::uint64_t   session_generation,
                                          EditorMaterializeCallback callback) -> bool {
    std::string error;
    auto        outcome = RecoverAndMaterialize(element_id, session_generation, &error);
    if (outcome.error.empty()) outcome.error = std::move(error);
    if (callback) callback(std::move(outcome));
    return true;
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
  /// Phase 5E: image-load request the coordinator currently accepts.
  std::uint64_t                     image_load_request_id = 0;
  /// Last request that was rejected at Submit (image load/token/scheduler).
  std::string                       last_rejection_reason;
  std::optional<EditorRenderReason> last_rejected_render_reason{};
  /// Last intent whose blocking render published a frame ready for composition.
  std::optional<FrameRole>          last_ready_frame_role{};
  std::optional<EditorRenderReason> last_ready_render_reason{};
  /// Monotonic counters of terminal outcomes for tests/diagnostics.
  std::size_t                       accepted_count  = 0;
  std::size_t                       failed_count    = 0;
  std::size_t                       ready_count     = 0;
};

/// Immutable render command. Built by the facade or edit controller and passed
/// to the render controller; the render controller does not read adjustment
/// state from any other component.
struct EditorRenderCommand {
  EditorRenderReason                  reason       = EditorRenderReason::InitialFrame;
  std::uint64_t                       operation_id = 0;
  std::optional<ViewportRenderRegion> view_region;
};

/// Sole path from the session service into pipeline work. Production wraps
/// EditorRenderCoordinator; tests may inject a recording stub.
class IEditorRenderSubmitPort {
 public:
  using SessionIdleCallback = std::function<void(std::uint64_t)>;

  virtual ~IEditorRenderSubmitPort()                                          = default;
  virtual auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult = 0;
  virtual void CancelSession(std::uint64_t session_generation)                = 0;
  /// Cancel without blocking the caller, then report when no scheduler work
  /// still owns this render session. Fakes with synchronous work complete
  /// immediately through the default adapter.
  virtual void CancelSession(std::uint64_t session_generation,
                             SessionIdleCallback on_idle) {
    CancelSession(session_generation);
    if (on_idle) {
      on_idle(session_generation);
    }
  }
  /// Stamps the active image-load request and cancels pending/in-flight work for
  /// other image-load requests.
  virtual void SetActiveImageLoadRequest(std::uint64_t image_load_request_id) = 0;
  /// Bind stable session render inputs at open/switch (epoch + element + image +
  /// presentation sink identity). Production adapter loads image/buffer once; fakes no-op.
  virtual void BindSessionRenderContext(std::uint64_t /*epoch*/, sl_element_id_t /*element_id*/,
                                        image_id_t /*image_id*/,
                                        PresentationSinkId /*presentation_sink_id*/ = 0) {}
  /// Clear bound session render inputs (close / pre-switch reset).
  virtual void ClearSessionRenderContext() {}
  /// Phase 5D diagnostics. Default impls report an idle coordinator so test
  /// fakes that do not override them stay QML-idle.
  [[nodiscard]] virtual auto diagnostics() const -> EditorRenderCoordinatorDiagnostics {
    return {};
  }
};

}  // namespace alcedo
