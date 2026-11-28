//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_service.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_root_state.hpp"
#include "app/source_dng_profile_binding.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/mask/ai_mask_asset_references.hpp"
#include "edit/mask/mask_model.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "storage/store/mask_asset/mask_asset_store.hpp"
#include "storage/store/storage_session.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {
/// True when storage still holds the materialized state that a writer read before its change.
auto SameMaterializedState(const MaterializedHistoryLabel& stored, const ImageEditState& expected)
    -> bool {
  return stored.root_id == expected.root_id &&
         stored.active_version_id == expected.active_version_id &&
         stored.head_commit_hash == expected.materialized_head_commit_hash &&
         stored.transaction_chain_hash == expected.materialized_transaction_chain_hash;
}

auto EditorOwnsHistoryError(sl_element_id_t id) -> std::string {
  return "PipelineMgmtService: image " + std::to_string(id) +
         " is open in the editor; its history is owned by the editor session";
}

/// Materialized history of one image and its decoded root, as storage holds them.
struct StoredHistory {
  CommitGraph     graph;
  LoadedRootState root;
};

/// One image of ReadStoredHistories: its history, or why it cannot be read.
struct StoredHistoryRead {
  std::optional<StoredHistory> history;
  std::string                  error;
};

/// Decode the stored root of the graph of @p id and check that the materialized state of
/// @p graph is its active Version. Reads no storage.
auto DecodeStoredHistory(sl_element_id_t id, CommitGraph graph,
                         const std::unordered_map<root_id_t, StoredRootPipelineState>& roots)
    -> StoredHistory {
  const auto root = roots.find(graph.GetRootId());
  if (root == roots.end()) {
    throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                             std::to_string(id));
  }
  if (root->second.element_id != id) {
    throw std::runtime_error("CommitGraphStore: root belongs to a different image");
  }
  if (root->second.serialized_pipeline_state.empty()) {
    throw std::runtime_error("CommitGraphStore: root serialized pipeline state is empty");
  }
  auto root_state = TryDecodeRootState(
      nlohmann::json::parse(root->second.serialized_pipeline_state), id, graph.GetRootId());
  if (!root_state.has_value()) {
    throw std::runtime_error("PipelineMgmtService: immutable root state is invalid for image " +
                             std::to_string(id));
  }

  const auto& state          = graph.GetImageEditState();
  const auto  expected_head  = graph.GetActiveVersionRef().head_commit_hash;
  const auto  expected_chain = graph.ChainHashForHead(expected_head);
  if (state.root_id != graph.GetRootId() || state.materialized_head_commit_hash != expected_head ||
      state.materialized_transaction_chain_hash != expected_chain) {
    throw std::runtime_error(
        "PipelineMgmtService: stored image edit state does not match the active Version");
  }
  return StoredHistory{std::move(graph), std::move(*root_state)};
}

/**
 * @brief Read the materialized histories of @p ids and their immutable roots, and bind the image
 *        DNG profile onto each root. The one storage read that the editor load, Copy, and Paste
 *        share.
 *
 * One storage session reads the rows of all images; the roots decode after it. An image that
 * has no root, whose root cannot be decoded, or whose materialized state does not match the
 * active Version gets an error in its entry. The entries follow the order of @p ids.
 * @pre The caller holds no database connection lock.
 * @throws std::runtime_error when a storage query fails.
 */
auto ReadStoredHistories(Storage& storage, std::span<const sl_element_id_t> ids)
    -> std::vector<StoredHistoryRead> {
  std::vector<StoredGraphRead>                           graphs;
  std::unordered_map<root_id_t, StoredRootPipelineState> roots;
  {
    auto  session       = storage.OpenSession();
    auto& graph_service = session.CommitGraph();
    graphs              = graph_service.LoadGraphs(ids);
    std::vector<root_id_t> root_ids;
    for (const auto& read : graphs) {
      if (read.graph.has_value()) {
        root_ids.push_back(read.graph->GetRootId());
      }
    }
    roots = graph_service.ListRootSerializedPipelineStates(root_ids);
  }

  std::vector<StoredHistoryRead> histories(ids.size());
  for (std::size_t index = 0; index < ids.size(); ++index) {
    const auto id    = ids[index];
    auto&      read  = graphs[index];
    auto&      entry = histories[index];
    try {
      if (!read.error.empty()) {
        throw std::runtime_error(read.error);
      }
      if (!read.graph.has_value()) {
        throw std::runtime_error("PipelineMgmtService: image " + std::to_string(id) +
                                 " has no edit history root");
      }
      auto history = DecodeStoredHistory(id, std::move(*read.graph), roots);
      BindSourceDngProfiles(storage, id, history.root);
      entry.history = std::move(history);
    } catch (const std::exception& e) {
      entry.error = e.what();
    }
  }
  return histories;
}

