//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/element_store.hpp"

#include <duckdb.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "type/type.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

auto BuildScopedFileQuery(sl_element_id_t                            folder_id,
                          const std::optional<duckorm::SqlFragment>& extra_filter)
    -> ScopedFileQuery {
  ScopedFileQuery scope;
  std::string     extra_where;
  if (extra_filter.has_value() && !extra_filter->empty()) {
    extra_where         = " AND (" + extra_filter->sql_ + ")";
    scope.binds_.sql_   = extra_filter->sql_;
    scope.binds_.binds_ = extra_filter->binds_;
  }

  if (folder_id == 0) {
    scope.from_where_ = std::format(
        "FROM Element e "
        "JOIN FileImage fi ON fi.file_id = e.id "
        "JOIN Image i ON i.id = fi.image_id "
        "WHERE e.type = {}{}",
        static_cast<uint32_t>(ElementType::FILE), extra_where);
    return scope;
  }

  scope.from_where_ = std::format(
      "FROM FolderContent fc "
      "JOIN Element e ON fc.element_id = e.id "
      "JOIN FileImage fi ON fi.file_id = e.id "
      "JOIN Image i ON i.id = fi.image_id "
      "WHERE fc.folder_id = {} AND e.type = {}{}",
      folder_id, static_cast<uint32_t>(ElementType::FILE), extra_where);
  return scope;
}

namespace {

/// Count of a listing COUNT query. A failed query reads as 0.
auto RunScalarInt64(duckdb_connection conn, const std::string& sql,
                    const duckorm::SqlFragment& binds) -> int64_t {
  try {
    return duckorm::select_int64(conn, duckorm::SqlFragment{sql, binds.binds_}).value_or(0);
  } catch (const std::runtime_error&) {
    return 0;
  }
}

// Display columns of a search result row, in SearchResultRow order. Aliases follow
// BuildScopedFileQuery (`e` Element, `fi` FileImage, `i` Image).
constexpr const char* kSearchResultColumns =
    "e.id, fi.image_id, i.file_name, i.camera_model, i.lens, "
    "CAST(i.capture_date AS VARCHAR), i.rating";
constexpr idx_t kSearchResultColumnCount = 7;

auto            ReadVarchar(duckdb_result* result, idx_t column, idx_t row) -> std::string {
  std::string value;
  if (duckdb_value_is_null(result, column, row)) {
    return value;
  }
  char* raw = duckdb_value_varchar(result, column, row);
  if (raw) {
    value = raw;
    duckdb_free(raw);
  }
  return value;
}

auto ReadSearchResultRow(duckdb_result* result, idx_t row) -> SearchResultRow {
  SearchResultRow out;
  out.file_id_      = static_cast<sl_element_id_t>(duckdb_value_int64(result, 0, row));
  out.image_id_     = static_cast<image_id_t>(duckdb_value_int64(result, 1, row));
  out.file_name_    = ReadVarchar(result, 2, row);
  out.camera_model_ = ReadVarchar(result, 3, row);
  out.lens_         = ReadVarchar(result, 4, row);
  out.capture_date_ = ReadVarchar(result, 5, row);
  out.rating_       = static_cast<int>(duckdb_value_int32(result, 6, row));
  return out;
}

/// Run a query and keep its result, or throw std::runtime_error with DuckDB's message.
void ExecuteQueryOrThrow(duckdb_connection conn, const std::string& sql,
                         const duckorm::SqlFragment& binds, duckdb_result* result) {
  if (duckorm::execute_query(conn, sql, binds, result) == DuckDBSuccess) {
    return;
  }
  const char* error   = duckdb_result_error(result);
  std::string message = error ? error : "unknown DuckDB error";
  duckdb_destroy_result(result);
  throw std::runtime_error(message);
}

// Temporary table with the rows of one match set: the files that match a filter, with the
// columns that the stats buckets and the search result rows read. It is a copy because the
// page, the total, and every bucket must come from one evaluation of the filter; a query on
// the live tables evaluates it again for each statement (Phase S8). Only the seven read
// columns are copied. The table is read-only, never written back, and exists on the
// ElementStore connection only while ElementStore::ReadMatchSet runs under the connection
// lock, so no write can change the rows between the reads; MatchSetTable drops it when the
// read ends.
constexpr const char* kMatchSetTable = "SearchMatchSet";

/// Owns the match set table for one read and drops it at scope exit, also after a failed
/// statement, so the matched rows do not stay in memory between searches.
class MatchSetTable {
 public:
  MatchSetTable(duckdb_connection conn, const ScopedFileQuery& scope) : conn_(conn) {
    duckorm::execute(conn_, duckorm::SqlFragment{
                                std::format("CREATE OR REPLACE TEMP TABLE {} AS SELECT e.id AS "
                                            "file_id, fi.image_id, i.file_name, i.camera_model, "
                                            "i.lens, i.capture_date, i.rating {}",
                                            kMatchSetTable, scope.from_where_),
                                scope.binds_.binds_});
  }
  MatchSetTable(const MatchSetTable&)            = delete;
  MatchSetTable& operator=(const MatchSetTable&) = delete;
  ~MatchSetTable() {
    duckdb_result result;
    // A failed drop leaves the table until the next CREATE OR REPLACE; nothing else reads it.
    duckdb_query(conn_, std::format("DROP TABLE IF EXISTS temp.{}", kMatchSetTable).c_str(),
                 &result);
    duckdb_destroy_result(&result);
  }

