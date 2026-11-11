//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"

namespace duckorm {

/**
 * @brief The error of a failed Statement or Result call.
 *
 * `what()` is the DuckDB or duckorm message: the text that a caller can show to the user.
 * sql_head() is the first 120 bytes of the SQL text (cut before a UTF-8 continuation byte). The
 * throw writes both to the application log as a warning, so the statement stays known for
 * diagnosis.
 */
class StatementError : public std::runtime_error {
 public:
  StatementError(const std::string& message, std::string sql_head)
      : std::runtime_error(message), sql_head_(std::move(sql_head)) {}

  [[nodiscard]] auto sql_head() const -> const std::string& { return sql_head_; }

 private:
  std::string sql_head_;
};

/**
 * @brief C++ types that one result cell decodes into and that a row struct member can have.
 *
 * Integer types, `float`, `double`, `bool`, `std::string` (VARCHAR), `std::vector<std::uint8_t>`
 * (BLOB), and `std::chrono::sys_days` (DATE). A member of type `std::optional<T>` of one of these
 * reads a NULL cell as `std::nullopt`.
 */
template <typename T>
concept CellScalar =
    std::same_as<T, std::int8_t> || std::same_as<T, std::int16_t> ||
    std::same_as<T, std::int32_t> || std::same_as<T, std::int64_t> ||
    std::same_as<T, std::uint8_t> || std::same_as<T, std::uint16_t> ||
    std::same_as<T, std::uint32_t> || std::same_as<T, std::uint64_t> || std::same_as<T, float> ||
    std::same_as<T, double> || std::same_as<T, bool> || std::same_as<T, std::string> ||
    std::same_as<T, std::vector<std::uint8_t>> || std::same_as<T, std::chrono::sys_days>;

/// `kOptional` tells whether @p T is `std::optional<U>`; `Value` is `U`, or @p T itself.
template <typename T>
struct CellOptionalTraits {
  static constexpr bool kOptional = false;
  using Value                     = T;
};

template <typename T>
struct CellOptionalTraits<std::optional<T>> {
  static constexpr bool kOptional = true;
  using Value                     = T;
};

/// A CellScalar, or `std::optional` of a CellScalar.
template <typename T>
concept CellValue = CellScalar<typename CellOptionalTraits<T>::Value>;

/// The C++ type of a row member, used in column type checks and their error messages.
enum class MemberKind : std::uint8_t {
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kUInt8,
  kUInt16,
  kUInt32,
  kUInt64,
  kFloat,
  kDouble,
  kBool,
  kText,
  kBlob,
  kDate,
};

template <CellScalar T>
consteval auto MemberKindOf() -> MemberKind {
  if constexpr (std::same_as<T, std::int8_t>) {
    return MemberKind::kInt8;
  } else if constexpr (std::same_as<T, std::int16_t>) {
    return MemberKind::kInt16;
  } else if constexpr (std::same_as<T, std::int32_t>) {
    return MemberKind::kInt32;
  } else if constexpr (std::same_as<T, std::int64_t>) {
    return MemberKind::kInt64;
  } else if constexpr (std::same_as<T, std::uint8_t>) {
    return MemberKind::kUInt8;
  } else if constexpr (std::same_as<T, std::uint16_t>) {
    return MemberKind::kUInt16;
  } else if constexpr (std::same_as<T, std::uint32_t>) {
    return MemberKind::kUInt32;
  } else if constexpr (std::same_as<T, std::uint64_t>) {
    return MemberKind::kUInt64;
  } else if constexpr (std::same_as<T, float>) {
    return MemberKind::kFloat;
  } else if constexpr (std::same_as<T, double>) {
    return MemberKind::kDouble;
  } else if constexpr (std::same_as<T, bool>) {
    return MemberKind::kBool;
  } else if constexpr (std::same_as<T, std::string>) {
    return MemberKind::kText;
  } else if constexpr (std::same_as<T, std::vector<std::uint8_t>>) {
    return MemberKind::kBlob;
  } else {
    return MemberKind::kDate;
  }
}

/**
 * @brief Whether a column of DuckDB type @p column decodes into a member of @p member without
 *        loss.
 *
 * An integer member takes an integer column of the same signedness with a range that fits, and
 * a signed member also takes a narrower unsigned column (UINTEGER into `int64_t`). `double`
 * takes FLOAT and DOUBLE; every other member takes only its own type: FLOAT, BOOLEAN, VARCHAR,
 * BLOB, DATE. The rule is the same for a `std::optional` member. The C API reports an untyped
 * NULL literal (`SELECT NULL`) as INTEGER, so such a column needs a cast (`NULL::VARCHAR`).
 */
[[nodiscard]] auto ColumnTypeFitsMember(duckdb_type column, MemberKind member) -> bool;

/**
 * @brief The materialized result of one Statement::Execute.
 *
 * Owned by its Statement. It stays valid until the next Execute of that statement or the
 * statement's destruction. Cell reads check the column type against the requested C++ type
 * (see ColumnTypeFitsMember) and throw StatementError that names the column, its DuckDB type,
 * and the C++ type.
 *
 * Not thread-safe: use it on the thread that holds the connection's lock.
 */
class Result {
 public:
  Result(const Result&)                    = delete;
  auto operator=(const Result&) -> Result& = delete;
  ~Result();

