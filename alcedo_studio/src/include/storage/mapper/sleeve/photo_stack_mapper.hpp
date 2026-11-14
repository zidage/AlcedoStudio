//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <cstdint>
#include <span>
#include <string>

#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Typed, single-table writes to PhotoStack. Owns no transaction or connection.
 * The caller holds the connection lock and transaction for the mapper's lifetime.
 * Each failed statement throws the actual DuckDB diagnostic.
 */
class PhotoStackMapper {
 public:
  explicit PhotoStackMapper(duckdb_connection& connection) : connection_(connection) {}
  /// Insert a new stack header; the caller must also insert its members before commit.
  void Insert(const std::string& stack_id, sl_element_id_t cover_file_id);
  /// Change only the saved cover. Membership validation belongs to the store.
  void SetCover(const std::string& stack_id, sl_element_id_t cover_file_id);
  /// Delete a stack header; the caller must also remove its memberships before commit.
  void Remove(const std::string& stack_id);

 private:
  duckdb_connection& connection_;
};

/// Transaction-neutral PhotoStackMember writes, with the same lifetime and error rules above.
class PhotoStackMemberMapper {
 public:
  explicit PhotoStackMemberMapper(duckdb_connection& connection) : connection_(connection) {}
  /// Insert one membership with a nonnegative, unique saved position.
  void Insert(sl_element_id_t file_id, const std::string& stack_id, std::int64_t member_order);
  /// Remove exactly these memberships; never remove Files or album links.
  void RemoveFiles(std::span<const sl_element_id_t> file_ids);
  /// Remove every membership of one stack.
  void RemoveStack(const std::string& stack_id);

 private:
  duckdb_connection& connection_;
};

}  // namespace alcedo
