//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/duckorm/duckdb_statement.hpp"

#include <duckdb.h>

#include <QString>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "duckdb_value.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace duckorm {
namespace {

constexpr std::size_t kSqlHeadBytes = 120;

/// The first kSqlHeadBytes bytes of @p sql, cut before a UTF-8 continuation byte.
auto                  SqlHead(std::string_view sql) -> std::string_view {
  if (sql.size() <= kSqlHeadBytes) {
    return sql;
  }
  std::size_t end = kSqlHeadBytes;
  while (end > 0 && (static_cast<unsigned char>(sql[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  return sql.substr(0, end);
}

/// Logs @p message with the head of @p sql and throws StatementError. The exception text is
/// @p message only: callers pass it on to the user, and the SQL text belongs in the log.
[[noreturn]] void Fail(std::string_view message, std::string_view sql) {
  const std::string_view head = SqlHead(sql);
  qCWarning(alcedo::diag::appLog).noquote()
      << QString::fromUtf8(std::format("duckorm: {} [SQL: {}]", message, head));
  throw StatementError(std::string(message), std::string(head));
}

auto DuckTypeName(duckdb_type type) -> std::string {
  switch (type) {
    case DUCKDB_TYPE_BOOLEAN:
      return "BOOLEAN";
    case DUCKDB_TYPE_TINYINT:
      return "TINYINT";
    case DUCKDB_TYPE_SMALLINT:
      return "SMALLINT";
    case DUCKDB_TYPE_INTEGER:
      return "INTEGER";
    case DUCKDB_TYPE_BIGINT:
      return "BIGINT";
    case DUCKDB_TYPE_UTINYINT:
      return "UTINYINT";
    case DUCKDB_TYPE_USMALLINT:
      return "USMALLINT";
    case DUCKDB_TYPE_UINTEGER:
      return "UINTEGER";
    case DUCKDB_TYPE_UBIGINT:
      return "UBIGINT";
    case DUCKDB_TYPE_FLOAT:
      return "FLOAT";
    case DUCKDB_TYPE_DOUBLE:
      return "DOUBLE";
    case DUCKDB_TYPE_TIMESTAMP:
      return "TIMESTAMP";
    case DUCKDB_TYPE_DATE:
      return "DATE";
    case DUCKDB_TYPE_HUGEINT:
      return "HUGEINT";
    case DUCKDB_TYPE_VARCHAR:
      return "VARCHAR";
    case DUCKDB_TYPE_BLOB:
      return "BLOB";
    case DUCKDB_TYPE_DECIMAL:
      return "DECIMAL";
    case DUCKDB_TYPE_LIST:
      return "LIST";
    case DUCKDB_TYPE_ARRAY:
      return "ARRAY";
    default:
      return std::format("DuckDB type {}", static_cast<int>(type));
  }
}

auto MemberKindName(MemberKind member) -> const char* {
  switch (member) {
    case MemberKind::kInt8:
      return "int8_t";
    case MemberKind::kInt16:
      return "int16_t";
    case MemberKind::kInt32:
      return "int32_t";
    case MemberKind::kInt64:
      return "int64_t";
    case MemberKind::kUInt8:
      return "uint8_t";
    case MemberKind::kUInt16:
      return "uint16_t";
    case MemberKind::kUInt32:
      return "uint32_t";
    case MemberKind::kUInt64:
      return "uint64_t";
    case MemberKind::kFloat:
      return "float";
    case MemberKind::kDouble:
      return "double";
    case MemberKind::kBool:
      return "bool";
    case MemberKind::kText:
      return "std::string";
    case MemberKind::kBlob:
      return "std::vector<std::uint8_t>";
    case MemberKind::kDate:
      return "std::chrono::sys_days";
  }
  return "unknown member type";
}

/// Signed integer column types by width rank: TINYINT 1, SMALLINT 2, INTEGER 3, BIGINT 4.
auto SignedRank(duckdb_type type) -> int {
  switch (type) {
    case DUCKDB_TYPE_TINYINT:
      return 1;
    case DUCKDB_TYPE_SMALLINT:
      return 2;
    case DUCKDB_TYPE_INTEGER:
      return 3;
    case DUCKDB_TYPE_BIGINT:
      return 4;
    default:
      return 0;
  }
}

/// Unsigned integer column types by width rank: UTINYINT 1 ... UBIGINT 4.
auto UnsignedRank(duckdb_type type) -> int {
  switch (type) {
    case DUCKDB_TYPE_UTINYINT:
      return 1;
    case DUCKDB_TYPE_USMALLINT:
      return 2;
    case DUCKDB_TYPE_UINTEGER:
      return 3;
    case DUCKDB_TYPE_UBIGINT:
      return 4;
    default:
      return 0;
  }
}

/// Fits a signed member of width rank @p rank: a signed column of rank <= rank, or an
/// unsigned column of a smaller rank.
auto FitsSigned(duckdb_type column, int rank) -> bool {
  const int signed_rank   = SignedRank(column);
  const int unsigned_rank = UnsignedRank(column);
  return (signed_rank > 0 && signed_rank <= rank) || (unsigned_rank > 0 && unsigned_rank < rank);
}

auto FitsUnsigned(duckdb_type column, int rank) -> bool {
  const int unsigned_rank = UnsignedRank(column);
  return unsigned_rank > 0 && unsigned_rank <= rank;
}

class DuckLogicalTypeGuard {
 public:
  explicit DuckLogicalTypeGuard(duckdb_logical_type type) : type_(type) {}
  DuckLogicalTypeGuard(const DuckLogicalTypeGuard&)                    = delete;
  auto operator=(const DuckLogicalTypeGuard&) -> DuckLogicalTypeGuard& = delete;
  ~DuckLogicalTypeGuard() {
    if (type_ != nullptr) {
      duckdb_destroy_logical_type(&type_);
    }
  }

  [[nodiscard]] auto     get() const -> duckdb_logical_type { return type_; }
  [[nodiscard]] explicit operator bool() const { return type_ != nullptr; }

 private:
  duckdb_logical_type type_ = nullptr;
};

}  // namespace

namespace internal {
auto MakeFloatArrayValue(std::span<const float> values) -> DuckValueGuard {
  const DuckLogicalTypeGuard child_type(duckdb_create_logical_type(DUCKDB_TYPE_FLOAT));
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
}  // namespace internal

auto ColumnTypeFitsMember(duckdb_type column, MemberKind member) -> bool {
  switch (member) {
    case MemberKind::kInt8:
      return FitsSigned(column, 1);
    case MemberKind::kInt16:
      return FitsSigned(column, 2);
    case MemberKind::kInt32:
      return FitsSigned(column, 3);
    case MemberKind::kInt64:
      return FitsSigned(column, 4);
    case MemberKind::kUInt8:
      return FitsUnsigned(column, 1);
    case MemberKind::kUInt16:
      return FitsUnsigned(column, 2);
    case MemberKind::kUInt32:
      return FitsUnsigned(column, 3);
    case MemberKind::kUInt64:
      return FitsUnsigned(column, 4);
    case MemberKind::kFloat:
      return column == DUCKDB_TYPE_FLOAT;
    case MemberKind::kDouble:
      return column == DUCKDB_TYPE_FLOAT || column == DUCKDB_TYPE_DOUBLE;
    case MemberKind::kBool:
      return column == DUCKDB_TYPE_BOOLEAN;
    case MemberKind::kText:
      return column == DUCKDB_TYPE_VARCHAR;
    case MemberKind::kBlob:
      return column == DUCKDB_TYPE_BLOB;
    case MemberKind::kDate:
      return column == DUCKDB_TYPE_DATE;
  }
  return false;
}

// --- Result ---------------------------------------------------------------------------------

Result::~Result() { Reset(); }

void Result::Reset() {
  if (has_result_) {
    duckdb_destroy_result(&result_);
    has_result_ = false;
  }
  result_ = {};
}

auto Result::RowCount() const -> idx_t { return duckdb_row_count(&result_); }

auto Result::ColumnCount() const -> idx_t { return duckdb_column_count(&result_); }

auto Result::RowsChanged() const -> idx_t { return duckdb_rows_changed(&result_); }

auto Result::ColumnName(idx_t column) const -> std::string {
  const char* name = duckdb_column_name(&result_, column);
  return name != nullptr ? std::string{name} : std::string{};
}

auto Result::ColumnType(idx_t column) const -> duckdb_type {
  return duckdb_column_type(&result_, column);
}

auto Result::IsNull(idx_t column, idx_t row) const -> bool {
  return duckdb_value_is_null(&result_, column, row);
}

void Result::RequireColumnCount(idx_t expected) const {
  const idx_t actual = ColumnCount();
  if (actual != expected) {
    Fail(std::format("duckorm: the query returns {} columns; the row type lists {}", actual,
                     expected),
         sql_);
  }
}

void Result::RequireAtMostOneRow() const {
  const idx_t rows = RowCount();
  if (rows > 1) {
    Fail(std::format("duckorm: the query returns {} rows; at most one is expected", rows), sql_);
  }
}

void Result::RequireColumnKind(idx_t column, MemberKind member, bool optional) const {
  if (column >= ColumnCount()) {
    Fail(std::format("duckorm: column {} is past the {} columns of the result", column,
                     ColumnCount()),
         sql_);
  }
  const duckdb_type type = ColumnType(column);
  if (!ColumnTypeFitsMember(type, member)) {
    Fail(std::format("duckorm: column {} '{}' has type {}, which does not fit the member type {}{}",
                     column, ColumnName(column), DuckTypeName(type), MemberKindName(member),
                     optional ? " (std::optional)" : ""),
         sql_);
  }
}

void Result::RequireValue(idx_t column, idx_t row) const {
  if (IsNull(column, row)) {
    Fail(std::format("duckorm: column {} '{}' is NULL in row {}, and its member is not "
                     "std::optional",
                     column, ColumnName(column), row),
         sql_);
  }
}

void Result::ReadValue(idx_t column, idx_t row, std::int8_t& value) const {
  value = duckdb_value_int8(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::int16_t& value) const {
  value = duckdb_value_int16(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::int32_t& value) const {
  value = duckdb_value_int32(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::int64_t& value) const {
  value = duckdb_value_int64(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::uint8_t& value) const {
  value = duckdb_value_uint8(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::uint16_t& value) const {
  value = duckdb_value_uint16(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::uint32_t& value) const {
  value = duckdb_value_uint32(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::uint64_t& value) const {
  value = duckdb_value_uint64(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, float& value) const {
  value = duckdb_value_float(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, double& value) const {
  value = duckdb_value_double(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, bool& value) const {
  value = duckdb_value_boolean(&result_, column, row);
}

void Result::ReadValue(idx_t column, idx_t row, std::string& value) const {
  duckdb_string text = duckdb_value_string(&result_, column, row);
  value.assign(text.data != nullptr ? text.data : "", static_cast<std::size_t>(text.size));
  duckdb_free(text.data);
}

void Result::ReadValue(idx_t column, idx_t row, std::vector<std::uint8_t>& value) const {
  duckdb_blob blob  = duckdb_value_blob(&result_, column, row);
  const auto* begin = static_cast<const std::uint8_t*>(blob.data);
  value.assign(begin, begin + blob.size);
  duckdb_free(blob.data);
}

void Result::ReadValue(idx_t column, idx_t row, std::chrono::sys_days& value) const {
  const duckdb_date date = duckdb_value_date(&result_, column, row);
  value                  = std::chrono::sys_days{std::chrono::days{date.days}};
}

auto Result::ReadFieldCell(DuckDBType type, idx_t column, idx_t row) const -> VarTypes {
  switch (type) {
    case DuckDBType::INT32:
      return duckdb_value_int32(&result_, column, row);
    case DuckDBType::INT64:
      return duckdb_value_int64(&result_, column, row);
    case DuckDBType::UINT32:
      return duckdb_value_uint32(&result_, column, row);
    case DuckDBType::UINT64:
      return duckdb_value_uint64(&result_, column, row);
    case DuckDBType::DOUBLE:
      return duckdb_value_double(&result_, column, row);
    case DuckDBType::VARCHAR:
    case DuckDBType::JSON:
    case DuckDBType::BOOLEAN:
    case DuckDBType::TIMESTAMP:
    case DuckDBType::STRING:
    case DuckDBType::NULLABLE_STRING: {
      if (IsNull(column, row)) {
        return std::unique_ptr<std::string>{};
      }
      char* text = duckdb_value_varchar(&result_, column, row);
      if (text == nullptr) {
        return std::unique_ptr<std::string>{};
      }
      auto value = std::make_unique<std::string>(text);
      duckdb_free(text);
      return value;
    }
    case DuckDBType::NULLABLE_DOUBLE:
      return IsNull(column, row)
                 ? std::optional<double>{}
                 : std::optional<double>{duckdb_value_double(&result_, column, row)};
    case DuckDBType::NULLABLE_INT64:
      return IsNull(column, row)
                 ? std::optional<int64_t>{}
                 : std::optional<int64_t>{duckdb_value_int64(&result_, column, row)};
    default:
      Fail(std::format("duckorm: field type {} has no select decoding", static_cast<int>(type)),
           sql_);
  }
}

// --- Statement ------------------------------------------------------------------------------

Statement::Statement(duckdb_connection& conn, std::string_view sql) : sql_(sql) {
  if (duckdb_prepare(conn, sql_.c_str(), &statement_) != DuckDBSuccess) {
    const char*       error   = duckdb_prepare_error(statement_);
    const std::string message = error != nullptr && *error != '\0' ? error : "prepare failed";
    duckdb_destroy_prepare(&statement_);
    Fail(message, sql_);
  }
}

Statement::~Statement() {
  result_.Reset();
  duckdb_destroy_prepare(&statement_);
}

auto Statement::ParameterCount() const -> idx_t { return duckdb_nparams(statement_); }

void Statement::CheckBind(duckdb_state state, idx_t index, const char* type) const {
  if (state != DuckDBSuccess) {
    Fail(std::format("duckorm: bind of {} parameter {} failed ({} parameters)", type, index,
                     ParameterCount()),
         sql_);
  }
}

void Statement::BindInt8(idx_t index, std::int8_t value) {
  CheckBind(duckdb_bind_int8(statement_, index, value), index, "TINYINT");
}

void Statement::BindInt16(idx_t index, std::int16_t value) {
  CheckBind(duckdb_bind_int16(statement_, index, value), index, "SMALLINT");
}

void Statement::BindInt32(idx_t index, std::int32_t value) {
  CheckBind(duckdb_bind_int32(statement_, index, value), index, "INTEGER");
}

void Statement::BindInt64(idx_t index, std::int64_t value) {
  CheckBind(duckdb_bind_int64(statement_, index, value), index, "BIGINT");
}

void Statement::BindUInt8(idx_t index, std::uint8_t value) {
  CheckBind(duckdb_bind_uint8(statement_, index, value), index, "UTINYINT");
}

void Statement::BindUInt16(idx_t index, std::uint16_t value) {
  CheckBind(duckdb_bind_uint16(statement_, index, value), index, "USMALLINT");
}

void Statement::BindUInt32(idx_t index, std::uint32_t value) {
  CheckBind(duckdb_bind_uint32(statement_, index, value), index, "UINTEGER");
}

void Statement::BindUInt64(idx_t index, std::uint64_t value) {
  CheckBind(duckdb_bind_uint64(statement_, index, value), index, "UBIGINT");
}

void Statement::Bind(idx_t index, float value) {
  CheckBind(duckdb_bind_float(statement_, index, value), index, "FLOAT");
}

void Statement::Bind(idx_t index, double value) {
  CheckBind(duckdb_bind_double(statement_, index, value), index, "DOUBLE");
}

void Statement::Bind(idx_t index, bool value) {
  CheckBind(duckdb_bind_boolean(statement_, index, value), index, "BOOLEAN");
}

void Statement::Bind(idx_t index, std::string_view value) {
  CheckBind(
      duckdb_bind_varchar_length(statement_, index, value.data(), static_cast<idx_t>(value.size())),
      index, "VARCHAR");
}

void Statement::Bind(idx_t index, std::span<const std::uint8_t> blob) {
  // DuckDB copies the bytes; a non-null pointer is required for an empty blob.
  static constexpr std::uint8_t kEmpty = 0;
  const void*                   data   = blob.empty() ? &kEmpty : blob.data();
  CheckBind(duckdb_bind_blob(statement_, index, data, static_cast<idx_t>(blob.size())), index,
            "BLOB");
}

void Statement::Bind(idx_t index, std::chrono::sys_days date) {
  const duckdb_date value{static_cast<std::int32_t>(date.time_since_epoch().count())};
  CheckBind(duckdb_bind_date(statement_, index, value), index, "DATE");
}

void Statement::Bind(idx_t index, std::nullopt_t /*null*/) {
  CheckBind(duckdb_bind_null(statement_, index), index, "NULL");
}

void Statement::BindFloatArray(idx_t index, std::span<const float> values) {
  const auto array_value = internal::MakeFloatArrayValue(values);
  CheckBind(duckdb_bind_value(statement_, index, array_value.get()), index, "FLOAT[]");
}

void Statement::BindVariant(idx_t index, const BindValue& value) {
  std::visit(
      [&](const auto& held) {
        using T = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          Bind(index, std::nullopt);
        } else if constexpr (std::is_same_v<T, std::string>) {
          Bind(index, std::string_view{held});
        } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
          Bind(index, std::span<const std::uint8_t>{held});
        } else {
          Bind(index, held);
        }
      },
      value);
}

void Statement::BindFragment(idx_t start_index, const SqlFragment& fragment) {
  for (std::size_t i = 0; i < fragment.binds_.size(); ++i) {
    BindVariant(start_index + static_cast<idx_t>(i), fragment.binds_[i]);
  }
}

void Statement::ClearBindings() {
  if (duckdb_clear_bindings(statement_) != DuckDBSuccess) {
    Fail("duckorm: clearing the bound values failed", sql_);
  }
}

auto Statement::Execute() -> Result& {
  result_.Reset();
  const duckdb_state state = duckdb_execute_prepared(statement_, &result_.result_);
  result_.has_result_      = true;
  if (state != DuckDBSuccess) {
    const char*       error   = duckdb_result_error(&result_.result_);
    const std::string message = error != nullptr ? error : "statement failed";
    result_.Reset();
    Fail(message, sql_);
  }
  return result_;
}

}  // namespace duckorm