 private:
  duckdb_connection conn_;
};

// GROUPING(d, m, l, r) of each grouping set in ReadMatchSetBuckets: the bit of a column is 1
// when the set does not group by it (first argument is the highest bit).
constexpr int64_t kDateGroup   = 0b0111;
constexpr int64_t kCameraGroup = 0b1011;
constexpr int64_t kLensGroup   = 0b1101;
constexpr int64_t kRatingGroup = 0b1110;
constexpr int64_t kTotalGroup  = 0b1111;

/// Read the total and the date, camera, lens, and rating buckets of the match set in one
/// GROUPING SETS statement. Orders: dates and ratings descending (unknown date last), cameras
/// and lenses by count descending, then by name.
void              ReadMatchSetBuckets(duckdb_connection conn, FolderStatsView& out) {
  const auto sql = std::format(
      "SELECT g, label, c FROM ("
                   "SELECT GROUPING(d, m, l, r) AS g, d, r, COALESCE(d, m, l, r) AS label, COUNT(*) AS c "
                   "FROM (SELECT CAST(capture_date AS VARCHAR) AS d, "
                   "COALESCE(NULLIF(camera_model, ''), '(unknown)') AS m, "
                   "COALESCE(NULLIF(lens, ''), '(unknown)') AS l, CAST(rating AS VARCHAR) AS r FROM {}) "
                   "GROUP BY GROUPING SETS ((d), (m), (l), (r), ())) "
                   "ORDER BY g, CASE WHEN g = {} THEN d END DESC NULLS LAST, "
                   "CASE WHEN g = {} THEN r END DESC NULLS LAST, c DESC, label",
      kMatchSetTable, kDateGroup, kRatingGroup);
  duckdb_result result;
  ExecuteQueryOrThrow(conn, sql, {}, &result);

  const auto row_count = duckdb_row_count(&result);
  for (idx_t row = 0; row < row_count; ++row) {
    const auto group = duckdb_value_int64(&result, 0, row);
    const auto count = static_cast<int>(duckdb_value_int64(&result, 2, row));
    if (group == kTotalGroup) {
      out.total_photo_count_ = count;
      continue;
    }
    StorageStatsBucket bucket{.label_ = ReadVarchar(&result, 1, row), .count_ = count};
    switch (group) {
      case kDateGroup:
        out.date_stats_.push_back(std::move(bucket));
        break;
      case kCameraGroup:
        out.camera_stats_.push_back(std::move(bucket));
        break;
      case kLensGroup:
        out.lens_stats_.push_back(std::move(bucket));
        break;
      case kRatingGroup:
        out.rating_stats_.push_back(std::move(bucket));
        break;
      default:
        break;
    }
  }
  duckdb_destroy_result(&result);
}

/// Semantic label buckets of the match set for the active model: files for each label, by
/// count descending, then by label.
auto ReadMatchSetLabelBuckets(duckdb_connection conn, const std::string& active_semantic_model_key)
    -> std::vector<StorageStatsBucket> {
  const auto sql = std::format(
      "SELECT sl.label, COUNT(DISTINCT sl.file_id) AS c FROM SemanticImageLabel sl "
      "WHERE sl.model_key = ? AND sl.label IS NOT NULL AND sl.label <> '' "
      "AND sl.file_id IN (SELECT file_id FROM {}) "
      "GROUP BY sl.label ORDER BY c DESC, sl.label",
      kMatchSetTable);
  duckdb_result result;
  ExecuteQueryOrThrow(conn, sql,
                      duckorm::SqlFragment{"", {duckorm::BindValue{active_semantic_model_key}}},
                      &result);
  std::vector<StorageStatsBucket> buckets;
  const auto                      row_count = duckdb_row_count(&result);
  buckets.reserve(static_cast<size_t>(row_count));
  for (idx_t row = 0; row < row_count; ++row) {
    buckets.push_back({.label_ = ReadVarchar(&result, 0, row),
                       .count_ = static_cast<int>(duckdb_value_int64(&result, 1, row))});
  }
  duckdb_destroy_result(&result);
  return buckets;
}

void DeleteSemanticAndAiRowsForFiles(duckdb_connection                conn,
                                     std::span<const sl_element_id_t> file_ids) {
  if (file_ids.empty()) {
    return;
  }
  DeleteSemanticRowsForFiles(conn, file_ids);
  // Phase 5f: AI image understanding + rating rows. Routed through the duckorm `remove`
  // path (`DeleteAiAnnotationRowsForFiles`) rather than a hand-written DELETE so the AI
  // ser/deser stays ORM-faithful. This runs on the ElementStore's own connection so
  // the cleanup is atomic with element deletion; rating rows are dropped here too even
  // though they are not part of full-text search, so a re-import does not resurrect an
  // old AI rating under a new image id.
  DeleteAiAnnotationRowsForFiles(conn, file_ids);
}

auto SortedIds(const std::unordered_set<sl_element_id_t>& ids) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> sorted(ids.begin(), ids.end());
  std::sort(sorted.begin(), sorted.end());
  return sorted;
}

