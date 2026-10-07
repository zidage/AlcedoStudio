//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/album_query.hpp"

#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_select.hpp"
#include "storage/store/sleeve/album_edit_state_sql.hpp"
#include "storage/store/sleeve/album_query_sql.hpp"

namespace alcedo {
namespace {

auto IsKnownSortField(AlbumSortField field) -> bool {
  switch (field) {
    case AlbumSortField::kNone:
    case AlbumSortField::kCaptureTime:
    case AlbumSortField::kImportTime:
    case AlbumSortField::kCameraModel:
    case AlbumSortField::kLens:
    case AlbumSortField::kRating:
    case AlbumSortField::kLabels:
    case AlbumSortField::kEditTime:
      return true;
  }
  return false;
}

auto IsKnownGroupField(AlbumGroupField field) -> bool {
  switch (field) {
    case AlbumGroupField::kNone:
    case AlbumGroupField::kCaptureDay:
    case AlbumGroupField::kImportDay:
    case AlbumGroupField::kCameraModel:
    case AlbumGroupField::kLens:
    case AlbumGroupField::kRating:
    case AlbumGroupField::kLabels:
    case AlbumGroupField::kEditDay:
      return true;
  }
  return false;
}

auto IsKnownDirection(SortDirection direction) -> bool {
  return direction == SortDirection::kAscending || direction == SortDirection::kDescending;
}

}  // namespace

void ValidateAlbumQueryOptions(const AlbumQueryOptions& options) {
  if (!IsKnownSortField(options.sort_field_)) {
    throw std::invalid_argument("Album query: unknown sort field");
  }
  if (!IsKnownDirection(options.sort_direction_)) {
    throw std::invalid_argument("Album query: unknown sort direction");
  }
  if (!IsKnownGroupField(options.group_field_)) {
    throw std::invalid_argument("Album query: unknown group field");
  }
  if (options.group_field_ == AlbumGroupField::kImportDay && options.local_day_time_zone_.empty()) {
    throw std::invalid_argument("Album query: import-day groups need a time zone");
  }
  if (options.group_field_ == AlbumGroupField::kEditDay && options.local_day_time_zone_.empty()) {
    throw std::invalid_argument("Album query: edit-day groups need a time zone");
  }
}

namespace album_query_sql {
namespace expr = duckorm::expr;
using duckorm::NullPlacement;
using duckorm::OrderDirection;
using duckorm::OrderTerm;
using duckorm::SqlFragment;

auto ImportDayColumn(const std::string& time_zone) -> SqlFragment {
  if (time_zone.empty()) {
    return expr::raw("CAST(NULL AS DATE)");
  }
  // timezone('UTC', TIMESTAMP) reads the stored UTC text as an instant; timezone(zone,
  // TIMESTAMPTZ) gives the local wall time in that zone. Neither reads the connection's
  // TimeZone setting.
  auto column = expr::raw("CAST(timezone(CAST(");
  column.append(expr::param(time_zone));
  column.sql_.append(" AS VARCHAR), timezone('UTC', e.added_time)) AS DATE)");
  return column;
}

auto UsesLabelRelation(const AlbumQueryOptions& options) -> bool {
  return options.group_field_ == AlbumGroupField::kLabels ||
         options.sort_field_ == AlbumSortField::kLabels;
}

auto LabelRelations(const std::string& active_semantic_model_key) -> SqlFragment {
  // Label assignment stores canonical keys, and opening a project rewrites older alias texts
  // (SemanticLabelStore::CanonicalizeImageLabels), so the stored label is the key. LOWER and
  // TRIM only fold the case and spaces of a label outside the taxonomy.
  auto relations = expr::raw(
      "label_membership AS (SELECT DISTINCT sl.file_id, LOWER(TRIM(sl.label)) AS label_key "
      "FROM SemanticImageLabel sl ");
  relations.sql_.append("WHERE sl.model_key = ");
  relations.append(expr::param(active_semantic_model_key));
  relations.sql_.append(std::format(
      " AND TRIM(sl.label) <> '' AND sl.file_id IN (SELECT file_id FROM {})), "
      "label_sort AS (SELECT file_id, MIN(label_key) AS label_sort_key FROM label_membership "
      "GROUP BY file_id)",
      kMatchSetTable));
  return relations;
}

auto UsesEditStateRelation(const AlbumQueryOptions& options) -> bool {
  return options.group_field_ == AlbumGroupField::kEditDay ||
         options.sort_field_ == AlbumSortField::kEditTime;
}

auto RelationsPrefix(const AlbumQueryOptions& options, const std::string& active_semantic_model_key)
    -> SqlFragment {
  SqlFragment prefix;
  const auto  add = [&prefix](SqlFragment definition) {
    prefix.sql_.append(prefix.empty() ? "WITH " : ", ");
    prefix.append(std::move(definition));
  };
  if (UsesLabelRelation(options)) {
    add(LabelRelations(active_semantic_model_key));
  }
  if (UsesEditStateRelation(options)) {
    add(album_edit_state_sql::EditStateRelation(options.local_day_time_zone_, kMatchSetTable));
  }
  if (!prefix.empty()) {
    prefix.sql_.append(" ");
  }
  return prefix;
}

auto OccurrenceSource(const AlbumQueryOptions& options) -> SqlFragment {
  auto source = expr::raw(std::format("FROM {} s", kMatchSetTable));
  if (UsesEditStateRelation(options)) {
    source.sql_.append(" LEFT JOIN edit_state es ON es.file_id = s.file_id");
  }
  if (options.group_field_ == AlbumGroupField::kLabels) {
    source.sql_.append(" LEFT JOIN label_membership m ON m.file_id = s.file_id");
  }
  if (options.sort_field_ == AlbumSortField::kLabels) {
    source.sql_.append(" LEFT JOIN label_sort ls ON ls.file_id = s.file_id");
  }
  return source;
}

auto GroupKeyExpression(AlbumGroupField field) -> SqlFragment {
  switch (field) {
    case AlbumGroupField::kNone:
      return {};
    case AlbumGroupField::kCaptureDay:
      return expr::col("s.capture_date");
    case AlbumGroupField::kImportDay:
      return expr::col("s.import_day");
    case AlbumGroupField::kCameraModel:
      return expr::col("NULLIF(s.camera_model, '')");
    case AlbumGroupField::kLens:
      return expr::col("NULLIF(s.lens, '')");
    case AlbumGroupField::kRating:
      return expr::col("s.rating");
    case AlbumGroupField::kLabels:
      return expr::col("m.label_key");
    case AlbumGroupField::kEditDay:
      return expr::col("es.edit_day");
  }
  throw std::invalid_argument("Album query: unknown group field");
}

namespace {

/// The photo sort that reads the values of @p field (the same Inspector field).
auto SortFieldOfGroup(AlbumGroupField field) -> AlbumSortField {
  switch (field) {
    case AlbumGroupField::kCaptureDay:
      return AlbumSortField::kCaptureTime;
    case AlbumGroupField::kImportDay:
      return AlbumSortField::kImportTime;
    case AlbumGroupField::kEditDay:
      return AlbumSortField::kEditTime;
    case AlbumGroupField::kCameraModel:
      return AlbumSortField::kCameraModel;
    case AlbumGroupField::kLens:
      return AlbumSortField::kLens;
    case AlbumGroupField::kRating:
      return AlbumSortField::kRating;
    case AlbumGroupField::kLabels:
      return AlbumSortField::kLabels;
    case AlbumGroupField::kNone:
      break;
  }
  return AlbumSortField::kNone;
}

/// Fixed order of the groups of @p field when no sort of the same field is selected.
auto DefaultGroupOrderTerm(AlbumGroupField field) -> OrderTerm {
  switch (field) {
    case AlbumGroupField::kCaptureDay:
    case AlbumGroupField::kImportDay:
    case AlbumGroupField::kEditDay:
    case AlbumGroupField::kRating:
      return {GroupKeyExpression(field), OrderDirection::kDescending, NullPlacement::kLast};
    case AlbumGroupField::kCameraModel:
    case AlbumGroupField::kLens:
    case AlbumGroupField::kLabels:
      return {GroupKeyExpression(field), OrderDirection::kAscending, NullPlacement::kLast};
    case AlbumGroupField::kNone:
      break;
  }
  throw std::invalid_argument("Album query: the flat mode has no group order");
}

}  // namespace

auto GroupOrderTerm(const AlbumQueryOptions& options) -> OrderTerm {
  auto term = DefaultGroupOrderTerm(options.group_field_);
  if (options.sort_field_ != AlbumSortField::kNone &&
      options.sort_field_ == SortFieldOfGroup(options.group_field_)) {
    term.direction_ = options.sort_direction_ == SortDirection::kDescending
                          ? OrderDirection::kDescending
                          : OrderDirection::kAscending;
  }
  return term;
}

namespace {

auto SortExpression(AlbumSortField field) -> SqlFragment {
  switch (field) {
    case AlbumSortField::kNone:
      return {};
    case AlbumSortField::kCaptureTime:
      return expr::col("s.capture_at");
    case AlbumSortField::kImportTime:
      return expr::col("s.added_time");
    case AlbumSortField::kCameraModel:
      return expr::col("NULLIF(s.camera_model, '')");
    case AlbumSortField::kLens:
      return expr::col("NULLIF(s.lens, '')");
    case AlbumSortField::kRating:
      return expr::col("s.rating");
    case AlbumSortField::kLabels:
      return expr::col("ls.label_sort_key");
    case AlbumSortField::kEditTime:
      return expr::col("es.edited_at_ns");
  }
  throw std::invalid_argument("Album query: unknown sort field");
}

/// Full timestamp of a date group, with the sort field that reads the same timestamp.
struct DateGroupTimestamp {
  const char*    column_;
  AlbumSortField same_sort_field_;
};

auto DateGroupTimestampOf(AlbumGroupField field) -> std::optional<DateGroupTimestamp> {
  switch (field) {
    case AlbumGroupField::kCaptureDay:
      return DateGroupTimestamp{"s.capture_at", AlbumSortField::kCaptureTime};
    case AlbumGroupField::kImportDay:
      return DateGroupTimestamp{"s.added_time", AlbumSortField::kImportTime};
    case AlbumGroupField::kEditDay:
      return DateGroupTimestamp{"es.edited_at_ns", AlbumSortField::kEditTime};
    default:
      return std::nullopt;
  }
}

}  // namespace

auto OccurrenceOrderTerms(const AlbumQueryOptions& options) -> std::vector<OrderTerm> {
  std::vector<OrderTerm> terms;
  if (options.group_field_ != AlbumGroupField::kNone) {
    terms.push_back(GroupOrderTerm(options));
  }
  if (options.sort_field_ != AlbumSortField::kNone) {
    terms.push_back({SortExpression(options.sort_field_),
                     options.sort_direction_ == SortDirection::kDescending
                         ? OrderDirection::kDescending
                         : OrderDirection::kAscending,
                     NullPlacement::kLast});
  }
  if (const auto timestamp = DateGroupTimestampOf(options.group_field_);
      timestamp.has_value() && options.sort_field_ != timestamp->same_sort_field_) {
    terms.push_back(
        {expr::col(timestamp->column_), OrderDirection::kDescending, NullPlacement::kLast});
  }
  terms.push_back({expr::col("s.file_id"), OrderDirection::kAscending, NullPlacement::kLast});
  return terms;
}

}  // namespace album_query_sql
}  // namespace alcedo
