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
#include "duckdb_statement.hpp"
#include "duckdb_types.hpp"

namespace duckorm {
duckdb_state insert(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count);

duckdb_state insert_or_replace(duckdb_connection& conn, const char* table, const void* obj,
                               std::span<const DuckFieldDesc> fields, size_t field_count);

duckdb_state insert_by_query(duckdb_connection& conn, const std::string& sql, const void* obj,
                             std::span<const DuckFieldDesc> fields, size_t field_count);

/**
 * @brief Update rows using a SqlFragment WHERE clause (and optional binds).
 *
 * @param where_clause Predicate fragment. Binds are applied after field binds.
 * @return DuckDB execution state.
 */
duckdb_state update(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count,
                    const SqlFragment& where_clause);

/**
 * @brief Upsert several rows with one multi-row INSERT ... ON CONFLICT DO UPDATE statement.
 *
 * Each pointer in @p rows addresses one object laid out as @p fields describes. The rows must
 * not share a conflict key: DuckDB rejects a statement that updates one row twice. One prepared
 * statement for the whole set replaces one prepare and execute per row.
 */
duckdb_state            upsert_rows(duckdb_connection& conn, const char* table,
                                    std::span<const void* const> rows, std::span<const DuckFieldDesc> fields,
                                    size_t field_count);

/**
 * @brief Insert several rows with one multi-row INSERT statement. Fails like a single-row
 *        insert on a constraint violation.
 */
duckdb_state            insert_rows(duckdb_connection& conn, const char* table,
                                    std::span<const void* const> rows, std::span<const DuckFieldDesc> fields,
                                    size_t field_count);

/// Rows per multi-row statement used by the batch helpers. Larger statements cost more to
/// parse and bind than they save.
inline constexpr size_t kMultiRowStatementRows = 256;

/**
 * @brief Remove rows using a SqlFragment WHERE clause (and optional binds).
 */
duckdb_state remove(duckdb_connection& conn, const char* table, const SqlFragment& where_clause);

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
 * @brief Run a statement and discard its result.
 *
 * @param statement SQL text and binds. Without binds the text runs through `duckdb_query`, so
 *        it may be any statement DuckDB accepts there (DDL, SET, LOAD, PRAGMA) or several
 *        statements separated by `;`. With binds it is prepared and must be one statement.
 * @throws std::runtime_error with DuckDB's error message when the statement fails.
 */
void execute(duckdb_connection& conn, const SqlFragment& statement);

/**
 * @brief Run one statement and return the number of rows it inserted, updated, or deleted.
 *
 * An `INSERT ... ON CONFLICT DO NOTHING` of a stored key returns 0.
 * @throws std::runtime_error with DuckDB's error message when the statement fails.
 */
[[nodiscard]] auto execute_count(duckdb_connection& conn, const SqlFragment& statement) -> idx_t;

/**
 * @brief Run a SELECT and decode every row into @p Row (see RowColumns).
 *
 * The column count and the column types are checked once, before row 0 is read.
 * @throws std::runtime_error when the query fails, the column count differs from the row's
 *         column list, a column type does not fit its member, or a NULL cell meets a member
 *         that is not `std::optional`.
 */
template <TypedRow Row>
[[nodiscard]] auto query(duckdb_connection& conn, const SqlFragment& query) -> std::vector<Row> {
  Statement statement(conn, query.sql_);
  statement.BindFragment(1, query);
  return DecodeRows<Row>(statement.Execute());
}

/**
 * @brief Run a SELECT that returns at most one row and decode it into @p Row.
 *
 * @return std::nullopt when the query returns no row.
 * @throws std::runtime_error as query<Row> does, and when the query returns more than one row.
 */
template <TypedRow Row>
[[nodiscard]] auto query_one(duckdb_connection& conn, const SqlFragment& query)
    -> std::optional<Row> {
  Statement statement(conn, query.sql_);
  statement.BindFragment(1, query);
  const Result& result = statement.Execute();
  RequireRowColumns<Row>(result);
  result.RequireAtMostOneRow();
  if (result.RowCount() == 0) {
    return std::nullopt;
  }
  return DecodeRow<Row>(result, 0);
}

/**
 * @brief The first cell of a one-column SELECT as @p T.
 *
 * @return std::nullopt when the query returns no row or the first cell is NULL. Rows after the
 *         first are not read.
 * @throws std::runtime_error when the query fails, returns a column count other than 1, or its
 *         column type does not fit @p T (see ColumnTypeFitsMember).
 */
template <CellScalar T>
[[nodiscard]] auto scalar(duckdb_connection& conn, const SqlFragment& query) -> std::optional<T> {
  Statement statement(conn, query.sql_);
  statement.BindFragment(1, query);
  const Result& result = statement.Execute();
  result.RequireColumnCount(1);
  result.RequireColumnType<std::optional<T>>(0);
  if (result.RowCount() == 0) {
    return std::nullopt;
  }
  return result.ReadChecked<std::optional<T>>(0, 0);
}

/**
 * @brief scalar<int64_t>: the first cell of a one-column integer SELECT.
 *
 * Kept for its callers until they read typed rows (storage plan S3).
 */
[[nodiscard]] auto select_int64(duckdb_connection& conn, const SqlFragment& query)
    -> std::optional<int64_t>;

/**
 * @brief scalar<std::string>: the first cell of a one-column VARCHAR SELECT.
 *
 * Kept for its callers until they read typed rows (storage plan S3).
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