/// Clear the pending folder content changes of the folders in @p elements. Runs after the
/// transaction that wrote their rows committed.
void MarkFolderContentsSynced(std::span<const std::shared_ptr<SleeveElement>> elements) {
  for (const auto& element : elements) {
    if (element && element->type_ == ElementType::FOLDER) {
      std::static_pointer_cast<SleeveFolder>(element)->MarkContentSynced();
    }
  }
}
}  // namespace

/**
 * @brief Construct a new Element Controller:: Element Controller object
 *
 * @param guard
 */
ElementStore::ElementStore(ConnectionGuard&& guard)
    : guard_(std::move(guard)),
      element_mapper_(guard_.conn_),
      element_id_mapper_(guard_.conn_),
      file_mapper_(guard_.conn_),
      folder_mapper_(guard_.conn_),
      pipeline_mapper_(guard_.conn_) {}
/**
 * @brief Add an element to the database.
 *
 * @param element
 */
void ElementStore::AddElement(const std::shared_ptr<SleeveElement> element) {
  auto db_lock = guard_.Lock();
  InsertElementRows(element);
  MarkFolderContentsSynced(std::span<const std::shared_ptr<SleeveElement>>(&element, 1));
  element->sync_flag_ = SyncFlag::SYNCED;
}

void ElementStore::InsertElementRows(const std::shared_ptr<SleeveElement>& element) {
  element_mapper_.Insert(element);
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    file_mapper_.Insert({file->element_id_, file->image_id_});
  } else if (element->type_ == ElementType::FOLDER) {
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    folder_mapper_.InsertFolderContents(folder->element_id_, folder->ListElements());
  }
}

