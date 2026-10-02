//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/element_store.hpp"

#include <duckdb.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_select.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/sleeve/album_query_sql.hpp"
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

namespace album_sql = album_query_sql;
using album_sql::kMatchSetTable;

/// Count of a listing COUNT query. Throws std::runtime_error with DuckDB's message on failure.
auto RunScalarInt64(duckdb_connection conn, const std::string& sql,
                    const duckorm::SqlFragment& binds) -> int64_t {
  return duckorm::select_int64(conn, duckorm::SqlFragment{sql, binds.binds_}).value_or(0);
}

// Display columns of a search result row, in SearchResultRow order. Aliases follow
// BuildScopedFileQuery (`e` Element, `fi` FileImage, `i` Image).
constexpr const char* kSearchResultColumns =
    "e.id, fi.image_id, i.file_name, i.camera_model, i.lens, "
    "CAST(i.capture_date AS VARCHAR), i.rating, CAST(epoch(e.added_time) AS BIGINT)";
constexpr idx_t       kSearchResultColumnCount = 8;
// The same columns read from the match set table (alias `s`).
constexpr const char* kMatchSetResultColumns =
    "s.file_id, s.image_id, s.file_name, s.camera_model, s.lens, "
    "CAST(s.capture_date AS VARCHAR), s.rating, CAST(epoch(s.added_time) AS BIGINT)";

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
  if (!duckdb_value_is_null(result, 7, row)) {
    out.added_time_ = duckdb_value_int64(result, 7, row);
  }
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
// columns that the stats buckets, the groups, the sort, and the result rows read. It is a copy
// because the page, the total, every group, and every bucket must come from one evaluation of
// the filter; a query on the live tables evaluates it again for each statement (Phase S8).
// Only the read columns are copied (no metadata JSON). The table is read-only, never written
// back, and exists on the connection of one ElementStore operation only while that operation
// holds the database lock, so no write can change the rows between the reads; MatchSetTable
// drops it before the operation releases its connection.

/// Owns the match set table for one read and drops it at scope exit, also after a failed
/// statement, so the matched rows do not stay in memory between reads.
class MatchSetTable {
 public:
  /// @param import_day_time_zone IANA zone of the import_day column; empty stores NULL there.
  MatchSetTable(duckdb_connection conn, const ScopedFileQuery& scope,
                const std::string& import_day_time_zone = {})
      : conn_(conn) {
    namespace expr = duckorm::expr;
    const std::vector<duckorm::SqlFragment> projection{
        expr::raw("e.id AS file_id, fi.image_id, i.file_name, i.camera_model, i.lens, "
                  "i.capture_date, i.capture_at, i.rating, e.added_time"),
        duckorm::clause::as(album_sql::ImportDayColumn(import_day_time_zone), "import_day")};
    auto statement = expr::raw(std::format("CREATE OR REPLACE TEMP TABLE {} AS ", kMatchSetTable));
    statement.append(duckorm::clause::select_query(
        projection, duckorm::SqlFragment{scope.from_where_, scope.binds_.binds_}));
    duckorm::execute(conn_, statement);
  }
  MatchSetTable(const MatchSetTable&)            = delete;
  MatchSetTable& operator=(const MatchSetTable&) = delete;
  ~MatchSetTable() {
    duckdb_result result;
    // A failed drop leaves the table until the connection closes at the end of the operation.
    duckdb_query(conn_, std::format("DROP TABLE IF EXISTS temp.{}", kMatchSetTable).c_str(),
                 &result);
    duckdb_destroy_result(&result);
  }

 private:
  duckdb_connection conn_;
};

// GROUPING(d, m, l, r, im) of each grouping set in ReadMatchSetBuckets: the bit of a column is
// 1 when the set does not group by it (first argument is the highest bit).
constexpr int64_t kDateGroup       = 0b01111;
constexpr int64_t kCameraGroup     = 0b10111;
constexpr int64_t kLensGroup       = 0b11011;
constexpr int64_t kRatingGroup     = 0b11101;
constexpr int64_t kImportDateGroup = 0b11110;
constexpr int64_t kTotalGroup      = 0b11111;

