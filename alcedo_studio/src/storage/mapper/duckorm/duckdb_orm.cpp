//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/duckorm/duckdb_orm.hpp"

#include <duckdb.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace duckorm {
namespace {
class DuckValueGuard {
 public:
  explicit DuckValueGuard(duckdb_value value = nullptr) : value_(value) {}
  DuckValueGuard(const DuckValueGuard&)                    = delete;
  auto operator=(const DuckValueGuard&) -> DuckValueGuard& = delete;
  DuckValueGuard(DuckValueGuard&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
  auto operator=(DuckValueGuard&& other) noexcept -> DuckValueGuard& {
    if (this != &other) {
      Reset();
      value_       = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }
  ~DuckValueGuard() { Reset(); }

  [[nodiscard]] auto     get() const -> duckdb_value { return value_; }
  [[nodiscard]] explicit operator bool() const { return value_ != nullptr; }

 private:
  void Reset() {
    if (value_) {
      duckdb_destroy_value(&value_);
      value_ = nullptr;
    }
  }

  duckdb_value value_ = nullptr;
};

class DuckLogicalTypeGuard {
 public:
  explicit DuckLogicalTypeGuard(duckdb_logical_type type = nullptr) : type_(type) {}
  DuckLogicalTypeGuard(const DuckLogicalTypeGuard&)                    = delete;
  auto operator=(const DuckLogicalTypeGuard&) -> DuckLogicalTypeGuard& = delete;
  ~DuckLogicalTypeGuard() {
    if (type_) {
      duckdb_destroy_logical_type(&type_);
    }
  }

  [[nodiscard]] auto     get() const -> duckdb_logical_type { return type_; }
  [[nodiscard]] explicit operator bool() const { return type_ != nullptr; }

 private:
  duckdb_logical_type type_ = nullptr;
};

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

auto make_float_array_value(const std::vector<float>& values) -> DuckValueGuard {
  DuckLogicalTypeGuard child_type(duckdb_create_logical_type(DUCKDB_TYPE_FLOAT));
  if (!child_type) {
    throw std::runtime_error("DuckDB failed to create FLOAT logical type");
  }

  std::vector<DuckValueGuard> value_guards;
  value_guards.reserve(values.size());
  std::vector<duckdb_value> raw_values;
  raw_values.reserve(values.size());
  for (const float value : values) {
    value_guards.emplace_back(duckdb_create_float(value));
    if (!value_guards.back()) {
      throw std::runtime_error("DuckDB failed to create FLOAT value");
    }
    raw_values.push_back(value_guards.back().get());
  }

  DuckValueGuard array_value(duckdb_create_array_value(child_type.get(), raw_values.data(),
                                                       static_cast<idx_t>(raw_values.size())));
  if (!array_value) {
    throw std::runtime_error("DuckDB failed to create FLOAT array value");
  }
  return array_value;
}

void bind_field(duckdb_prepared_statement stmt, idx_t index, const void* obj,
                const DuckFieldDesc& field) {
  const char* ptr = reinterpret_cast<const char*>(obj) + field.offset_;
  switch (field.type_) {
    case DuckDBType::INT32:
      duckdb_bind_int32(stmt, index, *reinterpret_cast<const int32_t*>(ptr));
      break;
    case DuckDBType::INT64:
      duckdb_bind_int64(stmt, index, *reinterpret_cast<const int64_t*>(ptr));
      break;
    case DuckDBType::UINT32:
      duckdb_bind_uint32(stmt, index, *reinterpret_cast<const uint32_t*>(ptr));
      break;
    case DuckDBType::UINT64:
      duckdb_bind_uint64(stmt, index, *reinterpret_cast<const uint64_t*>(ptr));
      break;
    case DuckDBType::DOUBLE:
      duckdb_bind_double(stmt, index, *reinterpret_cast<const double*>(ptr));
      break;
    case DuckDBType::TIMESTAMP:
    case DuckDBType::JSON:
    case DuckDBType::VARCHAR: {
      const auto* value = reinterpret_cast<const std::unique_ptr<std::string>*>(ptr);
      if (!value->get()) {
        duckdb_bind_null(stmt, index);
      } else {
        duckdb_bind_varchar(stmt, index, value->get()->c_str());
      }
      break;
    }
    case DuckDBType::STRING:
      duckdb_bind_varchar(stmt, index, reinterpret_cast<const std::string*>(ptr)->c_str());
      break;
    case DuckDBType::NULLABLE_STRING: {
      const auto& value = *reinterpret_cast<const std::string*>(ptr);
      if (value.empty()) {
        duckdb_bind_null(stmt, index);
      } else {
        duckdb_bind_varchar(stmt, index, value.c_str());
      }
      break;
    }
    case DuckDBType::NULLABLE_DOUBLE: {
      const auto& value = *reinterpret_cast<const std::optional<double>*>(ptr);
      if (value.has_value()) {
        duckdb_bind_double(stmt, index, *value);
      } else {
        duckdb_bind_null(stmt, index);
      }
      break;
    }
    case DuckDBType::FLOAT_ARRAY: {
      auto array_value = make_float_array_value(*reinterpret_cast<const std::vector<float>*>(ptr));
      if (duckdb_bind_value(stmt, index, array_value.get()) != DuckDBSuccess) {
        throw std::runtime_error("DuckDB failed to bind FLOAT array value");
      }
      break;
    }
    case DuckDBType::BOOLEAN:
      duckdb_bind_boolean(stmt, index, *reinterpret_cast<const bool*>(ptr));
      break;
    default:
      throw std::runtime_error("Unsupported DuckFieldType");
  }
}

duckdb_state run_prepared(duckdb_connection& conn, const std::string& sql, const void* obj,
                          std::span<const DuckFieldDesc> fields, size_t field_count) {
  PreparedStatement prepared{conn, sql};
  for (size_t i = 0; i < field_count; ++i) {
    bind_field(prepared.stmt_, static_cast<idx_t>(i + 1), obj, fields[i]);
  }
  duckdb_state state = duckdb_execute_prepared(prepared.stmt_, &prepared.result_);
  if (state != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&prepared.result_);
    throw std::runtime_error(error_message ? error_message : "DuckDB prepared statement failed");
  }
  return state;
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

void bind_fragment_value(duckdb_prepared_statement stmt, idx_t index, const BindValue& value) {
  std::visit(
      [&](const auto& held) {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          duckdb_bind_null(stmt, index);
        } else if constexpr (std::is_same_v<T, int64_t>) {
          duckdb_bind_int64(stmt, index, held);
        } else if constexpr (std::is_same_v<T, double>) {
          duckdb_bind_double(stmt, index, held);
        } else if constexpr (std::is_same_v<T, bool>) {
          duckdb_bind_boolean(stmt, index, held);
        } else if constexpr (std::is_same_v<T, std::string>) {
          duckdb_bind_varchar(stmt, index, held.c_str());
        }
      },
      value);
}

void bind_fragment_values(duckdb_prepared_statement stmt, idx_t start_index,
                          const SqlFragment& fragment) {
  for (size_t i = 0; i < fragment.binds_.size(); ++i) {
    bind_fragment_value(stmt, start_index + static_cast<idx_t>(i), fragment.binds_[i]);
  }
}

auto decode_select_rows(PreparedStatement& select_pre, std::span<const DuckFieldDesc> sample_fields,
                        size_t field_count) -> std::vector<std::vector<VarTypes>> {
  std::vector<std::vector<VarTypes>> results;
  if (duckdb_column_count(&select_pre.result_) != field_count) {
    throw std::runtime_error("Column count mismatch in select query");
  }

  idx_t row_count = duckdb_row_count(&select_pre.result_);
  if (row_count == 0) {
    return results;
  }
  results.resize(row_count);
  for (idx_t i = 0; i < row_count; ++i) {
    results[i].resize(field_count);
    for (size_t j = 0; j < field_count; ++j) {
      switch (sample_fields[j].type_) {
        case DuckDBType::INT32: {
          int32_t value = duckdb_value_int32(&select_pre.result_, j, i);
          results[i][j] = value;
          break;
        };
        case DuckDBType::INT64: {
          int64_t value = duckdb_value_int64(&select_pre.result_, j, i);
          results[i][j] = value;
          break;
        }
        case DuckDBType::UINT32: {
          uint32_t value = duckdb_value_uint32(&select_pre.result_, j, i);
          results[i][j]  = value;
          break;
        }
        case DuckDBType::UINT64: {
          uint64_t value = duckdb_value_uint64(&select_pre.result_, j, i);
          results[i][j]  = value;
          break;
        }
        case DuckDBType::DOUBLE: {
          double value  = duckdb_value_double(&select_pre.result_, j, i);
          results[i][j] = value;
          break;
        }
        case DuckDBType::VARCHAR:
        case DuckDBType::JSON:
        case DuckDBType::BOOLEAN:
        case DuckDBType::TIMESTAMP: {
          const char* value = duckdb_value_varchar(&select_pre.result_, j, i);
          results[i][j]     = std::make_unique<std::string>(value);
          break;
        }
        default:
          throw std::runtime_error("Unsupported DuckFieldType in select()");
      }
    }
  }

  return results;
}
}  // namespace

duckdb_state begin_transaction(duckdb_connection& conn) {
  return run_transaction_control(conn, "BEGIN TRANSACTION;");
}

duckdb_state commit_transaction(duckdb_connection& conn) {
  return run_transaction_control(conn, "COMMIT;");
}

duckdb_state rollback_transaction(duckdb_connection& conn) {
  return run_transaction_control(conn, "ROLLBACK;");
}

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

/**
 * @brief Update an object in a DuckDB table.
 *
 * @param conn
 * @param table
 * @param obj
 * @param fields
 * @param field_count
 * @param where_clause
 * @return duckdb_state
 */
duckdb_state update(duckdb_connection& conn, const char* table, const void* obj,
                    std::span<const DuckFieldDesc> fields, size_t field_count,
                    const char* where_clause) {
  return update(conn, table, obj, fields, field_count,
                SqlFragment{where_clause ? where_clause : "", {}});
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
  std::string sql_str = sql.str();

  PreparedStatement update_pre(conn, sql_str);

  for (size_t i = 0; i < field_count; ++i) {
    bind_field(update_pre.stmt_, static_cast<idx_t>(i + 1), obj, fields[i]);
  }
  bind_fragment_values(update_pre.stmt_, static_cast<idx_t>(field_count + 1), where_clause);

  duckdb_state state = duckdb_execute_prepared(update_pre.stmt_, &update_pre.result_);
  if (state != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&update_pre.result_);
    throw std::runtime_error(error_message ? error_message : "DuckDB update failed");
  }

