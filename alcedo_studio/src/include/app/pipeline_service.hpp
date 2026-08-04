//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "app/image_pool_service.hpp"
#include "decoders/processor/raw_color_context.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/pipeline/pipeline.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "json.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "sleeve/storage_service.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"

namespace alcedo {

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
/// - Serialized checkpoint identity is (head, chain, params). Load compares that label
///   to the history tip; match skips first-parent replay.
struct PipelineGuard {
  std::shared_ptr<CPUPipelineExecutor> pipeline_;
  sl_element_id_t                      id_;
  bool                                 dirty_     = false;
  /// Cache pin only: LoadPipeline / SavePipeline refcount so LRU eviction and
  /// "unpinned → re-init stages" do not drop a live editor/export guard.
  /// Live-pipeline *mutation* ownership is CPUPipelineExecutor::render_lock_
  /// (held for the full render task including present); pin_count_ is not that.
  bool                                 pinned_    = false;
  size_t                               pin_count_ = 0;

  /// Immutable root id for this image's edit graph (history identity, not a tip).
  root_id_t                            root_id_{};
  bool                                 serialized_state_needs_writeback_ = false;

  /// Sole live CommitGraph for this element. Active Version head is the only logical
  /// working head. Advances: MoveWorkingHead / SetActiveVersionId / PublishPrepared*.
  std::shared_ptr<CommitGraph>         commit_graph_;

  /// Active Version tip on commit_graph_ (history-owned). Empty graph → nullopt.
  [[nodiscard]] auto working_head_commit_hash() const -> head_commit_hash_t {
    if (!commit_graph_) {
      return std::nullopt;
    }
    return commit_graph_->GetActiveVersionRef().head_commit_hash;
  }

  /// First-parent chain fold for the active tip. Same algorithm history uses when
  /// recording commits; used as the checkpoint label next to exported params.
  [[nodiscard]] auto transaction_chain_hash() const -> transaction_chain_hash_t {
    if (!commit_graph_) {
      return {};
    }
    return commit_graph_->ChainHashForHead(working_head_commit_hash());
  }
};

// Phase 3: a read-only clone of a pipeline's params captured into an independent
// executor, used for background analysis rendering. The snapshot never pins the
// live PipelineGuard, never writes storage, and never clears the live guard's
// dirty state. Rendering on `executor_` does not affect the live pipeline.
struct PipelineSnapshot {
  sl_element_id_t                      element_id_ = 0;
  image_id_t                           image_id_   = 0;
  nlohmann::json                       pipeline_params_;
  std::shared_ptr<CPUPipelineExecutor> executor_;
};

class PipelineMgmtService final {
 private:
  std::shared_ptr<StorageService>                                     storage_service_;

  LRUCache<sl_element_id_t, sl_element_id_t>                          pipeline_cache_;

  std::unordered_map<sl_element_id_t, std::shared_ptr<PipelineGuard>> loaded_pipelines_;

  std::mutex                                                          lock_;

  static constexpr size_t                                             default_cache_capacity_ = 16;

  AcceleratorBackendPreference accelerator_preference_ = AcceleratorBackendPreference::Auto;

  std::uint64_t editor_pipeline_history_rebuild_count_ = 0;

  void                         HandleEviction(sl_element_id_t evicted_id);

 public:
  PipelineMgmtService() = delete;
  explicit PipelineMgmtService(std::shared_ptr<StorageService> storage_service)
      : storage_service_(storage_service),
        pipeline_cache_(default_cache_capacity_),
        loaded_pipelines_() {}

  void               SavePipeline(std::shared_ptr<PipelineGuard> pipeline);

  /// Persist the current editor graph and serialized pipeline state while the
  /// caller keeps its editor guard pinned. `expected_materialized_state` is
  /// the state observed before the in-memory history mutation and prevents a
  /// concurrent writer from being overwritten.
  auto PersistEditorHistoryState(const std::shared_ptr<PipelineGuard>& pipeline,
                                 const ImageEditState&                 expected_materialized_state,
                                 std::string* error = nullptr) -> bool;

  auto               LoadPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard>;

  /// Load editor params for `id` using history tip as authority.
  /// If checkpoint (params + head/chain label) matches active Version tip, import params
  /// (skip first-parent replay). Otherwise rebuild from root + first-parent chain and mark
  /// write-back. Thumbnail/export must use LoadPipeline (no editor history validation).
  auto               LoadEditorPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard>;

