//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/album_query.hpp"

#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_select.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
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
  if (options.group_field_ == AlbumGroupField::kImportDay &&
      options.import_day_time_zone_.empty()) {
    throw std::invalid_argument("Album query: import-day groups need a time zone");
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
  // Alias text -> canonical key pairs of the label taxonomy, as trusted literals. The keys are
  // already normalized with NormalizeSemanticLabelKey (trimmed, ASCII lowercase).
  const std::map<std::string, std::string> aliases(SemanticLabelCanonicalLookup().begin(),
                                                   SemanticLabelCanonicalLookup().end());
  auto relations = expr::raw("WITH label_membership AS (SELECT DISTINCT sl.file_id, ");
  if (aliases.empty()) {
    relations.sql_.append("LOWER(TRIM(sl.label)) AS label_key FROM SemanticImageLabel sl ");
  } else {
    relations.sql_.append(
        "COALESCE(lm.canonical_key, LOWER(TRIM(sl.label))) AS label_key "
        "FROM SemanticImageLabel sl LEFT JOIN (VALUES ");
    bool first = true;
    for (const auto& [alias, canonical] : aliases) {
      if (!first) {
        relations.sql_.append(", ");
      }
      first = false;
      relations.sql_.append("(");
      relations.append(expr::lit(alias));
      relations.sql_.append(", ");
      relations.append(expr::lit(canonical));
      relations.sql_.append(")");
    }
    relations.sql_.append(
        ") AS lm(alias_key, canonical_key) ON lm.alias_key = LOWER(TRIM(sl.label)) ");
  }
  relations.sql_.append("WHERE sl.model_key = ");
  relations.append(expr::param(active_semantic_model_key));
  relations.sql_.append(std::format(
      " AND TRIM(sl.label) <> '' AND sl.file_id IN (SELECT file_id FROM {})), "
      "label_sort AS (SELECT file_id, MIN(label_key) AS label_sort_key FROM label_membership "
      "GROUP BY file_id) ",
      kMatchSetTable));
  return relations;
}

auto OccurrenceSource(const AlbumQueryOptions& options) -> SqlFragment {
  auto source = expr::raw(std::format("FROM {} s", kMatchSetTable));
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
  }
  throw std::invalid_argument("Album query: unknown group field");
}

auto GroupOrderTerm(AlbumGroupField field) -> OrderTerm {
  switch (field) {
    case AlbumGroupField::kCaptureDay:
    case AlbumGroupField::kImportDay:
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
    default:
      return std::nullopt;
  }
}

}  // namespace

auto OccurrenceOrderTerms(const AlbumQueryOptions& options) -> std::vector<OrderTerm> {
  std::vector<OrderTerm> terms;
  if (options.group_field_ != AlbumGroupField::kNone) {
    terms.push_back(GroupOrderTerm(options.group_field_));
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
