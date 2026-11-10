//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/query/stats_dimension.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "image/color_label.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace alcedo::stats_dimension {
namespace {

using duckorm::SqlFragment;
namespace expr                               = duckorm::expr;

/// The unknown label of the camera model and the lens buckets.
constexpr std::string_view kUnknownTextLabel = "(unknown)";

[[noreturn]] void          ThrowUnknownDimension() {
  throw std::invalid_argument("Stats dimension: unknown id");
}

[[noreturn]] void ThrowKeyTypeMismatch() {
  throw std::invalid_argument("Stats dimension: the bucket key type does not fit the dimension");
}

/// A column of the photo row of @p source: the Image column @p image_column (`i.` or `s.`).
auto ImageColumn(ColumnSource source, std::string_view image_column) -> SqlFragment {
  return expr::col(
      std::format("{}.{}", source == ColumnSource::kLiveTables ? "i" : "s", image_column));
}

auto AddedTimeColumn(ColumnSource source) -> SqlFragment {
  return expr::col(source == ColumnSource::kLiveTables ? "e.added_time" : "s.added_time");
}

auto FileIdColumn(ColumnSource source) -> SqlFragment {
  return expr::col(source == ColumnSource::kLiveTables ? "e.id" : "s.file_id");
}

/// The text label of a camera model or lens column: the text, or the unknown label for NULL and
/// for the empty text.
auto TextLabelExpression(SqlFragment column) -> SqlFragment {
  auto label = expr::raw("COALESCE(NULLIF(");
  label.append(std::move(column));
  label.sql_.append(std::format(", ''), {})", expr::lit(kUnknownTextLabel).sql_));
  return label;
}

auto CastToVarchar(SqlFragment value) -> SqlFragment {
  auto cast = expr::raw("CAST(");
  cast.append(std::move(value));
  cast.sql_.append(" AS VARCHAR)");
  return cast;
}

/// Bucket label expression of the dimensions with one value per photo, over @p source. The
/// import day of the match set is its import_day column; the live tables have no import day
/// label (the predicate there is a range on the import time).
auto LabelExpression(StatsDimensionId dimension, ColumnSource source) -> SqlFragment {
  switch (dimension) {
    case StatsDimensionId::kCaptureDate:
      return CastToVarchar(ImageColumn(source, "capture_date"));
    case StatsDimensionId::kCameraModel:
      return TextLabelExpression(ImageColumn(source, "camera_model"));
    case StatsDimensionId::kLens:
      return TextLabelExpression(ImageColumn(source, "lens"));
    case StatsDimensionId::kRating:
      return CastToVarchar(ImageColumn(source, "rating"));
    case StatsDimensionId::kImportDay:
      return CastToVarchar(expr::col("s.import_day"));
    case StatsDimensionId::kColorLabel:
    case StatsDimensionId::kDeleteMark:
    case StatsDimensionId::kSemanticLabel:
      break;
  }
  ThrowUnknownDimension();
}

auto LabelKey(const StatsBucketKey& key) -> const std::string& {
  if (const auto* label = std::get_if<std::string>(&key)) {
    return *label;
  }
  ThrowKeyTypeMismatch();
}

/// The semantic label key of one SemanticImageLabel row: the lowercase trimmed label.
auto SemanticLabelKeyExpression() -> SqlFragment { return expr::raw("LOWER(TRIM(sl.label))"); }

/// UTC instant of local midnight of @p day in @p time_zone, as the text form of `e.added_time`.
/// timezone(zone, TIMESTAMP) reads a local wall time in the zone as an instant;
/// timezone('UTC', TIMESTAMPTZ) gives that instant as UTC.
auto UtcMidnight(const std::chrono::year_month_day& day, const std::string& time_zone)
    -> SqlFragment {
  const auto local_midnight =
      std::format("{:04}-{:02}-{:02} 00:00:00", static_cast<int>(day.year()),
                  static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
  auto bound = expr::raw("timezone('UTC', timezone(CAST(");
  bound.append(expr::param(time_zone));
  bound.sql_.append(" AS VARCHAR), CAST(");
  bound.append(expr::param(local_midnight));
  bound.sql_.append(" AS TIMESTAMP)))");
  return bound;
}

auto ImportDayPredicate(const StatsBucketCondition& condition, ColumnSource source) -> SqlFragment {
  if (std::holds_alternative<std::monostate>(condition.key)) {
    return expr::is_null(AddedTimeColumn(source));
  }
  const auto& label = LabelKey(condition.key);
  const auto  day   = ParseStatsBucketDay(label);
  if (source == ColumnSource::kMatchSet) {
    return expr::eq(LabelExpression(StatsDimensionId::kImportDay, source), expr::param(label));
  }
  if (condition.time_zone.empty()) {
    throw std::invalid_argument("Stats dimension: the import day has no time zone");
  }
  // The bounds are constants, so the import time column is not wrapped in a function. The next
  // calendar day, not start + 24 hours: each local midnight is converted on its own, so a
  // daylight-saving day keeps its real length.
  const std::chrono::year_month_day next{std::chrono::sys_days{day} + std::chrono::days{1}};
  return expr::and_({expr::ge(AddedTimeColumn(source), UtcMidnight(day, condition.time_zone)),
                     expr::lt(AddedTimeColumn(source), UtcMidnight(next, condition.time_zone))});
}

auto SemanticLabelPredicate(const StatsBucketCondition& condition, ColumnSource source)
    -> SqlFragment {
  const auto& label = LabelKey(condition.key);
  if (condition.semantic_model_key.empty()) {
    return expr::raw("1 = 0");
  }
  // The key gets the same LOWER and TRIM as the stored label, so a label text in another case
  // selects its bucket.
  auto key = expr::raw("LOWER(TRIM(");
  key.append(expr::param(label));
  key.sql_.append("))");
  auto subquery = expr::raw("SELECT 1 FROM SemanticImageLabel sl WHERE ");
  subquery.append(expr::and_({expr::eq(expr::col("sl.file_id"), FileIdColumn(source)),
                              SemanticLabelRowCondition(condition.semantic_model_key),
                              expr::eq(SemanticLabelKeyExpression(), std::move(key))}));
  return expr::exists(std::move(subquery));
}

}  // namespace

auto UnknownLabel(StatsDimensionId dimension) -> std::optional<std::string_view> {
  switch (dimension) {
    case StatsDimensionId::kCameraModel:
    case StatsDimensionId::kLens:
      return kUnknownTextLabel;
    case StatsDimensionId::kCaptureDate:
    case StatsDimensionId::kRating:
    case StatsDimensionId::kImportDay:
    case StatsDimensionId::kColorLabel:
    case StatsDimensionId::kDeleteMark:
    case StatsDimensionId::kSemanticLabel:
      return std::nullopt;
  }
  ThrowUnknownDimension();
}

auto GroupExpression(StatsDimensionId dimension) -> SqlFragment {
  switch (dimension) {
    case StatsDimensionId::kCaptureDate:
    case StatsDimensionId::kCameraModel:
    case StatsDimensionId::kLens:
    case StatsDimensionId::kRating:
    case StatsDimensionId::kImportDay:
      return LabelExpression(dimension, ColumnSource::kMatchSet);
    case StatsDimensionId::kSemanticLabel:
      return SemanticLabelKeyExpression();
    case StatsDimensionId::kColorLabel:
    case StatsDimensionId::kDeleteMark:
      throw std::invalid_argument(
          "Stats dimension: color labels and the Delete mark have no group expression");
  }
  ThrowUnknownDimension();
}

auto BucketPredicate(const StatsBucketCondition& condition, ColumnSource source) -> SqlFragment {
  switch (condition.dimension) {
    case StatsDimensionId::kCaptureDate:
      if (std::holds_alternative<std::monostate>(condition.key)) {
        return expr::is_null(ImageColumn(source, "capture_date"));
      }
      return expr::eq(LabelExpression(condition.dimension, source),
                      expr::param(LabelKey(condition.key)));
    case StatsDimensionId::kCameraModel:
    case StatsDimensionId::kLens: {
      // The unknown bucket is the bucket of the unknown label; COALESCE puts NULL and the empty
      // text there.
      const std::string label = std::holds_alternative<std::monostate>(condition.key)
                                    ? std::string(kUnknownTextLabel)
                                    : LabelKey(condition.key);
      return expr::eq(LabelExpression(condition.dimension, source), expr::param(label));
    }
    case StatsDimensionId::kRating:
      if (std::holds_alternative<std::monostate>(condition.key)) {
        return expr::is_null(ImageColumn(source, "rating"));
      }
      if (const auto* rating = std::get_if<int64_t>(&condition.key)) {
        // Equal to the label comparison CAST(rating AS VARCHAR) = '<rating>' for an integer.
        return expr::eq(ImageColumn(source, "rating"), expr::param(*rating));
      }
      ThrowKeyTypeMismatch();
    case StatsDimensionId::kImportDay:
      return ImportDayPredicate(condition, source);
    case StatsDimensionId::kColorLabel: {
      const auto* code = std::get_if<ColorLabel>(&condition.key);
      if (code == nullptr) {
        ThrowKeyTypeMismatch();
      }
      const auto index = static_cast<int>(*code);
      if (index < 0 || index >= kColorLabelCount) {
        throw std::invalid_argument("Stats dimension: unknown color label code");
      }
      // The bit index is a checked integer of the enum, so it stays in the SQL text.
      auto predicate = expr::raw("(((");
      predicate.append(ImageColumn(source, "color_labels"));
      predicate.sql_.append(std::format(" >> {}) & 1) = 1)", index));
      return predicate;
    }
    case StatsDimensionId::kDeleteMark:
      if (!std::holds_alternative<std::monostate>(condition.key)) {
        ThrowKeyTypeMismatch();
      }
      return ImageColumn(source, "delete_mark");
    case StatsDimensionId::kSemanticLabel:
      return SemanticLabelPredicate(condition, source);
  }
  ThrowUnknownDimension();
}

auto ImportDayValue(const std::string& time_zone) -> SqlFragment {
  if (time_zone.empty()) {
    return expr::raw("CAST(NULL AS DATE)");
  }
  // timezone('UTC', TIMESTAMP) reads the stored UTC text as an instant; timezone(zone,
  // TIMESTAMPTZ) gives the local wall time in that zone. Neither reads the connection's
  // TimeZone setting.
  auto value = expr::raw("CAST(timezone(CAST(");
  value.append(expr::param(time_zone));
  value.sql_.append(" AS VARCHAR), timezone('UTC', e.added_time)) AS DATE)");
  return value;
}

auto SemanticLabelRowCondition(const std::string& semantic_model_key) -> SqlFragment {
  return expr::and_({expr::eq(expr::col("sl.model_key"), expr::param(semantic_model_key)),
                     expr::raw("TRIM(sl.label) <> ''")});
}

}  // namespace alcedo::stats_dimension
