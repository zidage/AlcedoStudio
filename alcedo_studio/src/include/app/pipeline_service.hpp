//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "app/committed_snapshot_cache.hpp"
#include "app/image_pool_service.hpp"
#include "app/pipeline_root_state.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "json.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"

namespace alcedo {


/// Live editor handle: one pipeline executor (render lock + renderers) bound to the live
/// document, plus a pointer to the image's CommitGraph.
///
/// Binding identity model (see commit_types.hpp and the single-live-pipeline roadmap
/// "Final locked identity model"):
/// - pipeline_ holds no document. Each render task freezes document_ under the render lock
///   (FreezeLiveSnapshot) and passes that snapshot to Apply. It owns no parameters and does not
///   own HEAD.
/// - commit_graph_ is the sole owner of Version tips (working head).
/// - working_head_commit_hash() / transaction_chain_hash() are convenience reads of
///   the active Version tip and its first-parent chain fold. They are not independent
///   caches; never write a parallel head field onto this guard.
/// - On commit, history advances head once and folds chain hash once.
/// - Serialized checkpoint identity is (root, head, chain, document). Load compares
///   that label to the history tip; match loads the document and skips first-parent replay.
struct PipelineGuard {
  std::shared_ptr<PipelineExecutor>       pipeline_;
  /// Authoritative live pipeline DAG. Written under pipeline_'s render lock; renders read a
  /// frozen copy of it (FreezeLiveSnapshot).
  std::shared_ptr<PipelineDocument>    document_;
  /// History identity of document_. A new value each time document_ is replaced (load and
  /// BindLivePipelineDocument), so the executor releases the resources of the previous document.
  PipelineLineageId                    lineage_;
  sl_element_id_t                      id_;
  bool                                 dirty_     = false;
  /// Cache pin only: LoadPipeline / ReleasePipelineUse / SavePipeline refcount so
  /// LRU eviction and "unpinned → re-init executor" do not drop a live editor
  /// guard. Live-pipeline *mutation* ownership is PipelineExecutor::render_lock_
  /// (held for the full render task including present); pin_count_ is not that.
  bool                                 pinned_    = false;
  size_t                               pin_count_ = 0;
  /// False until construct/reinit finished. Cache hits wait; never treat an unready live as usable.
  bool                                 live_ready_ = false;
  bool                                 initializing_ = false;
  std::exception_ptr                   load_error_;
  /// True while live values are not a history HEAD. Saves and checkpoints refuse to persist the
  /// live document while it is set. The editor does not use guards (see EditorHistoryLease).
  bool                                    unsettled_preview_ = false;

  /// Immutable root id for this image's edit graph (history identity, not a tip).
  root_id_t                            root_id_{};
  /// Immutable replay start for Version checkout and recovery. Loaded with the
  /// stored root document; never mutated after the image enters history.
  std::shared_ptr<const PipelineDocument> root_document_;
  bool                                 serialized_state_needs_writeback_ = false;

  /// Sole live CommitGraph for this element. Active Version head is the only logical
  /// working head. Advances: MoveWorkingHead / SetActiveVersionId / PublishPrepared*.
  std::shared_ptr<CommitGraph>         commit_graph_;

  /// Active Version tip on commit_graph_ (history-owned). Empty graph → nullopt.
  [[nodiscard]] auto                   working_head_commit_hash() const -> head_commit_hash_t {
    if (!commit_graph_) {
      return std::nullopt;
    }
    return commit_graph_->GetActiveVersionRef().head_commit_hash;
  }