/// Read the total and the date, camera, lens, rating, and (with @p read_import_days) local
/// import-day buckets of the match set in one GROUPING SETS statement. Orders: dates and
/// ratings descending (unknown date last), cameras and lenses by count descending, then by name.
void ReadMatchSetBuckets(duckdb_connection conn, FolderStatsView& out, bool read_import_days) {
  namespace expr = duckorm::expr;
  // GROUPING() needs every argument in some set, so the import-day set is always read; it is
  // dropped below when the read has no import-day time zone (every value is then NULL).
  const std::vector<std::vector<duckorm::SqlFragment>> sets{{expr::col("d")},  {expr::col("m")},
                                                            {expr::col("l")},  {expr::col("r")},
                                                            {expr::col("im")}, {}};
  auto                                                 sql = std::format(
      "SELECT g, label, c FROM ("
                                                      "SELECT GROUPING(d, m, l, r, im) AS g, d, r, im, COALESCE(d, m, l, r, im) AS label, "
                                                      "COUNT(*) AS c "
                                                      "FROM (SELECT CAST(capture_date AS VARCHAR) AS d, "
                                                      "COALESCE(NULLIF(camera_model, ''), '(unknown)') AS m, "
                                                      "COALESCE(NULLIF(lens, ''), '(unknown)') AS l, CAST(rating AS VARCHAR) AS r, "
                                                      "CAST(import_day AS VARCHAR) AS im FROM {})",
      kMatchSetTable);
  sql += duckorm::clause::grouping_sets(sets).sql_;
  sql += std::format(
      ") ORDER BY g, CASE WHEN g = {} THEN d END DESC NULLS LAST, "
      "CASE WHEN g = {} THEN r END DESC NULLS LAST, "
      "CASE WHEN g = {} THEN im END DESC NULLS LAST, c DESC, label",
      kDateGroup, kRatingGroup, kImportDateGroup);
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
      case kImportDateGroup:
        if (read_import_days) {
          out.import_date_stats_.push_back(std::move(bucket));
        }
        break;
      default:
        break;
    }
  }
  duckdb_destroy_result(&result);
}

