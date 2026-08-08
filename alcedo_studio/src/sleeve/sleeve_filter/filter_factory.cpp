//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_filter/filter_factory.hpp"

#include <cstdint>
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
  // Use json_extract_string: comparing json_extract(...) to '' would cast ''
  // to JSON and fail (Malformed JSON) in DuckDB.
  const auto date_col = expr::col("json_extract_string(i.metadata, '$.DateTimeString')");
  const auto fragment =
      expr::or_({expr::is_null(date_col), expr::eq(date_col, expr::param(""))});
  return MakeRawSQLNode(fragment);
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
  return MakeRawSQLNode(expr::is_null(expr::col("json_extract(i.metadata, '$.Rating')")));
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