  [[nodiscard]] auto RowCount() const -> idx_t;
  [[nodiscard]] auto ColumnCount() const -> idx_t;
  /// Rows that an INSERT, UPDATE, or DELETE changed; 0 for a SELECT.
  [[nodiscard]] auto RowsChanged() const -> idx_t;
  [[nodiscard]] auto ColumnName(idx_t column) const -> std::string;
  [[nodiscard]] auto ColumnType(idx_t column) const -> duckdb_type;
  [[nodiscard]] auto IsNull(idx_t column, idx_t row) const -> bool;

  /// Throw unless the result has exactly @p expected columns.
  void               RequireColumnCount(idx_t expected) const;

  /// Throw when the result has more than one row.
  void               RequireAtMostOneRow() const;

  /// Throw unless @p column decodes into @p T (see ColumnTypeFitsMember).
  template <CellValue T>
  void RequireColumnType(idx_t column) const {
    using Traits = CellOptionalTraits<T>;
    RequireColumnKind(column, MemberKindOf<typename Traits::Value>(), Traits::kOptional);
  }

  /**
   * @brief Value of one cell, with the column type check.
   * @throws std::runtime_error when the column type does not fit @p T, or the cell is NULL and
   *         @p T is not `std::optional`.
   */
  template <CellValue T>
  [[nodiscard]] auto Get(idx_t column, idx_t row) const -> T {
    RequireColumnType<T>(column);
    return ReadChecked<T>(column, row);
  }

  /**
   * @brief Value of one cell of a column that passed RequireColumnType<T>.
   *
   * query<Row> checks each column once and then reads every row through this method.
   * @throws std::runtime_error when the cell is NULL and @p T is not `std::optional`.
   */
  template <CellValue T>
  [[nodiscard]] auto ReadChecked(idx_t column, idx_t row) const -> T {
    using Traits = CellOptionalTraits<T>;
    if constexpr (Traits::kOptional) {
      if (IsNull(column, row)) {
        return std::nullopt;
      }
      typename Traits::Value value{};
      ReadValue(column, row, value);
      return value;
    } else {
      RequireValue(column, row);
      T value{};
      ReadValue(column, row, value);
      return value;
    }
  }

  /**
   * @brief One cell decoded as a positional VarTypes value of @p type, with DuckDB's own value
   *        cast and no column type check.
   *
   * Used by the DuckFieldDesc select paths (`select`, `select_by_query`) until their callers use
   * typed rows.
   */
  [[nodiscard]] auto ReadFieldCell(DuckDBType type, idx_t column, idx_t row) const -> VarTypes;

 private:
  friend class Statement;

  explicit Result(const std::string& sql) : sql_(sql) {}
  void                  Reset();
  void                  RequireColumnKind(idx_t column, MemberKind member, bool optional) const;
  void                  RequireValue(idx_t column, idx_t row) const;

  void                  ReadValue(idx_t column, idx_t row, std::int8_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::int16_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::int32_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::int64_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::uint8_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::uint16_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::uint32_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::uint64_t& value) const;
  void                  ReadValue(idx_t column, idx_t row, float& value) const;
  void                  ReadValue(idx_t column, idx_t row, double& value) const;
  void                  ReadValue(idx_t column, idx_t row, bool& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::string& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::vector<std::uint8_t>& value) const;
  void                  ReadValue(idx_t column, idx_t row, std::chrono::sys_days& value) const;

