//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/duckorm/duckdb_orm.hpp"

#include <duckdb.h>

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb_value.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"

namespace duckorm {
namespace {

duckdb_state run_transaction_control(duckdb_connection& conn, const char* sql) {
  duckdb_result result;
  duckdb_state  state = duckdb_query(conn, sql, &result);
  if (state != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message ? error_message : "DuckDB transaction command failed");
  }
  duckdb_destroy_result(&result);
  return state;
}

void bind_field(Statement& statement, idx_t index, const void* obj, const DuckFieldDesc& field) {
  const char* ptr = reinterpret_cast<const char*>(obj) + field.offset_;
  switch (field.type_) {
    case DuckDBType::INT32:
      statement.Bind(index, *reinterpret_cast<const int32_t*>(ptr));
      break;
    case DuckDBType::INT64:
      statement.Bind(index, *reinterpret_cast<const int64_t*>(ptr));
      break;
    case DuckDBType::UINT32:
      statement.Bind(index, *reinterpret_cast<const uint32_t*>(ptr));
      break;
    case DuckDBType::UINT64:
      statement.Bind(index, *reinterpret_cast<const uint64_t*>(ptr));
      break;
    case DuckDBType::DOUBLE:
      statement.Bind(index, *reinterpret_cast<const double*>(ptr));
      break;
    case DuckDBType::TIMESTAMP:
    case DuckDBType::JSON:
    case DuckDBType::VARCHAR: {
      const auto* value = reinterpret_cast<const std::unique_ptr<std::string>*>(ptr);
      if (!value->get()) {
        statement.Bind(index, std::nullopt);
      } else {
        statement.Bind(index, std::string_view{*value->get()});
      }
      break;
    }
    case DuckDBType::STRING:
      statement.Bind(index, std::string_view{*reinterpret_cast<const std::string*>(ptr)});
      break;
    case DuckDBType::NULLABLE_STRING: {
      const auto& value = *reinterpret_cast<const std::string*>(ptr);
      if (value.empty()) {
        statement.Bind(index, std::nullopt);
      } else {
        statement.Bind(index, std::string_view{value});
      }
      break;
    }
    case DuckDBType::NULLABLE_DOUBLE:
      statement.Bind(index, *reinterpret_cast<const std::optional<double>*>(ptr));
      break;
    case DuckDBType::NULLABLE_INT64:
      statement.Bind(index, *reinterpret_cast<const std::optional<int64_t>*>(ptr));
      break;
    case DuckDBType::FLOAT_ARRAY:
      statement.BindFloatArray(index, *reinterpret_cast<const std::vector<float>*>(ptr));
      break;
    case DuckDBType::BOOLEAN:
      statement.Bind(index, *reinterpret_cast<const bool*>(ptr));
      break;
    default:
      throw std::runtime_error("Unsupported DuckFieldType");
  }
}

duckdb_state run_prepared(duckdb_connection& conn, const std::string& sql, const void* obj,
                          std::span<const DuckFieldDesc> fields, size_t field_count) {
  Statement statement(conn, sql);
  for (size_t i = 0; i < field_count; ++i) {
    bind_field(statement, static_cast<idx_t>(i + 1), obj, fields[i]);
  }
  (void)statement.Execute();
  return DuckDBSuccess;
}

auto make_insert_sql(const char* table, std::span<const DuckFieldDesc> fields, size_t field_count,
                     bool or_replace) -> std::string {
  std::ostringstream sql;
  sql << "INSERT ";
  if (or_replace) {
    sql << "OR REPLACE ";
  }
  sql << "INTO " << table << " (";
  for (size_t i = 0; i < field_count; ++i) {
    sql << fields[i].name_;
    if (i < field_count - 1) {
      sql << ",";
    }
  }
  sql << ") VALUES (";
  for (size_t i = 0; i < field_count; ++i) {
    sql << "?";
    if (i < field_count - 1) {
      sql << ",";
    }
  }
  sql << ");";
  return sql.str();
}

auto decode_select_rows(const Result& result, std::span<const DuckFieldDesc> sample_fields,
                        size_t field_count) -> std::vector<std::vector<VarTypes>> {
  result.RequireColumnCount(static_cast<idx_t>(field_count));
  const idx_t                        row_count = result.RowCount();
  std::vector<std::vector<VarTypes>> rows(static_cast<size_t>(row_count));
  for (idx_t row = 0; row < row_count; ++row) {
    rows[row].reserve(field_count);
    for (size_t column = 0; column < field_count; ++column) {
      rows[row].push_back(
          result.ReadFieldCell(sample_fields[column].type_, static_cast<idx_t>(column), row));
    }
  }
  return rows;
}

auto run_select(duckdb_connection& conn, const SqlFragment& query,
                std::span<const DuckFieldDesc> sample_fields, size_t field_count)
    -> std::vector<std::vector<VarTypes>> {
  Statement statement(conn, query.sql_);
  statement.BindFragment(1, query);
  return decode_select_rows(statement.Execute(), sample_fields, field_count);
}

}  // namespace

