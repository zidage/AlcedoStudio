//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <unordered_map>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/database.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/semantic/semantic_label_store.hpp"
#include "storage/store/semantic/semantic_model_registry.hpp"
#include "storage/store/semantic/semantic_vector_search.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "storage/store/sleeve/folder_entry_store.hpp"
#include "storage/store/storage_maintenance.hpp"
#include "storage/store/storage_session.hpp"
#include "type/type.hpp"

namespace alcedo {

class NodeStorageHandler {
 private:
  ElementStore&                                                   element_store_;
  FolderEntryStore&                                               folder_entry_store_;

  std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>& storage_;

 public:
  NodeStorageHandler(ElementStore& element_store, FolderEntryStore& folder_entry_store,
                     std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>& storage);
  void AddToStorage(std::shared_ptr<SleeveElement> new_element);
  /// Load every child of @p folder into the node map and the folder. A failed load writes one
  /// warning with the folder id and the error text and leaves the folder unloaded.
  void EnsureChildrenLoaded(std::shared_ptr<SleeveFolder> folder);
  auto GetElement(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  /**
   * @brief The live child folders of @p folder. Loads no file element.
   *
   * @details A folder with loaded children answers from memory, so the changes of the current
   * write are in the result. Otherwise one FolderEntryStore statement reads the child folders:
   * a folder that the node map already holds keeps its live object, and each other folder goes
   * into the map. The folder itself stays unloaded. Deleted folders are not in the result.
   * @throws The store error of FolderEntryStore::ReadChildFolders.
   */
  auto ChildFolders(const SleeveFolder& folder) -> std::vector<std::shared_ptr<SleeveFolder>>;
  void GarbageCollect();
};

/**
 * @brief The stores of one project database.
 *
 * Code outside storage reaches the database only through the stores that this class gives,
 * through OpenSession, and through Maintenance. The connection and the database lock stay inside
 * storage (scripts/check_storage_boundary.py rejects code outside storage that reads them).
 */
class Storage {
 private:
  Database                                                              database_;
  ElementStore                                                         element_store_;
  FolderEntryStore                                                     folder_entry_store_;
  ImageStore                                                         image_store_;
  MountStore                                                           mount_store_;
  SemanticModelRegistry                                                 semantic_models_;
  SemanticEmbeddingStore                                                semantic_embeddings_;
  SemanticLabelStore                                                    semantic_labels_;
  SemanticVectorSearch                                                  semantic_vector_search_;
  AiStore                                                               ai_store_;
  StorageMaintenance                                                    maintenance_;

  /// Test fixtures write rows that no store writes (old schemas, broken rows); see
  /// tests/support/storage_test_access.hpp. Production code does not define this type.
  friend struct StorageTestAccess;

 public:
  Storage(std::filesystem::path db_path);

  /**
   * @brief Open a connection under the database lock with the commit graph and mask asset
   *        stores on it (see StorageSession).
   *
   * Keep the session only for one operation: other threads wait for the database lock while it
   * exists.
   */
  auto OpenSession() -> StorageSession;
  /// Diagnostics summary, package snapshot, and HNSW index removal (see StorageMaintenance).
  auto Maintenance() -> StorageMaintenance&;
  auto GetElementStore() -> ElementStore&;
  /// Folder structure reads that load no file element (see FolderEntryStore).
  auto GetFolderEntryStore() -> FolderEntryStore&;
  auto GetImageStore() -> ImageStore&;
  /// MountRoot and SourceFile rows (mounted folders).
  auto GetMountStore() -> MountStore&;
  auto GetSemanticModelRegistry() -> SemanticModelRegistry&;
  auto GetSemanticEmbeddingStore() -> SemanticEmbeddingStore&;
  auto GetSemanticLabelStore() -> SemanticLabelStore&;
  auto GetSemanticVectorSearch() -> SemanticVectorSearch&;
  auto GetAiStore() -> AiStore&;
};
};  // namespace alcedo