  /// The C API reads take a non-const result pointer; reads do not change the result.
  mutable duckdb_result result_     = {};
  bool                  has_result_ = false;
  const std::string&    sql_;
};

/**
 * @brief The single owner of one prepared DuckDB statement and of the result of its last run.
 *
 * The constructor prepares the SQL text; Bind sets parameters (1-based, as in DuckDB); Execute
 * runs the statement and returns the result. A statement can be bound and executed again; each
 * Execute releases the previous result. Every failed DuckDB call throws StatementError with
 * DuckDB's message.
 *
 * Not thread-safe: use it on the thread that holds the connection's lock. The connection must
 * outlive the statement. The statement cannot be copied or moved, because its result refers to
 * its SQL text.
 */
class Statement {
 public:
  /// Prepare @p sql (one statement) on @p conn. Throws on a prepare error.
  Statement(duckdb_connection& conn, std::string_view sql);
  Statement(const Statement&)                    = delete;
  auto operator=(const Statement&) -> Statement& = delete;
  ~Statement();

  [[nodiscard]] auto ParameterCount() const -> idx_t;
  [[nodiscard]] auto Sql() const -> const std::string& { return sql_; }

  /// Bind an integer as the DuckDB integer type of the same width and signedness.
  template <std::integral T>
    requires(!std::same_as<T, bool> && !std::same_as<T, char>)
  void Bind(idx_t index, T value) {
    if constexpr (std::is_signed_v<T>) {
      if constexpr (sizeof(T) == 1) {
        BindInt8(index, static_cast<std::int8_t>(value));
      } else if constexpr (sizeof(T) == 2) {
        BindInt16(index, static_cast<std::int16_t>(value));
      } else if constexpr (sizeof(T) == 4) {
        BindInt32(index, static_cast<std::int32_t>(value));
      } else {
        static_assert(sizeof(T) == 8);
        BindInt64(index, static_cast<std::int64_t>(value));
      }
    } else {
      if constexpr (sizeof(T) == 1) {
        BindUInt8(index, static_cast<std::uint8_t>(value));
      } else if constexpr (sizeof(T) == 2) {
        BindUInt16(index, static_cast<std::uint16_t>(value));
      } else if constexpr (sizeof(T) == 4) {
        BindUInt32(index, static_cast<std::uint32_t>(value));
      } else {
        static_assert(sizeof(T) == 8);
        BindUInt64(index, static_cast<std::uint64_t>(value));
      }
    }
  }
  void Bind(idx_t index, float value);
  void Bind(idx_t index, double value);
  void Bind(idx_t index, bool value);
  /// VARCHAR. The bytes are copied; @p value may contain NUL characters.
  void Bind(idx_t index, std::string_view value);
  /// VARCHAR; this overload keeps a C string from binding as `bool`.
  void Bind(idx_t index, const char* value) { Bind(index, std::string_view{value}); }
  /// BLOB. The bytes are copied.
  void Bind(idx_t index, std::span<const std::uint8_t> blob);
  void Bind(idx_t index, std::chrono::sys_days date);
  void Bind(idx_t index, std::nullopt_t);
  template <typename T>
  void Bind(idx_t index, const std::optional<T>& value) {
    if (value.has_value()) {
      Bind(index, *value);
    } else {
      Bind(index, std::nullopt);
    }
  }
  /// A `FLOAT[n]` array value; n must match the parameter's array size.
  void BindFloatArray(idx_t index, std::span<const float> values);

  /**
   * @brief Bind the values of @p fragment at @p start_index, @p start_index + 1, ...
   *
   * `std::monostate` binds NULL; the other BindValue kinds bind as BIGINT, DOUBLE, BOOLEAN,
   * VARCHAR, and BLOB.
   */
  void BindFragment(idx_t start_index, const SqlFragment& fragment);

  /// Remove every bound value.
  void ClearBindings();

  /**
   * @brief Run the statement with the bound values.
   * @return The result, valid until the next Execute or the destruction of the statement.
   * @throws std::runtime_error with DuckDB's message when the statement fails.
   */
  auto Execute() -> Result&;

 private:
  void                      BindInt8(idx_t index, std::int8_t value);
  void                      BindInt16(idx_t index, std::int16_t value);
  void                      BindInt32(idx_t index, std::int32_t value);
  void                      BindInt64(idx_t index, std::int64_t value);
  void                      BindUInt8(idx_t index, std::uint8_t value);
  void                      BindUInt16(idx_t index, std::uint16_t value);
  void                      BindUInt32(idx_t index, std::uint32_t value);
  void                      BindUInt64(idx_t index, std::uint64_t value);
  void                      BindVariant(idx_t index, const BindValue& value);
  void                      CheckBind(duckdb_state state, idx_t index, const char* type) const;