/// @ref ReadStoredHistories for one image.
/// @throws std::runtime_error when the history of @p id cannot be read.
auto ReadStoredHistory(Storage& storage, sl_element_id_t id) -> StoredHistory {
  auto  histories = ReadStoredHistories(storage, std::span<const sl_element_id_t>(&id, 1));
  auto& entry     = histories.front();
  if (!entry.history.has_value()) {
    throw std::runtime_error(entry.error);
  }
  return std::move(*entry.history);
}

}  // namespace

auto PipelineMgmtService::EditorHoldsImage(sl_element_id_t id) -> bool {
  std::unique_lock<std::mutex> lease_lock(lock_);
  return editor_leases_.contains(id);
}

void PipelineMgmtService::WriteElementPipelineJson(const PipelineGraphSnapshot& snapshot) {
  const auto& document = snapshot.Document();
  ValidateProductDocument(document, snapshot.ElementId());
  storage_->GetElementStore().UpdatePipelineJsonByElementId(snapshot.ElementId(),
                                                            document.ToJson());
}

auto PipelineMgmtService::AcquireCommittedSnapshot(sl_element_id_t id)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  return committed_snapshots_.Acquire(id, EditorHoldsImage(id));
}

void PipelineMgmtService::PublishCommitted(std::shared_ptr<const PipelineGraphSnapshot> snapshot) {
  if (!snapshot) {
    throw std::invalid_argument("PipelineMgmtService: cannot publish a null snapshot");
  }
  const auto id = snapshot->ElementId();
  committed_snapshots_.Publish(std::move(snapshot), EditorHoldsImage(id));
}

auto PipelineMgmtService::LoadHistorySnapshot(sl_element_id_t id) -> ImageHistorySnapshot {
  auto  reads = LoadHistorySnapshots(std::span<const sl_element_id_t>(&id, 1));
  auto& read  = reads.front();
  if (!read.snapshot.has_value()) {
    throw std::runtime_error(read.error);
  }
  return std::move(*read.snapshot);
}

auto PipelineMgmtService::LoadHistorySnapshots(std::span<const sl_element_id_t> ids)
    -> std::vector<ImageHistorySnapshotRead> {
  std::vector<ImageHistorySnapshotRead> reads(ids.size());
  std::vector<sl_element_id_t>          stored_ids;
  std::vector<std::size_t>              stored_indexes;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    reads[index].element_id = ids[index];
    if (EditorHoldsImage(ids[index])) {
      reads[index].error = EditorOwnsHistoryError(ids[index]);
      continue;
    }
    stored_ids.push_back(ids[index]);
    stored_indexes.push_back(index);
  }
  auto histories = ReadStoredHistories(*storage_, stored_ids);
  for (std::size_t stored = 0; stored < stored_indexes.size(); ++stored) {
    auto& read    = reads[stored_indexes[stored]];
    auto& history = histories[stored];
    if (!history.history.has_value()) {
      read.error = std::move(history.error);
      continue;
    }
    read.snapshot = ImageHistorySnapshot{
        .graph_ = std::make_shared<const CommitGraph>(std::move(history.history->graph)),
        .root_  = std::make_shared<const LoadedRootState>(std::move(history.history->root)),
    };
  }
  return reads;
}

auto PipelineMgmtService::PersistHistory(const ImageHistorySnapshot& base, const CommitGraph& graph)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  if (base.graph_ && EditorHoldsImage(graph.GetElementId())) {
    throw std::runtime_error(EditorOwnsHistoryError(graph.GetElementId()));
  }
  auto  write   = PrepareHistoryWrite(base, graph);
  auto  results = PersistHistoryWrites(std::span<PreparedHistoryWrite>(&write, 1));
  auto& result  = results.front();
  if (!result.snapshot) {
    throw std::runtime_error(result.error);
  }
  return std::move(result.snapshot);
}