void ElementStore::AddElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto db_lock = guard_.Lock();
  duckorm::begin_transaction(guard_.conn_);
  try {
    for (const auto& element : elements) {
      InsertElementRows(element);
    }
    duckorm::commit_transaction(guard_.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard_.conn_);
    throw;
  }
  MarkFolderContentsSynced(elements);
  for (const auto& element : elements) {
    element->sync_flag_ = SyncFlag::SYNCED;
  }
}

/**
 * @brief Add a content to a folder in the database.
 *
 * @param folder_id
 * @param content_id
 */
void ElementStore::AddFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id) {
  auto db_lock = guard_.Lock();
  folder_mapper_.Insert({folder_id, content_id});
}

void ElementStore::RemoveFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id) {
  auto db_lock = guard_.Lock();
  folder_mapper_.RemoveFolderContent(folder_id, content_id);
}

/**
 * @brief Get an element by its ID from the database.
 *
 * @param id
 * @return std::shared_ptr<SleeveElement>
 */
auto ElementStore::GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  auto db_lock = guard_.Lock();
  auto result  = element_mapper_.GetElementById(id);
  if (result->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(result);
    try {
      file->image_id_ = file_mapper_.GetBoundImageById(file->element_id_);
    } catch (...) {
      file->image_id_ = 0;
    }
  }
  result->SetSyncFlag(SyncFlag::SYNCED);
  return result;
}

/**
 * @brief Get the content of a folder by its ID from the database.
 *
 * @param folder_id
 * @return std::vector<sl_element_id_t>
 */
auto ElementStore::GetFolderContent(const sl_element_id_t folder_id)
    -> std::vector<sl_element_id_t> {
  auto db_lock = guard_.Lock();
  return folder_mapper_.GetFolderContent(folder_id);
}

/**
 * @brief Remove an element by its ID from the database, only be called when the ref count to the
 * element is 0.
 *
 * Low-level row delete: removes the Element row only. It does NOT cascade
 * AI / semantic / pipeline / file-binding rows — that orchestration
 * lives at the service layer (SleeveServiceImpl::DeleteElement flows through
 * Write -> Sync -> RemoveElements / RemoveElement(shared_ptr), which call
 * DeleteSemanticAndAiRowsForFiles on the same connection). Keep this primitive
 * a pure row delete so the storage-controller layer does not reach back up
 * into element-fetch / cascade orchestration.
 *
 * @param id
 */
void ElementStore::RemoveElement(const sl_element_id_t id) {
  auto db_lock = guard_.Lock();
  element_mapper_.RemoveById(id);
}

void ElementStore::RemoveElement(const std::shared_ptr<SleeveElement> element) {
  auto db_lock = guard_.Lock();
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    DeleteSemanticAndAiRowsForFiles(guard_.conn_,
                                    std::span<const sl_element_id_t>(&file->element_id_, 1));
    pipeline_mapper_.RemoveById(file->element_id_);
    file_mapper_.RemoveById(file->element_id_);
    folder_mapper_.RemoveContentById(file->element_id_);
  } else if (element->type_ == ElementType::FOLDER) {
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    folder_mapper_.RemoveById(folder->element_id_);
  }
  element_mapper_.RemoveById(element->element_id_);
}

