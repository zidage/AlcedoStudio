//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/element_store.hpp"

#include <QLoggingCategory>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "image/color_label.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_select.hpp"
#include "storage/query/filter_compiler.hpp"
#include "storage/query/stats_dimension.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/semantic/semantic_vector_search.hpp"
#include "storage/store/sleeve/album_edit_state_sql.hpp"
#include "storage/store/sleeve/album_query_sql.hpp"
#include "type/type.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

auto BuildScopedFileQuery(sl_element_id_t                            folder_id,
                          const std::optional<duckorm::SqlFragment>& extra_filter,
                          const std::optional<duckorm::SqlFragment>& relevance_select)
    -> ScopedFileQuery {
  ScopedFileQuery scope;
  std::string     relevance_join;
  if (relevance_select.has_value()) {
    // The join comes before the WHERE text, so its binds come before the filter binds.
    relevance_join = " JOIN (" + relevance_select->sql_ +
                     ") rel ON rel.file_id = e.id AND rel.image_id = fi.image_id";
    scope.binds_.binds_    = relevance_select->binds_;
    scope.joins_relevance_ = true;
  }
  std::string extra_where;
  if (extra_filter.has_value() && !extra_filter->empty()) {
    extra_where       = " AND (" + extra_filter->sql_ + ")";
    scope.binds_.sql_ = extra_filter->sql_;
    scope.binds_.binds_.insert(scope.binds_.binds_.end(), extra_filter->binds_.begin(),
                               extra_filter->binds_.end());
  }

  if (folder_id == 0) {
    scope.from_where_ = std::format(
        "FROM Element e "
        "JOIN FileImage fi ON fi.file_id = e.id "
        "JOIN Image i ON i.id = fi.image_id{} "
        "WHERE e.type = {}{}",
        relevance_join, static_cast<uint32_t>(ElementType::FILE), extra_where);
    return scope;
  }

  scope.from_where_ = std::format(
      "FROM FolderContent fc "
      "JOIN Element e ON fc.element_id = e.id "
      "JOIN FileImage fi ON fi.file_id = e.id "
      "JOIN Image i ON i.id = fi.image_id{} "
      "WHERE fc.folder_id = {} AND e.type = {}{}",
      relevance_join, folder_id, static_cast<uint32_t>(ElementType::FILE), extra_where);
  return scope;
}

namespace {

namespace album_sql = album_query_sql;
using album_sql::kMatchSetTable;

/// The relevance select of @p semantic_relevance over @p folder_id on @p conn, or empty without
/// a natural-language search. The caller holds the connection lock. Throws like
/// BuildSemanticRelevanceSelect.
auto RelevanceSelectOf(duckorm::Connection conn, sl_element_id_t folder_id,
                       const std::optional<SemanticQueryEmbedding>& semantic_relevance)
    -> std::optional<duckorm::SqlFragment> {
  if (!semantic_relevance.has_value()) {
    return std::nullopt;
  }
  return BuildSemanticRelevanceSelect(conn, folder_id, *semantic_relevance);
}

/// COUNT(*) of @p from_where with the binds of @p binds. Throws std::runtime_error on failure.
auto CountRows(duckorm::Connection conn, const std::string& from_where,
               const duckorm::SqlFragment& binds) -> int64_t {
  return duckorm::scalar<int64_t>(
             conn, duckorm::SqlFragment{"SELECT COUNT(*) " + from_where, binds.binds_})
      .value_or(0);
}

// Display columns of a search result row, in SearchResultDbRow order. Aliases follow
// BuildScopedFileQuery (`e` Element, `fi` FileImage, `i` Image).
constexpr const char* kSearchResultColumns =
    "e.id, fi.image_id, i.file_name, i.camera_model, i.lens, "
    "CAST(i.capture_date AS VARCHAR), i.rating, CAST(epoch(e.added_time) AS BIGINT), "
    "i.color_labels, i.delete_mark";
// The same columns read from the match set table (alias `s`).
constexpr const char* kMatchSetResultColumns =
    "s.file_id, s.image_id, s.file_name, s.camera_model, s.lens, "
    "CAST(s.capture_date AS VARCHAR), s.rating, CAST(epoch(s.added_time) AS BIGINT), "
    "s.color_labels, s.delete_mark";

/// One row of kSearchResultColumns or kMatchSetResultColumns. The text, rating, and mark
/// columns are optional: databases written before their NOT NULL defaults (and the #768 mark
/// migration, which adds no NOT NULL constraint) can hold NULL there, and a NULL reads as the
/// empty text, rating 0, no color label, and no Delete mark.
struct SearchResultDbRow {
  int64_t                    file_id  = 0;
  int64_t                    image_id = 0;
  std::optional<std::string> file_name;
  std::optional<std::string> camera_model;
  std::optional<std::string> lens;
  std::optional<std::string> capture_date;
  std::optional<int32_t>     rating;
  std::optional<int64_t>     added_time;
  std::optional<uint8_t>     color_labels;
  std::optional<bool>        delete_mark;
  using Columns =
      duckorm::RowColumns<&SearchResultDbRow::file_id, &SearchResultDbRow::image_id,
                          &SearchResultDbRow::file_name, &SearchResultDbRow::camera_model,
                          &SearchResultDbRow::lens, &SearchResultDbRow::capture_date,
                          &SearchResultDbRow::rating, &SearchResultDbRow::added_time,
                          &SearchResultDbRow::color_labels, &SearchResultDbRow::delete_mark>;
};

/// A search result row followed by `COUNT(*) OVER ()`: the rows that match before LIMIT.
struct SearchResultPageDbRow : SearchResultDbRow {
  int64_t total_count = 0;
  using Columns =
      duckorm::AppendRowColumns<SearchResultDbRow::Columns, &SearchResultPageDbRow::total_count>;
};

auto ToSearchResultRow(const SearchResultDbRow& row) -> SearchResultRow {
  return SearchResultRow{.file_id_      = static_cast<sl_element_id_t>(row.file_id),
                         .image_id_     = static_cast<image_id_t>(row.image_id),
                         .file_name_    = row.file_name.value_or(std::string{}),
                         .camera_model_ = row.camera_model.value_or(std::string{}),
                         .lens_         = row.lens.value_or(std::string{}),
                         .capture_date_ = row.capture_date.value_or(std::string{}),
                         .rating_       = static_cast<int>(row.rating.value_or(0)),
                         .color_labels_ = static_cast<ColorLabelMask>(row.color_labels.value_or(0) &
                                                                      kColorLabelMaskAll),
                         .delete_mark_  = row.delete_mark.value_or(false),
                         .added_time_   = row.added_time};
}

/// One file id column (`e.id`, `s.file_id`, or an occurrence file id).
struct FileIdRow {
  int64_t file_id = 0;
  using Columns   = duckorm::RowColumns<&FileIdRow::file_id>;
};

auto ReadFileIds(duckorm::Connection conn, const duckorm::SqlFragment& query)
    -> std::vector<sl_element_id_t> {
  const auto                   rows = duckorm::query<FileIdRow>(conn, query);
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) {
    ids.push_back(static_cast<sl_element_id_t>(row.file_id));
  }
  return ids;
}

