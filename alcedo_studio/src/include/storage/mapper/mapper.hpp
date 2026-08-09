//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"

namespace alcedo {

/**
 * @brief CRTP base for one-table persistence: row schema facts, domain conversion, and CRUD.
 *
 * @tparam Derived Concrete mapper that supplies table name, field descriptors, FromRawData,
 *                 and domain conversion via ToParams / FromParams when domain methods are used.
 * @tparam Domain Domain object type persisted by this mapper (may equal RowParams for pure-row
 *                mappers).
 * @tparam RowParams POD-like row parameter type used with duckorm field descriptors.
 * @tparam Id Primary-key type used by RemoveById / Update paths.
 *
 * Preconditions: `conn` must outlive this mapper. Derived must expose TableName, FieldDesc,
 * FieldCount, PrimeKeyClause (via FieldReflectable) and FromRawData.
 *
 * Side effects: methods run SQL on the bound DuckDB connection. Batch insert/update open a
 * transaction when the batch is non-empty.
 *
 * Failure: duckorm throws or returns errors as today; invalid primary-key clause throws.
 *
 * Thread affinity: not thread-safe; callers serialize access (typically via ConnectionGuard).
 */
template <typename Derived, typename Domain, typename RowParams, typename Id>
class Mapper {
 public:
  duckdb_connection& conn_;

  explicit Mapper(duckdb_connection& conn) : conn_(conn) {}

  // ---------------------------------------------------------------------------
  // RowParams CRUD (single-table; no domain conversion)
  // ---------------------------------------------------------------------------

  /**
   * @brief Insert one row from already-built row parameters.
   */
  void InsertParams(const RowParams& obj) {
    duckorm::insert(conn_, Derived::TableName(), &obj, Derived::FieldDesc(), Derived::FieldCount());
  }

  void InsertParamsBatch(std::span<const RowParams> objects) {
    if (objects.empty()) {
      return;
    }
    duckorm::begin_transaction(conn_);
    try {
      for (const auto& obj : objects) {
        duckorm::insert(conn_, Derived::TableName(), &obj, Derived::FieldDesc(),
                        Derived::FieldCount());
      }
      duckorm::commit_transaction(conn_);
    } catch (...) {
      duckorm::rollback_transaction(conn_);
      throw;
    }
  }

  /**
   * @brief Remove a row by primary key.
   */
  void RemoveById(const Id remove_id) {
    std::string remove_clause = std::format(Derived::PrimeKeyClause(), remove_id);
    duckorm::remove(conn_, Derived::TableName(), remove_clause.c_str());
  }

  void RemoveByIds(std::span<const Id> remove_ids) {
    if (remove_ids.empty()) {
      return;
    }

    std::ostringstream           id_list;
    std::unordered_set<uint64_t> seen;
    const std::string            key_clause = Derived::PrimeKeyClause();
    const auto                   equals_pos = key_clause.find('=');
    if (equals_pos == std::string::npos || equals_pos == 0) {
      throw std::runtime_error("Mapper: invalid primary key clause");
    }
    const std::string key_column = key_clause.substr(0, equals_pos);

    bool first = true;
    for (const auto id : remove_ids) {
      const auto normalized_id = static_cast<uint64_t>(id);
      if (!seen.insert(normalized_id).second) {
        continue;
      }
      if (!first) {
        id_list << ",";
      }
      id_list << normalized_id;
      first = false;
    }

    if (first) {
      return;
    }

    duckorm::remove(conn_, Derived::TableName(),
                    std::format("{} IN ({})", key_column, id_list.str()).c_str());
  }

  /**
   * @brief Remove rows matching a string WHERE predicate (no table prefix).
   */
  void RemoveByClause(const std::string& predicate) {
    duckorm::remove(conn_, Derived::TableName(), predicate.c_str());
  }

  /**
   * @brief Remove rows matching a SqlFragment WHERE predicate (optional binds).
   */
  void RemoveByClause(const duckorm::SqlFragment& predicate) {
    duckorm::remove(conn_, Derived::TableName(), predicate);
  }

  /**
   * @brief Select rows as RowParams for a string WHERE clause.
   */
  auto GetParams(const char* where_clause) -> std::vector<RowParams> {
    auto                  raw = duckorm::select(conn_, Derived::TableName(), Derived::FieldDesc(),
                                                Derived::FieldCount(), where_clause);
    std::vector<RowParams> result;
    result.reserve(raw.size());
    for (auto& row : raw) {
      result.emplace_back(Derived::FromRawData(std::move(row)));
    }
    return result;
  }

  /**
   * @brief Select rows as RowParams for a SqlFragment WHERE clause.
   */
  auto GetParams(const duckorm::SqlFragment& where_clause) -> std::vector<RowParams> {
    auto                  raw = duckorm::select(conn_, Derived::TableName(), Derived::FieldDesc(),
                                                Derived::FieldCount(), where_clause);
    std::vector<RowParams> result;
    result.reserve(raw.size());
    for (auto& row : raw) {
      result.emplace_back(Derived::FromRawData(std::move(row)));
    }
    return result;
  }

  auto GetParamsByQuery(const char* query) -> std::vector<RowParams> {
    auto raw = duckorm::select_by_query(conn_, Derived::FieldDesc(), Derived::FieldCount(), query);
    std::vector<RowParams> result;
    result.reserve(raw.size());
    for (auto& row : raw) {
      result.emplace_back(Derived::FromRawData(std::move(row)));
    }
    return result;
  }

  void UpdateParams(const Id target_id, const RowParams& updated) {
    std::string where_clause = std::format(Derived::PrimeKeyClause(), target_id);
    duckorm::update(conn_, Derived::TableName(), &updated, Derived::FieldDesc(),
                    Derived::FieldCount(), where_clause.c_str());
  }