/// Semantic label buckets of the match set for the active model: files for each canonical
/// label key (the same relation as the label group), by count descending, then by key.
auto ReadMatchSetLabelBuckets(duckdb_connection conn, const std::string& active_semantic_model_key)
    -> std::vector<StorageStatsBucket> {
  auto query = album_sql::LabelRelations(active_semantic_model_key);
  query.sql_.append(
      "SELECT label_key, COUNT(*) AS c FROM label_membership GROUP BY label_key "
      "ORDER BY c DESC, label_key");
  duckdb_result result;
  ExecuteQueryOrThrow(conn, query.sql_, query, &result);
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

/// Stats (and, when @p page is set, one page ordered by file id) of the files that match
/// @p extra_filter. The caller holds the lock of @p conn.
auto ReadMatchSet(duckdb_connection conn, sl_element_id_t folder_id,
                  const std::optional<duckorm::SqlFragment>& extra_filter,
                  const std::string& active_semantic_model_key, SearchResultPage* page,
                  size_t offset, size_t limit) -> FolderStatsView {
  // The only statement that evaluates the filter; every read below uses its rows.
  const MatchSetTable match_set(conn, BuildScopedFileQuery(folder_id, extra_filter));

  FolderStatsView     out;
  ReadMatchSetBuckets(conn, out, false);
  if (!active_semantic_model_key.empty()) {
    out.label_stats_ = ReadMatchSetLabelBuckets(conn, active_semantic_model_key);
  }

  if (page != nullptr) {
    auto sql = std::format("SELECT {} FROM {} s ORDER BY s.file_id", kMatchSetResultColumns,
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

/// Group key of one result cell, decoded by the group field: a number for ratings, text for
/// every other field, and std::monostate for NULL (the unknown group) or the flat mode.
auto ReadGroupKey(duckdb_result* result, idx_t column, idx_t row, AlbumGroupField field)
    -> AlbumGroupKey {
  if (field == AlbumGroupField::kNone || duckdb_value_is_null(result, column, row)) {
    return std::monostate{};
  }
  if (field == AlbumGroupField::kRating) {
    return duckdb_value_int64(result, column, row);
  }
  return ReadVarchar(result, column, row);
}

/// Group key projection: the typed key as text (BIGINT for ratings), or NULL in the flat mode.
auto GroupKeyProjection(AlbumGroupField field) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;
  if (field == AlbumGroupField::kNone) {
    return expr::raw("NULL");
  }
  if (field == AlbumGroupField::kRating) {
    auto out = expr::raw("CAST(");
    out.append(album_sql::GroupKeyExpression(field));
    out.sql_.append(" AS BIGINT)");
    return out;
  }
  auto out = expr::raw("CAST(");
  out.append(album_sql::GroupKeyExpression(field));
  out.sql_.append(" AS VARCHAR)");
  return out;
}

/// `ROW_NUMBER() OVER (ORDER BY <occurrence order>) - 1`: the 0-based occurrence position.
auto OccurrencePosition(const AlbumQueryOptions& options) -> duckorm::SqlFragment {
  auto position = duckorm::expr::raw("ROW_NUMBER() OVER (");
  position.append(duckorm::clause::order_by(album_sql::OccurrenceOrderTerms(options)));
  position.sql_.append(") - 1");
  return position;
}

/// The label relations prefix when the options read labels, else an empty fragment.
auto RelationsPrefix(const AlbumQueryOptions& options, const std::string& active_semantic_model_key)
    -> duckorm::SqlFragment {
  if (!album_sql::UsesLabelRelation(options)) {
    return {};
  }
  return album_sql::LabelRelations(active_semantic_model_key);
}

/// Read one bounded page, the counts, the groups, and the statistics from the match set.
auto ReadAlbumQueryFromMatchSet(duckdb_connection conn, const AlbumQueryOptions& options,
                                const std::string&    active_semantic_model_key,
                                const AlbumQueryRead& read) -> AlbumQueryResult {
  namespace expr = duckorm::expr;
  AlbumQueryResult out;
  out.first_occurrence_  = read.offset_;
  out.unique_file_count_ = RunScalarInt64(
      conn, std::format("SELECT COUNT(*) FROM {}", kMatchSetTable), duckorm::SqlFragment{});
  out.occurrence_count_ = out.unique_file_count_;

  const auto source     = album_sql::OccurrenceSource(options);
  if (options.group_field_ == AlbumGroupField::kLabels) {
    auto count = RelationsPrefix(options, active_semantic_model_key);
    count.append(duckorm::clause::select_query(std::vector{duckorm::clause::count_all()}, source));
    out.occurrence_count_ = RunScalarInt64(conn, count.sql_, count);
  }

  if (read.read_groups_ && options.group_field_ != AlbumGroupField::kNone) {
    auto query = RelationsPrefix(options, active_semantic_model_key);
    query.append(duckorm::clause::select_query(
        std::vector{
            duckorm::clause::as(GroupKeyProjection(options.group_field_), "group_key"),
            duckorm::clause::as(album_sql::GroupKeyExpression(options.group_field_), "group_order"),
            duckorm::clause::as(duckorm::clause::count_all(), "photo_count")},
        source));
    // group_key is the text (or BIGINT) form of group_order, so both group keys give one
    // group per typed key; the typed group_order keeps the order of dates and ratings.
    query.append(
        duckorm::clause::group_by(std::vector{expr::col("group_order"), expr::col("group_key")}));
    auto group_order        = album_sql::GroupOrderTerm(options.group_field_);
    group_order.expression_ = expr::col("group_order");
    query.append(duckorm::clause::order_by(std::vector{group_order}));
    duckdb_result result;
    ExecuteQueryOrThrow(conn, query.sql_, query, &result);
    const auto row_count = duckdb_row_count(&result);
    out.groups_.reserve(static_cast<size_t>(row_count));
    int64_t next_occurrence = 0;
    for (idx_t row = 0; row < row_count; ++row) {
      AlbumGroupDescriptor group;
      group.key_              = ReadGroupKey(&result, 0, row, options.group_field_);
      group.photo_count_      = duckdb_value_int64(&result, 2, row);
      group.first_occurrence_ = next_occurrence;
      next_occurrence += group.photo_count_;
      out.groups_.push_back(std::move(group));
    }
    duckdb_destroy_result(&result);
  }

  if (read.limit_ > 0) {
    auto query = RelationsPrefix(options, active_semantic_model_key);
    query.append(duckorm::clause::select_query(
        std::vector{expr::raw(kMatchSetResultColumns), GroupKeyProjection(options.group_field_)},
        source));
    query.append(duckorm::clause::order_by(album_sql::OccurrenceOrderTerms(options)));
    query.append(duckorm::clause::limit_offset(read.limit_, read.offset_, kMaxAlbumQueryPageRows));
    duckdb_result result;
    ExecuteQueryOrThrow(conn, query.sql_, query, &result);
    const auto row_count = duckdb_row_count(&result);
    out.rows_.reserve(static_cast<size_t>(row_count));
    for (idx_t row = 0; row < row_count; ++row) {
      out.rows_.push_back({.photo_     = ReadSearchResultRow(&result, row),
                           .group_key_ = ReadGroupKey(&result, kSearchResultColumnCount, row,
                                                      options.group_field_)});
    }
    duckdb_destroy_result(&result);
  }

  if (read.read_statistics_) {
    FolderStatsView statistics;
    ReadMatchSetBuckets(conn, statistics, !options.import_day_time_zone_.empty());
    if (!active_semantic_model_key.empty()) {
      statistics.label_stats_ = ReadMatchSetLabelBuckets(conn, active_semantic_model_key);
    }
    out.statistics_ = std::move(statistics);
  }
  return out;
}

/// Validate the scalar inputs of an album read before any statement runs.
void ValidateAlbumQueryRead(const AlbumQueryOptions& options, const AlbumQueryRead& read) {
  ValidateAlbumQueryOptions(options);
  if (read.offset_ < 0) {
    throw std::invalid_argument("Album query: the page offset is negative");
  }
  if (read.limit_ < 0 || read.limit_ > kMaxAlbumQueryPageRows) {
    throw std::invalid_argument("Album query: the page limit is out of range");
  }
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

ElementStore::ElementStore(Database& database) : database_(database) {}

/**
 * @brief Add an element to the database.
 *
 * @param element
 */
void ElementStore::AddElement(const std::shared_ptr<SleeveElement> element) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  InsertElementRows(guard.conn_, element);
  MarkFolderContentsSynced(std::span<const std::shared_ptr<SleeveElement>>(&element, 1));
  element->sync_flag_ = SyncFlag::SYNCED;
}

void ElementStore::InsertElementRows(duckdb_connection&                    conn,
                                     const std::shared_ptr<SleeveElement>& element) {
  ElementMapper(conn).Insert(element);
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    FileMapper(conn).Insert({file->element_id_, file->image_id_});
  } else if (element->type_ == ElementType::FOLDER) {
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    FolderMapper(conn).InsertFolderContents(folder->element_id_, folder->ListElements());
  }
}

void ElementStore::AddElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  // Element and file binding rows go in multi-row statements; one prepared insert per row
  // dominated the project sync after an import.
  std::vector<ElementMapperParams> element_rows;
  std::vector<FileMapperParams>    file_rows;
  element_rows.reserve(elements.size());
  file_rows.reserve(elements.size());
  for (const auto& element : elements) {
    element_rows.push_back(ElementMapper::ToParams(element));
    if (element->type_ == ElementType::FILE) {
      const auto file = std::static_pointer_cast<SleeveFile>(element);
      file_rows.push_back(FileMapper::ToParams({file->element_id_, file->image_id_}));
    }
  }

  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  duckorm::begin_transaction(guard.conn_);
  try {
    ElementMapper(guard.conn_).InsertParamsRows(element_rows);
    FileMapper(guard.conn_).InsertParamsRows(file_rows);
    FolderMapper folder_mapper(guard.conn_);
    for (const auto& element : elements) {
      if (element->type_ == ElementType::FOLDER) {
        const auto folder = std::static_pointer_cast<SleeveFolder>(element);
        folder_mapper.InsertFolderContents(folder->element_id_, folder->ListElements());
      }
    }
    duckorm::commit_transaction(guard.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard.conn_);
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
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  FolderMapper(guard.conn_).Insert({folder_id, content_id});
}

void ElementStore::RemoveFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  FolderMapper(guard.conn_).RemoveFolderContent(folder_id, content_id);
}

/**
 * @brief Get an element by its ID from the database.
 *
 * @param id
 * @return std::shared_ptr<SleeveElement>
 */
auto ElementStore::GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto result  = ElementMapper(guard.conn_).GetElementById(id);
  if (result->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(result);
    try {
      file->image_id_ = FileMapper(guard.conn_).GetBoundImageById(file->element_id_);
    } catch (...) {
      file->image_id_ = 0;
    }
  }
  result->SetSyncFlag(SyncFlag::SYNCED);
  return result;
}