void ElementStore::RemoveElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto                         db_lock = guard_.Lock();

  std::vector<sl_element_id_t> file_ids;
  std::vector<sl_element_id_t> folder_ids;
  std::vector<sl_element_id_t> element_ids;
  file_ids.reserve(elements.size());
  folder_ids.reserve(elements.size());
  element_ids.reserve(elements.size());

  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(elements.size() * 2 + 1);
  for (const auto& element : elements) {
    if (!element || !seen.insert(element->element_id_).second) {
      continue;
    }

    element_ids.push_back(element->element_id_);
    if (element->type_ == ElementType::FILE) {
      file_ids.push_back(element->element_id_);
    } else if (element->type_ == ElementType::FOLDER) {
      folder_ids.push_back(element->element_id_);
    }
  }

  if (!file_ids.empty()) {
    DeleteSemanticAndAiRowsForFiles(guard_.conn_, file_ids);
    pipeline_mapper_.RemoveByIds(file_ids);
    file_mapper_.RemoveByIds(file_ids);
    folder_mapper_.RemoveContentByIds(file_ids);
  }
  if (!folder_ids.empty()) {
    folder_mapper_.RemoveByIds(folder_ids);
  }
  if (!element_ids.empty()) {
    element_mapper_.RemoveByIds(element_ids);
  }
}

/**
 * @brief Update an element in the database.
 *
 * @param element
 */
void ElementStore::UpdateElement(const std::shared_ptr<SleeveElement> element) {
  auto db_lock = guard_.Lock();
  UpdateElementRows(element);
  MarkFolderContentsSynced(std::span<const std::shared_ptr<SleeveElement>>(&element, 1));
  element->sync_flag_ = SyncFlag::SYNCED;
}

void ElementStore::UpdateElementRows(const std::shared_ptr<SleeveElement>& element) {
  element_mapper_.Update(element, element->element_id_);
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    file_mapper_.Update({file->element_id_, file->image_id_}, file->image_id_);
  } else if (element->type_ == ElementType::FOLDER) {
    // Write only the membership changes since the last sync, so the cost of a sync does not
    // grow with the number of children the folder already has.
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    folder_mapper_.RemoveFolderContents(folder->element_id_,
                                        SortedIds(folder->ContentRemovedSinceSync()));
    folder_mapper_.InsertFolderContents(folder->element_id_,
                                        SortedIds(folder->ContentAddedSinceSync()));
  }
}

void ElementStore::UpdateElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto db_lock = guard_.Lock();
  duckorm::begin_transaction(guard_.conn_);
  try {
    for (const auto& element : elements) {
      UpdateElementRows(element);
    }
    duckorm::commit_transaction(guard_.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard_.conn_);
    throw;
  }
  MarkFolderContentsSynced(elements);
  for (const auto& element : elements) {
    element->sync_flag_ = SyncFlag::SYNCED;
  }
}

auto ElementStore::GetElementsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                               const sl_element_id_t              folder_id)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  auto       db_lock    = guard_.Lock();
  const auto where_frag = FilterSQLCompiler::Compile(filter->GetRoot());
  const auto where      = where_frag.empty() ? std::optional<duckorm::SqlFragment>{} : where_frag;
  // Always resolve ids through the prepared-bind list path so SqlFragment
  // binds stay valid. Then load full elements by primary key.
  const auto ids        = ListFilteredFileIds(folder_id, where);
  std::vector<std::shared_ptr<SleeveElement>> out;
  out.reserve(ids.size());
  for (const auto id : ids) {
    if (auto element = element_mapper_.GetElementById(id)) {
      out.push_back(std::move(element));
    }
  }
  return out;
}

auto ElementStore::GetElementIdsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                                 const sl_element_id_t              folder_id)
    -> std::vector<sl_element_id_t> {
  auto       db_lock    = guard_.Lock();
  const auto where_frag = FilterSQLCompiler::Compile(filter->GetRoot());
  const auto where      = where_frag.empty() ? std::optional<duckorm::SqlFragment>{} : where_frag;
  return ListFilteredFileIds(folder_id, where);
}