auto PipelineMgmtService::PrepareHistoryWrite(const ImageHistorySnapshot& base,
                                              const CommitGraph& graph) -> PreparedHistoryWrite {
  if (!base.graph_ || !base.root_) {
    throw std::invalid_argument("PipelineMgmtService: history snapshot is empty");
  }
  const auto id = graph.GetElementId();
  if (id != base.graph_->GetElementId() || graph.GetRootId() != base.graph_->GetRootId()) {
    throw std::runtime_error(
        "PipelineMgmtService: the history to persist belongs to another image or root");
  }

  // Build phase: storage is not touched until the document of the new history exists.
  const auto  head = graph.GetActiveVersionRef().head_commit_hash;
  std::string replay_error;
  auto        document = BuildDocumentFromRoot(graph, *base.root_, head, &replay_error);
  if (!document) {
    throw std::runtime_error("PipelineMgmtService: replay of the new history of image " +
                             std::to_string(id) + " failed: " + replay_error);
  }
  ValidateProductDocument(*document, id);
  auto materialization =
      graph.CaptureMaterializationWithSerializedPipelineState(EncodePipelineDocumentCheckpoint(
          graph.GetRootId(), head, graph.ChainHashForHead(head), *document));
  return PreparedHistoryWrite{
      .base            = base,
      .materialization = std::move(materialization),
      .document        = std::move(document),
  };
}

auto PipelineMgmtService::PersistHistoryWrites(std::span<PreparedHistoryWrite> writes)
    -> std::vector<HistoryWriteResult> {
  std::vector<HistoryWriteResult> results(writes.size());
  std::vector<std::size_t>        pending;
  std::vector<sl_element_id_t>    pending_ids;
  for (std::size_t index = 0; index < writes.size(); ++index) {
    const auto id             = writes[index].materialization.image_state.element_id;
    results[index].element_id = id;
    if (EditorHoldsImage(id)) {
      results[index].error = EditorOwnsHistoryError(id);
      continue;
    }
    pending.push_back(index);
    pending_ids.push_back(id);
  }
  if (pending.empty()) {
    return results;
  }

  std::vector<std::size_t> written;
  try {
    auto       session       = storage_->OpenSession();
    auto&      graph_service = session.CommitGraph();
    const auto stored_labels = graph_service.ListMaterializedHistoryLabels(pending_ids);
    std::vector<const CommitGraphMaterialization*> materializations;
    for (const auto index : pending) {
      const auto& expected = writes[index].base.graph_->GetImageEditState();
      const auto  stored   = stored_labels.find(expected.element_id);
      if (stored == stored_labels.end() || !SameMaterializedState(stored->second, expected)) {
        results[index].error = "PipelineMgmtService: stored history of image " +
                               std::to_string(expected.element_id) +
                               " changed before the new history was written";
        continue;
      }
      materializations.push_back(&writes[index].materialization);
      written.push_back(index);
    }
    graph_service.MaterializeAll(materializations);
  } catch (const std::exception& e) {
    // The transaction wrote nothing, so every image that was still to be written failed.
    for (const auto index : pending) {
      if (results[index].error.empty()) {
        results[index].error = e.what();
      }
    }
    return results;
  }

  for (const auto index : written) {
    auto&       write    = writes[index];
    const auto& state    = write.materialization.image_state;
    auto        snapshot = PipelineGraphSnapshot::Committed(
        std::move(write.document), state.element_id, PipelineLineageId::Next(),
        state.materialized_head_commit_hash, state.materialized_transaction_chain_hash);
    committed_snapshots_.Publish(snapshot, EditorHoldsImage(state.element_id));
    results[index].snapshot = std::move(snapshot);
  }
  return results;
}

void PipelineMgmtService::InitializeImageRoot(sl_element_id_t id, PipelineDocument document,
                                              const RawRuntimeColorContext* raw_color_context) {
  std::vector<EncodedImageRoot> roots;
  roots.push_back(EncodeImageRoot(id, std::move(document), raw_color_context));
  WriteImageRoots(std::move(roots));
}