  std::string               sql_;
  duckdb_prepared_statement statement_ = nullptr;
  Result                    result_{sql_};
};

/// Type of the member that @p Member points to, and the class that holds it.
template <typename T>
struct MemberPointerTraits;

template <typename Class, typename Member>
struct MemberPointerTraits<Member Class::*> {
  using ClassType  = Class;
  using MemberType = Member;
};

/**
 * @brief The result columns of a row struct, in SELECT order, as pointers to its members.
 *
 * A member may belong to a public base class of the row struct, so several row structs can
 * share leading columns (see AppendRowColumns).
 *
 * @code
 * struct LabelBucketRow {
 *   std::string label_key;
 *   int64_t     count = 0;
 *   using Columns = duckorm::RowColumns<&LabelBucketRow::label_key, &LabelBucketRow::count>;
 * };
 * @endcode
 */
template <auto... Members>
struct RowColumns {
  static constexpr std::size_t kCount = sizeof...(Members);
  static_assert(kCount > 0, "a row struct lists at least one column");
  static_assert((CellValue<typename MemberPointerTraits<decltype(Members)>::MemberType> && ...),
                "every row member type is a CellValue");
};

namespace detail {
template <typename Columns, auto... More>
struct AppendRowColumns;

template <auto... Members, auto... More>
struct AppendRowColumns<RowColumns<Members...>, More...> {
  using Type = RowColumns<Members..., More...>;
};
}  // namespace detail

/**
 * @brief The columns of @p Columns followed by @p More.
 *
 * @code
 * struct PageRow : SearchRow {
 *   int64_t total = 0;
 *   using Columns = duckorm::AppendRowColumns<SearchRow::Columns, &PageRow::total>;
 * };
 * @endcode
 */
template <typename Columns, auto... More>
using AppendRowColumns = typename detail::AppendRowColumns<Columns, More...>::Type;

/// A default-constructible struct whose `Columns` is a RowColumns of its own members or of the
/// members of its base classes.
template <typename Row>
concept TypedRow = std::default_initializable<Row> && requires { Row::Columns::kCount; };

namespace detail {
template <typename Row, auto... Members>
void RequireRowColumns(const Result& result, RowColumns<Members...> /*columns*/) {
  static_assert(
      (std::is_base_of_v<typename MemberPointerTraits<decltype(Members)>::ClassType, Row> && ...),
      "RowColumns lists members of the row struct or of its base classes");
  result.RequireColumnCount(sizeof...(Members));
  idx_t column = 0;
  (result.RequireColumnType<typename MemberPointerTraits<decltype(Members)>::MemberType>(column++),
   ...);
}

template <typename Row, auto... Members>
auto DecodeRow(const Result& result, idx_t row, RowColumns<Members...> /*columns*/) -> Row {
  Row   decoded{};
  idx_t column = 0;
  ((decoded.*Members =
        result.ReadChecked<typename MemberPointerTraits<decltype(Members)>::MemberType>(column++,
                                                                                        row)),
   ...);
  return decoded;
}
}  // namespace detail

/**
 * @brief Check that the columns of @p result decode into @p Row: the column count once, then
 *        each column type.
 * @throws std::runtime_error that names the counts, or the column index, its DuckDB type, and
 *         the member type.
 */
template <TypedRow Row>
void RequireRowColumns(const Result& result) {
  detail::RequireRowColumns<Row>(result, typename Row::Columns{});
}

/// Decode row @p row of a result that passed RequireRowColumns<Row>.
template <TypedRow Row>
[[nodiscard]] auto DecodeRow(const Result& result, idx_t row) -> Row {
  return detail::DecodeRow<Row>(result, row, typename Row::Columns{});
}

/// Every row of @p result, after one RequireRowColumns<Row> check.
template <TypedRow Row>
[[nodiscard]] auto DecodeRows(const Result& result) -> std::vector<Row> {
  RequireRowColumns<Row>(result);
  const idx_t      row_count = result.RowCount();
  std::vector<Row> rows;
  rows.reserve(static_cast<std::size_t>(row_count));
  for (idx_t row = 0; row < row_count; ++row) {
    rows.push_back(DecodeRow<Row>(result, row));
  }
  return rows;
}

}  // namespace duckorm