auto ElementStore::GetFolderChildren(const sl_element_id_t folder_id)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto children =
      ElementMapper(guard.conn_)
          .GetByQuery(std::format(
              "SELECT e.id, e.type, e.element_name, e.added_time, e.modified_time, e.ref_count "
              "FROM Element e JOIN FolderContent fc ON fc.element_id = e.id WHERE fc.folder_id = "
              "{}",
              folder_id));
  const auto bindings =
      FileMapper(guard.conn_)
          .GetByQuery(std::format(
              "SELECT fi.file_id, fi.image_id FROM FileImage fi "
              "JOIN FolderContent fc ON fc.element_id = fi.file_id WHERE fc.folder_id = {}",
              folder_id));

  // GetElementById binds an image only when the file has exactly one FileImage row.
  std::unordered_map<sl_element_id_t, std::pair<image_id_t, size_t>> image_by_file;
  image_by_file.reserve(bindings.size());
  for (const auto& [file_id, image_id] : bindings) {
    auto& entry = image_by_file[file_id];
    entry.first = image_id;
    ++entry.second;
  }
  for (const auto& child : children) {
    if (child->type_ == ElementType::FILE) {
      auto       file = std::static_pointer_cast<SleeveFile>(child);
      const auto it   = image_by_file.find(file->element_id_);
      file->image_id_ =
          (it != image_by_file.end() && it->second.second == 1) ? it->second.first : 0;
    }
    child->SetSyncFlag(SyncFlag::SYNCED);
  }
  return children;
}

