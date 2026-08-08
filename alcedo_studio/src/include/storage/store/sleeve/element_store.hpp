//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "edit/history/edit_history.hpp"
#include "edit/history/editor_journal_recovery.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/store/store_types.hpp"
#include "storage/mapper/sleeve/edit_history/recovery_metadata_mapper.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "storage/mapper/sleeve/edit_history/history_mapper.hpp"
#include "storage/mapper/sleeve/element/element_id_mapper.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/mapper/sleeve/element/file_mapper.hpp"
#include "storage/mapper/sleeve/element/folder_mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
struct StorageStatsBucket {
  std::string label_{};
  int         count_ = 0;
};

struct FolderStatsView {
  int                             total_photo_count_ = 0;
  std::vector<StorageStatsBucket> date_stats_{};
  std::vector<StorageStatsBucket> camera_stats_{};
  std::vector<StorageStatsBucket> lens_stats_{};
  std::vector<StorageStatsBucket> label_stats_{};
  std::vector<StorageStatsBucket> rating_stats_{};
};

struct ScopedFileQuery {
  std::string from_where_;
};

/// Build the shared scope query fragment (FROM ... JOIN ... WHERE) for file-in-folder queries.
/// All folder-scoped file lookups (search, stats, listing, pagination) must use this builder so
/// the scope definition stays consistent across the application.
auto BuildScopedFileQuery(sl_element_id_t                    folder_id,
                          const std::optional<std::wstring>& extra_filter_where = std::nullopt)
    -> ScopedFileQuery;

struct FileListEntry {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  std::string     file_name_{};
};

class ElementStore {
 private:
  ConnectionGuard    guard_;

  ElementMapper     element_mapper_;
  ElementIdMapper   element_id_mapper_;

  FileMapper        file_mapper_;
  FolderMapper      folder_mapper_;
  EditHistoryMapper history_mapper_;
  PipelineMapper    pipeline_mapper_;
  EditHistoryMapper edit_history_mapper_;
  std::function<void()> materialize_pre_commit_hook_{};

  // Insert the element row plus its child rows (file binding / folder content /
  // edit history). Does not touch sync_flag_ and does not manage a transaction, so
  // it can run either autocommit (AddElement) or inside a shared transaction
  // (AddElements).
  void InsertElementRows(const std::shared_ptr<SleeveElement>& element);
  // Update the element row plus its child rows. Same transaction-neutrality contract
  // as InsertElementRows.
  void UpdateElementRows(const std::shared_ptr<SleeveElement>& element);

 public:
  ElementStore(ConnectionGuard&& guard);

  void AddElement(const std::shared_ptr<SleeveElement> element);
  // Bulk-insert a batch of elements (and their file/folder-content/edit-history child
  // rows) in a single transaction. Import sync should prefer this over the per-row
  // AddElement loop: one transaction for N elements instead of one autocommit
  // transaction per element.
  void AddElements(std::span<const std::shared_ptr<SleeveElement>> elements);

  void AddFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id);
  void RemoveFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id);
  auto GetFolderContent(const sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;

  void RemoveElement(const sl_element_id_t id);
  void RemoveElement(const std::shared_ptr<SleeveElement> element);
  void RemoveElements(std::span<const std::shared_ptr<SleeveElement>> elements);
  void UpdateElement(const std::shared_ptr<SleeveElement> element);
  // Bulk-update a batch of elements in a single transaction.
  void UpdateElements(std::span<const std::shared_ptr<SleeveElement>> elements);
  auto GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement>;

  auto GetElementsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                   const sl_element_id_t              folder_id)
      -> std::vector<std::shared_ptr<SleeveElement>>;

  auto GetElementIdsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                     const sl_element_id_t              folder_id)
      -> std::vector<sl_element_id_t>;
  auto BuildFolderStats(sl_element_id_t                    folder_id,
                        const std::optional<std::wstring>& extra_filter_where = std::nullopt,
                        const std::string& active_semantic_model_key = {}) -> FolderStatsView;

  /// Return lightweight file metadata for every live File in a folder, queried directly from DB
  /// without materializing full SleeveElement objects.
  auto ListFilesInFolder(sl_element_id_t folder_id) const -> std::vector<FileListEntry>;
  auto ListFilesInFolderPage(sl_element_id_t folder_id, size_t offset, size_t limit,
                             const std::optional<std::wstring>& extra_filter_where =
                                 std::nullopt) const -> std::vector<FileListEntry>;
  auto CountFilesInFolder(
      sl_element_id_t                    folder_id,
      const std::optional<std::wstring>& extra_filter_where = std::nullopt) const -> size_t;

  /// Return element IDs for files in a folder matching an extra SQL WHERE clause.
  /// Uses the same BuildScopedFileQuery infrastructure for consistency with stats queries.
  auto ListFilteredFileIds(sl_element_id_t                    folder_id,
                           const std::optional<std::wstring>& extra_filter_where =
                               std::nullopt) const -> std::vector<sl_element_id_t>;

  void EnsureChildrenLoaded(sl_element_id_t folder_id);

  auto GetPipelineByElementId(const sl_element_id_t element_id)
      -> std::shared_ptr<CPUPipelineExecutor>;
  auto UpdatePipelineByElementId(const sl_element_id_t                      element_id,
                                 const std::shared_ptr<CPUPipelineExecutor> pipeline) -> void;
  auto RemovePipelineByElementId(const sl_element_id_t element_id) -> void;
  auto RemovePipelinesByElementIds(std::span<const sl_element_id_t> element_ids) -> void;

  auto GetEditHistoryByFileId(const sl_element_id_t file_id) -> std::shared_ptr<EditHistory>;
  auto UpdateEditHistoryByFileId(const sl_element_id_t              file_id,
                                 const std::shared_ptr<EditHistory> history) -> void;
  auto RemoveEditHistoryByFileId(const sl_element_id_t file_id) -> void;
  auto RemoveEditHistoriesByFileIds(std::span<const sl_element_id_t> file_ids) -> void;

  /// Atomically update active Version history, active pipeline params, and
  /// recovery metadata on this controller's connection. Editor materialization
  /// and Version publication must use this path instead of separate
  /// SaveHistory()/SavePipeline() calls.
  auto MaterializeEditorState(const std::shared_ptr<EditHistory>& history,
                              const std::shared_ptr<CPUPipelineExecutor>& pipeline,
                              const EditorRecoveryMetadata& recovery_metadata,
                              std::string* error = nullptr) -> bool;

  auto GetEditorRecoveryMetadata(sl_element_id_t file_id) -> std::optional<EditorRecoveryMetadata>;

  /// Test-only seam: a hook invoked after the history, pipeline, and recovery
  /// metadata writes inside `MaterializeEditorState` but before the transaction
  /// commits. Throwing from the hook forces a rollback so tests can prove all
  /// three writes roll back together. Production leaves it unset.
  void SetMaterializePreCommitHook(std::function<void()> hook) {
    materialize_pre_commit_hook_ = std::move(hook);
  }

  auto GetEditHistoryMapper() -> std::shared_ptr<EditHistoryMapper>;

  void UpdateEditHistoryMapper(const std::shared_ptr<EditHistoryMapper> new_service);
};
};  // namespace alcedo
