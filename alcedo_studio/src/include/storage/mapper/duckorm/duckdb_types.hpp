//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace duckorm {
enum class DuckDBType : uint8_t {
  INT32,
  INT64,
  UINT32,
  UINT64,
  DOUBLE,
  VARCHAR,
  JSON,
  BOOLEAN,
  TIMESTAMP,
  STRING,
  NULLABLE_STRING,
  NULLABLE_DOUBLE,
  NULLABLE_INT64,
  FLOAT_ARRAY,
};

/**
 * @brief A RAII class for managing prepared statements in DuckDB.
 *
 */
class PreparedStatement {
 private:
  void RecycleResources();

 public:
  duckdb_result             result_;
  duckdb_prepared_statement stmt_;
  duckdb_connection&        con_;

  bool                      prepared_ = false;
  PreparedStatement(duckdb_connection& con);
  PreparedStatement(duckdb_connection& con, const std::string& prepare_query);
  PreparedStatement();
  ~PreparedStatement();
  auto GetStmtGuard(const std::string& prepare_query) -> duckdb_prepared_statement&;
  void SetConnection(duckdb_connection& con);
};

/**
 * @brief Field descriptor for DuckDB ORM.
 *
 */
struct DuckFieldDesc {
  const char* name_;
  DuckDBType  type_;
  size_t      offset_;
};

// Macro to define a field descriptor for a specific type and field.
#define FIELD(type, field, field_type) \
  duckorm::DuckFieldDesc { #field, duckorm::DuckDBType::field_type, offsetof(type, field) }

#define FIELD_AS(type, field, column, field_type) \
  duckorm::DuckFieldDesc { column, duckorm::DuckDBType::field_type, offsetof(type, field) }

// brief Type alias for a variant that can hold various DuckDB-supported types.
//
// Text types (VARCHAR, JSON, BOOLEAN, TIMESTAMP, STRING, NULLABLE_STRING) read as
// `std::unique_ptr<std::string>`; a NULL cell reads as an empty pointer. NULLABLE_DOUBLE and
// NULLABLE_INT64 read as `std::optional`, with `std::nullopt` for a NULL cell.
using VarTypes =
    std::variant<int32_t, int64_t, uint32_t, uint64_t, double, std::unique_ptr<std::string>,
                 std::optional<double>, std::optional<int64_t>>;
};  // namespace duckorm