  /// Test/instrumentation counter: increments each time LoadEditorPipeline rebuilds from
  /// first-parent history instead of importing the serialized checkpoint.
  [[nodiscard]] auto EditorPipelineHistoryRebuildCount() const -> std::uint64_t {
    return editor_pipeline_history_rebuild_count_;
  }
  void ResetEditorPipelineHistoryRebuildCountForTesting() {
    editor_pipeline_history_rebuild_count_ = 0;
  }

  /// Persist the current metadata-resolved pipeline as the immutable root for a newly imported
  /// image. Calling this again for an image that already has a root verifies and loads that root;
  /// it never replaces the stored root state.
  void               InitializeImageRoot(const std::shared_ptr<PipelineGuard>& pipeline,
                                         const RawRuntimeColorContext*         raw_color_context = nullptr);

  /// Switch the live editor parameter table to another Version on the same image.
  ///
  /// Preconditions: `pipeline` is a loaded editor guard with a commit graph. The caller has already
  /// completed a save checkpoint so the working journal is empty for this image.
  ///
  /// Behavior: sets the active Version on the CommitGraph (history-owned head moves here), then
  /// rebuilds the executor params from the immutable root plus the first-parent chain under the
  /// render lock (or imports a matching checkpoint if present). On any failure the prior Version
  /// remains active and the prior pipeline is restored — never publishes a partially reconstructed
  /// pipeline. Does not invent a second head on the guard.
  ///
  /// @return true when the Version tip and pipeline params both match the checked-out head.
  auto CheckoutVersion(const std::shared_ptr<PipelineGuard>& pipeline,
                       const version_ref_id_t& version_id, std::string* error = nullptr) -> bool;

  /// Rebuild the executor params from the immutable root and the first-parent chain of the
  /// currently active Version tip. Used when the checkpoint label does not match history.
  auto RebuildActiveEditorPipeline(const std::shared_ptr<PipelineGuard>& pipeline,
                                   std::string* error = nullptr) -> bool;

  /// Clean project-exit garbage collection: mark from every Version head through both parents and
  /// delete unreachable EditCommit rows. Must run only after the final successful save; abnormal
  /// shutdown must not call this.
  /// @return number of deleted commit rows.
  auto CollectUnreachableEditCommits() -> std::size_t;

  void               DeletePipeline(sl_element_id_t id);
  void               DeletePipelines(std::span<const sl_element_id_t> ids);

  void               SetAcceleratorBackendPreference(AcceleratorBackendPreference preference);
  [[nodiscard]] auto GetAcceleratorBackendPreference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }

  void Sync();

  /// Persist only the requested live pipeline. This avoids saving unrelated dirty editor state.
  void SyncPipeline(sl_element_id_t id);

  // Phase 3: capture a read-only snapshot of the current pipeline state without
  // pinning the live guard, forcing it to disk, or touching dirty state. The
  // returned executor is an independent clone; rendering on it does not affect the
  // live pipeline. May briefly block on the live executor's render lock
  // (serializes with an in-flight editor render on the same executor). Returns
  // nullptr and writes *error on failure.
  auto LoadPipelineSnapshot(sl_element_id_t id, image_id_t image_id, std::string* error)
      -> std::shared_ptr<PipelineSnapshot>;

  // Release the snapshot executor's intermediate buffers (mirrors SavePipeline's
  // last-pin cleanup). Safe to call from the snapshot's task callback; the
  // shared_ptr then drops naturally. Not a storage write. No-op if null.
  void ReleasePipelineSnapshot(std::shared_ptr<PipelineSnapshot> snapshot);
};

/// Checkpoint label: which history tip the serialized params claim to match.
/// Not a pipeline-owned head — only a tag stored next to the parameter table blob.
struct PipelineCheckpointIdentity {
  head_commit_hash_t       head  = std::nullopt;
  transaction_chain_hash_t chain{};
};

/// True when the checkpoint label on `state` matches the history tip after WAL attach.
/// Match ⇒ safe to import params without first-parent SetOperator replay.
[[nodiscard]] auto CheckpointMatchesLogicalHead(const ImageEditState& state,
                                                head_commit_hash_t logical_head,
                                                const transaction_chain_hash_t& logical_chain)
    -> bool;

}  // namespace alcedo
