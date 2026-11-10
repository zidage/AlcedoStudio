//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"

namespace alcedo {

/**
 * @brief Key types a Mapper binds as one prepared-statement value.
 *
 * An integer key binds as BIGINT, so it must fit in `int64_t`; a text key binds as VARCHAR.
 */
template <typename Id>
concept MapperKey = (std::integral<Id> && !std::same_as<Id, bool> &&
                     (std::is_signed_v<Id> || sizeof(Id) < sizeof(std::int64_t))) ||
                    std::same_as<Id, std::string>;

/**
 * @brief CRTP base for one-table persistence: row schema facts, domain conversion, and CRUD.
 *
 * @tparam Derived Concrete mapper that supplies the table name, the primary-key column, field
 *                 descriptors, FromRawData, and ToParams / FromParams when domain methods are
 *                 used.
 * @tparam Domain Domain object type persisted by this mapper (may equal RowParams for pure-row
 *                mappers).
 * @tparam RowParams Row type laid out as the duckorm field descriptors describe.
 * @tparam Id Type of the column that PrimaryKeyColumn names (see MapperKey).
 *
 * Every key and every WHERE value is a bound value: key methods build
 * `(PrimaryKeyColumn() = ?)`, and WHERE input is a duckorm::SqlFragment.
 *
 * Transactions belong to the caller. No method opens, commits, or rolls back a transaction; a
 * caller that needs several writes to commit together opens a duckorm::Transaction around them.
 *
 * Preconditions: `conn` must outlive this mapper.
 * Failure: each SQL failure throws std::runtime_error with the DuckDB message (duckorm).
 * Thread affinity: not thread-safe; callers serialize access (typically via ConnectionGuard).
 */
template <typename Derived, typename Domain, typename RowParams, MapperKey Id>
class Mapper {
 public:
  duckdb_connection& conn_;

  explicit Mapper(duckdb_connection& conn) : conn_(conn) {}

  /**
   * @brief `(PrimaryKeyColumn() = ?)` with @p id bound.
   */
  static auto KeyEquals(const Id& id) -> duckorm::SqlFragment {
    if constexpr (std::integral<Id>) {
      return duckorm::expr::column_eq(Derived::PrimaryKeyColumn(), static_cast<std::int64_t>(id));
    } else {
      return duckorm::expr::column_eq(Derived::PrimaryKeyColumn(), id);
    }
  }

  // ---------------------------------------------------------------------------
  // RowParams CRUD (single-table; no domain conversion)
  // ---------------------------------------------------------------------------

  /// Insert one row.
  void InsertParams(const RowParams& obj) {
    duckorm::insert(conn_, Derived::TableName(), &obj, Derived::FieldDesc(), Derived::FieldCount());
  }

  /// Insert @p rows with multi-row statements inside the caller's transaction. Without a caller
  /// transaction each statement of up to duckorm::kMultiRowStatementRows rows commits alone.
  void InsertParamsRows(std::span<const RowParams> rows) {
    std::vector<const void*> pointers;
    for (size_t begin = 0; begin < rows.size(); begin += duckorm::kMultiRowStatementRows) {
      const size_t end = std::min(rows.size(), begin + duckorm::kMultiRowStatementRows);
      pointers.clear();
      for (size_t index = begin; index < end; ++index) {
        pointers.push_back(&rows[index]);
      }
      duckorm::insert_rows(conn_, Derived::TableName(), pointers, Derived::FieldDesc(),
                           Derived::FieldCount());
    }
  }

  /// Upsert @p rows with multi-row statements inside the caller's transaction. The rows must
  /// have distinct primary keys.
  void UpsertParamsRows(std::span<const RowParams> rows) {
    std::vector<const void*> pointers;
    for (size_t begin = 0; begin < rows.size(); begin += duckorm::kMultiRowStatementRows) {
      const size_t end = std::min(rows.size(), begin + duckorm::kMultiRowStatementRows);
      pointers.clear();
      for (size_t index = begin; index < end; ++index) {
        pointers.push_back(&rows[index]);
      }
      duckorm::upsert_rows(conn_, Derived::TableName(), pointers, Derived::FieldDesc(),
                           Derived::FieldCount());
    }
  }

  /**
   * @brief Write @p updated as the row of @p target_id.
   *
   * Runs `INSERT ... ON CONFLICT DO UPDATE ... WHERE (key = ?)`: a new key inserts the row, and
   * a stored row is replaced when its key equals @p target_id. The table needs a PRIMARY KEY or
   * UNIQUE constraint.
   */
  void UpdateParams(const Id& target_id, const RowParams& updated) {
    duckorm::update(conn_, Derived::TableName(), &updated, Derived::FieldDesc(),
                    Derived::FieldCount(), KeyEquals(target_id));
  }

  /// UpdateParams for each pair, inside the caller's transaction.
  void UpdateParamsRows(std::span<const std::pair<Id, RowParams>> updates) {
    for (const auto& [target_id, updated] : updates) {
      UpdateParams(target_id, updated);
    }
  }

