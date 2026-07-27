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

struct PipelineGuard {
  std::shared_ptr<CPUPipelineExecutor> pipeline_;
  sl_element_id_t                      id_;
  bool                                 dirty_     = false;
  bool                                 pinned_    = false;
  size_t                               pin_count_ = 0;

  // The editor's live runtime snapshot. These values describe history only;
  // executor stages, GPU resources, and render/cache state stay on pipeline_.
  root_id_t                            root_id_{};
  head_commit_hash_t                   working_head_commit_hash_ = std::nullopt;
  transaction_chain_hash_t             transaction_chain_hash_{};
  bool                                 serialized_state_needs_writeback_ = false;

  // The validated graph backing the focused editor pipeline. Its Version ref
  // advances for journaled edits while ImageEditState.materialized_* remains
  // at the last DuckDB checkpoint.
  std::shared_ptr<CommitGraph>         commit_graph_;
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

  /// Load an editor pipeline and validate its serialized state against the immutable commit graph.
  /// Matching state is imported directly. Stale state is rebuilt from the persisted root and
  /// first-parent commits and written back when the guard is returned.
  /// Thumbnail and export callers must continue to use LoadPipeline so they do not repeat this
  /// editor-only history validation.
  auto               LoadEditorPipeline(sl_element_id_t id) -> std::shared_ptr<PipelineGuard>;

  /// Persist the current metadata-resolved pipeline as the immutable root for a newly imported
  /// image. Calling this again for an image that already has a root verifies and loads that root;
  /// it never replaces the stored root state.
  void               InitializeImageRoot(const std::shared_ptr<PipelineGuard>& pipeline,
                                         const RawRuntimeColorContext*         raw_color_context = nullptr);

  /// Switch the live editor pipeline to another Version on the same image.
  ///
  /// Preconditions: `pipeline` is a loaded editor guard with a commit graph. The caller has already
  /// completed a save checkpoint so the working journal is empty for this image.
  ///
  /// Behavior: sets the active Version, rebuilds the executor from the immutable root plus the
  /// first-parent chain under the render lock, then updates working head and chain hash. On any
  /// failure the prior Version remains active and the prior pipeline is restored — never publishes
  /// a partially reconstructed pipeline.
  ///
  /// @return true when the Version and pipeline both match the checked-out head.
  auto CheckoutVersion(const std::shared_ptr<PipelineGuard>& pipeline,
                       const version_ref_id_t& version_id, std::string* error = nullptr) -> bool;

  /// Rebuild the executor from the immutable root and the first-parent chain of the
  /// currently active Version. The caller may use this after changing the in-memory graph
  /// before the graph is materialized.
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
}  // namespace alcedo