  void UpdateParamsBatch(std::span<const std::pair<Id, RowParams>> updates) {
    if (updates.empty()) {
      return;
    }
    duckorm::begin_transaction(conn_);
    try {
      for (const auto& [target_id, updated] : updates) {
        std::string where_clause = std::format(Derived::PrimeKeyClause(), target_id);
        duckorm::update(conn_, Derived::TableName(), &updated, Derived::FieldDesc(),
                        Derived::FieldCount(), where_clause.c_str());
      }
      duckorm::commit_transaction(conn_);
    } catch (...) {
      duckorm::rollback_transaction(conn_);
      throw;
    }
  }

  // ---------------------------------------------------------------------------
  // Domain CRUD (uses Derived::ToParams / FromParams)
  // ---------------------------------------------------------------------------

  /**
   * @brief Insert one domain object.
   *
   * When Domain is RowParams, inserts the row directly. Otherwise converts with
   * Derived::ToParams.
   */
  void Insert(const Domain& obj) {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      InsertParams(obj);
    } else {
      InsertParams(Derived::ToParams(obj));
    }
  }

  void InsertBatch(std::span<const Domain> objects) {
    if (objects.empty()) {
      return;
    }
    if constexpr (std::is_same_v<Domain, RowParams>) {
      InsertParamsBatch(objects);
    } else {
      std::vector<RowParams> params;
      params.reserve(objects.size());
      for (const auto& obj : objects) {
        params.push_back(Derived::ToParams(obj));
      }
      InsertParamsBatch(params);
    }
  }

  /**
   * @brief Load domain objects for a string WHERE predicate.
   */
  auto GetByPredicate(std::string&& predicate) -> std::vector<Domain> {
    std::vector<RowParams> param_results = GetParams(predicate.c_str());
    return ParamsToDomain(std::move(param_results));
  }

  /**
   * @brief Load domain objects for a SqlFragment WHERE predicate.
   */
  auto GetByPredicate(const duckorm::SqlFragment& predicate) -> std::vector<Domain> {
    std::vector<RowParams> param_results = GetParams(predicate);
    return ParamsToDomain(std::move(param_results));
  }

  /**
   * @brief Load domain objects from a full SQL query (projection must match FieldDesc).
   */
  auto GetByQuery(std::string&& query) -> std::vector<Domain> {
    std::vector<RowParams> param_results = GetParamsByQuery(query.c_str());
    return ParamsToDomain(std::move(param_results));
  }

  void Update(const Domain& obj, const Id update_id) {
    if constexpr (std::is_same_v<Domain, RowParams>) {
      UpdateParams(update_id, obj);
    } else {
      UpdateParams(update_id, Derived::ToParams(obj));
    }
  }

  void UpdateBatch(std::span<const std::pair<Id, Domain>> updates) {
    if (updates.empty()) {
      return;
    }
    if constexpr (std::is_same_v<Domain, RowParams>) {
      UpdateParamsBatch(updates);
    } else {
      std::vector<std::pair<Id, RowParams>> params;
      params.reserve(updates.size());
      for (const auto& [id, obj] : updates) {
        params.emplace_back(id, Derived::ToParams(obj));
      }
      UpdateParamsBatch(params);
    }
  }

 private:
  static auto ParamsToDomain(std::vector<RowParams>&& param_results) -> std::vector<Domain> {
    std::vector<Domain> results;
    results.reserve(param_results.size());
    if constexpr (std::is_same_v<Domain, RowParams>) {
      for (auto& param : param_results) {
        results.push_back(std::move(param));
      }
    } else {
      for (auto& param : param_results) {
        results.push_back(Derived::FromParams(std::move(param)));
      }
    }
    return results;
  }

 public:

  // ---------------------------------------------------------------------------
  // Compatibility aliases for pure-row call sites (historical MapperInterface names)
  // ---------------------------------------------------------------------------

  /**
   * @brief Alias for GetParams. Prefer GetParams in new code.
   */
  auto Get(const char* where_clause) -> std::vector<RowParams> { return GetParams(where_clause); }

  /**
   * @brief Alias for GetParamsByQuery. Prefer GetParamsByQuery in new code.
   */
  auto GetByQuery(const char* query) -> std::vector<RowParams> {
    return GetParamsByQuery(query);
  }

  /**
   * @brief Alias for RemoveById. Prefer RemoveById in new code.
   */
  void Remove(const Id remove_id) { RemoveById(remove_id); }

  /**
   * @brief Alias for UpdateParams. Prefer UpdateParams in new code.
   *
   * Distinct parameter order from Update(const Domain&, const Id).
   */
  void Update(const Id target_id, const RowParams& updated) { UpdateParams(target_id, updated); }
};

/**
 * @brief Reflect table name, primary-key clause, and field descriptors from a concrete Mapper.
 */
template <typename Derived>
struct FieldReflectable {
 public:
  using FieldArrayType = std::span<const duckorm::DuckFieldDesc>;
  static constexpr FieldArrayType FieldDesc() { return Derived::field_descs_; }
  static constexpr uint32_t       FieldCount() { return Derived::field_count_; }
  static constexpr const char*    TableName() { return Derived::table_name_; }
  static constexpr const char*    PrimeKeyClause() { return Derived::prime_key_clause_; }
};

// Temporary compatibility alias during the Phase 3 rename. Prefer Mapper.
template <typename Derived, typename RowParams, typename Id>
using MapperInterface = Mapper<Derived, RowParams, RowParams, Id>;

}  // namespace alcedo
