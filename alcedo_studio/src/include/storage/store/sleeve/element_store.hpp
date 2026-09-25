//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "storage/mapper/sleeve/element/element_id_mapper.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/mapper/sleeve/element/file_mapper.hpp"
#include "storage/mapper/sleeve/element/folder_mapper.hpp"
#include "storage/store/store_types.hpp"
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

/**
 * @brief Shared FROM/JOIN/WHERE scope for album file queries.
 *
 * @details @c from_where_ is the SQL text after SELECT columns (starts with FROM).
 * @c binds_ holds prepared-statement values for any `?` placeholders in that text
 * (extra filter predicates only; folder scope ids stay embedded as trusted integers).
 */
struct ScopedFileQuery {
  std::string          from_where_;
  duckorm::SqlFragment binds_{};
};

/// Build the shared scope query fragment (FROM ... JOIN ... WHERE) for file-in-folder queries.
/// All folder-scoped file lookups (search, stats, listing, pagination) must use this builder so
/// the scope definition stays consistent across the application.
///
/// Aliases: `e` Element, `fi` FileImage, `i` Image (with its search columns). Predicates on
/// other tables (AI search text, semantic labels, BM25 documents) are subqueries on `e.id`,
/// so the scope never joins them.
auto BuildScopedFileQuery(sl_element_id_t                            folder_id,
                          const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt)
    -> ScopedFileQuery;

struct FileListEntry {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  std::string     file_name_{};
};

/// One search result: the file and image ids plus the typed Image columns that the search
/// dialog shows. Read from the query row, so no caller needs an image pool read.
struct SearchResultRow {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  std::string     file_name_{};     ///< `Image.file_name`.
  std::string     camera_model_{};  ///< Empty when unknown.
  std::string     lens_{};          ///< Empty when unknown.
  std::string     capture_date_{};  ///< `YYYY-MM-DD`; empty when unknown.
  int             rating_ = 0;
};

/// One page of search results and the number of rows that match the whole query.
struct SearchResultPage {
  size_t                       total_ = 0;
  std::vector<SearchResultRow> rows_{};
};

class ElementStore {
 private:
  ConnectionGuard       guard_;

  ElementMapper         element_mapper_;
  ElementIdMapper       element_id_mapper_;

  FileMapper            file_mapper_;
  FolderMapper          folder_mapper_;
  PipelineMapper        pipeline_mapper_;

  // Insert the element row plus its child rows (file binding / folder content).
  // Does not touch sync_flag_ and does not manage a transaction, so it can run
  // either autocommit (AddElement) or inside a shared transaction (AddElements).
  void                  InsertElementRows(const std::shared_ptr<SleeveElement>& element);
  // Update the element row plus its child rows. For a folder, only the FolderContent rows of
  // the children added or removed since the last sync are written. Same
  // transaction-neutrality contract as InsertElementRows; the caller clears the folder's
  // pending content changes after the commit.
  void                  UpdateElementRows(const std::shared_ptr<SleeveElement>& element);

 public:
  ElementStore(ConnectionGuard&& guard);

  void AddElement(const std::shared_ptr<SleeveElement> element);
  // Bulk-insert a batch of elements (and their file/folder-content child
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
  auto BuildFolderStats(sl_element_id_t                            folder_id,
                        const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt,
                        const std::string& active_semantic_model_key = {}) -> FolderStatsView;

  /// Return lightweight file metadata for every live File in a folder, queried directly from DB
  /// without materializing full SleeveElement objects.
  auto ListFilesInFolder(sl_element_id_t folder_id) const -> std::vector<FileListEntry>;
  auto ListFilesInFolderPage(sl_element_id_t folder_id, size_t offset, size_t limit,
                             const std::optional<duckorm::SqlFragment>& extra_filter =
                                 std::nullopt) const -> std::vector<FileListEntry>;
  auto CountFilesInFolder(
      sl_element_id_t                            folder_id,
      const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt) const -> size_t;

  /// Return one page of files that match @p extra_filter, ordered by element id, with the
  /// display columns and the total match count from one statement (`COUNT(*) OVER ()`).
  /// @p limit 0 returns every row. When the page is empty and @p offset is past the first row,
  /// the window value is not available and a `COUNT(*)` statement supplies the total.
  /// Takes the connection lock; safe to call from any thread.
  auto ListSearchResultPage(sl_element_id_t folder_id, size_t offset, size_t limit,
                            const std::optional<duckorm::SqlFragment>& extra_filter =
                                std::nullopt) const -> SearchResultPage;
  /// Return the display rows of @p file_ids in the order given. Ids without a live file
  /// row are skipped. Takes the connection lock; safe to call from any thread.
  auto ListSearchResultRows(std::span<const sl_element_id_t> file_ids) const
      -> std::vector<SearchResultRow>;

  /// Return element IDs for files in a folder matching an extra SqlFragment predicate.
  /// Uses the same BuildScopedFileQuery infrastructure for consistency with stats queries.
  auto ListFilteredFileIds(sl_element_id_t                            folder_id,
                           const std::optional<duckorm::SqlFragment>& extra_filter =
                               std::nullopt) const -> std::vector<sl_element_id_t>;

  void EnsureChildrenLoaded(sl_element_id_t folder_id);

  [[nodiscard]] auto GetPipelineJsonByElementId(sl_element_id_t element_id)
      -> std::optional<nlohmann::json>;
  void UpdatePipelineJsonByElementId(sl_element_id_t element_id, const nlohmann::json& document);
  auto RemovePipelineByElementId(const sl_element_id_t element_id) -> void;
  auto RemovePipelinesByElementIds(std::span<const sl_element_id_t> element_ids) -> void;
};
};  // namespace alcedo