  /**
   * @brief The row whose primary key equals @p id.
   *
   * @return std::nullopt when no row has the key.
   * @throws std::runtime_error when more than one row has the key (the table has no key
   *         constraint and holds duplicates).
   */
  auto GetById(const Id& id) -> std::optional<RowParams> {
    auto rows = GetWhere(KeyEquals(id));
    if (rows.size() > 1) {
      throw std::runtime_error(std::string("Mapper: ") + Derived::TableName() + " has " +
                               std::to_string(rows.size()) + " rows for one " +
                               Derived::PrimaryKeyColumn() + " value");
    }
    if (rows.empty()) {
      return std::nullopt;
    }
    return std::move(rows.front());
  }

  /// Rows that match @p where (`SELECT * FROM table WHERE ...`, binds applied in order).
  auto GetWhere(const duckorm::SqlFragment& where) -> std::vector<RowParams> {
    return RowsFromRaw(duckorm::select(conn_, Derived::TableName(), Derived::FieldDesc(),
                                       Derived::FieldCount(), where));
  }

  /// Rows of a full SELECT whose projection matches the field descriptors in order.
  auto GetRowsByQuery(const duckorm::SqlFragment& query) -> std::vector<RowParams> {
    return RowsFromRaw(
        duckorm::select_by_query(conn_, Derived::FieldDesc(), Derived::FieldCount(), query));
  }

  /// Delete the row whose primary key equals @p remove_id.
  void RemoveById(const Id& remove_id) {
    duckorm::remove(conn_, Derived::TableName(), KeyEquals(remove_id));
  }

  /// Delete the rows whose primary key is in @p remove_ids, in one statement.
  void RemoveByIds(std::span<const Id> remove_ids)
    requires std::integral<Id>
  {
    if (remove_ids.empty()) {
      return;
    }
    duckorm::remove(
        conn_, Derived::TableName(),
        duckorm::expr::in_list(duckorm::expr::col(Derived::PrimaryKeyColumn()), remove_ids));
  }

  /// Delete the rows that match @p where.
  void RemoveWhere(const duckorm::SqlFragment& where) {
    duckorm::remove(conn_, Derived::TableName(), where);
  }

  // ---------------------------------------------------------------------------
  // Domain CRUD (uses Derived::ToParams / FromParams)
  // ---------------------------------------------------------------------------

  /// Insert one domain object (converted with Derived::ToParams unless Domain is RowParams).
  void Insert(const Domain& obj) {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      InsertParams(obj);
    } else {
      InsertParams(Derived::ToParams(obj));
    }
  }

  /// Insert @p objects with multi-row statements inside the caller's transaction.
  void InsertBatch(std::span<const Domain> objects) {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      InsertParamsRows(objects);
    } else {
      std::vector<RowParams> params;
      params.reserve(objects.size());
      for (const auto& obj : objects) {
        params.push_back(Derived::ToParams(obj));
      }
      InsertParamsRows(params);
    }
  }

  /// Domain objects of the rows that match @p predicate.
  auto GetByPredicate(const duckorm::SqlFragment& predicate) -> std::vector<Domain> {
    return ParamsToDomain(GetWhere(predicate));
  }

  /// Domain objects of a full SELECT whose projection matches the field descriptors.
  auto GetByQuery(const duckorm::SqlFragment& query) -> std::vector<Domain> {
    return ParamsToDomain(GetRowsByQuery(query));
  }

  /// UpdateParams with the row of @p obj.
  void Update(const Domain& obj, const Id& update_id) {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      UpdateParams(update_id, obj);
    } else {
      UpdateParams(update_id, Derived::ToParams(obj));
    }
  }

  /// Update for each pair, inside the caller's transaction.
  void UpdateBatch(std::span<const std::pair<Id, Domain>> updates) {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      UpdateParamsRows(updates);
    } else {
      for (const auto& [id, obj] : updates) {
        UpdateParams(id, Derived::ToParams(obj));
      }
    }
  }

 private:
  static auto RowsFromRaw(std::vector<std::vector<duckorm::VarTypes>>&& raw)
      -> std::vector<RowParams> {
    std::vector<RowParams> result;
    result.reserve(raw.size());
    for (auto& row : raw) {
      result.emplace_back(Derived::FromRawData(std::move(row)));
    }
    return result;
  }

  static auto ParamsToDomain(std::vector<RowParams>&& param_results) -> std::vector<Domain> {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      return std::move(param_results);
    } else {
      std::vector<Domain> results;
      results.reserve(param_results.size());
      for (auto& param : param_results) {
        results.push_back(Derived::FromParams(std::move(param)));
      }
      return results;
    }
  }
};

/**
 * @brief Reflect the table name, the primary-key column, and the field descriptors of a
 *        concrete Mapper.
 *
 * Derived declares `table_name_`, `primary_key_column_`, `field_count_`, and `field_descs_`.
 */
template <typename Derived>
struct FieldReflectable {
 public:
  using FieldArrayType = std::span<const duckorm::DuckFieldDesc>;
  static constexpr FieldArrayType FieldDesc() { return Derived::field_descs_; }
  static constexpr uint32_t       FieldCount() { return Derived::field_count_; }
  static constexpr const char*    TableName() { return Derived::table_name_; }
  static constexpr const char*    PrimaryKeyColumn() { return Derived::primary_key_column_; }
};

// Temporary compatibility alias during the Phase 3 rename. Prefer Mapper.
template <typename Derived, typename RowParams, typename Id>
using MapperInterface = Mapper<Derived, RowParams, RowParams, Id>;

}  // namespace alcedo
