//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/committed_snapshot_cache.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/pipeline_root_state.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"

namespace alcedo {
namespace {

auto MatchesLabel(const PipelineGraphSnapshot& snapshot, const MaterializedHistoryLabel& label)
    -> bool {
  return snapshot.Head() == label.head_commit_hash &&
         snapshot.Chain() == label.transaction_chain_hash;
}

auto ReadMaterializedLabel(Storage& storage, sl_element_id_t element_id)
    -> std::optional<MaterializedHistoryLabel> {
  auto             db_guard = storage.GetDatabase().GetConnectionGuard();
  auto             db_lock  = db_guard.Lock();
  CommitGraphStore graph_service(db_guard.conn_);
  return graph_service.GetMaterializedHistoryLabel(element_id);
}

}  // namespace

auto LoadCommittedSnapshotFromStorage(Storage& storage, sl_element_id_t element_id)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  std::optional<ImageEditState>             state;
  std::optional<LoadedRootState>            root_state;
  std::optional<PipelineDocumentCheckpoint> checkpoint;
  std::optional<CommitGraph>                graph;
  {
    auto             db_guard = storage.GetDatabase().GetConnectionGuard();
    auto             db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    state = graph_service.GetImageEditState(element_id);
    if (!state.has_value()) {
      throw std::runtime_error("PipelineMgmtService: image " + std::to_string(element_id) +
                               " has no edit history root");
    }
    const auto root_encoded =
        graph_service.GetRootSerializedPipelineState(element_id, state->root_id);
    if (!root_encoded.has_value()) {
      throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                               std::to_string(element_id));
    }
    root_state = TryDecodeRootState(*root_encoded, element_id, state->root_id);
    if (!root_state.has_value()) {
      throw std::runtime_error("PipelineMgmtService: immutable root state is invalid for image " +
                               std::to_string(element_id));
    }
    if (state->serialized_pipeline_state.has_value()) {
      auto stored = TryDecodeCheckpoint(*state->serialized_pipeline_state);
      if (stored.has_value() && stored->root_id == state->root_id &&
          stored->head_commit_hash == state->materialized_head_commit_hash &&
          stored->transaction_chain_hash == state->materialized_transaction_chain_hash) {
        checkpoint = std::move(stored);
      }
    }
    if (!checkpoint.has_value()) {
      graph = graph_service.LoadGraph(element_id);
      if (!graph.has_value()) {
        throw std::runtime_error("PipelineMgmtService: edit history is missing for image " +
                                 std::to_string(element_id));
      }
    }
  }

  BindSourceDngProfiles(storage, element_id, *root_state);
  std::shared_ptr<PipelineDocument> document;
  if (checkpoint.has_value()) {
    document = BuildDocumentFromCheckpoint(*checkpoint, *root_state);
  } else {
    std::string replay_error;
    document = BuildDocumentFromRoot(*graph, *root_state, state->materialized_head_commit_hash,
                                     &replay_error);
    if (!document) {
      throw std::runtime_error("PipelineMgmtService: committed document replay failed for image " +
                               std::to_string(element_id) + ": " + replay_error);
    }
  }
  ValidateProductDocument(*document, element_id);
  return PipelineGraphSnapshot::Committed(
      std::move(document), element_id, PipelineLineageId::Next(),
      state->materialized_head_commit_hash, state->materialized_transaction_chain_hash);
}

CommittedSnapshotCache::CommittedSnapshotCache(std::shared_ptr<Storage> storage,
                                               std::size_t              capacity)
    : storage_(std::move(storage)), stored_order_(capacity) {
  if (!storage_) {
    throw std::invalid_argument("CommittedSnapshotCache: storage is null");
  }
}

auto CommittedSnapshotCache::Acquire(sl_element_id_t element_id, bool editor_holds_image)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  std::shared_ptr<const PipelineGraphSnapshot> stored;
  {
    std::scoped_lock lock(mutex_);
    if (editor_holds_image) {
      if (const auto it = published_.find(element_id); it != published_.end()) {
        return it->second;
      }
    }
    if (const auto it = stored_.find(element_id); it != stored_.end()) {
      stored = it->second;
    }
  }

  if (stored) {
    const auto label = ReadMaterializedLabel(*storage_, element_id);
    if (label.has_value() && MatchesLabel(*stored, *label)) {
      std::scoped_lock lock(mutex_);
      stored_order_.AccessElement(element_id);
      return stored;
    }
  }

  auto             loaded = LoadCommittedSnapshotFromStorage(*storage_, element_id);
  std::scoped_lock lock(mutex_);
  ++storage_load_count_;
  StoreLocked(loaded);
  return loaded;
}

void CommittedSnapshotCache::Publish(std::shared_ptr<const PipelineGraphSnapshot> snapshot,
                                     bool editor_holds_image) {
  if (!snapshot || !snapshot->IsCommitted()) {
    throw std::invalid_argument(
        "CommittedSnapshotCache: only committed snapshots can be published");
  }
  std::scoped_lock lock(mutex_);
  if (editor_holds_image) {
    published_[snapshot->ElementId()] = std::move(snapshot);
    return;
  }
  StoreLocked(std::move(snapshot));
}

void CommittedSnapshotCache::EndEditorPublication(sl_element_id_t element_id) {
  std::scoped_lock lock(mutex_);
  const auto       it = published_.find(element_id);
  if (it == published_.end()) {
    return;
  }
  auto snapshot = std::move(it->second);
  published_.erase(it);
  StoreLocked(std::move(snapshot));
}

void CommittedSnapshotCache::Forget(sl_element_id_t element_id) {
  std::scoped_lock lock(mutex_);
  published_.erase(element_id);
  stored_.erase(element_id);
  stored_order_.RemoveRecord(element_id);
}

auto CommittedSnapshotCache::StorageLoadCount() const -> std::size_t {
  std::scoped_lock lock(mutex_);
  return storage_load_count_;
}

void CommittedSnapshotCache::StoreLocked(std::shared_ptr<const PipelineGraphSnapshot> snapshot) {
  const auto element_id = snapshot->ElementId();
  const auto evicted    = stored_order_.RecordAccess_WithEvict(element_id, element_id);
  if (evicted.has_value()) {
    stored_.erase(*evicted);
  }
  stored_[element_id] = std::move(snapshot);
}

}  // namespace alcedo
