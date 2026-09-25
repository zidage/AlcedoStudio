//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once
#include <duckdb.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/**
 * @brief Run a statement and discard its result.
 *
 * @param statement SQL text and binds. Without binds the text runs through `duckdb_query`, so
 *        it may be any statement DuckDB accepts there (DDL, SET, LOAD, PRAGMA) or several
 *        statements separated by `;`. With binds it is prepared and must be one statement.
 * @throws std::runtime_error with DuckDB's error message when the statement fails.
 */
void execute(duckdb_connection& conn, const SqlFragment& statement);

/**
 * @brief First column of the first row as an integer.
 *
 * @return std::nullopt when the query returns no row or a NULL value.
 * @throws std::runtime_error with DuckDB's error message when the query fails.
 */
[[nodiscard]] auto select_int64(duckdb_connection& conn, const SqlFragment& query)
    -> std::optional<int64_t>;

/**
 * @brief First column of the first row as text.
 *
 * @return std::nullopt when the query returns no row or a NULL value.
 * @throws std::runtime_error with DuckDB's error message when the query fails.
 */
[[nodiscard]] auto select_string(duckdb_connection& conn, const SqlFragment& query)
    -> std::optional<std::string>;

/**
 * @brief Run a SELECT with binds and decode its rows by @p sample_fields (see VarTypes).
 *
 * @throws std::runtime_error when the query fails or its column count differs from
 *         @p field_count.
 */
std::vector<std::vector<VarTypes>> select_by_query(duckdb_connection&             conn,
                                                   std::span<const DuckFieldDesc> sample_fields,
                                                   size_t field_count, const SqlFragment& query);

/**
 * @brief Text of a decoded cell of a text type (VARCHAR, JSON, BOOLEAN, TIMESTAMP, STRING,
 *        NULLABLE_STRING). Empty for a NULL cell.
 */
[[nodiscard]] auto cell_text(const VarTypes& cell) -> std::string;

/**
 * @brief Value of a decoded BOOLEAN cell, which select decodes as the text "true" / "false".
 *        False for a NULL cell.
 */
[[nodiscard]] auto cell_bool(const VarTypes& cell) -> bool;

/**
 * @brief One transaction on a connection, rolled back unless committed.
 *
 * The constructor runs BEGIN and commit() runs COMMIT; both throw on failure. When the object
 * is destroyed without a successful commit() (an exception or an early return), the destructor
 * runs ROLLBACK. The destructor cannot throw, so a failing ROLLBACK there is not reported; the
 * error that ended the transaction is the one the caller sees.
 *
 * Not thread-safe: use it on the thread that holds the connection's lock.
 */
class Transaction {
 public:
  explicit Transaction(duckdb_connection& conn);
  Transaction(const Transaction&)                    = delete;
  auto operator=(const Transaction&) -> Transaction& = delete;
  ~Transaction();

  /// Run COMMIT. Throws std::runtime_error on failure; the destructor then rolls back.
  void commit();

 private:
  duckdb_connection& conn_;
  bool               committed_ = false;
};

/**
 * @brief Bulk row writer for one table (DuckDB Appender).
 *
 * The rows join the connection's current transaction and become visible to other connections
 * on COMMIT. Append one value for each column given to the constructor, in that order, then
 * call end_row(). flush() writes the buffered rows; the destructor releases the appender
 * without flushing. Every method throws std::runtime_error with the appender's error message.
 *
 * Not thread-safe: use it on the thread that holds the connection's lock.
 */
class Appender {
 public:
  Appender(duckdb_connection& conn, const char* table, std::span<const char* const> columns);
  Appender(const Appender&)                    = delete;
  auto operator=(const Appender&) -> Appender& = delete;
  ~Appender();

  void append_int32(int32_t value);
  void append_uint32(uint32_t value);
  void append_double(double value);
  void append_bool(bool value);
  void append_varchar(std::string_view value);
  void append_null();
  /// A FLOAT[n] array value; n must match the column's array size.
  void append_float_array(std::span<const float> values);
  void end_row();
  void flush();

 private:
  void            check(duckdb_state state, const char* operation) const;

  duckdb_appender appender_ = nullptr;
  std::string     table_;
};
}  // namespace duckorm