  return state;
}

/**
 * @brief Remove an object from a DuckDB table based on a where clause.
 *
 * @param conn
 * @param table
 * @param where_clause
 * @return duckdb_state
 */
duckdb_state remove(duckdb_connection& conn, const char* table, const char* where_clause) {
  return remove(conn, table, SqlFragment{where_clause ? where_clause : "", {}});
}

duckdb_state remove(duckdb_connection& conn, const char* table, const SqlFragment& where_clause) {
  std::ostringstream sql;
  sql << "DELETE FROM " << table << " WHERE " << where_clause.sql_ << ";";
  std::string sql_str = sql.str();

  PreparedStatement delete_pre(conn, sql_str);
  bind_fragment_values(delete_pre.stmt_, 1, where_clause);

  duckdb_state state = duckdb_execute_prepared(delete_pre.stmt_, &delete_pre.result_);
  if (state != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&delete_pre.result_);
    throw std::runtime_error(error_message ? error_message : "DuckDB remove failed");
  }

  return state;
}

/**
 * @brief Select rows from a DuckDB table based on a where clause.
 *
 * @param conn
 * @param table
 * @param sample_fields
 * @param field_count
 * @param where_clause
 * @return std::vector<std::vector<VarTypes>>
 */
std::vector<std::vector<VarTypes>> select(duckdb_connection& conn, const std::string table,
                                          std::span<const DuckFieldDesc> sample_fields,
                                          size_t field_count, const char* where_clause) {
  return select(conn, table, sample_fields, field_count,
                SqlFragment{where_clause ? where_clause : "", {}});
}