  /**
   * @brief Freeze document_ into the snapshot that one render task passes to Apply.
   *
   * Transitional until the editor owns its executor: the snapshot is a preview (no HEAD, empty
   * chain) because document_ may hold uncommitted editor values, and only the editor renders it
   * (thumbnails, analysis, and export render committed snapshots, see
   * PipelineMgmtService::AcquireCommittedSnapshot). The chain is left empty because
   * the render thread must not read commit_graph_, which the history owner replaces without this
   * lock.
   *
   * @pre Caller holds pipeline_->GetRenderLock(); document_ is set and lineage_ is not empty.
   * @throws std::invalid_argument when document_ is null or lineage_ is empty.
   */
  [[nodiscard]] auto FreezeLiveSnapshot() const -> std::shared_ptr<const PipelineGraphSnapshot>;

  /// First-parent chain fold for the active tip. Same algorithm history uses when
  /// recording commits; used as the checkpoint label next to the saved document.
  [[nodiscard]] auto transaction_chain_hash() const -> transaction_chain_hash_t {
    if (!commit_graph_) {
      return {};
    }
    return commit_graph_->ChainHashForHead(working_head_commit_hash());
  }
};

/**
 * @brief Materialized history of one image, read from storage for a history user other than the
 *        editor (the Copy source and the Paste targets in the library).
 *
 * Why a copy: the Copy dialog reads the graph after the call returns while other writers may
 * change storage, and a Paste target edits its own graph and persists it only when the whole
 * paste succeeded. Captured: the CommitGraph at its materialized state and the decoded immutable
 * root with the image DNG profile bound. Both are immutable; a Paste copies graph_ into a private
 * CommitGraph before it edits. Released with the last reference; nothing is written back except
 * through @ref PipelineMgmtService::PersistHistory, which checks graph_ against storage first.
 */
struct ImageHistorySnapshot {
  std::shared_ptr<const CommitGraph>     graph_;
  std::shared_ptr<const LoadedRootState> root_;
};

/**
 * @brief History of one image that the editor session takes over with its lease.
 *
 * Returned by @ref PipelineMgmtService::AcquireEditorLease. Not a copy of shared state: the
 * session becomes the only owner of the image's history and working document until it releases
 * the lease, and no other module loads or changes them in that time.
 * - graph_: the materialized CommitGraph; the session appends its commits to it.
 * - root_: the decoded immutable root with the image DNG profile bound; the start of every replay.
 * - document_: the document of the active Version head, from the matching checkpoint or from a
 *   replay of the root; the session's working document.
 */
struct EditorHistoryLease {
  CommitGraph                            graph_;
  std::shared_ptr<const LoadedRootState> root_;
  std::shared_ptr<PipelineDocument>      document_;
};

class PipelineMgmtService final {
 private:
  std::shared_ptr<Storage>                                            storage_;

  LRUCache<sl_element_id_t, sl_element_id_t>                          pipeline_cache_;

  std::unordered_map<sl_element_id_t, std::shared_ptr<PipelineGuard>> loaded_pipelines_;

  std::mutex                                                          lock_;
  std::condition_variable                                             cache_cv_;

  std::uint64_t                pipeline_construct_count_ = 0;
  std::uint64_t                pipeline_load_count_      = 0;

  static constexpr size_t                                             default_cache_capacity_ = 16;

  AcceleratorBackendPreference accelerator_preference_ = AcceleratorBackendPreference::Auto;

  std::uint64_t                editor_pipeline_history_rebuild_count_ = 0;

  CommittedSnapshotCache       committed_snapshots_;

  /// Images whose history the editor session holds (AcquireEditorLease .. ReleaseEditorLease).
  /// The single-writer lease table, guarded by lock_. It carries no executor and no document.
  std::unordered_set<sl_element_id_t> editor_leases_;

  /// True while the editor session holds the lease of @p id.
  [[nodiscard]] auto           EditorHoldsImage(sl_element_id_t id) -> bool;

  /// Write the document of @p snapshot as the element pipeline JSON, kept for older versions of
  /// the application (plan decision 3). The history in storage stays the source of truth.
  void                         WriteElementPipelineJson(const PipelineGraphSnapshot& snapshot);