/**
 * @brief Get the content of a folder by its ID from the database.
 *
 * @param folder_id
 * @return std::vector<sl_element_id_t>
 */
auto ElementStore::GetFolderContent(const sl_element_id_t folder_id)
    -> std::vector<sl_element_id_t> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  return FolderMapper(guard.conn_).GetFolderContent(folder_id);
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
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  ElementMapper(guard.conn_).RemoveById(id);
}

void ElementStore::RemoveElement(const std::shared_ptr<SleeveElement> element) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    DeleteSemanticAndAiRowsForFiles(guard.conn_,
                                    std::span<const sl_element_id_t>(&file->element_id_, 1));
    PipelineMapper(guard.conn_).RemoveById(file->element_id_);
    FileMapper(guard.conn_).RemoveById(file->element_id_);
    FolderMapper(guard.conn_).RemoveContentById(file->element_id_);
  } else if (element->type_ == ElementType::FOLDER) {
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    FolderMapper(guard.conn_).RemoveById(folder->element_id_);
  }
  ElementMapper(guard.conn_).RemoveById(element->element_id_);
}

void ElementStore::RemoveElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto                         guard   = database_.GetConnectionGuard();
  auto                         db_lock = guard.Lock();

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

  FolderMapper folder_mapper(guard.conn_);
  if (!file_ids.empty()) {
    DeleteSemanticAndAiRowsForFiles(guard.conn_, file_ids);
    PipelineMapper(guard.conn_).RemoveByIds(file_ids);
    FileMapper(guard.conn_).RemoveByIds(file_ids);
    folder_mapper.RemoveContentByIds(file_ids);
  }
  if (!folder_ids.empty()) {
    folder_mapper.RemoveByIds(folder_ids);
  }
  if (!element_ids.empty()) {
    ElementMapper(guard.conn_).RemoveByIds(element_ids);
  }
}

/**
 * @brief Update an element in the database.
 *
 * @param element
 */
void ElementStore::UpdateElement(const std::shared_ptr<SleeveElement> element) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  UpdateElementRows(guard.conn_, element);
  MarkFolderContentsSynced(std::span<const std::shared_ptr<SleeveElement>>(&element, 1));
  element->sync_flag_ = SyncFlag::SYNCED;
}

void ElementStore::UpdateElementRows(duckdb_connection&                    conn,
                                     const std::shared_ptr<SleeveElement>& element) {
  ElementMapper(conn).Update(element, element->element_id_);
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    FileMapper(conn).Update({file->element_id_, file->image_id_}, file->image_id_);
  } else if (element->type_ == ElementType::FOLDER) {
    // Write only the membership changes since the last sync, so the cost of a sync does not
    // grow with the number of children the folder already has.
    auto         folder = std::static_pointer_cast<SleeveFolder>(element);
    FolderMapper folder_mapper(conn);
    folder_mapper.RemoveFolderContents(folder->element_id_,
                                       SortedIds(folder->ContentRemovedSinceSync()));
    folder_mapper.InsertFolderContents(folder->element_id_,
                                       SortedIds(folder->ContentAddedSinceSync()));
  }
}

void ElementStore::UpdateElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  duckorm::begin_transaction(guard.conn_);
  try {
    for (const auto& element : elements) {
      UpdateElementRows(guard.conn_, element);
    }
    duckorm::commit_transaction(guard.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard.conn_);
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
  const auto where_frag = FilterSQLCompiler::Compile(filter->GetRoot());
  const auto where      = where_frag.empty() ? std::optional<duckorm::SqlFragment>{} : where_frag;
  // Always resolve ids through the prepared-bind list path so SqlFragment
  // binds stay valid. Then load full elements by primary key.
  const auto ids        = ListFilteredFileIds(folder_id, where);
  auto          guard      = database_.GetConnectionGuard();
  auto          db_lock    = guard.Lock();
  ElementMapper element_mapper(guard.conn_);
  std::vector<std::shared_ptr<SleeveElement>> out;
  out.reserve(ids.size());
  for (const auto id : ids) {
    if (auto element = element_mapper.GetElementById(id)) {
      out.push_back(std::move(element));
    }
  }
  return out;
}

