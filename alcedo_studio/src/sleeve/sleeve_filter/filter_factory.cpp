//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_filter/filter_factory.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "utils/string/convert.hpp"

namespace alcedo::sleeve_filter {
namespace {

using duckorm::SqlFragment;
namespace expr = duckorm::expr;

auto MakeConditionNode(FilterField field, CompareOp op, FilterValue value) -> FilterNode {
  FieldCondition cond{.field_ = field, .op_ = op, .value_ = std::move(value)};
  return FilterNode{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
}

// Bridge node that owns compiler output (SQL + binds). Sleeve factories may
// build these; UI code must not author RawSQL text.
auto MakeRawSQLNode(SqlFragment fragment) -> FilterNode {
  FilterNode node{FilterNode::Type::RawSQL, FilterOp::AND, {}, std::nullopt,
                  conv::FromBytes(fragment.sql_)};
  node.raw_binds_ = std::move(fragment.binds_);
  return node;
}

}  // namespace

auto BuildCameraModelBucketFilter(const std::wstring& label) -> FilterNode {
  return MakeConditionNode(FilterField::CameraModelLabel, CompareOp::EQUALS, label);
}

auto BuildLensBucketFilter(const std::wstring& label) -> FilterNode {
  return MakeConditionNode(FilterField::LensLabel, CompareOp::EQUALS, label);
}

auto BuildCaptureDateBucketFilter(const std::wstring& date_yyyy_mm_dd) -> FilterNode {
  return MakeConditionNode(FilterField::CaptureDateLabel, CompareOp::EQUALS, date_yyyy_mm_dd);
}

auto BuildCaptureDateUnknownFilter() -> FilterNode {
  // ImageMapper writes NULL when the capture date is missing or does not parse; this is the
  // row set of the NULL date bucket in ElementStore::BuildFolderStats.
  return MakeRawSQLNode(expr::is_null(expr::col("i.capture_date")));
}

auto BuildImportDateBucketFilter(const std::wstring& date_yyyy_mm_dd, const std::string& time_zone)
    -> FilterNode {
  if (time_zone.empty()) {
    throw std::invalid_argument("Import-day filter: the time zone is empty");
  }
  const std::string  text = conv::ToBytes(date_yyyy_mm_dd);
  int                year = 0, month = 0, day = 0;
  char               separator_1 = 0, separator_2 = 0;
  std::istringstream stream(text);
  stream >> year >> separator_1 >> month >> separator_2 >> day;
  const std::chrono::year_month_day date{std::chrono::year{year},
                                         std::chrono::month{static_cast<unsigned>(month)},
                                         std::chrono::day{static_cast<unsigned>(day)}};
  if (text.size() != 10 || stream.fail() || !stream.eof() || separator_1 != '-' ||
      separator_2 != '-' || !date.ok()) {
    throw std::invalid_argument("Import-day filter: the date is not a valid YYYY-MM-DD day");
  }
  // The next calendar day, not start + 24 hours: each local midnight is converted on its own.
  const std::chrono::year_month_day next{std::chrono::sys_days{date} + std::chrono::days{1}};
  const auto                        local_midnight = [](const std::chrono::year_month_day& value) {
    return std::format("{:04}-{:02}-{:02} 00:00:00", static_cast<int>(value.year()),
                                              static_cast<unsigned>(value.month()), static_cast<unsigned>(value.day()));
  };
  // timezone(zone, TIMESTAMP) reads a local wall time in the zone as an instant;
  // timezone('UTC', TIMESTAMPTZ) gives that instant as UTC text, the form of e.added_time.
  // The bounds are constants, so the column itself is not wrapped in a function.
  const auto utc_bound = [&time_zone](const std::string& local_time) {
    auto bound = expr::raw("timezone('UTC', timezone(CAST(");
    bound.append(expr::param(time_zone));
    bound.sql_.append(" AS VARCHAR), CAST(");
    bound.append(expr::param(local_time));
    bound.sql_.append(" AS TIMESTAMP)))");
    return bound;
  };
  return MakeRawSQLNode(
      expr::and_({expr::ge(expr::col("e.added_time"), utc_bound(local_midnight(date))),
                  expr::lt(expr::col("e.added_time"), utc_bound(local_midnight(next)))}));
}

auto BuildImportDateUnknownFilter() -> FilterNode {
  return MakeRawSQLNode(expr::is_null(expr::col("e.added_time")));
}

auto BuildRatingBucketFilter(const std::wstring& label) -> FilterNode {
  const std::string narrow = conv::ToBytes(label);
  size_t            pos    = 0;
  int               value  = 0;
  try {
    value = std::stoi(narrow, &pos);
  } catch (...) {
    pos = 0;
  }
  if (pos == narrow.size() && pos > 0) {
    return MakeConditionNode(FilterField::RatingLabel, CompareOp::EQUALS, int64_t{value});
  }
  // Non-numeric bucket (for example "(unknown)"): the rating must be NULL.
  return MakeRawSQLNode(expr::is_null(expr::col("i.rating")));
}

auto BuildSemanticLabelExistsFilter(const std::string&           model_key,
                                    std::span<const std::string> aliases) -> FilterNode {
  if (model_key.empty()) {
    return MakeRawSQLNode(expr::raw("1 = 0"));
  }

  std::vector<SqlFragment> label_terms;
  label_terms.reserve(aliases.size());
  for (const auto& alias : aliases) {
    auto lower_alias = expr::raw("LOWER(");
    lower_alias.append(expr::param(alias));
    lower_alias.append(expr::raw(")"));
    label_terms.push_back(expr::eq(expr::raw("LOWER(sl.label)"), std::move(lower_alias)));
  }

  SqlFragment alias_match =
      label_terms.size() == 1 ? std::move(label_terms.front()) : expr::or_(label_terms);
  auto subquery = expr::raw("SELECT 1 FROM SemanticImageLabel sl WHERE ");
  subquery.append(expr::and_({expr::eq(expr::col("sl.file_id"), expr::col("e.id")),
                              expr::eq(expr::col("sl.model_key"), expr::param(model_key)),
                              std::move(alias_match)}));
  return MakeRawSQLNode(expr::exists(std::move(subquery)));
}

}  // namespace alcedo::sleeve_filter
