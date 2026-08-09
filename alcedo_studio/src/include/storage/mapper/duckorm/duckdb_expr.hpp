//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace duckorm {

/**
 * @brief Value bound to a prepared-statement placeholder in a SqlFragment.
 *
 * @details Prefer expr::lit for product WHERE text that is still pasted as a
 * string. Use expr::param when the caller prepares the fragment and binds.
 */
using BindValue = std::variant<std::monostate, int64_t, double, bool, std::string>;

/**
 * @brief Generic SQL expression fragment with optional bind values.
 *
 * @details Owns SQL text for one expression or clause. Does not know sleeve
 * FilterField names, album aliases, or UI types. Callers compose fragments
 * with duckorm::expr helpers.
 *
 * Thread safety: value type; not shared across threads without external sync.
 */
struct SqlFragment {
  std::string              sql_;
  std::vector<BindValue>   binds_;

  [[nodiscard]] auto empty() const -> bool { return sql_.empty(); }

  /**
   * @brief Append another fragment's SQL and bind list in order.
   *
   * @param other Fragment to append. Binds are moved in declaration order.
   */
  void append(const SqlFragment& other) {
    sql_.append(other.sql_);
    binds_.insert(binds_.end(), other.binds_.begin(), other.binds_.end());
  }

  void append(SqlFragment&& other) {
    sql_.append(other.sql_);
    binds_.insert(binds_.end(), std::make_move_iterator(other.binds_.begin()),
                  std::make_move_iterator(other.binds_.end()));
  }
};

namespace expr {

/**
 * @brief Escape a string for use inside a single-quoted SQL literal.
 *
 * @param value Unquoted string content.
 * @return Content with each `'` doubled. Does not add surrounding quotes.
 */
[[nodiscard]] auto escape_string(std::string_view value) -> std::string;

/**
 * @brief Build a fragment for a column name or raw SQL expression text.
 *
 * @param name Column or expression text. Not quoted as a string literal.
 * @return Fragment with the text as-is and no binds.
 *
 * @pre @p name is trusted SQL (identifiers or fixed expressions only).
 */
[[nodiscard]] auto col(std::string_view name) -> SqlFragment;

/**
 * @brief Build a fragment from trusted SQL text without escaping.
 *
 * @param sql Trusted SQL (subquery, function call, or bridge text).
 * @return Fragment with the text as-is and no binds.
 *
 * @pre @p sql is trusted. Do not pass unescaped user text.
 */
[[nodiscard]] auto raw(std::string_view sql) -> SqlFragment;

[[nodiscard]] auto lit_null() -> SqlFragment;
[[nodiscard]] auto lit(int64_t value) -> SqlFragment;
[[nodiscard]] auto lit(double value) -> SqlFragment;
[[nodiscard]] auto lit(bool value) -> SqlFragment;

/**
 * @brief Build a single-quoted string literal with SQL escaping.
 *
 * @param value String content. May contain quotes; they are escaped.
 * @return Fragment such as `'O''Brien'` with no binds.
 *
 * @note Prefer this (or the `const char*` overload) for text. A bare C string
 * must not fall through to `lit(bool)`.
 */
[[nodiscard]] auto lit(std::string_view value) -> SqlFragment;
[[nodiscard]] auto lit(const char* value) -> SqlFragment;
[[nodiscard]] auto lit(const std::string& value) -> SqlFragment;

/**
 * @brief Build a `?` placeholder and keep the value for later binding.
 */
[[nodiscard]] auto param(int64_t value) -> SqlFragment;
[[nodiscard]] auto param(double value) -> SqlFragment;
[[nodiscard]] auto param(bool value) -> SqlFragment;
[[nodiscard]] auto param(std::string_view value) -> SqlFragment;
[[nodiscard]] auto param(const char* value) -> SqlFragment;
[[nodiscard]] auto param(const std::string& value) -> SqlFragment;

[[nodiscard]] auto eq(SqlFragment left, SqlFragment right) -> SqlFragment;
[[nodiscard]] auto ne(SqlFragment left, SqlFragment right) -> SqlFragment;
[[nodiscard]] auto gt(SqlFragment left, SqlFragment right) -> SqlFragment;
[[nodiscard]] auto lt(SqlFragment left, SqlFragment right) -> SqlFragment;
[[nodiscard]] auto ge(SqlFragment left, SqlFragment right) -> SqlFragment;
[[nodiscard]] auto le(SqlFragment left, SqlFragment right) -> SqlFragment;
[[nodiscard]] auto like(SqlFragment left, SqlFragment pattern) -> SqlFragment;
[[nodiscard]] auto not_like(SqlFragment left, SqlFragment pattern) -> SqlFragment;

/**
 * @brief Escape a string for use inside a LIKE pattern.
 *
 * @param value Pattern content. May contain `%`, `_`, or the escape character.
 * @param escape_char Escape character used by the LIKE clause (default `~`).
 * @return Content with `%`, `_`, and @p escape_char prefixed by @p escape_char.
 */
[[nodiscard]] auto escape_like_pattern(std::string_view value, char escape_char = '~')
    -> std::string;

/**
 * @brief Build a LIKE predicate with an explicit escape character.
 *
 * @param left Column or expression fragment.
 * @param pattern Pattern fragment (escape wildcards with `escape_like_pattern`).
 * @param escape_char Escape character (default `~`).
 * @return `(left LIKE pattern ESCAPE 'c')`.
 */
[[nodiscard]] auto like_escape(SqlFragment left, SqlFragment pattern,
                               char escape_char = '~') -> SqlFragment;

[[nodiscard]] auto between(SqlFragment value, SqlFragment low, SqlFragment high) -> SqlFragment;
[[nodiscard]] auto is_null(SqlFragment operand) -> SqlFragment;
[[nodiscard]] auto is_not_null(SqlFragment operand) -> SqlFragment;
[[nodiscard]] auto exists(SqlFragment subquery) -> SqlFragment;
[[nodiscard]] auto not_(SqlFragment operand) -> SqlFragment;

/**
 * @brief Join fragments with AND inside one parenthesized group.
 *
 * @param parts Child predicates. Empty input yields an empty fragment.
 * @return `(a AND b AND …)` or a single parenthesized child.
 */
[[nodiscard]] auto and_(std::span<const SqlFragment> parts) -> SqlFragment;
[[nodiscard]] auto or_(std::span<const SqlFragment> parts) -> SqlFragment;
[[nodiscard]] auto and_(std::initializer_list<SqlFragment> parts) -> SqlFragment;
[[nodiscard]] auto or_(std::initializer_list<SqlFragment> parts) -> SqlFragment;

}  // namespace expr
}  // namespace duckorm