std::vector<std::vector<VarTypes>> select(duckdb_connection& conn, const std::string& table,
                                          std::span<const DuckFieldDesc> sample_fields,
                                          size_t field_count, const SqlFragment& where_clause) {
  std::ostringstream sql;
  sql << "SELECT * FROM " << table << " WHERE " << where_clause.sql_ << ";";

  PreparedStatement select_pre(conn, sql.str());
  bind_fragment_values(select_pre.stmt_, 1, where_clause);

  if (duckdb_execute_prepared(select_pre.stmt_, &select_pre.result_) != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&select_pre.result_);
    throw std::runtime_error(error_message ? error_message : "DuckDB select failed");
  }

  return decode_select_rows(select_pre, sample_fields, field_count);
}

/**
 * @brief Generic select by SQL query
 *
 * @param conn
 * @param sample_fields
 * @param field_count
 * @param sql
 * @return std::vector<std::vector<VarTypes>>
 */
std::vector<std::vector<VarTypes>> select_by_query(duckdb_connection&             conn,
                                                   std::span<const DuckFieldDesc> sample_fields,
                                                   size_t field_count, const std::string& sql) {
  PreparedStatement select_pre(conn, sql);

  if (duckdb_execute_prepared(select_pre.stmt_, &select_pre.result_) != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&select_pre.result_);
    throw std::runtime_error(error_message ? error_message : "DuckDB select_by_query failed");
  }

  return decode_select_rows(select_pre, sample_fields, field_count);
}

};  // namespace duckorm