/// One file of ListFilesInFolderPage; `Element.element_name` has no NOT NULL constraint.
struct FileListDbRow {
  int64_t                    file_id  = 0;
  int64_t                    image_id = 0;
  std::optional<std::string> element_name;
  using Columns = duckorm::RowColumns<&FileListDbRow::file_id, &FileListDbRow::image_id,
                                      &FileListDbRow::element_name>;
};

/// The one row of ReadProjectOverview. The dates are NULL when no file has a capture date.
struct ProjectOverviewRow {
  int64_t                    photo_count        = 0;
  int64_t                    edited_photo_count = 0;
  std::optional<std::string> earliest_capture_date;
  std::optional<std::string> latest_capture_date;
  using Columns =
      duckorm::RowColumns<&ProjectOverviewRow::photo_count, &ProjectOverviewRow::edited_photo_count,
                          &ProjectOverviewRow::earliest_capture_date,
                          &ProjectOverviewRow::latest_capture_date>;
};

/// Photos of one folder, or of the whole project when the folder id is NULL.
struct FolderPhotoCountRow {
  std::optional<int64_t> folder_id;
  int64_t                photo_count = 0;
  using Columns =
      duckorm::RowColumns<&FolderPhotoCountRow::folder_id, &FolderPhotoCountRow::photo_count>;
};

/// The file and image ids of the last edited file.
struct LastEditedFileRow {
  int64_t file_id  = 0;
  int64_t image_id = 0;
  using Columns    = duckorm::RowColumns<&LastEditedFileRow::file_id, &LastEditedFileRow::image_id>;
};

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
  /// The relevance column holds `rel.relevance` when @p scope joins a relevance select, else
  /// NULL; the relevance select therefore runs once for the whole read.
  MatchSetTable(duckorm::Connection conn, const ScopedFileQuery& scope,
                const std::string& import_day_time_zone = {})
      : conn_(conn) {
    namespace expr = duckorm::expr;
    const std::vector<duckorm::SqlFragment> projection{
        expr::raw("e.id AS file_id, fi.image_id, i.file_name, i.camera_model, i.lens, "
                  "i.capture_date, i.capture_at, i.rating, e.added_time, i.color_labels, "
                  "i.delete_mark"),
        duckorm::clause::as(stats_dimension::ImportDayValue(import_day_time_zone), "import_day"),
        duckorm::clause::as(expr::raw(scope.joins_relevance_ ? "CAST(rel.relevance AS DOUBLE)"
                                                             : "CAST(NULL AS DOUBLE)"),
                            "relevance")};
    auto statement = expr::raw(std::format("CREATE OR REPLACE TEMP TABLE {} AS ", kMatchSetTable));
    statement.append(duckorm::clause::select_query(
        projection, duckorm::SqlFragment{scope.from_where_, scope.binds_.binds_}));
    duckorm::execute(conn_, statement);
  }
  MatchSetTable(const MatchSetTable&)            = delete;
  MatchSetTable& operator=(const MatchSetTable&) = delete;
  /// Drops the table. The destructor does not throw, so the error that ended the read (if any)
  /// is the one the caller sees. A failed drop leaves the table until the connection closes at
  /// the end of the operation. During the unwind of a failed read inside a transaction the
  /// drop fails because DuckDB aborted the transaction, and the ROLLBACK removes the table, so
  /// only a drop that fails outside an unwind is logged.
  ~MatchSetTable() noexcept {
    try {
      duckorm::execute(
          conn_, duckorm::expr::raw(std::format("DROP TABLE IF EXISTS temp.{}", kMatchSetTable)));
    } catch (const std::exception& error) {
      if (std::uncaught_exceptions() == 0) {
        qCWarning(diag::appLog) << "ElementStore: dropping the match set table failed:"
                                << error.what();
      }
    }
  }

 private:
  duckorm::Connection conn_;
};

