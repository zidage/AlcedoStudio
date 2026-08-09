//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/duckorm/duckdb_expr.hpp"

#include <charconv>
#include <string>
#include <utility>

namespace duckorm {
namespace expr {
namespace {

auto join_parts(std::span<const SqlFragment> parts, std::string_view op) -> SqlFragment {
  if (parts.empty()) {
    return {};
  }

  SqlFragment out;
  out.sql_.push_back('(');
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      out.sql_.push_back(' ');
      out.sql_.append(op);
      out.sql_.push_back(' ');
    }
    out.append(parts[i]);
  }
  out.sql_.push_back(')');
  return out;
}

auto bin_op(SqlFragment left, std::string_view op, SqlFragment right) -> SqlFragment {
  SqlFragment out;
  out.sql_.reserve(left.sql_.size() + right.sql_.size() + op.size() + 4);
  out.sql_.push_back('(');
  out.append(std::move(left));
  out.sql_.push_back(' ');
  out.sql_.append(op);
  out.sql_.push_back(' ');
  out.append(std::move(right));
  out.sql_.push_back(')');
  return out;
}

auto unary_suffix(SqlFragment operand, std::string_view suffix) -> SqlFragment {
  SqlFragment out;
  out.sql_.reserve(operand.sql_.size() + suffix.size() + 2);
  out.sql_.push_back('(');
  out.append(std::move(operand));
  out.sql_.push_back(' ');
  out.sql_.append(suffix);
  out.sql_.push_back(')');
  return out;
}

auto format_double(double value) -> std::string {
  char buffer[64];
  auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                 std::chars_format::general, 17);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, static_cast<size_t>(ptr - buffer));
}

}  // namespace

auto escape_string(std::string_view value) -> std::string {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\'') {
      out.push_back('\'');
    }
    out.push_back(ch);
  }
  return out;
}

auto col(std::string_view name) -> SqlFragment {
  return SqlFragment{std::string(name), {}};
}

auto raw(std::string_view sql) -> SqlFragment {
  return SqlFragment{std::string(sql), {}};
}

auto lit_null() -> SqlFragment {
  return SqlFragment{"NULL", {}};
}

auto lit(int64_t value) -> SqlFragment {
  return SqlFragment{std::to_string(value), {}};
}

auto lit(double value) -> SqlFragment {
  return SqlFragment{format_double(value), {}};
}

auto lit(bool value) -> SqlFragment {
  return SqlFragment{value ? "1" : "0", {}};
}

auto lit(std::string_view value) -> SqlFragment {
  std::string sql;
  sql.reserve(value.size() + 2);
  sql.push_back('\'');
  sql.append(escape_string(value));
  sql.push_back('\'');
  return SqlFragment{std::move(sql), {}};
}

auto lit(const char* value) -> SqlFragment {
  return lit(std::string_view(value ? value : ""));
}

auto lit(const std::string& value) -> SqlFragment {
  return lit(std::string_view(value));
}

auto param(int64_t value) -> SqlFragment {
  return SqlFragment{"?", {BindValue{value}}};
}

auto param(double value) -> SqlFragment {
  return SqlFragment{"?", {BindValue{value}}};
}

auto param(bool value) -> SqlFragment {
  return SqlFragment{"?", {BindValue{value}}};
}

auto param(std::string_view value) -> SqlFragment {
  return SqlFragment{"?", {BindValue{std::string(value)}}};
}

auto param(const char* value) -> SqlFragment {
  return param(std::string_view(value ? value : ""));
}

auto param(const std::string& value) -> SqlFragment {
  return param(std::string_view(value));
}

auto eq(SqlFragment left, SqlFragment right) -> SqlFragment {
  return bin_op(std::move(left), "=", std::move(right));
}

auto ne(SqlFragment left, SqlFragment right) -> SqlFragment {
  return bin_op(std::move(left), "!=", std::move(right));
}

auto gt(SqlFragment left, SqlFragment right) -> SqlFragment {
  return bin_op(std::move(left), ">", std::move(right));
}

auto lt(SqlFragment left, SqlFragment right) -> SqlFragment {
  return bin_op(std::move(left), "<", std::move(right));
}

auto ge(SqlFragment left, SqlFragment right) -> SqlFragment {
  return bin_op(std::move(left), ">=", std::move(right));
}

auto le(SqlFragment left, SqlFragment right) -> SqlFragment {
  return bin_op(std::move(left), "<=", std::move(right));
}

auto like(SqlFragment left, SqlFragment pattern) -> SqlFragment {
  return bin_op(std::move(left), "LIKE", std::move(pattern));
}

auto not_like(SqlFragment left, SqlFragment pattern) -> SqlFragment {
  return bin_op(std::move(left), "NOT LIKE", std::move(pattern));
}

auto escape_like_pattern(std::string_view value, char escape_char) -> std::string {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == escape_char || ch == '%' || ch == '_') {
      out.push_back(escape_char);
    }
    out.push_back(ch);
  }
  return out;
}

auto like_escape(SqlFragment left, SqlFragment pattern, char escape_char) -> SqlFragment {
  SqlFragment out;
  out.sql_.reserve(left.sql_.size() + pattern.sql_.size() + 24);
  out.sql_.push_back('(');
  out.append(std::move(left));
  out.sql_.append(" LIKE ");
  out.append(std::move(pattern));
  out.sql_.append(" ESCAPE '");
  out.sql_.push_back(escape_char);
  out.sql_.append("')");
  return out;
}

auto between(SqlFragment value, SqlFragment low, SqlFragment high) -> SqlFragment {
  SqlFragment out;
  out.sql_.push_back('(');
  out.append(std::move(value));
  out.sql_.append(" BETWEEN ");
  out.append(std::move(low));
  out.sql_.append(" AND ");
  out.append(std::move(high));
  out.sql_.push_back(')');
  return out;
}

auto is_null(SqlFragment operand) -> SqlFragment {
  return unary_suffix(std::move(operand), "IS NULL");
}

auto is_not_null(SqlFragment operand) -> SqlFragment {
  return unary_suffix(std::move(operand), "IS NOT NULL");
}

auto exists(SqlFragment subquery) -> SqlFragment {
  SqlFragment out;
  out.sql_.append("EXISTS (");
  out.append(std::move(subquery));
  out.sql_.push_back(')');
  return out;
}

auto not_(SqlFragment operand) -> SqlFragment {
  SqlFragment out;
  out.sql_.append("(NOT ");
  out.append(std::move(operand));
  out.sql_.push_back(')');
  return out;
}

auto and_(std::span<const SqlFragment> parts) -> SqlFragment {
  return join_parts(parts, "AND");
}

auto or_(std::span<const SqlFragment> parts) -> SqlFragment {
  return join_parts(parts, "OR");
}

auto and_(std::initializer_list<SqlFragment> parts) -> SqlFragment {
  return and_(std::span<const SqlFragment>(parts.begin(), parts.size()));
}

auto or_(std::initializer_list<SqlFragment> parts) -> SqlFragment {
  return or_(std::span<const SqlFragment>(parts.begin(), parts.size()));
}

}  // namespace expr
}  // namespace duckorm