auto ElementStore::GetElementIdsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                                 const sl_element_id_t              folder_id)
    -> std::vector<sl_element_id_t> {
  const auto where_frag = FilterSQLCompiler::Compile(filter->GetRoot());
  const auto where      = where_frag.empty() ? std::optional<duckorm::SqlFragment>{} : where_frag;
  return ListFilteredFileIds(folder_id, where);
}

auto ElementStore::BuildFolderStats(sl_element_id_t                            folder_id,
                                    const std::optional<duckorm::SqlFragment>& extra_filter,
                                    const std::string& active_semantic_model_key) const
    -> FolderStatsView {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  return ReadMatchSet(guard.conn_, folder_id, extra_filter, active_semantic_model_key, nullptr, 0,
                      0);
}

auto ElementStore::ListSearchResultPageWithStats(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter,
    const std::string& active_semantic_model_key) const -> SearchResultPageWithStats {
  auto                      guard   = database_.GetConnectionGuard();
  auto                      db_lock = guard.Lock();
  SearchResultPageWithStats out;
  out.stats_ = ReadMatchSet(guard.conn_, folder_id, extra_filter, active_semantic_model_key,
                            &out.page_, offset, limit);
  return out;
}

auto ElementStore::ReadAlbumQuery(sl_element_id_t                            folder_id,
                                  const std::optional<duckorm::SqlFragment>& extra_filter,
                                  const AlbumQueryOptions&                   options,
                                  const std::string&    active_semantic_model_key,
                                  const AlbumQueryRead& read) const -> AlbumQueryResult {
  ValidateAlbumQueryRead(options, read);
  auto                 guard   = database_.GetConnectionGuard();
  auto                 db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  AlbumQueryResult     out;
  {
    // The only statement that evaluates the filter; every read below uses its rows.
    const MatchSetTable match_set(guard.conn_, BuildScopedFileQuery(folder_id, extra_filter),
                                  options.import_day_time_zone_);
    out = ReadAlbumQueryFromMatchSet(guard.conn_, options, active_semantic_model_key, read);
  }
  transaction.commit();
  return out;
}

auto ElementStore::ReadAlbumFilePosition(sl_element_id_t                            folder_id,
                                         const std::optional<duckorm::SqlFragment>& extra_filter,
                                         const AlbumQueryOptions&                   options,
                                         const std::string& active_semantic_model_key,
                                         sl_element_id_t    file_id,
                                         const std::optional<AlbumGroupKey>& preferred_group) const
    -> std::optional<AlbumFilePosition> {
  namespace expr = duckorm::expr;
  ValidateAlbumQueryOptions(options);
  auto                             guard   = database_.GetConnectionGuard();
  auto                             db_lock = guard.Lock();
  duckorm::Transaction             transaction(guard.conn_);
  std::optional<AlbumFilePosition> out;
  {
    const MatchSetTable match_set(guard.conn_, BuildScopedFileQuery(folder_id, extra_filter),
                                  options.import_day_time_zone_);
    auto                query = RelationsPrefix(options, active_semantic_model_key);
    query.sql_.append("SELECT position, group_key FROM (");
    query.append(duckorm::clause::select_query(
        std::vector{expr::raw("s.file_id AS occurrence_file_id"),
                    duckorm::clause::as(GroupKeyProjection(options.group_field_), "group_key"),
                    duckorm::clause::as(OccurrencePosition(options), "position")},
        album_sql::OccurrenceSource(options)));
    query.sql_.append(") WHERE occurrence_file_id = ");
    query.append(expr::param(static_cast<int64_t>(file_id)));
    std::vector<duckorm::OrderTerm> order;
    const bool                      has_preferred_group =
        preferred_group.has_value() && !std::holds_alternative<std::monostate>(*preferred_group);
    if (has_preferred_group) {
      auto preferred = expr::raw("(group_key = ");
      if (const auto* number = std::get_if<int64_t>(&*preferred_group)) {
        preferred.append(expr::param(*number));
      } else {
        preferred.append(expr::param(std::get<std::string>(*preferred_group)));
      }
      preferred.sql_.append(")");
      order.push_back({std::move(preferred), duckorm::OrderDirection::kDescending,
                       duckorm::NullPlacement::kLast});
    } else if (preferred_group.has_value()) {
      // The unknown group is the NULL key.
      order.push_back({expr::raw("(group_key IS NULL)"), duckorm::OrderDirection::kDescending,
                       duckorm::NullPlacement::kLast});
    }
    order.push_back({expr::col("position"), duckorm::OrderDirection::kAscending,
                     duckorm::NullPlacement::kLast});
    query.append(duckorm::clause::order_by(order));
    query.append(duckorm::clause::limit_offset(1, 0, 1));

    duckdb_result result;
    ExecuteQueryOrThrow(guard.conn_, query.sql_, query, &result);
    if (duckdb_row_count(&result) > 0) {
      out = AlbumFilePosition{.occurrence_index_ = duckdb_value_int64(&result, 0, 0),
                              .group_key_ = ReadGroupKey(&result, 1, 0, options.group_field_)};
    }
    duckdb_destroy_result(&result);
  }
  transaction.commit();
  return out;
}