  void                         HandleEviction(sl_element_id_t evicted_id);
  void                         SyncDirtyPipelineDocument(
      const std::shared_ptr<PipelineGuard>& pipeline);
  void CleanupIdlePipelineResources(const std::shared_ptr<PipelineGuard>& pipeline);

 public:
  PipelineMgmtService() = delete;
  explicit PipelineMgmtService(std::shared_ptr<Storage> storage_service)
      : storage_(storage_service),
        pipeline_cache_(default_cache_capacity_),
        loaded_pipelines_(),
        committed_snapshots_(storage_service) {}

  void               SavePipeline(std::shared_ptr<PipelineGuard> pipeline);

  /**
   * @brief Unpin a live pipeline without writing storage or clearing dirty.
   *
   * Callers that must not persist the live guard call this instead of @ref SavePipeline.
   * When other pins remain (the editor), GPU session caches stay. When this is
   * the last pin, the executor releases the resources of its binding (both
   * renderers) so unused LRU entries do not keep VRAM. Must not be called while holding
   * @c PipelineExecutor::GetRenderLock().
   *
   * @param pipeline Guard returned by @ref LoadPipeline; no-op if null.
   */
  void               ReleasePipelineUse(std::shared_ptr<PipelineGuard> pipeline);

  auto               LoadPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard>;

  /**
   * @brief Committed pipeline graph snapshot of @p id, for renders that must not see
   *        uncommitted editor values (thumbnails, analysis, export).
   *
   * For the image the editor holds, returns the snapshot the editor published last
   * (@ref PublishCommitted); when it has published none yet, the stored state. For every other
   * image, builds the snapshot from the materialized history in storage, or reuses the cached one
   * while its head and chain still equal the stored labels. Never loads a PipelineGuard, never
   * takes a render lock, and never reads the element pipeline JSON.
   *
   * Thread: any thread; storage reads run on the calling thread, so call it off the UI thread.
   * @throws std::runtime_error when the image has no history root or its history cannot be built.
   */
  [[nodiscard]] auto AcquireCommittedSnapshot(sl_element_id_t id)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /**
   * @brief Publish the editor's snapshot of a committed state of the image it holds.
   *
   * Called by the editor history after each change of its committed state. Later
   * @ref AcquireCommittedSnapshot calls for that image return it until the next publication or
   * until the editor releases the image.
   * @throws std::invalid_argument when @p snapshot is null or not committed.
   */
  void               PublishCommitted(std::shared_ptr<const PipelineGraphSnapshot> snapshot);

  /**
   * @brief Read the materialized history and the immutable root of @p id from storage.
   *
   * For history users other than the editor: the Copy source and the Paste targets in the
   * library. Loads no PipelineGuard, constructs no executor, and changes no state.
   *
   * Thread: any thread; storage reads run on the calling thread.
   * @throws std::runtime_error when the editor session holds @p id (its history may have commits
   *         that storage does not have yet; read it through the session), when the image has no
   *         history root, or when the stored history or root cannot be decoded or disagrees with
   *         the active Version.
   */
  [[nodiscard]] auto LoadHistorySnapshot(sl_element_id_t id) -> ImageHistorySnapshot;

  /**
   * @brief Persist @p graph, an edited copy of @p base, as the history of its image in one
   *        storage transaction, and publish its committed snapshot.
   *
   * Replays the document of the active Version of @p graph from the root of @p base, then writes
   * the new commits, the Version refs, the image edit state, and the checkpoint of that document
   * together. The returned snapshot is also stored in the committed snapshot cache, so the next
   * thumbnail or export of the image renders it without a replay. Loads no PipelineGuard and
   * constructs no executor. The element pipeline JSON is not written.
   *
   * Thread: any thread; storage writes run on the calling thread.
   * @pre @p base came from @ref LoadHistorySnapshot for the same image as @p graph.
   * @throws std::runtime_error when the editor session holds the image, when @p graph belongs to
   *         another image or root, when replay fails, or when the stored history no longer equals
   *         the materialized state of @p base (another writer changed it). Storage is unchanged
   *         on every failure.
   */
  auto               PersistHistory(const ImageHistorySnapshot& base, const CommitGraph& graph)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /// Test/instrumentation: committed snapshots built from storage since construction.
  [[nodiscard]] auto CommittedSnapshotStorageLoadCount() const -> std::size_t {
    return committed_snapshots_.StorageLoadCount();
  }