auto PipelineMgmtService::EncodeImageRoot(sl_element_id_t id, PipelineDocument document,
                                          const RawRuntimeColorContext* raw_color_context) const
    -> EncodedImageRoot {
  // The document is private to this call until the root is written, so binding needs no lock.
  BindSourceDngColorProfile(*storage_, id, document);
  std::optional<nlohmann::json> raw_json;
  if (raw_color_context != nullptr) {
    BindImportedCameraProfile(document, *raw_color_context);
    raw_json = RawColorContextToJson(*raw_color_context);
  } else if (document.Develop() == nullptr || !document.Develop()->Params().RasterInput()) {
    BindWorkingSpaceDevelopData(document);
  }
  ValidateProductDocument(document, id);
  return EncodedImageRoot{
      .root                  = CommitGraphStore::EncodeRootPipeline(id, document, raw_json),
      .element_pipeline_json = document.ToJson().dump(),
  };
}

void PipelineMgmtService::WriteImageRoots(std::vector<EncodedImageRoot> roots) {
  if (roots.empty()) {
    return;
  }
  std::vector<EncodedRootPipeline> root_rows;
  std::vector<std::string>         element_pipeline_json;
  root_rows.reserve(roots.size());
  element_pipeline_json.reserve(roots.size());
  for (auto& root : roots) {
    root_rows.push_back(std::move(root.root));
    element_pipeline_json.push_back(std::move(root.element_pipeline_json));
  }
  auto session = storage_->OpenSession();
  session.InsertImageRoots(root_rows, std::move(element_pipeline_json));
}

auto PipelineMgmtService::AcquireEditorLease(sl_element_id_t id) -> EditorHistoryLease {
  {
    std::unique_lock<std::mutex> lease_lock(lock_);
    if (!editor_leases_.insert(id).second) {
      throw std::runtime_error("PipelineMgmtService: image " + std::to_string(id) +
                               " is already held by an editor session");
    }
  }
  try {
    auto                              stored         = ReadStoredHistory(*storage_, id);
    const auto&                       graph          = stored.graph;
    const auto                        expected_head  = graph.GetActiveVersionRef().head_commit_hash;
    const auto                        expected_chain = graph.ChainHashForHead(expected_head);

    // History tip is the sole authority. A checkpoint is used only when its root, head, and chain
    // labels match the active Version; otherwise the document is replayed from the root.
    std::shared_ptr<PipelineDocument> document;
    const auto&                       state = graph.GetImageEditState();
    if (state.serialized_pipeline_state.has_value()) {
      const auto checkpoint = TryDecodeCheckpoint(*state.serialized_pipeline_state);
      if (checkpoint.has_value() && checkpoint->root_id == graph.GetRootId() &&
          checkpoint->head_commit_hash == expected_head &&
          checkpoint->transaction_chain_hash == expected_chain) {
        document = BuildDocumentFromCheckpoint(*checkpoint, stored.root);
      }
    }
    if (!document) {
      std::string replay_error;
      document = BuildDocumentFromRoot(graph, stored.root, expected_head, &replay_error);
      if (!document) {
        throw std::runtime_error(replay_error);
      }
      std::unique_lock<std::mutex> lease_lock(lock_);
      ++editor_pipeline_history_rebuild_count_;
    }
    ValidateProductDocument(*document, id);
    return EditorHistoryLease{
        .graph_    = std::move(stored.graph),
        .root_     = std::make_shared<const LoadedRootState>(std::move(stored.root)),
        .document_ = std::move(document),
    };
  } catch (const std::exception& e) {
    {
      std::unique_lock<std::mutex> lease_lock(lock_);
      editor_leases_.erase(id);
    }
    throw std::runtime_error("PipelineMgmtService: editor history of image " + std::to_string(id) +
                             " cannot be loaded: " + e.what());
  }
}

void PipelineMgmtService::ReleaseEditorLease(sl_element_id_t id) {
  {
    std::unique_lock<std::mutex> lease_lock(lock_);
    if (editor_leases_.erase(id) == 0) {
      return;
    }
  }
  if (committed_snapshots_.EndEditorPublication(id)) {
    // The last publication may hold commits that only the editor journal recorded, which a
    // Close with Discard drops. The element pipeline JSON receives only the materialized state.
    WriteElementPipelineJson(*committed_snapshots_.AcquireMaterialized(id));
  }
}