// GROUPING(d, m, l, r, im) of each grouping set in ReadMatchSetBuckets: the bit of a column is
// 1 when the set does not group by it (first argument is the highest bit).
constexpr int64_t kDateGroup       = 0b01111;
constexpr int64_t kCameraGroup     = 0b10111;
constexpr int64_t kLensGroup       = 0b11011;
constexpr int64_t kRatingGroup     = 0b11101;
constexpr int64_t kImportDateGroup = 0b11110;
constexpr int64_t kTotalGroup      = 0b11111;

/// One row of the GROUPING SETS statement of ReadMatchSetBuckets. The label is NULL for the
/// total and for an unknown capture or import day.
struct StatsBucketRow {
  int64_t                    grouping_id = 0;
  std::optional<std::string> label;
  int64_t                    count = 0;
  using Columns = duckorm::RowColumns<&StatsBucketRow::grouping_id, &StatsBucketRow::label,
                                      &StatsBucketRow::count>;
};

/// Photos of each color label code (in ColorLabel order) and photos with the Delete mark.
struct MarkCountRow {
  int64_t red         = 0;
  int64_t orange      = 0;
  int64_t yellow      = 0;
  int64_t green       = 0;
  int64_t blue        = 0;
  int64_t purple      = 0;
  int64_t gray        = 0;
  int64_t delete_mark = 0;
  using Columns =
      duckorm::RowColumns<&MarkCountRow::red, &MarkCountRow::orange, &MarkCountRow::yellow,
                          &MarkCountRow::green, &MarkCountRow::blue, &MarkCountRow::purple,
                          &MarkCountRow::gray, &MarkCountRow::delete_mark>;
};
static_assert(MarkCountRow::Columns::kCount == kColorLabelCount + 1,
              "MarkCountRow has one column for each color label code and one for the Delete mark");
static_assert(static_cast<int>(ColorLabel::kRed) == 0 && static_cast<int>(ColorLabel::kGray) == 6,
              "MarkCountRow lists the color label codes in code order");

/// Files of one canonical semantic label key.
struct LabelBucketRow {
  std::string label_key;
  int64_t     count = 0;
  using Columns     = duckorm::RowColumns<&LabelBucketRow::label_key, &LabelBucketRow::count>;
};