auto ElementStore::ReadAlbumFileIds(sl_element_id_t                            folder_id,
                                    const std::optional<duckorm::SqlFragment>& extra_filter,
                                    const AlbumQueryOptions&                   options,
                                    const std::string& active_semantic_model_key,
                                    int64_t occurrence_begin, int64_t occurrence_end) const
    -> std::vector<sl_element_id_t> {
  namespace expr = duckorm::expr;
  ValidateAlbumQueryOptions(options);
  if (occurrence_begin < 0 || occurrence_end < occurrence_begin) {
    throw std::invalid_argument("Album query: the occurrence range is invalid");
  }
  std::vector<sl_element_id_t> out;
  if (occurrence_begin == occurrence_end) {
    return out;
  }
  auto                 guard   = database_.GetConnectionGuard();
  auto                 db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  {
    const MatchSetTable match_set(guard.conn_, BuildScopedFileQuery(folder_id, extra_filter),
                                  options.import_day_time_zone_);
    auto                query = RelationsPrefix(options, active_semantic_model_key);
    query.sql_.append("SELECT occurrence_file_id FROM (");
    query.append(duckorm::clause::select_query(
        std::vector{expr::raw("s.file_id AS occurrence_file_id"),
                    duckorm::clause::as(OccurrencePosition(options), "position")},
        album_sql::OccurrenceSource(options)));
    query.sql_.append(") WHERE position >= ");
    query.append(expr::param(occurrence_begin));
    query.sql_.append(" AND position < ");
    query.append(expr::param(occurrence_end));
    query.append(duckorm::clause::group_by(std::vector{expr::col("occurrence_file_id")}));
    query.append(duckorm::clause::order_by(std::vector<duckorm::OrderTerm>{
        {expr::raw("MIN(position)"), duckorm::OrderDirection::kAscending,
         duckorm::NullPlacement::kLast}}));

    duckdb_result result;
    ExecuteQueryOrThrow(guard.conn_, query.sql_, query, &result);
    const auto row_count = duckdb_row_count(&result);
    out.reserve(static_cast<size_t>(row_count));
    for (idx_t row = 0; row < row_count; ++row) {
      out.push_back(static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, row)));
    }
    duckdb_destroy_result(&result);
  }
  transaction.commit();
  return out;
}

auto ElementStore::ListFilesInFolder(sl_element_id_t folder_id) const
    -> std::vector<FileListEntry> {
  return ListFilesInFolderPage(folder_id, 0, 0);
}

auto ElementStore::ListFilesInFolderPage(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter) const -> std::vector<FileListEntry> {
  auto                       guard   = database_.GetConnectionGuard();
  auto                       db_lock = guard.Lock();
  std::vector<FileListEntry> out;
  const auto                 scope = BuildScopedFileQuery(folder_id, extra_filter);
  auto                       sql =
      std::format("SELECT e.id, fi.image_id, e.element_name {} ORDER BY e.id", scope.from_where_);
  if (limit > 0) {
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
  }

  duckdb_result result;
  ExecuteQueryOrThrow(guard.conn_, sql, scope.binds_, &result);

  const auto row_count = duckdb_row_count(&result);
  out.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    FileListEntry entry;
    entry.file_id_   = static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, r));
    entry.image_id_  = static_cast<image_id_t>(duckdb_value_int64(&result, 1, r));
    entry.file_name_ = ReadVarchar(&result, 2, r);
    out.push_back(std::move(entry));
  }

  duckdb_destroy_result(&result);
  return out;
}