/**
 * @brief Insert an object into a DuckDB table.
 *
 * @param conn a reference to the DuckDB connection
 * @param table the name of the table to insert into
 * @param obj the object to insert, which should be a pointer to a struct
 *            with fields matching the DuckFieldDesc descriptions
 * @param fields a span of DuckFieldDesc describing the fields of the object
 * @param field_count the number of fields in the object
 * @return duckdb_state
 */
duckdb_state insert(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count) {
  return run_prepared(conn, make_insert_sql(table, fields, field_count, false), obj, fields,
                      field_count);
}

duckdb_state insert_or_replace(duckdb_connection& conn, const char* table, const void* obj,
                               std::span<const DuckFieldDesc> fields, size_t field_count) {
  return run_prepared(conn, make_insert_sql(table, fields, field_count, true), obj, fields,
                      field_count);
}

duckdb_state insert_by_query(duckdb_connection& conn, const std::string& sql, const void* obj,
                             std::span<const DuckFieldDesc> fields, size_t field_count) {
  return run_prepared(conn, sql, obj, fields, field_count);
}

duckdb_state update(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count,
                    const SqlFragment& where_clause) {
  std::ostringstream sql;
  // Upsert (INSERT ... ON CONFLICT DO UPDATE) using the table's PRIMARY KEY/UNIQUE constraints.
  // The provided where_clause is applied as a conditional guard on the DO UPDATE.
  sql << "INSERT INTO " << table << " (";
  for (size_t i = 0; i < field_count; ++i) {
    sql << fields[i].name_;
    if (i < field_count - 1) {
      sql << ",";
    }
  }
  sql << ") VALUES (";
  for (size_t i = 0; i < field_count; ++i) {
    sql << "?";
    if (i < field_count - 1) {
      sql << ",";
    }
  }
  sql << ") ON CONFLICT DO UPDATE SET ";
  for (size_t i = 0; i < field_count; ++i) {
    sql << fields[i].name_ << " = EXCLUDED." << fields[i].name_;
    if (i < field_count - 1) {
      sql << ",";
    }
  }
  if (!where_clause.sql_.empty()) {
    sql << " WHERE " << where_clause.sql_;
  }
  sql << ";";

  Statement statement(conn, sql.str());
  for (size_t i = 0; i < field_count; ++i) {
    bind_field(statement, static_cast<idx_t>(i + 1), obj, fields[i]);
  }
  statement.BindFragment(static_cast<idx_t>(field_count + 1), where_clause);
  (void)statement.Execute();
  return DuckDBSuccess;
}

namespace {
duckdb_state run_multi_row_insert(duckdb_connection& conn, const char* table,
                                  std::span<const void* const>   rows,
                                  std::span<const DuckFieldDesc> fields, size_t field_count,
                                  bool update_on_conflict) {
  if (rows.empty()) {
    return DuckDBSuccess;
  }
  std::ostringstream sql;
  sql << "INSERT INTO " << table << " (";
  for (size_t i = 0; i < field_count; ++i) {
    sql << fields[i].name_;
    if (i < field_count - 1) {
      sql << ",";
    }
  }
  sql << ") VALUES ";
  for (size_t row = 0; row < rows.size(); ++row) {
    sql << (row == 0 ? "(" : ",(");
    for (size_t i = 0; i < field_count; ++i) {
      sql << (i == 0 ? "?" : ",?");
    }
    sql << ")";
  }
  if (update_on_conflict) {
    sql << " ON CONFLICT DO UPDATE SET ";
    for (size_t i = 0; i < field_count; ++i) {
      sql << fields[i].name_ << " = EXCLUDED." << fields[i].name_;
      if (i < field_count - 1) {
        sql << ",";
      }
    }
  }
  sql << ";";

  Statement statement(conn, sql.str());
  idx_t     index = 1;
  for (const void* row : rows) {
    for (size_t i = 0; i < field_count; ++i) {
      bind_field(statement, index++, row, fields[i]);
    }
  }
  (void)statement.Execute();
  return DuckDBSuccess;
}
}  // namespace

duckdb_state upsert_rows(duckdb_connection& conn, const char* table,
                         std::span<const void* const> rows, std::span<const DuckFieldDesc> fields,
                         size_t field_count) {
  return run_multi_row_insert(conn, table, rows, fields, field_count, true);
}

duckdb_state insert_rows(duckdb_connection& conn, const char* table,
                         std::span<const void* const> rows, std::span<const DuckFieldDesc> fields,
                         size_t field_count) {
  return run_multi_row_insert(conn, table, rows, fields, field_count, false);
}

duckdb_state remove(duckdb_connection& conn, const char* table, const SqlFragment& where_clause) {
  std::ostringstream sql;
  sql << "DELETE FROM " << table << " WHERE " << where_clause.sql_ << ";";

  Statement statement(conn, sql.str());
  statement.BindFragment(1, where_clause);
  (void)statement.Execute();
  return DuckDBSuccess;
}