/// Read the total and the date, camera, lens, rating, and (with @p read_import_days) local
/// import-day buckets of the match set in one GROUPING SETS statement. Orders: dates and
/// ratings descending (unknown date last), cameras and lenses by count descending, then by name.
void ReadMatchSetBuckets(duckorm::Connection conn, FolderStatsView& out, bool read_import_days) {
  namespace expr = duckorm::expr;
  using stats_dimension::GroupExpression;
  // GROUPING() needs every argument in some set, so the import-day set is always read; it is
  // dropped below when the read has no import-day time zone (every value is then NULL).
  const std::vector<std::vector<duckorm::SqlFragment>> sets{{expr::col("d")},  {expr::col("m")},
                                                            {expr::col("l")},  {expr::col("r")},
                                                            {expr::col("im")}, {}};
  // One bucket label column for each dimension, from its stats dimension definition.
  const std::vector<duckorm::SqlFragment>              labels{
      duckorm::clause::as(GroupExpression(StatsDimensionId::kCaptureDate), "d"),
      duckorm::clause::as(GroupExpression(StatsDimensionId::kCameraModel), "m"),
      duckorm::clause::as(GroupExpression(StatsDimensionId::kLens), "l"),
      duckorm::clause::as(GroupExpression(StatsDimensionId::kRating), "r"),
      duckorm::clause::as(GroupExpression(StatsDimensionId::kImportDay), "im")};
  auto query = expr::raw(
      "SELECT g, label, c FROM (SELECT GROUPING(d, m, l, r, im) AS g, d, r, im, "
      "COALESCE(d, m, l, r, im) AS label, COUNT(*) AS c FROM (");
  query.append(
      duckorm::clause::select_query(labels, expr::raw(std::format("FROM {} s", kMatchSetTable))));
  query.sql_.append(")");
  query.append(duckorm::clause::grouping_sets(sets));
  query.sql_.append(
      std::format(") ORDER BY g, CASE WHEN g = {} THEN d END DESC NULLS LAST, "
                  "CASE WHEN g = {} THEN r END DESC NULLS LAST, "
                  "CASE WHEN g = {} THEN im END DESC NULLS LAST, c DESC, label",
                  kDateGroup, kRatingGroup, kImportDateGroup));

  for (auto& row : duckorm::query<StatsBucketRow>(conn, query)) {
    const auto count = static_cast<int>(row.count);
    if (row.grouping_id == kTotalGroup) {
      out.total_photo_count_ = count;
      continue;
    }
    StorageStatsBucket bucket{.label_ = std::move(row.label).value_or(std::string{}),
                              .count_ = count};
    switch (row.grouping_id) {
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
}

/// Photos of each color label code and photos with the Delete mark in the match set, in one scan.
/// A photo counts once for each code it holds.
void ReadMatchSetMarkCounts(duckorm::Connection conn, FolderStatsView& out) {
  using stats_dimension::BucketPredicate;
  using stats_dimension::ColumnSource;
  // Each count reads the bucket predicate of its stats dimension, the filter of a click.
  const auto count_of = [](const StatsBucketCondition& bucket) {
    auto count = duckorm::expr::raw("COUNT(*) FILTER (WHERE ");
    count.append(BucketPredicate(bucket, ColumnSource::kMatchSet));
    count.sql_.append(")");
    return count;
  };
  std::vector<duckorm::SqlFragment> count_columns;
  count_columns.reserve(kColorLabelCount + 1);
  for (int code = 0; code < kColorLabelCount; ++code) {
    count_columns.push_back(count_of(StatsBucketCondition{
        .dimension = StatsDimensionId::kColorLabel, .key = static_cast<ColorLabel>(code)}));
  }
  count_columns.push_back(
      count_of(StatsBucketCondition{.dimension = StatsDimensionId::kDeleteMark, .key = {}}));
  const auto query = duckorm::clause::select_query(
      count_columns, duckorm::expr::raw(std::format("FROM {} s", kMatchSetTable)));
  // An aggregate without GROUP BY returns one row, also for an empty match set.
  const auto row = duckorm::query_one<MarkCountRow>(conn, query).value_or(MarkCountRow{});
  const std::array<int64_t, kColorLabelCount> counts{row.red,  row.orange, row.yellow, row.green,
                                                     row.blue, row.purple, row.gray};
  for (size_t code = 0; code < counts.size(); ++code) {
    out.color_label_counts_[code] = static_cast<int>(counts[code]);
  }
  out.delete_mark_count_ = static_cast<int>(row.delete_mark);
}

/// Semantic label buckets of the match set for the active model: files for each canonical
/// label key (the same relation as the label group), by count descending, then by key.
auto ReadMatchSetLabelBuckets(duckorm::Connection conn,
                              const std::string&  active_semantic_model_key)
    -> std::vector<StorageStatsBucket> {
  auto query = duckorm::expr::raw("WITH ");
  query.append(album_sql::LabelRelations(active_semantic_model_key));
  query.sql_.append(
      " SELECT label_key, COUNT(*) AS c FROM label_membership GROUP BY label_key "
      "ORDER BY c DESC, label_key");
  std::vector<StorageStatsBucket> buckets;
  for (auto& row : duckorm::query<LabelBucketRow>(conn, query)) {
    buckets.push_back({.label_ = std::move(row.label_key), .count_ = static_cast<int>(row.count)});
  }
  return buckets;
}

/// Stats (and, when @p page is set, one page ordered by file id) of the files that match
/// @p extra_filter. The caller holds the lock of @p conn.
auto ReadMatchSet(duckorm::Connection conn, sl_element_id_t folder_id,
                  const std::optional<duckorm::SqlFragment>& extra_filter,
                  const std::string& active_semantic_model_key, SearchResultPage* page,
                  size_t offset, size_t limit,
                  const std::optional<duckorm::SqlFragment>& relevance_select = std::nullopt)
    -> FolderStatsView {
  // The only statement that evaluates the filter; every read below uses its rows.
  const MatchSetTable match_set(conn,
                                BuildScopedFileQuery(folder_id, extra_filter, relevance_select));

  FolderStatsView     out;
  ReadMatchSetBuckets(conn, out, false);
  ReadMatchSetMarkCounts(conn, out);
  if (!active_semantic_model_key.empty()) {
    out.label_stats_ = ReadMatchSetLabelBuckets(conn, active_semantic_model_key);
  }

  if (page != nullptr) {
    auto sql = std::format("SELECT {} FROM {} s ORDER BY s.file_id", kMatchSetResultColumns,
                           kMatchSetTable);
    if (limit > 0) {
      sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    }
    const auto rows = duckorm::query<SearchResultDbRow>(conn, duckorm::expr::raw(sql));
    page->rows_.reserve(rows.size());
    for (const auto& row : rows) {
      page->rows_.push_back(ToSearchResultRow(row));
    }
    page->total_ = static_cast<size_t>(out.total_photo_count_);
  }
  return out;
}

/**
 * @brief Call @p read with the C++ type of the group key column of @p field.
 *
 * `int64_t` for ratings (GroupKeyProjection casts the key to BIGINT); `std::string` for every
 * other field and for the flat mode, whose key column is a NULL VARCHAR.
 */
template <typename Read>
auto WithGroupKeyType(AlbumGroupField field, Read&& read) {
  if (field == AlbumGroupField::kRating) {
    return std::forward<Read>(read)(std::type_identity<int64_t>{});
  }
  return std::forward<Read>(read)(std::type_identity<std::string>{});
}

/// Group key of one row: the typed key, or std::monostate for NULL (the unknown group) and for
/// the flat mode.
template <typename Key>
auto ToGroupKey(AlbumGroupField field, std::optional<Key> key) -> AlbumGroupKey {
  if (field == AlbumGroupField::kNone || !key.has_value()) {
    return std::monostate{};
  }
  return std::move(*key);
}

/// One group of the group read: its key and its photo count.
template <typename Key>
struct AlbumGroupRow {
  std::optional<Key> group_key;
  int64_t            photo_count = 0;
  using Columns = duckorm::RowColumns<&AlbumGroupRow::group_key, &AlbumGroupRow::photo_count>;
};

/// One photo row of a page: the search result columns, the group key, and the edit state.
/// The edit columns are NULL for an unedited photo.
template <typename Key>
struct AlbumPageRow : SearchResultDbRow {
  std::optional<Key>         group_key;
  std::optional<uint64_t>    edited_at_ns;
  std::optional<std::string> version_name;
  using Columns =
      duckorm::AppendRowColumns<SearchResultDbRow::Columns, &AlbumPageRow::group_key,
                                &AlbumPageRow::edited_at_ns, &AlbumPageRow::version_name>;
};

/// Occurrence position of one file and the group key of that occurrence.
template <typename Key>
struct AlbumPositionRow {
  int64_t            position = 0;
  std::optional<Key> group_key;
  using Columns = duckorm::RowColumns<&AlbumPositionRow::position, &AlbumPositionRow::group_key>;
};

/// Group key projection: the typed key as text (BIGINT for ratings), or a NULL VARCHAR in the
/// flat mode. WithGroupKeyType names the C++ type of each case.
auto GroupKeyProjection(AlbumGroupField field) -> duckorm::SqlFragment {
  namespace expr = duckorm::expr;
  if (field == AlbumGroupField::kNone) {
    return expr::raw("CAST(NULL AS VARCHAR)");
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
auto OccurrencePosition(const AlbumQueryOptions& options, bool has_relevance)
    -> duckorm::SqlFragment {
  auto position = duckorm::expr::raw("ROW_NUMBER() OVER (");
  position.append(
      duckorm::clause::order_by(album_sql::OccurrenceOrderTerms(options, has_relevance)));
  position.sql_.append(") - 1");
  return position;
}

/// Read one bounded page, the counts, the groups, and the statistics from the match set.
/// @p has_relevance: the match set holds the relevance of a relevance select.
auto ReadAlbumQueryFromMatchSet(duckorm::Connection conn, const AlbumQueryOptions& options,
                                const std::string&    active_semantic_model_key,
                                const AlbumQueryRead& read, bool has_relevance)
    -> AlbumQueryResult {
  namespace expr = duckorm::expr;
  const auto       field = options.group_field_;
  AlbumQueryResult out;
  out.first_occurrence_  = read.offset_;
  out.unique_file_count_ = CountRows(conn, std::format("FROM {}", kMatchSetTable), {});
  out.occurrence_count_  = out.unique_file_count_;

  const auto source =
      album_sql::OccurrenceSource(options, album_sql::EditStateProjection::kNotRead);
  if (field == AlbumGroupField::kLabels) {
    auto count = album_sql::RelationsPrefix(options, active_semantic_model_key,
                                            album_sql::EditStateProjection::kNotRead);
    count.append(duckorm::clause::select_query(std::vector{duckorm::clause::count_all()}, source));
    out.occurrence_count_ = duckorm::scalar<int64_t>(conn, count).value_or(0);
  }

  if (read.read_groups_ && field != AlbumGroupField::kNone) {
    auto query = album_sql::RelationsPrefix(options, active_semantic_model_key,
                                            album_sql::EditStateProjection::kNotRead);
    query.append(duckorm::clause::select_query(
        std::vector{duckorm::clause::as(GroupKeyProjection(field), "group_key"),
                    duckorm::clause::as(duckorm::clause::count_all(), "photo_count")},
        source));
    // group_key is the text (or BIGINT) form of the typed key, so both group terms give one
    // group per typed key; the typed key (GroupOrderTerm) keeps the order of dates and ratings.
    // The key expression has no binds, so the GROUP BY and the ORDER BY repeat it.
    query.append(duckorm::clause::group_by(
        std::vector{album_sql::GroupKeyExpression(field), expr::col("group_key")}));
    query.append(duckorm::clause::order_by(std::vector{album_sql::GroupOrderTerm(options)}));
    WithGroupKeyType(field, [&]<typename Key>(std::type_identity<Key>) {
      auto rows = duckorm::query<AlbumGroupRow<Key>>(conn, query);
      out.groups_.reserve(rows.size());
      int64_t next_occurrence = 0;
      for (auto& row : rows) {
        out.groups_.push_back({.key_              = ToGroupKey(field, std::move(row.group_key)),
                               .photo_count_      = row.photo_count,
                               .first_occurrence_ = next_occurrence});
        next_occurrence += row.photo_count;
      }
    });
  }

  if (read.limit_ > 0) {
    // Every photo row carries its edit state, so the card shows the edit mark in any
    // presentation, not only when the edit-time sort or group reads the relation.
    auto query = album_sql::RelationsPrefix(options, active_semantic_model_key,
                                            album_sql::EditStateProjection::kRead);
    query.append(duckorm::clause::select_query(
        std::vector{expr::raw(kMatchSetResultColumns), GroupKeyProjection(field),
                    expr::raw("es.edited_at_ns, es.version_name")},
        album_sql::OccurrenceSource(options, album_sql::EditStateProjection::kRead)));
    query.append(
        duckorm::clause::order_by(album_sql::OccurrenceOrderTerms(options, has_relevance)));
    query.append(duckorm::clause::limit_offset(read.limit_, read.offset_, kMaxAlbumQueryPageRows));
    WithGroupKeyType(field, [&]<typename Key>(std::type_identity<Key>) {
      auto rows = duckorm::query<AlbumPageRow<Key>>(conn, query);
      out.rows_.reserve(rows.size());
      for (auto& row : rows) {
        AlbumQueryRow query_row{.photo_     = ToSearchResultRow(row),
                                .group_key_ = ToGroupKey(field, std::move(row.group_key))};
        if (row.edited_at_ns.has_value()) {
          query_row.edited_at_ns_      = static_cast<int64_t>(*row.edited_at_ns);
          query_row.edit_version_name_ = std::move(row.version_name).value_or(std::string{});
        }
        out.rows_.push_back(std::move(query_row));
      }
    });
  }

  if (read.read_occurrence_file_ids_) {
    auto query = album_sql::RelationsPrefix(options, active_semantic_model_key,
                                            album_sql::EditStateProjection::kNotRead);
    query.append(duckorm::clause::select_query(std::vector{expr::raw("s.file_id")}, source));
    query.append(
        duckorm::clause::order_by(album_sql::OccurrenceOrderTerms(options, has_relevance)));
    out.occurrence_file_ids_ = ReadFileIds(conn, query);
  }

  if (read.read_statistics_) {
    FolderStatsView statistics;
    ReadMatchSetBuckets(conn, statistics, !options.local_day_time_zone_.empty());
    ReadMatchSetMarkCounts(conn, statistics);
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

void DeleteSemanticAndAiRowsForFiles(duckorm::Connection              conn,
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

void ElementStore::InsertElementRows(duckorm::Connection&                  conn,
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
  {
    duckorm::Transaction transaction(guard.conn_);
    ElementMapper(guard.conn_).InsertParamsRows(element_rows);
    FileMapper(guard.conn_).InsertParamsRows(file_rows);
    FolderMapper folder_mapper(guard.conn_);
    for (const auto& element : elements) {
      if (element->type_ == ElementType::FOLDER) {
        const auto folder = std::static_pointer_cast<SleeveFolder>(element);
        folder_mapper.InsertFolderContents(folder->element_id_, folder->ListElements());
      }
    }
    transaction.commit();
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
  const auto folder_param = duckorm::expr::param(static_cast<int64_t>(folder_id));
  auto       children     = ElementMapper(guard.conn_)
                      .GetElementsByQuery(duckorm::SqlFragment{
                          "SELECT e.id, e.type, e.element_name, e.added_time, e.modified_time, "
                          "e.ref_count FROM Element e JOIN FolderContent fc "
                          "ON fc.element_id = e.id WHERE fc.folder_id = ?",
                          folder_param.binds_});
  const auto bindings =
      FileMapper(guard.conn_)
          .GetByQuery(duckorm::SqlFragment{"SELECT fi.file_id, fi.image_id FROM FileImage fi "
                                           "JOIN FolderContent fc ON fc.element_id = fi.file_id "
                                           "WHERE fc.folder_id = ?",
                                           folder_param.binds_});

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

void ElementStore::UpdateElementRows(duckorm::Connection&                  conn,
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
  {
    duckorm::Transaction transaction(guard.conn_);
    for (const auto& element : elements) {
      UpdateElementRows(guard.conn_, element);
    }
    transaction.commit();
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

auto ElementStore::BuildFolderStats(
    sl_element_id_t folder_id, const std::optional<duckorm::SqlFragment>& extra_filter,
    const std::string&                           active_semantic_model_key,
    const std::optional<SemanticQueryEmbedding>& semantic_relevance) const -> FolderStatsView {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  return ReadMatchSet(guard.conn_, folder_id, extra_filter, active_semantic_model_key, nullptr, 0,
                      0, RelevanceSelectOf(guard.conn_, folder_id, semantic_relevance));
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

auto ElementStore::ReadAlbumQuery(
    sl_element_id_t folder_id, const std::optional<duckorm::SqlFragment>& extra_filter,
    const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
    const AlbumQueryRead&                        read,
    const std::optional<SemanticQueryEmbedding>& semantic_relevance) const -> AlbumQueryResult {
  ValidateAlbumQueryRead(options, read);
  auto                 guard   = database_.GetConnectionGuard();
  auto                 db_lock = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  AlbumQueryResult     out;
  {
    // The only statement that evaluates the filter and the relevance select; every read below
    // uses its rows.
    const auto scope = BuildScopedFileQuery(
        folder_id, extra_filter, RelevanceSelectOf(guard.conn_, folder_id, semantic_relevance));
    const MatchSetTable match_set(guard.conn_, scope, options.local_day_time_zone_);
    out = ReadAlbumQueryFromMatchSet(guard.conn_, options, active_semantic_model_key, read,
                                     scope.joins_relevance_);
  }
  transaction.commit();
  return out;
}

auto ElementStore::ReadAlbumFilePosition(
    sl_element_id_t folder_id, const std::optional<duckorm::SqlFragment>& extra_filter,
    const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
    sl_element_id_t file_id, const std::optional<AlbumGroupKey>& preferred_group,
    const std::optional<SemanticQueryEmbedding>& semantic_relevance) const
    -> std::optional<AlbumFilePosition> {
  namespace expr = duckorm::expr;
  ValidateAlbumQueryOptions(options);
  auto                             guard   = database_.GetConnectionGuard();
  auto                             db_lock = guard.Lock();
  duckorm::Transaction             transaction(guard.conn_);
  std::optional<AlbumFilePosition> out;
  {
    const auto scope = BuildScopedFileQuery(
        folder_id, extra_filter, RelevanceSelectOf(guard.conn_, folder_id, semantic_relevance));
    const MatchSetTable match_set(guard.conn_, scope, options.local_day_time_zone_);
    auto                query = album_sql::RelationsPrefix(
        options, active_semantic_model_key, album_sql::EditStateProjection::kNotRead);
    query.sql_.append("SELECT position, group_key FROM (");
    query.append(duckorm::clause::select_query(
        std::vector{
            expr::raw("s.file_id AS occurrence_file_id"),
            duckorm::clause::as(GroupKeyProjection(options.group_field_), "group_key"),
            duckorm::clause::as(OccurrencePosition(options, scope.joins_relevance_), "position")},
        album_sql::OccurrenceSource(options, album_sql::EditStateProjection::kNotRead)));
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

    WithGroupKeyType(options.group_field_, [&]<typename Key>(std::type_identity<Key>) {
      if (auto row = duckorm::query_one<AlbumPositionRow<Key>>(guard.conn_, query)) {
        out = AlbumFilePosition{
            .occurrence_index_ = row->position,
            .group_key_        = ToGroupKey(options.group_field_, std::move(row->group_key))};
      }
    });
  }
  transaction.commit();
  return out;
}

auto ElementStore::ReadAlbumFileIds(
    sl_element_id_t folder_id, const std::optional<duckorm::SqlFragment>& extra_filter,
    const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
    int64_t occurrence_begin, int64_t occurrence_end,
    const std::optional<SemanticQueryEmbedding>& semantic_relevance) const
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
    const auto scope = BuildScopedFileQuery(
        folder_id, extra_filter, RelevanceSelectOf(guard.conn_, folder_id, semantic_relevance));
    const MatchSetTable match_set(guard.conn_, scope, options.local_day_time_zone_);
    auto                query = album_sql::RelationsPrefix(
        options, active_semantic_model_key, album_sql::EditStateProjection::kNotRead);
    query.sql_.append("SELECT occurrence_file_id FROM (");
    query.append(duckorm::clause::select_query(
        std::vector{
            expr::raw("s.file_id AS occurrence_file_id"),
            duckorm::clause::as(OccurrencePosition(options, scope.joins_relevance_), "position")},
        album_sql::OccurrenceSource(options, album_sql::EditStateProjection::kNotRead)));
    query.sql_.append(") WHERE position >= ");
    query.append(expr::param(occurrence_begin));
    query.sql_.append(" AND position < ");
    query.append(expr::param(occurrence_end));
    query.append(duckorm::clause::group_by(std::vector{expr::col("occurrence_file_id")}));
    query.append(duckorm::clause::order_by(std::vector<duckorm::OrderTerm>{
        {expr::raw("MIN(position)"), duckorm::OrderDirection::kAscending,
         duckorm::NullPlacement::kLast}}));

    out = ReadFileIds(guard.conn_, query);
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
  const auto                 scope   = BuildScopedFileQuery(folder_id, extra_filter);
  auto                       sql =
      std::format("SELECT e.id, fi.image_id, e.element_name {} ORDER BY e.id", scope.from_where_);
  if (limit > 0) {
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
  }

  const auto rows = duckorm::query<FileListDbRow>(
      guard.conn_, duckorm::SqlFragment{std::move(sql), scope.binds_.binds_});
  std::vector<FileListEntry> out;
  out.reserve(rows.size());
  for (const auto& row : rows) {
    out.push_back({.file_id_   = static_cast<sl_element_id_t>(row.file_id),
                   .image_id_  = static_cast<image_id_t>(row.image_id),
                   .file_name_ = row.element_name.value_or(std::string{})});
  }
  return out;
}

auto ElementStore::CountFilesInFolder(sl_element_id_t                            folder_id,
                                      const std::optional<duckorm::SqlFragment>& extra_filter) const
    -> size_t {
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(folder_id, extra_filter);
  return static_cast<size_t>(CountRows(guard.conn_, scope.from_where_, scope.binds_));
}

auto ElementStore::ReadProjectOverview() const -> ProjectOverviewCounts {
  // Import writes the edit history root and an empty Version head but no EditCommit row. A file
  // counts as edited by the same rule as the library edit groups: its active head is a commit.
  static constexpr const char* kOverviewQuery =
      "SELECT COUNT(*), COUNT(*) FILTER (WHERE e.id IN ({})), "
      "CAST(MIN(i.capture_date) AS VARCHAR), CAST(MAX(i.capture_date) AS VARCHAR) {}";
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(0);
  const auto sql =
      std::format(kOverviewQuery, album_edit_state_sql::EditedFileIdsQuery(), scope.from_where_);

  ProjectOverviewCounts out;
  if (auto row = duckorm::query_one<ProjectOverviewRow>(
          guard.conn_, duckorm::SqlFragment{sql, scope.binds_.binds_})) {
    out.photo_count_           = static_cast<uint64_t>(row->photo_count);
    out.edited_photo_count_    = static_cast<uint64_t>(row->edited_photo_count);
    out.earliest_capture_date_ = std::move(row->earliest_capture_date);
    out.latest_capture_date_   = std::move(row->latest_capture_date);
  }
  return out;
}

auto ElementStore::ReadFolderImageCounts() const -> FolderImageCounts {
  // One row per folder that holds a photo, then one row with a NULL folder id for the whole
  // project. Both parts join the tables of BuildScopedFileQuery, so every count equals the
  // total of the library view of its folder.
  static constexpr const char* kFolderCountsQuery =
      "SELECT fc.folder_id, COUNT(*) "
      "FROM FolderContent fc "
      "JOIN Element e ON fc.element_id = e.id "
      "JOIN FileImage fi ON fi.file_id = e.id "
      "JOIN Image i ON i.id = fi.image_id "
      "WHERE e.type = {} GROUP BY fc.folder_id "
      "UNION ALL SELECT CAST(NULL AS BIGINT), COUNT(*) {}";

  auto              guard   = database_.GetConnectionGuard();
  auto              db_lock = guard.Lock();
  const auto        project = BuildScopedFileQuery(0);
  const auto        sql = std::format(kFolderCountsQuery, static_cast<uint32_t>(ElementType::FILE),
                                      project.from_where_);

  FolderImageCounts out;
  for (const auto& row : duckorm::query<FolderPhotoCountRow>(
           guard.conn_, duckorm::SqlFragment{sql, project.binds_.binds_})) {
    const auto count = static_cast<uint64_t>(row.photo_count);
    if (row.folder_id.has_value()) {
      out.folder_photo_counts_[static_cast<sl_element_id_t>(*row.folder_id)] = count;
    } else {
      out.project_photo_count_ = count;
    }
  }
  return out;
}

auto ElementStore::ReadLastEditedFile() const -> std::optional<LastEditedFile> {
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  // Ties of the strictly increasing commit clock cannot occur; the file id only makes the
  // order total.
  const auto sql     = std::format(
      "SELECT ies.element_id, fi.image_id FROM {} "
          "JOIN Element e ON e.id = ies.element_id AND e.type = {} "
          "JOIN FileImage fi ON fi.file_id = e.id "
          "ORDER BY hc.created_at_ns DESC, ies.element_id DESC LIMIT 1",
      album_edit_state_sql::kActiveHeadCommitJoin, static_cast<uint32_t>(ElementType::FILE));
  const auto row = duckorm::query_one<LastEditedFileRow>(guard.conn_, duckorm::expr::raw(sql));
  if (!row.has_value()) {
    return std::nullopt;
  }
  return LastEditedFile{.file_id_  = static_cast<sl_element_id_t>(row->file_id),
                        .image_id_ = static_cast<image_id_t>(row->image_id)};
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

  const auto rows = duckorm::query<SearchResultPageDbRow>(
      guard.conn_, duckorm::SqlFragment{std::move(sql), scope.binds_.binds_});
  out.rows_.reserve(rows.size());
  for (const auto& row : rows) {
    out.rows_.push_back(ToSearchResultRow(row));
  }
  if (!rows.empty()) {
    out.total_ = static_cast<size_t>(rows.front().total_count);
  } else if (offset > 0) {
    out.total_ = static_cast<size_t>(CountRows(guard.conn_, scope.from_where_, scope.binds_));
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

  const auto rows = duckorm::query<SearchResultDbRow>(guard.conn_, query);
  std::unordered_map<sl_element_id_t, SearchResultRow> rows_by_id;
  rows_by_id.reserve(rows.size());
  for (const auto& db_row : rows) {
    auto row = ToSearchResultRow(db_row);
    rows_by_id.emplace(row.file_id_, std::move(row));
  }

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
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(folder_id, extra_filter);
  return ReadFileIds(guard.conn_, duckorm::SqlFragment{std::format("SELECT e.id {} ORDER BY e.id",
                                                                   scope.from_where_),
                                                       scope.binds_.binds_});
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
