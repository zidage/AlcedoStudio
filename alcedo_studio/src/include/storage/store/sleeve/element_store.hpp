//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sleeve/album_query.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "storage/mapper/sleeve/element/element_id_mapper.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/mapper/sleeve/element/file_mapper.hpp"
#include "storage/mapper/sleeve/element/folder_mapper.hpp"
#include "storage/store/database.hpp"
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
  /// Local import-day buckets (`YYYY-MM-DD`; an empty label for no import time). Read only by
  /// ElementStore::ReadAlbumQuery when the options name an import-day time zone.
  std::vector<StorageStatsBucket> import_date_stats_{};
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
  /// `Element.added_time` (the file's import time, stored as UTC) as Unix seconds; empty when
  /// the row has no import time.
  std::optional<int64_t> added_time_{};
};

/// One page of search results and the number of rows that match the whole query.
struct SearchResultPage {
  size_t                       total_ = 0;
  std::vector<SearchResultRow> rows_{};
};

/// One page of the files that match a filter and the stats of all matching files, read from
/// one evaluation of the filter.
struct SearchResultPageWithStats {
  SearchResultPage page_{};
  FolderStatsView  stats_{};
};

/// One photo occurrence of an album query: the photo's display columns and the key of the group
/// that holds this occurrence (std::monostate in the flat mode and for the unknown group).
struct AlbumQueryRow {
  SearchResultRow photo_{};
  AlbumGroupKey   group_key_{};
};

/// One group of an album query: its key, its photo count, and the position of its first
/// occurrence in the ordered occurrence stream. It does not hold the photos.
struct AlbumGroupDescriptor {
  AlbumGroupKey key_{};
  int64_t       photo_count_      = 0;
  int64_t       first_occurrence_ = 0;
};

/// Parts of one album query read. A read without groups and statistics is a scroll page.
struct AlbumQueryRead {
  int64_t offset_          = 0;  ///< First occurrence of the page.
  int64_t limit_           = 0;  ///< Occurrences in the page; 0 reads no photo row.
  bool    read_groups_     = false;
  bool    read_statistics_ = false;
};

/// Result of ElementStore::ReadAlbumQuery: counts, the requested groups and statistics, and one
/// bounded page of the ordered occurrence stream.
struct AlbumQueryResult {
  /// Matching unique files. The occurrence count can be larger in the label group.
  int64_t                           unique_file_count_ = 0;
  int64_t                           occurrence_count_  = 0;
  std::vector<AlbumGroupDescriptor> groups_{};
  int64_t                           first_occurrence_ = 0;
  std::vector<AlbumQueryRow>        rows_{};
  std::optional<FolderStatsView>    statistics_{};
};

/// Position of one file's occurrence in the ordered occurrence stream.
struct AlbumFilePosition {
  int64_t       occurrence_index_ = 0;
  AlbumGroupKey group_key_{};
};

/// Whole-project counts that the welcome surface shows for a loaded project. A query result
/// read by ElementStore::ReadProjectOverview; the scope equals `CountFilesInFolder(0)`.
struct ProjectOverviewCounts {
  uint64_t                   photo_count_        = 0;
  /// Files whose active Version head is a commit (see album_edit_state_sql).
  uint64_t                   edited_photo_count_ = 0;
  /// `YYYY-MM-DD` of the earliest `Image.capture_date`; empty when no file has a capture date.
  std::optional<std::string> earliest_capture_date_{};
  /// `YYYY-MM-DD` of the latest `Image.capture_date`; empty when no file has a capture date.
  std::optional<std::string> latest_capture_date_{};
};

/// The project file whose active Version head is the newest commit. Read by
/// ElementStore::ReadLastEditedFile.
struct LastEditedFile {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
};

/**
 * @brief Element, file binding, folder content, and pipeline rows of one project, and the
 *        scoped library reads over them.
 *
 * @details Every public operation requests its own connection from the Database owner and holds
 * the database lock for the whole operation, so connection lifetime and transaction scope stay
 * inside that operation. Reads are safe to call from any thread.
 */
class ElementStore {
 private:
  Database&   database_;

  // Insert the element row plus its child rows (file binding / folder content) on @p conn.
  // Does not touch sync_flag_ and does not manage a transaction, so it can run
  // either autocommit (AddElement) or inside a shared transaction (AddElements).
  static void InsertElementRows(duckdb_connection&                    conn,
                                const std::shared_ptr<SleeveElement>& element);
  // Update the element row plus its child rows. For a folder, only the FolderContent rows of
  // the children added or removed since the last sync are written. Same
  // transaction-neutrality behavior as InsertElementRows; the caller clears the folder's
  // pending content changes after the commit.
  static void UpdateElementRows(duckdb_connection&                    conn,
                                const std::shared_ptr<SleeveElement>& element);

 public:
  explicit ElementStore(Database& database);

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
  // Every element that FolderContent lists under @p folder_id, loaded as GetElementById loads
  // one element (SYNCED, file image binding read), with two queries for the whole folder.
  // Order is unspecified.
  auto GetFolderChildren(const sl_element_id_t folder_id)
      -> std::vector<std::shared_ptr<SleeveElement>>;