auto ElementStore::ReadMatchSet(sl_element_id_t                            folder_id,
                                const std::optional<duckorm::SqlFragment>& extra_filter,
                                const std::string& active_semantic_model_key,
                                SearchResultPage* page, size_t offset, size_t limit) const
    -> FolderStatsView {
  duckdb_connection   conn = guard_.conn_;
  // The only statement that evaluates the filter; every read below uses its rows.
  const MatchSetTable match_set(conn, BuildScopedFileQuery(folder_id, extra_filter));

  FolderStatsView     out;
  ReadMatchSetBuckets(conn, out);
  if (!active_semantic_model_key.empty()) {
    out.label_stats_ = ReadMatchSetLabelBuckets(conn, active_semantic_model_key);
  }

  if (page != nullptr) {
    auto sql = std::format(
        "SELECT file_id, image_id, file_name, camera_model, lens, "
        "CAST(capture_date AS VARCHAR), rating FROM {} ORDER BY file_id",
        kMatchSetTable);
    if (limit > 0) {
      sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    }
    duckdb_result result;
    ExecuteQueryOrThrow(conn, sql, {}, &result);
    const auto row_count = duckdb_row_count(&result);
    page->rows_.reserve(static_cast<size_t>(row_count));
    for (idx_t row = 0; row < row_count; ++row) {
      page->rows_.push_back(ReadSearchResultRow(&result, row));
    }
    duckdb_destroy_result(&result);
    page->total_ = static_cast<size_t>(out.total_photo_count_);
  }
  return out;
}

auto ElementStore::BuildFolderStats(sl_element_id_t                            folder_id,
                                    const std::optional<duckorm::SqlFragment>& extra_filter,
                                    const std::string& active_semantic_model_key) const
    -> FolderStatsView {
  auto db_lock = guard_.Lock();
  return ReadMatchSet(folder_id, extra_filter, active_semantic_model_key, nullptr, 0, 0);
}

auto ElementStore::ListSearchResultPageWithStats(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter,
    const std::string& active_semantic_model_key) const -> SearchResultPageWithStats {
  auto                      db_lock = guard_.Lock();
  SearchResultPageWithStats out;
  out.stats_ =
      ReadMatchSet(folder_id, extra_filter, active_semantic_model_key, &out.page_, offset, limit);
  return out;
}

auto ElementStore::ListFilesInFolder(sl_element_id_t folder_id) const
    -> std::vector<FileListEntry> {
  auto db_lock = guard_.Lock();
  return ListFilesInFolderPage(folder_id, 0, 0);
}

auto ElementStore::ListFilesInFolderPage(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter) const -> std::vector<FileListEntry> {
  auto                       db_lock = guard_.Lock();
  std::vector<FileListEntry> out;
  const auto                 scope = BuildScopedFileQuery(folder_id, extra_filter);
  auto                       sql =
      std::format("SELECT e.id, fi.image_id, e.element_name {} ORDER BY e.id", scope.from_where_);
  if (limit > 0) {
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
  }

  duckdb_result     result;
  duckdb_connection conn = guard_.conn_;
  if (duckorm::execute_query(conn, sql, scope.binds_, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return out;
  }

  const auto row_count = duckdb_row_count(&result);
  out.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    FileListEntry entry;
    entry.file_id_  = static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, r));
    entry.image_id_ = static_cast<image_id_t>(duckdb_value_int64(&result, 1, r));
    char* name_raw  = duckdb_value_varchar(&result, 2, r);
    if (name_raw) {
      entry.file_name_ = name_raw;
      duckdb_free(name_raw);
    }
    out.push_back(std::move(entry));
  }

  duckdb_destroy_result(&result);
  return out;
}

auto ElementStore::CountFilesInFolder(sl_element_id_t                            folder_id,
                                      const std::optional<duckorm::SqlFragment>& extra_filter) const
    -> size_t {
  auto              db_lock = guard_.Lock();
  const auto        scope   = BuildScopedFileQuery(folder_id, extra_filter);
  duckdb_connection conn    = guard_.conn_;
  return static_cast<size_t>(
      RunScalarInt64(conn, std::format("SELECT COUNT(*) {}", scope.from_where_), scope.binds_));
}

