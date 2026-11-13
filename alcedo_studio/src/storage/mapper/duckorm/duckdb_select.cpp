//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/duckorm/duckdb_select.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace duckorm::clause {
namespace {

auto IsIdentifier(std::string_view text) -> bool {
  if (text.empty() || (text.front() >= '0' && text.front() <= '9')) {
    return false;
  }
  for (const char ch : text) {
    const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit  = ch >= '0' && ch <= '9';
    if (!letter && !digit && ch != '_') {
      return false;
    }
  }
  return true;
}

void RequireExpression(const SqlFragment& expression, const char* operation) {
  if (expression.empty()) {
    throw std::invalid_argument(std::string("duckorm::clause::") + operation +
                                ": an expression is empty");
  }
}

/// Append `e1, e2, ...` to @p out.
void AppendList(SqlFragment& out, std::span<const SqlFragment> expressions, const char* operation) {
  for (size_t index = 0; index < expressions.size(); ++index) {
    RequireExpression(expressions[index], operation);
    if (index > 0) {
      out.sql_.append(", ");
    }
    out.append(expressions[index]);
  }
}

auto DirectionText(OrderDirection direction) -> const char* {
  switch (direction) {
    case OrderDirection::kAscending:
      return " ASC";
    case OrderDirection::kDescending:
      return " DESC";
  }
  throw std::invalid_argument("duckorm::clause::order_by: unknown direction");
}

auto NullPlacementText(NullPlacement nulls) -> const char* {
  switch (nulls) {
    case NullPlacement::kFirst:
      return " NULLS FIRST";
    case NullPlacement::kLast:
      return " NULLS LAST";
  }
  throw std::invalid_argument("duckorm::clause::order_by: unknown NULL placement");
}

}  // namespace

auto as(SqlFragment expression, std::string_view alias) -> SqlFragment {
  RequireExpression(expression, "as");
  if (!IsIdentifier(alias)) {
    throw std::invalid_argument("duckorm::clause::as: alias is not an SQL identifier");
  }
  expression.sql_.append(" AS ");
  expression.sql_.append(alias);
  return expression;
}

auto count_all() -> SqlFragment { return expr::raw("COUNT(*)"); }

auto count_distinct(SqlFragment expression) -> SqlFragment {
  RequireExpression(expression, "count_distinct");
  auto out = expr::raw("COUNT(DISTINCT ");
  out.append(std::move(expression));
  out.sql_.append(")");
  return out;
}

auto select_query(std::span<const SqlFragment> projection, const SqlFragment& source,
                  SelectRows rows) -> SqlFragment {
  if (projection.empty()) {
    throw std::invalid_argument("duckorm::clause::select_query: projection is empty");
  }
  if (source.empty()) {
    throw std::invalid_argument("duckorm::clause::select_query: source is empty");
  }
  SqlFragment out;
  switch (rows) {
    case SelectRows::kAll:
      out.sql_ = "SELECT ";
      break;
    case SelectRows::kDistinct:
      out.sql_ = "SELECT DISTINCT ";
      break;
    default:
      throw std::invalid_argument("duckorm::clause::select_query: unknown row selection");
  }
  AppendList(out, projection, "select_query");
  out.sql_.append(" ");
  out.append(source);
  return out;
}

auto where(SqlFragment predicate) -> SqlFragment {
  if (predicate.empty()) {
    return {};
  }
  auto out = expr::raw(" WHERE (");
  out.append(std::move(predicate));
  out.sql_.append(")");
  return out;
}

auto group_by(std::span<const SqlFragment> keys) -> SqlFragment {
  if (keys.empty()) {
    throw std::invalid_argument("duckorm::clause::group_by: no key");
  }
  auto out = expr::raw(" GROUP BY ");
  AppendList(out, keys, "group_by");
  return out;
}

auto grouping_sets(std::span<const std::vector<SqlFragment>> sets) -> SqlFragment {
  bool has_key = false;
  for (const auto& set : sets) {
    has_key = has_key || !set.empty();
  }
  if (!has_key) {
    throw std::invalid_argument("duckorm::clause::grouping_sets: no set groups by a key");
  }
  auto out = expr::raw(" GROUP BY GROUPING SETS (");
  for (size_t index = 0; index < sets.size(); ++index) {
    if (index > 0) {
      out.sql_.append(", ");
    }
    out.sql_.append("(");
    AppendList(out, sets[index], "grouping_sets");
    out.sql_.append(")");
  }
  out.sql_.append(")");
  return out;
}

auto order_by(std::span<const OrderTerm> terms) -> SqlFragment {
  if (terms.empty()) {
    throw std::invalid_argument("duckorm::clause::order_by: no term");
  }
  auto out = expr::raw(" ORDER BY ");
  for (size_t index = 0; index < terms.size(); ++index) {
    const auto& term = terms[index];
    RequireExpression(term.expression_, "order_by");
    // Resolve both texts before appending so a rejected enumerator leaves no partial clause.
    const char* direction = DirectionText(term.direction_);
    const char* nulls     = NullPlacementText(term.nulls_);
    if (index > 0) {
      out.sql_.append(", ");
    }
    out.append(term.expression_);
    out.sql_.append(direction);
    out.sql_.append(nulls);
  }
  return out;
}

auto limit_offset(int64_t limit, int64_t offset, int64_t max_limit) -> SqlFragment {
  if (limit < 1 || limit > max_limit) {
    throw std::invalid_argument("duckorm::clause::limit_offset: limit is out of range");
  }
  if (offset < 0) {
    throw std::invalid_argument("duckorm::clause::limit_offset: offset is negative");
  }
  auto out = expr::raw(" LIMIT ");
  out.append(expr::param(limit));
  out.sql_.append(" OFFSET ");
  out.append(expr::param(offset));
  return out;
}

}  // namespace duckorm::clause
