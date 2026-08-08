//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include <duckdb.h>

#include <span>
#include <string>
#include <vector>

#include "duckdb_expr.hpp"
#include "duckdb_types.hpp"

namespace duckorm {
duckdb_state begin_transaction(duckdb_connection& conn);
duckdb_state commit_transaction(duckdb_connection& conn);
duckdb_state rollback_transaction(duckdb_connection& conn);

duckdb_state insert(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count);

duckdb_state insert_or_replace(duckdb_connection& conn, const char* table, const void* obj,
                               std::span<const DuckFieldDesc> fields, size_t field_count);

duckdb_state insert_by_query(duckdb_connection& conn, const std::string& sql, const void* obj,
                             std::span<const DuckFieldDesc> fields, size_t field_count);

duckdb_state update(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count,
                    const char* where_clause);

/**
 * @brief Update rows using a SqlFragment WHERE clause (and optional binds).
 *
 * @param where_clause Predicate fragment. Binds are applied after field binds.
 * @return DuckDB execution state.
 */
duckdb_state update(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count,
                    const SqlFragment& where_clause);

duckdb_state remove(duckdb_connection& conn, const char* table, const char* where_clause);

/**
 * @brief Remove rows using a SqlFragment WHERE clause (and optional binds).
 */
duckdb_state remove(duckdb_connection& conn, const char* table, const SqlFragment& where_clause);

std::vector<std::vector<VarTypes>> select(duckdb_connection& conn, const std::string table,
                                          std::span<const DuckFieldDesc> sample_fields,
                                          size_t field_count, const char* where_clause);

/**
 * @brief Select rows using a SqlFragment WHERE clause (and optional binds).
 */
std::vector<std::vector<VarTypes>> select(duckdb_connection& conn, const std::string& table,
                                          std::span<const DuckFieldDesc> sample_fields,
                                          size_t field_count, const SqlFragment& where_clause);

std::vector<std::vector<VarTypes>> select_by_query(duckdb_connection&             conn,
                                                   std::span<const DuckFieldDesc> sample_fields,
                                                   size_t field_count, const std::string& sql);

/**
 * @brief Bind SqlFragment values onto a prepared statement starting at @p start_index.
 *
 * @param stmt Prepared statement that already includes matching `?` placeholders.
 * @param start_index 1-based DuckDB parameter index of the first fragment bind.
 * @param fragment Fragment whose binds_ list is applied in declaration order.
 */
void bind_fragment_values(duckdb_prepared_statement stmt, idx_t start_index,
                          const SqlFragment& fragment);

/**
 * @brief Prepare and run a SQL statement, binding values from @p fragment.
 *
 * @param conn Open DuckDB connection.
 * @param sql Full SQL text. May contain `?` placeholders for @p fragment binds.
 * @param fragment Bind values applied at positions 1..n (empty binds still prepare).
 * @param out_result Receives a materialised result on success. Caller destroys it.
 * @return DuckDBSuccess on success. On failure @p out_result is still valid for error
 *         inspection and must be destroyed by the caller.
 *
 * @details Album scope queries and other multi-table SELECTs use this path so filter
 * predicates keep prepared-statement binds end-to-end.
 */
duckdb_state execute_query(duckdb_connection& conn, const std::string& sql,
                           const SqlFragment& fragment, duckdb_result* out_result);
}  // namespace duckorm