auto ElementStore::CountFilesInFolder(sl_element_id_t                            folder_id,
                                      const std::optional<duckorm::SqlFragment>& extra_filter) const
    -> size_t {
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(folder_id, extra_filter);
  return static_cast<size_t>(RunScalarInt64(
      guard.conn_, std::format("SELECT COUNT(*) {}", scope.from_where_), scope.binds_));
}

auto ElementStore::ReadProjectOverview() const -> ProjectOverviewCounts {
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(0);
  // Import writes the edit history root and an empty Version head but no EditCommit row, so a
  // file counts as edited only when its root has at least one commit.
  const auto sql = std::format(
      "SELECT COUNT(*), "
      "COUNT(*) FILTER (WHERE EXISTS (SELECT 1 FROM ImageEditState s JOIN EditCommit c "
      "ON c.root_id = s.root_id WHERE s.element_id = e.id)), "
      "CAST(MIN(i.capture_date) AS VARCHAR), CAST(MAX(i.capture_date) AS VARCHAR) {}",
      scope.from_where_);

  duckdb_result result;
  ExecuteQueryOrThrow(guard.conn_, sql, scope.binds_, &result);
  ProjectOverviewCounts out;
  if (duckdb_row_count(&result) > 0) {
    out.photo_count_        = static_cast<uint64_t>(duckdb_value_int64(&result, 0, 0));
    out.edited_photo_count_ = static_cast<uint64_t>(duckdb_value_int64(&result, 1, 0));
    if (!duckdb_value_is_null(&result, 2, 0)) {
      out.earliest_capture_date_ = ReadVarchar(&result, 2, 0);
    }
    if (!duckdb_value_is_null(&result, 3, 0)) {
      out.latest_capture_date_ = ReadVarchar(&result, 3, 0);
    }
  }
  duckdb_destroy_result(&result);
  return out;
}

auto ElementStore::ListSearchResultPage(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter) const -> SearchResultPage {
  auto             guard   = database_.GetConnectionGuard();
  auto             db_lock = guard.Lock();
  SearchResultPage out;
  const auto       scope = BuildScopedFileQuery(folder_id, extra_filter);
  // The window value counts the rows before LIMIT applies, so one statement gives the page
  // and the total.
  auto sql = std::format("SELECT {}, COUNT(*) OVER () {} ORDER BY e.id", kSearchResultColumns,
                         scope.from_where_);
  if (limit > 0) {
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
  }

  duckdb_result result;
  ExecuteQueryOrThrow(guard.conn_, sql, scope.binds_, &result);

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
    out.total_ = static_cast<size_t>(RunScalarInt64(
        guard.conn_, std::format("SELECT COUNT(*) {}", scope.from_where_), scope.binds_));
  }
  return out;
}

auto ElementStore::ListSearchResultRows(std::span<const sl_element_id_t> file_ids) const
    -> std::vector<SearchResultRow> {
  std::vector<SearchResultRow> out;
  if (file_ids.empty()) {
    return out;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = duckorm::expr::raw(
      std::format("SELECT {} FROM Element e "
                    "JOIN FileImage fi ON fi.file_id = e.id "
                    "JOIN Image i ON i.id = fi.image_id "
                    "WHERE e.type = {} AND ",
                  kSearchResultColumns, static_cast<uint32_t>(ElementType::FILE)));
  query.append(duckorm::expr::in_list(duckorm::expr::col("e.id"), file_ids));

  duckdb_result result;
  ExecuteQueryOrThrow(guard.conn_, query.sql_, query, &result);

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
  auto                         guard   = database_.GetConnectionGuard();
  auto                         db_lock = guard.Lock();
  std::vector<sl_element_id_t> out;
  const auto                   scope = BuildScopedFileQuery(folder_id, extra_filter);
  const auto                   sql = std::format("SELECT e.id {} ORDER BY e.id", scope.from_where_);

  duckdb_result                result;
  ExecuteQueryOrThrow(guard.conn_, sql, scope.binds_, &result);

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
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  return PipelineMapper(guard.conn_).GetPipelineJsonByFileId(element_id);
}

void ElementStore::UpdatePipelineJsonByElementId(sl_element_id_t       element_id,
                                                 const nlohmann::json& document) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  PipelineMapper(guard.conn_).UpdatePipelineJsonByFileId(element_id, document);
}

auto ElementStore::RemovePipelineByElementId(const sl_element_id_t element_id) -> void {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  PipelineMapper(guard.conn_).RemoveById(element_id);
}

auto ElementStore::RemovePipelinesByElementIds(std::span<const sl_element_id_t> element_ids)
    -> void {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  PipelineMapper(guard.conn_).RemoveByIds(element_ids);
}

};  // namespace alcedo
