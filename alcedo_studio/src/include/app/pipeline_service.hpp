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

#include "app/image_pool_service.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/pipeline/pipeline.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "json.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"

namespace alcedo {

class MaskStore;

/// Live editor handle: one pipeline executor (parameter table + run state) plus a
/// pointer to the image's CommitGraph.
///
/// Binding identity model (see commit_types.hpp and the single-live-pipeline roadmap
/// "Final locked identity model"):
/// - pipeline_ is only the parameter table / executor. It does not own HEAD.
/// - commit_graph_ is the sole owner of Version tips (working head).
/// - working_head_commit_hash() / transaction_chain_hash() are convenience reads of
///   the active Version tip and its first-parent chain fold. They are not independent
///   caches; never write a parallel head field onto this guard.
/// - On commit (including merge), history advances head once and folds chain hash once.
///   Applying that commit to the table may call SetOperator many times; those calls are
///   not separate chain-hash steps.
/// - Serialized checkpoint identity is (root, head, chain, document). Load compares
///   that label to the history tip; match loads the document and skips first-parent replay.
struct PipelineGuard {
  std::shared_ptr<CPUPipelineExecutor> pipeline_;
  /// Authoritative pipeline DAG used by the CUDA product renderer.
  std::shared_ptr<PipelineDocument>    document_;
  sl_element_id_t                      id_;
  bool                                 dirty_     = false;
  /// Cache pin only: LoadPipeline / ReleasePipelineUse / SavePipeline refcount so
  /// LRU eviction and "unpinned → re-init stages" do not drop a live editor/export
  /// guard. Live-pipeline *mutation* ownership is CPUPipelineExecutor::render_lock_
  /// (held for the full render task including present); pin_count_ is not that.
  bool                                 pinned_    = false;
  size_t                               pin_count_ = 0;
  /// False until construct/reinit finished. Cache hits wait; never treat an unready live as usable.
  bool                                 live_ready_ = false;
  bool                                 initializing_ = false;
  std::exception_ptr                   load_error_;
  /// True while an editor input sequence has live values that are not a history HEAD.
  /// Thumbnail/export disk caches must not store pixels under the committed label.
  bool                                 unsettled_preview_ = false;

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

  /// First-parent chain fold for the active tip. Same algorithm history uses when
  /// recording commits; used as the checkpoint label next to the saved document.
  [[nodiscard]] auto transaction_chain_hash() const -> transaction_chain_hash_t {
    if (!commit_graph_) {
      return {};
    }
    return commit_graph_->ChainHashForHead(working_head_commit_hash());
  }
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

  void                         HandleEviction(sl_element_id_t evicted_id);
  void                         SyncDirtyPipelineDocument(
      const std::shared_ptr<PipelineGuard>& pipeline);
  void                         CleanupIdlePipelineResources(const std::shared_ptr<PipelineGuard>& pipeline);

 public:
  PipelineMgmtService() = delete;
  explicit PipelineMgmtService(std::shared_ptr<Storage> storage_service)
      : storage_(storage_service), pipeline_cache_(default_cache_capacity_), loaded_pipelines_() {}

  void               SavePipeline(std::shared_ptr<PipelineGuard> pipeline);

  /**
   * @brief Unpin a live pipeline without writing storage or clearing dirty.
   *
   * Thumbnail, analysis, and export must call this instead of @ref SavePipeline.
   * When other pins remain (the editor), GPU session caches stay. When this is
   * the last pin, intermediate GPU caches and the one-shot device are released
   * so unused LRU entries do not keep VRAM. Must not be called while holding
   * @c CPUPipelineExecutor::GetRenderLock().
   *
   * @param pipeline Guard returned by @ref LoadPipeline; no-op if null.
   */
  void               ReleasePipelineUse(std::shared_ptr<PipelineGuard> pipeline);

  /// Load image-local RAW color data, including DNG profiles absent from older projects.
  static void InjectImageRawMetadata(CPUPipelineExecutor& executor, const Image& image);