auto PipelineMgmtService::PersistEditorHistory(CommitGraph&            graph,
                                               const ImageEditState&   expected_materialized_state,
                                               const PipelineDocument& document, std::string* error)
    -> bool {
  try {
    const auto id = graph.GetElementId();
    if (!EditorHoldsImage(id)) {
      throw std::runtime_error("PipelineMgmtService: image " + std::to_string(id) +
                               " is not held by an editor session");
    }
    ValidateProductDocument(document, id);
    const auto head = graph.GetActiveVersionRef().head_commit_hash;
    const auto materialization =
        graph.CaptureMaterializationWithSerializedPipelineState(EncodePipelineDocumentCheckpoint(
            graph.GetRootId(), head, graph.ChainHashForHead(head), document));
    {
      auto             session       = storage_->OpenSession();
      auto&            graph_service = session.CommitGraph();
      const auto       stored_state  = graph_service.GetMaterializedHistoryLabel(id);
      if (!stored_state.has_value() || expected_materialized_state.element_id != id ||
          !SameMaterializedState(*stored_state, expected_materialized_state)) {
        throw std::runtime_error(
            "PipelineMgmtService: persisted history changed before editor history persistence");
      }
      graph_service.Materialize(materialization);
    }
    graph.ApplyMaterializedState(materialization.image_state);
    return true;
  } catch (const std::exception& ex) {
    if (error != nullptr) *error = ex.what();
  } catch (...) {
    if (error != nullptr) *error = "PipelineMgmtService: editor history persistence failed";
  }
  return false;
}

auto PipelineMgmtService::CollectUnreachableEditCommits() -> std::size_t {
  auto session = storage_->OpenSession();
  return session.CommitGraph().DeleteUnreachableCommitsForProject();
}

void PipelineMgmtService::PutMaskAsset(const MaskAsset& asset) {
  auto session = storage_->OpenSession();
  (void)session.MaskAssets().Put(asset);
}

auto PipelineMgmtService::ReadMaskAsset(const MaskAssetKey& key) -> MaskAsset {
  return mask_assets_->Read(key);
}

auto PipelineMgmtService::CollectUnreferencedMaskAssets(
    const std::filesystem::path& journal_directory) -> std::size_t {
  auto  session = storage_->OpenSession();
  auto& store   = session.MaskAssets();
  if (store.ListKeys().empty()) {
    return 0;
  }
  std::set<MaskAssetKey> referenced;
  const auto             mark = [&referenced](const nlohmann::json& value) {
    CollectAiMaskAssetKeys(value, referenced);
  };
  session.CommitGraph().VisitStoredHistoryJsonContaining(
      MaskSourceKindText(MaskSourceKind::AiSegmentation), mark);
  std::error_code directory_error;
  if (std::filesystem::is_directory(journal_directory, directory_error)) {
    for (const auto& entry : std::filesystem::directory_iterator(journal_directory)) {
      const auto name = entry.path().filename().string();
      if (!entry.is_regular_file() || !name.ends_with(".mini-git.wal")) {
        continue;
      }
      MiniGitJournal journal(entry.path());
      std::string    journal_error;
      if (!journal.Load(&journal_error)) {
        throw std::runtime_error("PipelineMgmtService: editor journal " + name +
                                 " cannot be read for Mask asset cleanup: " + journal_error);
      }
      for (const auto& record : journal.records()) {
        if (record.edit_commit.has_value()) {
          mark(record.edit_commit->GetPayloadJSON());
        }
      }
    }
  }
  return store.DeleteKeysNotIn(referenced);
}

void PipelineMgmtService::DeletePipeline(sl_element_id_t id) {
  committed_snapshots_.Forget(id);
  try {
    storage_->GetElementStore().RemovePipelineByElementId(id);
  } catch (...) {
  }
}

void PipelineMgmtService::DeletePipelines(std::span<const sl_element_id_t> ids) {
  for (const auto id : ids) {
    if (id != 0) {
      committed_snapshots_.Forget(id);
    }
  }
  try {
    auto  session       = storage_->OpenSession();
    auto& graph_service = session.CommitGraph();
    for (const auto id : ids) {
      if (id != 0) {
        graph_service.DeleteGraphForElement(id);
      }
    }
  } catch (...) {
  }
  try {
    storage_->GetElementStore().RemovePipelinesByElementIds(ids);
  } catch (...) {
  }
}

void PipelineMgmtService::Sync() {
  std::vector<sl_element_id_t> leased;
  {
    std::unique_lock<std::mutex> lease_lock(lock_);
    leased.assign(editor_leases_.begin(), editor_leases_.end());
  }
  for (const auto id : leased) {
    if (committed_snapshots_.EditorPublished(id)) {
      WriteElementPipelineJson(*committed_snapshots_.AcquireMaterialized(id));
    }
  }
}

}  // namespace alcedo
