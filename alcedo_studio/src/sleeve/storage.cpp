//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/storage.hpp"

namespace alcedo {
Storage::Storage(std::filesystem::path db_path)
    : database_(db_path),
      element_store_(database_),
      folder_entry_store_(database_),
      image_store_(database_.GetConnectionGuard()),
      source_file_store_(database_),
      mount_store_(database_, source_file_store_),
      semantic_models_(database_),
      semantic_embeddings_(database_),
      semantic_labels_(database_),
      semantic_vector_search_(database_),
      ai_store_(database_),
      maintenance_(database_),
      project_maintenance_(database_) {}

auto Storage::OpenSession() -> StorageSession { return StorageSession(database_); }

auto Storage::Maintenance() -> StorageMaintenance& { return maintenance_; }

auto Storage::GetProjectMaintenanceStore() -> ProjectMaintenanceStore& {
  return project_maintenance_;
}

auto Storage::GetElementStore() -> ElementStore& { return element_store_; }

auto Storage::GetFolderEntryStore() -> FolderEntryStore& { return folder_entry_store_; }

auto Storage::GetImageStore() -> ImageStore& { return image_store_; }

auto Storage::GetSourceFileStore() -> SourceFileStore& { return source_file_store_; }

auto Storage::GetMountStore() -> MountStore& { return mount_store_; }

auto Storage::GetSemanticModelRegistry() -> SemanticModelRegistry& { return semantic_models_; }

auto Storage::GetSemanticEmbeddingStore() -> SemanticEmbeddingStore& {
  return semantic_embeddings_;
}

auto Storage::GetSemanticLabelStore() -> SemanticLabelStore& { return semantic_labels_; }

auto Storage::GetSemanticVectorSearch() -> SemanticVectorSearch& {
  return semantic_vector_search_;
}

auto Storage::GetAiStore() -> AiStore& {
  return ai_store_;
}
};  // namespace alcedo