  /// Persist the current editor graph and serialized pipeline state while the
  /// caller keeps its editor guard pinned. `expected_materialized_state` is
  /// the state observed before the in-memory history mutation and prevents a
  /// concurrent writer from being overwritten.
  auto               PersistEditorHistoryState(const std::shared_ptr<PipelineGuard>& pipeline,
                                               const ImageEditState&                 expected_materialized_state,
                                               std::string*                          error = nullptr) -> bool;

  auto               LoadPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard>;

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

  /// Load editor document for `id` using history tip as authority.
  /// If checkpoint (document + root/head/chain labels) matches active Version tip, load the
  /// document (skip first-parent replay). Otherwise rebuild from root + first-parent typed
  /// batches and mark write-back. Thumbnail/export must use LoadPipeline.
  auto               LoadEditorPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard>;

  /// Test/instrumentation counter: increments each time LoadEditorPipeline rebuilds from
  /// first-parent history instead of importing the serialized checkpoint.
  [[nodiscard]] auto EditorPipelineHistoryRebuildCount() const -> std::uint64_t {
    return editor_pipeline_history_rebuild_count_;
  }
  void ResetEditorPipelineHistoryRebuildCountForTesting() {
    editor_pipeline_history_rebuild_count_ = 0;
  }

  /// Persist the current metadata-resolved document as the immutable root for a newly imported
  /// image. Calling this again for an image that already has a root verifies and loads that root;
  /// it never replaces the stored root state.
  void               InitializeImageRoot(const std::shared_ptr<PipelineGuard>& pipeline,
                                         const RawRuntimeColorContext*         raw_color_context = nullptr);

  /// Switch the live editor document to another Version on the same image.
  ///
  /// Preconditions: `pipeline` is a loaded editor guard with a commit graph. The caller has already
  /// completed a save checkpoint so the working journal is empty for this image.
  ///
  /// Behavior: resolves the target first-parent chain and Mask assets, then rebuilds the same live
  /// document from the immutable root plus typed batches under the render lock. On any failure the
  /// prior Version remains active and the prior document is restored. Does not invent a second head
  /// on the guard.
  ///
  /// @param mask_store Persistent Mask store used to verify Brush keys before the new head is
  ///        published. Null is accepted when the target document references no Mask assets.
  /// @return true when the Version tip and live document both match the checked-out head.
  auto               CheckoutVersion(const std::shared_ptr<PipelineGuard>& pipeline,
                                     const version_ref_id_t& version_id, std::string* error = nullptr,
                                     MaskStore* mask_store = nullptr) -> bool;

  /// Rebuild the live document from the immutable root and the first-parent chain of the
  /// currently active Version tip. Used when the checkpoint label does not match history.
  ///
  /// @param mask_store Persistent Mask store used to verify Brush keys. Null is accepted when
  ///        the rebuilt document references no Mask assets.
  auto               RebuildActiveEditorPipeline(const std::shared_ptr<PipelineGuard>& pipeline,
                                                 std::string*                          error = nullptr,
                                                 MaskStore* mask_store = nullptr) -> bool;

  /// Clean project-exit garbage collection: mark from every Version head through both parents and
  /// delete unreachable EditCommit rows. Must run only after the final successful save; abnormal
  /// shutdown must not call this.
  /// @return number of deleted commit rows.
  auto               CollectUnreachableEditCommits() -> std::size_t;

  void               DeletePipeline(sl_element_id_t id);
  void               DeletePipelines(std::span<const sl_element_id_t> ids);

  void               SetAcceleratorBackendPreference(AcceleratorBackendPreference preference);
  [[nodiscard]] auto GetAcceleratorBackendPreference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }

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
 * @brief Replace the guard's writable document and bind it to the existing executor.
 *
 * Does not clone @p document again. Does not take the render lock.
 *
 * @pre Caller holds the executor render lock when @p guard is live.
 * @param guard Loaded editor guard that already owns an executor.
 * @param document Complete DAG that becomes the only writable document.
 */
void BindLivePipelineDocument(PipelineGuard& guard, PipelineDocument document);

}  // namespace alcedo