std::vector<std::vector<VarTypes>> select(duckdb_connection& conn, const std::string& table,
                                          std::span<const DuckFieldDesc> sample_fields,
                                          size_t field_count, const SqlFragment& where_clause) {
  SqlFragment query{std::format("SELECT * FROM {} WHERE {};", table, where_clause.sql_),
                    where_clause.binds_};
  return run_select(conn, query, sample_fields, field_count);
}

std::vector<std::vector<VarTypes>> select_by_query(duckdb_connection&             conn,
                                                   std::span<const DuckFieldDesc> sample_fields,
                                                   size_t field_count, const std::string& sql) {
  return run_select(conn, SqlFragment{sql, {}}, sample_fields, field_count);
}

std::vector<std::vector<VarTypes>> select_by_query(duckdb_connection&             conn,
                                                   std::span<const DuckFieldDesc> sample_fields,
                                                   size_t field_count, const SqlFragment& query) {
  return run_select(conn, query, sample_fields, field_count);
}

void execute(duckdb_connection& conn, const SqlFragment& statement) {
  if (statement.binds_.empty()) {
    duckdb_result result;
    if (duckdb_query(conn, statement.sql_.c_str(), &result) != DuckDBSuccess) {
      const char*       error   = duckdb_result_error(&result);
      const std::string message = error ? error : "DuckDB statement failed";
      duckdb_destroy_result(&result);
      throw std::runtime_error(message);
    }
    duckdb_destroy_result(&result);
    return;
  }
  Statement prepared(conn, statement.sql_);
  prepared.BindFragment(1, statement);
  (void)prepared.Execute();
}

auto execute_count(duckdb_connection& conn, const SqlFragment& statement) -> idx_t {
  Statement prepared(conn, statement.sql_);
  prepared.BindFragment(1, statement);
  return prepared.Execute().RowsChanged();
}

auto select_int64(duckdb_connection& conn, const SqlFragment& query) -> std::optional<int64_t> {
  return scalar<int64_t>(conn, query);
}

auto select_string(duckdb_connection& conn, const SqlFragment& query)
    -> std::optional<std::string> {
  return scalar<std::string>(conn, query);
}

auto cell_text(const VarTypes& cell) -> std::string {
  const auto& text = std::get<std::unique_ptr<std::string>>(cell);
  return text ? *text : std::string{};
}

auto cell_bool(const VarTypes& cell) -> bool {
  const auto& text = std::get<std::unique_ptr<std::string>>(cell);
  return text && !text->empty() && ((*text)[0] == 't' || (*text)[0] == 'T');
}

Transaction::Transaction(duckdb_connection& conn) : conn_(conn) {
  run_transaction_control(conn_, "BEGIN TRANSACTION;");
}

Transaction::~Transaction() {
  if (committed_) {
    return;
  }
  duckdb_result result;
  duckdb_query(conn_, "ROLLBACK;", &result);
  duckdb_destroy_result(&result);
}

void Transaction::commit() {
  run_transaction_control(conn_, "COMMIT;");
  committed_ = true;
}

Appender::Appender(duckdb_connection& conn, const char* table,
                   std::span<const char* const> columns)
    : table_(table) {
  if (duckdb_appender_create(conn, nullptr, table, &appender_) != DuckDBSuccess) {
    const char*       error   = duckdb_appender_error(appender_);
    const std::string message =
        error ? error : std::format("DuckDB appender create failed for {}", table_);
    duckdb_appender_destroy(&appender_);
    throw std::runtime_error(message);
  }
  for (const char* column : columns) {
    check(duckdb_appender_add_column(appender_, column), "add_column");
  }
}

Appender::~Appender() {
  if (appender_ != nullptr) {
    duckdb_appender_destroy(&appender_);
  }
}

void Appender::check(duckdb_state state, const char* operation) const {
  if (state == DuckDBSuccess) {
    return;
  }
  const char* error = duckdb_appender_error(appender_);
  throw std::runtime_error(error ? std::string(error)
                                 : std::format("DuckDB appender {} failed for {}", operation,
                                               table_));
}

void Appender::append_int32(int32_t value) {
  check(duckdb_append_int32(appender_, value), "append");
}

void Appender::append_uint32(uint32_t value) {
  check(duckdb_append_uint32(appender_, value), "append");
}

void Appender::append_double(double value) {
  check(duckdb_append_double(appender_, value), "append");
}

void Appender::append_bool(bool value) { check(duckdb_append_bool(appender_, value), "append"); }

void Appender::append_varchar(std::string_view value) {
  check(duckdb_append_varchar_length(appender_, value.data(), static_cast<idx_t>(value.size())),
        "append");
}

void Appender::append_null() { check(duckdb_append_null(appender_), "append"); }

void Appender::append_float_array(std::span<const float> values) {
  const auto array_value = internal::MakeFloatArrayValue(values);
  check(duckdb_append_value(appender_, array_value.get()), "append");
}

void Appender::end_row() { check(duckdb_appender_end_row(appender_), "end_row"); }

void Appender::flush() { check(duckdb_appender_flush(appender_), "flush"); }

};  // namespace duckorm