  /**
   * @brief Wait until @p pipeline pin_count_ equals @p expected.
   * @pre Must not hold the cache lock or the pipeline render lock.
   * @return true if the count matched before @p timeout.
   */
  auto WaitUntilPinCount(const std::shared_ptr<PipelineGuard>& pipeline, size_t expected,
                         std::chrono::milliseconds timeout) -> bool;

  /// Test/instrumentation: executor+document constructions for cache misses.
  [[nodiscard]] auto PipelineConstructCount() const -> std::uint64_t {
    return pipeline_construct_count_;
  }
  /// Test/instrumentation: LoadPipeline returns, including cache hits and waiters.
  [[nodiscard]] auto PipelineLoadCount() const -> std::uint64_t { return pipeline_load_count_; }
  void               ResetPipelineAcquireCountsForTesting() {
    pipeline_construct_count_ = 0;
    pipeline_load_count_      = 0;
  }

  /** @brief Save the guard's authoritative GPU DAG document. */
  void               SyncPipelineDocument(const std::shared_ptr<PipelineGuard>& pipeline);

  /**
   * @brief Take the single-writer lease of @p id for the editor session and read its history.
   *
   * Reads the materialized history and the immutable root, then builds the document of the
   * active Version head: from the stored checkpoint when its root, head, and chain labels equal
   * the history tip, otherwise by replay from the root. While the lease is held, thumbnails,
   * analysis, and export read the snapshots the editor publishes (@ref PublishCommitted), and
   * @ref LoadHistorySnapshot and @ref PersistHistory refuse the image. Loads no PipelineGuard
   * and constructs no executor.
   *
   * Thread: any thread; storage reads run on the calling thread.
   * @throws std::runtime_error when the lease is already held, when the image has no history root
   *         (every imported image has one), or when its history or root cannot be decoded or
   *         replayed. The lease is not taken on failure.
   */
  [[nodiscard]] auto AcquireEditorLease(sl_element_id_t id) -> EditorHistoryLease;

  /**
   * @brief End the editor lease of @p id.
   *
   * The last committed snapshot the editor published becomes an ordinary cache entry that is
   * checked against storage, and its document is written as the element pipeline JSON. No effect
   * when the lease is not held.
   */
  void               ReleaseEditorLease(sl_element_id_t id);

  /**
   * @brief Persist the editor's @p graph and the checkpoint of @p document in one transaction.
   *
   * @p document must be the document of the active Version head of @p graph; the checkpoint is
   * labelled with that head and its chain. Writes only when storage still holds
   * @p expected_materialized_state, so a concurrent writer is never overwritten. On success the
   * materialized state of @p graph is advanced to what was written.
   *
   * Thread: the editor session owner thread (the only writer of @p graph).
   * @return false with @p error set when the lease is not held, the graph belongs to another
   *         image, the stored state changed, or the write failed. Storage and @p graph are
   *         unchanged on failure.
   */
  auto PersistEditorHistory(CommitGraph& graph, const ImageEditState& expected_materialized_state,
                            const PipelineDocument& document, std::string* error = nullptr) -> bool;

  /// Test/instrumentation counter: increments each time AcquireEditorLease rebuilds the document
  /// from first-parent history instead of importing the serialized checkpoint.
  [[nodiscard]] auto EditorPipelineHistoryRebuildCount() const -> std::uint64_t {
    return editor_pipeline_history_rebuild_count_;
  }
  void ResetEditorPipelineHistoryRebuildCountForTesting() {
    editor_pipeline_history_rebuild_count_ = 0;
  }