auto ElementStore::ListSearchResultPage(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter) const -> SearchResultPage {
  auto             db_lock = guard_.Lock();
  SearchResultPage out;
  const auto       scope = BuildScopedFileQuery(folder_id, extra_filter);
  // The window value counts the rows before LIMIT applies, so one statement gives the page
  // and the total.
  auto sql = std::format("SELECT {}, COUNT(*) OVER () {} ORDER BY e.id", kSearchResultColumns,
                         scope.from_where_);
  if (limit > 0) {
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
  }

  duckdb_result     result;
  duckdb_connection conn = guard_.conn_;
  if (duckorm::execute_query(conn, sql, scope.binds_, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return out;
  }

  const auto row_count = duckdb_row_count(&result);
  out.rows_.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    out.rows_.push_back(ReadSearchResultRow(&result, r));
  }
  if (row_count > 0) {
    out.total_ = static_cast<size_t>(duckdb_value_int64(&result, kSearchResultColumnCount, 0));
  }
  duckdb_destroy_result(&result);

  if (row_count == 0 && offset > 0) {
    out.total_ = static_cast<size_t>(
        RunScalarInt64(conn, std::format("SELECT COUNT(*) {}", scope.from_where_), scope.binds_));
  }
  return out;
}

auto ElementStore::ListSearchResultRows(std::span<const sl_element_id_t> file_ids) const
    -> std::vector<SearchResultRow> {
  std::vector<SearchResultRow> out;
  if (file_ids.empty()) {
    return out;
  }
  auto db_lock = guard_.Lock();
  auto query   = duckorm::expr::raw(
      std::format("SELECT {} FROM Element e "
                    "JOIN FileImage fi ON fi.file_id = e.id "
                    "JOIN Image i ON i.id = fi.image_id "
                    "WHERE e.type = {} AND ",
                  kSearchResultColumns, static_cast<uint32_t>(ElementType::FILE)));
  query.append(duckorm::expr::in_list(duckorm::expr::col("e.id"), file_ids));

  duckdb_result     result;
  duckdb_connection conn = guard_.conn_;
  if (duckorm::execute_query(conn, query.sql_, query, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return out;
  }

  std::unordered_map<sl_element_id_t, SearchResultRow> rows_by_id;
  const auto                                           row_count = duckdb_row_count(&result);
  rows_by_id.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    auto row = ReadSearchResultRow(&result, r);
    rows_by_id.emplace(row.file_id_, std::move(row));
  }
  duckdb_destroy_result(&result);

  out.reserve(file_ids.size());
  for (const auto file_id : file_ids) {
    if (const auto it = rows_by_id.find(file_id); it != rows_by_id.end()) {
      out.push_back(it->second);
    }
  }
  return out;
}

auto ElementStore::ListFilteredFileIds(
    sl_element_id_t folder_id, const std::optional<duckorm::SqlFragment>& extra_filter) const
    -> std::vector<sl_element_id_t> {
  auto                         db_lock = guard_.Lock();
  std::vector<sl_element_id_t> out;
  const auto                   scope = BuildScopedFileQuery(folder_id, extra_filter);
  const auto                   sql = std::format("SELECT e.id {} ORDER BY e.id", scope.from_where_);

  duckdb_result                result;
  duckdb_connection            conn = guard_.conn_;
  if (duckorm::execute_query(conn, sql, scope.binds_, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return out;
  }

  const auto row_count = duckdb_row_count(&result);
  out.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    out.push_back(static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, r)));
  }

  duckdb_destroy_result(&result);
  return out;
}

auto ElementStore::GetPipelineJsonByElementId(sl_element_id_t element_id)
    -> std::optional<nlohmann::json> {
  auto db_lock = guard_.Lock();
  return pipeline_mapper_.GetPipelineJsonByFileId(element_id);
}

void ElementStore::UpdatePipelineJsonByElementId(sl_element_id_t       element_id,
                                                 const nlohmann::json& document) {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.UpdatePipelineJsonByFileId(element_id, document);
}

auto ElementStore::RemovePipelineByElementId(const sl_element_id_t element_id) -> void {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.RemoveById(element_id);
}

auto ElementStore::RemovePipelinesByElementIds(std::span<const sl_element_id_t> element_ids)
    -> void {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.RemoveByIds(element_ids);
}

};  // namespace alcedo
