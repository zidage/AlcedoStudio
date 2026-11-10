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
auto SameMaterializedState(const ImageEditState& stored, const ImageEditState& expected) -> bool {
  return stored.element_id == expected.element_id && stored.root_id == expected.root_id &&
         stored.active_version_id == expected.active_version_id &&
         stored.materialized_head_commit_hash == expected.materialized_head_commit_hash &&
         stored.materialized_transaction_chain_hash == expected.materialized_transaction_chain_hash;
}

/// Materialized history of one image and its decoded root, as storage holds them.
struct StoredHistory {
  CommitGraph     graph;
  LoadedRootState root;
};

/**
 * @brief Read the materialized history of @p id and its immutable root, and bind the image DNG
 *        profile onto the root. The one storage read that the editor load, Copy, and Paste share.
 * @pre The caller holds no database connection lock.
 * @throws std::runtime_error when the image has no root, the root cannot be decoded, or the
 *         materialized state does not match the active Version.
 */
auto ReadStoredHistory(Storage& storage, sl_element_id_t id) -> StoredHistory {
  std::optional<CommitGraph>     graph;
  std::optional<LoadedRootState> root_state;
  {
    auto  session       = storage.OpenSession();
    auto& graph_service = session.CommitGraph();
    graph = graph_service.LoadGraph(id);
    if (!graph.has_value()) {
      throw std::runtime_error("PipelineMgmtService: image " + std::to_string(id) +
                               " has no edit history root");
    }
    const auto root_encoded = graph_service.GetRootSerializedPipelineState(id, graph->GetRootId());
    if (!root_encoded.has_value()) {
      throw std::runtime_error("PipelineMgmtService: immutable root state is missing for image " +
                               std::to_string(id));
    }
    root_state = TryDecodeRootState(*root_encoded, id, graph->GetRootId());
    if (!root_state.has_value()) {
      throw std::runtime_error("PipelineMgmtService: immutable root state is invalid for image " +
                               std::to_string(id));
    }

    const auto& state          = graph->GetImageEditState();
    const auto  expected_head  = graph->GetActiveVersionRef().head_commit_hash;
    const auto  expected_chain = graph->ChainHashForHead(expected_head);
    if (state.root_id != graph->GetRootId() ||
        state.materialized_head_commit_hash != expected_head ||
        state.materialized_transaction_chain_hash != expected_chain) {
      throw std::runtime_error(
          "PipelineMgmtService: stored image edit state does not match the active Version");
    }
  }
  BindSourceDngProfiles(storage, id, *root_state);
  return StoredHistory{std::move(*graph), std::move(*root_state)};
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
  if (EditorHoldsImage(id)) {
    throw std::runtime_error("PipelineMgmtService: image " + std::to_string(id) +
                             " is open in the editor; its history is owned by the editor session");
  }
  auto stored = ReadStoredHistory(*storage_, id);
  return ImageHistorySnapshot{
      .graph_ = std::make_shared<const CommitGraph>(std::move(stored.graph)),
      .root_  = std::make_shared<const LoadedRootState>(std::move(stored.root)),
  };
}

auto PipelineMgmtService::PersistHistory(const ImageHistorySnapshot& base, const CommitGraph& graph)
    -> std::shared_ptr<const PipelineGraphSnapshot> {
  if (!base.graph_ || !base.root_) {
    throw std::invalid_argument("PipelineMgmtService: history snapshot is empty");
  }
  const auto id = graph.GetElementId();
  if (id != base.graph_->GetElementId() || graph.GetRootId() != base.graph_->GetRootId()) {
    throw std::runtime_error(
        "PipelineMgmtService: the history to persist belongs to another image or root");
  }
  if (EditorHoldsImage(id)) {
    throw std::runtime_error("PipelineMgmtService: image " + std::to_string(id) +
                             " is open in the editor; its history is owned by the editor session");
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
  const auto materialization =
      graph.CaptureMaterializationWithSerializedPipelineState(EncodePipelineDocumentCheckpoint(
          graph.GetRootId(), head, graph.ChainHashForHead(head), *document));

  {
    auto             session       = storage_->OpenSession();
    auto&            graph_service = session.CommitGraph();
    const auto       stored_state = graph_service.GetImageEditState(id);
    if (!stored_state.has_value() ||
        !SameMaterializedState(*stored_state, base.graph_->GetImageEditState())) {
      throw std::runtime_error("PipelineMgmtService: stored history of image " +
                               std::to_string(id) + " changed before the new history was written");
    }
    graph_service.Materialize(materialization);
  }

  auto snapshot = PipelineGraphSnapshot::Committed(
      std::move(document), id, PipelineLineageId::Next(),
      materialization.image_state.materialized_head_commit_hash,
      materialization.image_state.materialized_transaction_chain_hash);
  committed_snapshots_.Publish(snapshot, EditorHoldsImage(id));
  return snapshot;
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
      const auto       stored_state = graph_service.GetImageEditState(id);
      if (!stored_state.has_value() ||
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