  /**
   * @brief Create the immutable history root of a newly imported image from @p document.
   *
   * Binds the camera profile onto the caller's private document: the source DNG profile, then
   * the RAW color context, or the Rec.709 working-space profile when @p raw_color_context is null
   * (non-RAW RGB files). Then writes the root state, the default Version, and the image edit state
   * in one storage transaction, and writes the document as the element pipeline JSON (kept for
   * older versions of the application). Loads no PipelineGuard and constructs no executor.
   *
   * @throws std::runtime_error when the document is not a valid product graph or the image
   *         already has a root (the stored root is never replaced). The element pipeline JSON is
   *         written only after the root.
   */
  void               InitializeImageRoot(sl_element_id_t id, PipelineDocument document,
                                         const RawRuntimeColorContext* raw_color_context);

  /// Clean project-exit garbage collection: mark from every Version head through first-parent
  /// reachability and delete unreachable EditCommit rows. Must run only after the final
  /// successful save; abnormal shutdown must not call this.
  /// @return number of deleted commit rows.
  auto               CollectUnreachableEditCommits() -> std::size_t;

  void               DeletePipeline(sl_element_id_t id);
  void               DeletePipelines(std::span<const sl_element_id_t> ids);

  void               SetAcceleratorBackendPreference(AcceleratorBackendPreference preference);
  [[nodiscard]] auto GetAcceleratorBackendPreference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }

  /// Write the element pipeline JSON of every dirty guard and of every image the editor holds
  /// (from its last published committed snapshot).
  void Sync();

  /// Persist only the requested live pipeline. This avoids saving unrelated dirty editor state.
  void SyncPipeline(sl_element_id_t id);
};

/// Checkpoint label: which history tip the serialized document claims to match.
/// Not a pipeline-owned head — only a tag stored next to the document blob.
struct PipelineCheckpointIdentity {
  head_commit_hash_t       head = std::nullopt;
  transaction_chain_hash_t chain{};
};

/// True when the checkpoint label on `state` matches the history tip after WAL attach.
/// Match ⇒ safe to import params without first-parent SetOperator replay.
[[nodiscard]] auto CheckpointMatchesLogicalHead(const ImageEditState&           state,
                                                head_commit_hash_t              logical_head,
                                                const transaction_chain_hash_t& logical_chain)
    -> bool;

/**
 * @brief Snapshot source for a render task that uses @p guard's shared executor.
 *
 * The returned function keeps @p guard alive and calls PipelineGuard::FreezeLiveSnapshot. The
 * scheduler calls it while it holds the render lock (PipelineTask::snapshot_under_render_lock_).
 */
[[nodiscard]] auto MakeLiveSnapshotSource(std::shared_ptr<const PipelineGuard> guard)
    -> std::function<std::shared_ptr<const PipelineGraphSnapshot>()>;

/**
 * @brief Swap step of build-then-swap: make @p document the guard's only writable document.
 *
 * Moves the pointer into the guard and takes a new lineage, so the next render on the guard's
 * executor releases every resource of the previous document. It does not copy, validate, or
 * replay anything, so callers build and bind the camera profile first and call this last. Does
 * not take the render lock.
 *
 * @pre @p document is not null. Caller holds the executor render lock when @p guard is live.
 * @param guard Loaded editor guard.
 * @param document Complete DAG that becomes the only writable document.
 * @return The document that was bound before the call (null when none was). A caller whose later
 *         step fails binds it back with a second call; the returned document was not changed.
 */
auto BindLivePipelineDocument(PipelineGuard&                    guard,
                              std::shared_ptr<PipelineDocument> document) noexcept
    -> std::shared_ptr<PipelineDocument>;

}  // namespace alcedo
