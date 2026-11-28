//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>

#include "storage/store/ai/ai_store.hpp"
#include "storage/store/database.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/store/image/source_file_store.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/semantic/semantic_label_store.hpp"
#include "storage/store/semantic/semantic_model_registry.hpp"
#include "storage/store/semantic/semantic_vector_search.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "storage/store/sleeve/folder_entry_store.hpp"
#include "storage/store/sleeve/project_maintenance_store.hpp"
#include "storage/store/storage_maintenance.hpp"
#include "storage/store/storage_session.hpp"
#include "type/type.hpp"

namespace alcedo {

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
  SourceFileStore                                                       source_file_store_;
  MountStore                                                           mount_store_;
  SemanticModelRegistry                                                 semantic_models_;
  SemanticEmbeddingStore                                                semantic_embeddings_;
  SemanticLabelStore                                                    semantic_labels_;
  SemanticVectorSearch                                                  semantic_vector_search_;
  AiStore                                                               ai_store_;
  StorageMaintenance                                                    maintenance_;
  ProjectMaintenanceStore                                               project_maintenance_;

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
  /// Project-entry inspection and removal through scoped storage operations.
  auto GetProjectMaintenanceStore() -> ProjectMaintenanceStore&;
  auto GetElementStore() -> ElementStore&;
  /// Folder structure reads that load no file element (see FolderEntryStore).
  auto GetFolderEntryStore() -> FolderEntryStore&;
  auto GetImageStore() -> ImageStore&;
  auto GetSourceFileStore() -> SourceFileStore&;
  /// MountRoot and SourceFile rows (mounted folders).
  auto GetMountStore() -> MountStore&;
  auto GetSemanticModelRegistry() -> SemanticModelRegistry&;
  auto GetSemanticEmbeddingStore() -> SemanticEmbeddingStore&;
  auto GetSemanticLabelStore() -> SemanticLabelStore&;
  auto GetSemanticVectorSearch() -> SemanticVectorSearch&;
  auto GetAiStore() -> AiStore&;
};
};  // namespace alcedo