  auto GetElementsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                   const sl_element_id_t              folder_id)
      -> std::vector<std::shared_ptr<SleeveElement>>;

  auto GetElementIdsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                     const sl_element_id_t              folder_id)
      -> std::vector<sl_element_id_t>;
  /// Total, date, camera, lens, semantic label (when @p active_semantic_model_key is set), and
  /// rating buckets of the files that match @p extra_filter. The filter is evaluated once: the
  /// matching rows go into a temporary table on this store's connection and every bucket is
  /// read from it. Throws std::runtime_error when a statement fails.
  auto BuildFolderStats(sl_element_id_t                            folder_id,
                        const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt,
                        const std::string& active_semantic_model_key = {}) const -> FolderStatsView;
  /// One page of the files that match @p extra_filter (ordered by element id; @p limit 0
  /// returns every row), the total, and the BuildFolderStats buckets, from one evaluation of
  /// the filter. Throws std::runtime_error when a statement fails.
  auto ListSearchResultPageWithStats(sl_element_id_t folder_id, size_t offset, size_t limit,
                                     const std::optional<duckorm::SqlFragment>& extra_filter,
                                     const std::string& active_semantic_model_key) const
      -> SearchResultPageWithStats;

  /// Return lightweight file metadata for every live File in a folder, queried directly from DB
  /// without materializing full SleeveElement objects.
  auto ListFilesInFolder(sl_element_id_t folder_id) const -> std::vector<FileListEntry>;
  auto ListFilesInFolderPage(sl_element_id_t folder_id, size_t offset, size_t limit,
                             const std::optional<duckorm::SqlFragment>& extra_filter =
                                 std::nullopt) const -> std::vector<FileListEntry>;
  auto CountFilesInFolder(
      sl_element_id_t                            folder_id,
      const std::optional<duckorm::SqlFragment>& extra_filter = std::nullopt) const -> size_t;

  /// Photo count, edited photo count, and capture date range of every file in the project,
  /// from one statement on the `BuildScopedFileQuery(0)` scope. Reads only; takes the
  /// connection lock and is safe to call from any thread. Throws std::runtime_error with the
  /// DuckDB message when the statement fails.
  auto ReadProjectOverview() const -> ProjectOverviewCounts;

  /// The file of the whole project whose active Version head commit is the newest, by the same
  /// edited rule as the library edit groups. Empty when no file is edited. Reads only; takes
  /// the connection lock. Throws std::runtime_error with the DuckDB message on failure.
  auto ReadLastEditedFile() const -> std::optional<LastEditedFile>;

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

  /**
   * @brief Read the matching files of a scope with the presentation @p options: counts, the
   *        groups and the Inspector statistics when @p read asks for them, and one bounded
   *        page of the ordered occurrence stream.
   *
   * @details The filter is evaluated once into a temporary match set inside one read
   * transaction; every count, group, statistic, and row of the call comes from that set. The
   * set is dropped before the connection is released. The label group and the label sort use
   * the canonical labels of @p active_semantic_model_key only.
   *
   * @throws std::invalid_argument for invalid options or page bounds (a limit above
   *         kMaxAlbumQueryPageRows or a negative offset).
   * @throws std::runtime_error with the DuckDB message when a statement fails.
   */
  auto ReadAlbumQuery(sl_element_id_t                            folder_id,
                      const std::optional<duckorm::SqlFragment>& extra_filter,
                      const AlbumQueryOptions&                   options,
                      const std::string&                         active_semantic_model_key,
                      const AlbumQueryRead&                      read) const -> AlbumQueryResult;

  /**
   * @brief Position of @p file_id in the ordered occurrence stream of the same query.
   *
   * @param preferred_group In the label group a file can occur more than once; the occurrence
   *        in this group is returned when it exists, else the first occurrence.
   * @return std::nullopt when the file does not match.
   * @throws Same as ReadAlbumQuery.
   */
  auto ReadAlbumFilePosition(sl_element_id_t                            folder_id,
                             const std::optional<duckorm::SqlFragment>& extra_filter,
                             const AlbumQueryOptions&                   options,
                             const std::string& active_semantic_model_key, sl_element_id_t file_id,
                             const std::optional<AlbumGroupKey>& preferred_group) const
      -> std::optional<AlbumFilePosition>;

  /**
   * @brief Unique file ids of the occurrences in [@p occurrence_begin, @p occurrence_end), in
   *        the order of each file's first occurrence in that range. Reads no photo metadata.
   * @throws Same as ReadAlbumQuery; std::invalid_argument for an invalid range.
   */
  auto               ReadAlbumFileIds(sl_element_id_t                            folder_id,
                                      const std::optional<duckorm::SqlFragment>& extra_filter,
                                      const AlbumQueryOptions&                   options,
                                      const std::string& active_semantic_model_key, int64_t occurrence_begin,
                                      int64_t occurrence_end) const -> std::vector<sl_element_id_t>;

  void EnsureChildrenLoaded(sl_element_id_t folder_id);

  [[nodiscard]] auto GetPipelineJsonByElementId(sl_element_id_t element_id)
      -> std::optional<nlohmann::json>;
  void UpdatePipelineJsonByElementId(sl_element_id_t element_id, const nlohmann::json& document);
  auto RemovePipelineByElementId(const sl_element_id_t element_id) -> void;
  auto RemovePipelinesByElementIds(std::span<const sl_element_id_t> element_ids) -> void;
};
};  // namespace alcedo
