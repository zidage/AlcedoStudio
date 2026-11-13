//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace duckorm {

/// Direction of one ORDER BY term.
enum class OrderDirection : uint8_t { kAscending, kDescending };

/// Placement of NULL values in one ORDER BY term. Every term states it explicitly.
enum class NullPlacement : uint8_t { kFirst, kLast };

/// Row selection of a SELECT projection.
enum class SelectRows : uint8_t { kAll, kDistinct };

/**
 * @brief One ORDER BY term: an expression, its direction, and its NULL placement.
 *
 * @details A statement description value. The expression is trusted SQL from a caller below
 * the application API; any values in it stay bound in declaration order.
 */
struct OrderTerm {
  SqlFragment    expression_{};
  OrderDirection direction_ = OrderDirection::kAscending;
  NullPlacement  nulls_     = NullPlacement::kLast;
};

namespace clause {

/**
 * @brief `expression AS alias`.
 *
 * @param alias SQL identifier: ASCII letters, digits, and `_`, not starting with a digit.
 * @throws std::invalid_argument when @p expression is empty or @p alias is not an identifier.
 */
[[nodiscard]] auto as(SqlFragment expression, std::string_view alias) -> SqlFragment;

/// `COUNT(*)`.
[[nodiscard]] auto count_all() -> SqlFragment;

/**
 * @brief `COUNT(DISTINCT expression)`.
 * @throws std::invalid_argument when @p expression is empty.
 */
[[nodiscard]] auto count_distinct(SqlFragment expression) -> SqlFragment;

/**
 * @brief `SELECT [DISTINCT] p1, p2, ... source`.
 *
 * @param projection Output expressions in column order. Their binds come first.
 * @param source Trusted FROM / JOIN text that starts with `FROM`, with its binds. It can also
 *        contain the WHERE clause of a shared scope (see ElementStore's BuildScopedFileQuery).
 * @return The statement text and the binds in placeholder order.
 * @throws std::invalid_argument when @p projection is empty, a projection is empty, or
 *         @p source is empty.
 */
[[nodiscard]] auto select_query(std::span<const SqlFragment> projection, const SqlFragment& source,
                                SelectRows rows = SelectRows::kAll) -> SqlFragment;

/**
 * @brief ` WHERE (predicate)`, or an empty fragment for an empty predicate.
 */
[[nodiscard]] auto where(SqlFragment predicate) -> SqlFragment;

/**
 * @brief ` GROUP BY k1, k2, ...`.
 * @throws std::invalid_argument when @p keys is empty or a key is empty.
 */
[[nodiscard]] auto group_by(std::span<const SqlFragment> keys) -> SqlFragment;

/**
 * @brief ` GROUP BY GROUPING SETS ((a, b), (c), ())`.
 *
 * @param sets Grouping sets in order. An empty set is the explicit total set `()`.
 * @throws std::invalid_argument when @p sets is empty, has only empty sets, or holds an
 *         empty key.
 */
[[nodiscard]] auto grouping_sets(std::span<const std::vector<SqlFragment>> sets) -> SqlFragment;

/**
 * @brief ` ORDER BY e1 ASC NULLS LAST, e2 DESC NULLS FIRST, ...`.
 * @throws std::invalid_argument when @p terms is empty, a term is empty, or a direction or a
 *         NULL placement is not a declared enumerator.
 */
[[nodiscard]] auto order_by(std::span<const OrderTerm> terms) -> SqlFragment;

/**
 * @brief ` LIMIT ? OFFSET ?` with both values bound.
 *
 * @param limit Number of rows; at least 1 and at most @p max_limit.
 * @param offset Number of rows to skip; not negative.
 * @param max_limit Largest accepted page.
 * @throws std::invalid_argument when a value is out of range.
 */
[[nodiscard]] auto limit_offset(int64_t limit, int64_t offset, int64_t max_limit) -> SqlFragment;

}  // namespace clause
}  // namespace duckorm
