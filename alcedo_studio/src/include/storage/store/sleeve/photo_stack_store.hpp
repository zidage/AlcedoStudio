//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "sleeve/photo_stack.hpp"
#include "storage/store/database.hpp"

namespace alcedo {

/**
 * @brief Sole owner of persisted photo-stack membership, order, and saved cover.
 *
 * Each call opens a connection, takes the database lock, and uses one transaction.
 * Complete input and affected-stack validation precedes writes. Writes commit together,
 * including source-stack repair. Invalid input throws std::invalid_argument; damaged rows
 * or SQL failures throw std::runtime_error with the diagnostic. No failed write changes rows.
 * The Database must outlive this store. Results belong to callers; no stack state is cached.
 */
class PhotoStackStore {
 public:
  explicit PhotoStackStore(Database& database) : database_(database) {}
  /// Create from first-occurrence ordered IDs (at least two live Files). A complete existing
  /// stack is a no-op. Otherwise reassign only these Files and repair their source stacks.
  auto CreateStack(const std::string& stack_id, std::span<const sl_element_id_t> file_ids,
                   sl_element_id_t cover_file_id) -> PhotoStackChange;
  /// Append only new members in input order, retaining the target cover and existing order.
  auto AddMembers(const std::string& stack_id, std::span<const sl_element_id_t> file_ids)
      -> PhotoStackChange;
  /// Remove only explicit memberships. Promote a removed cover by saved order; dissolve <2.
  auto RemoveMembers(std::span<const sl_element_id_t> file_ids) -> PhotoStackChange;
  /// Remove an entire stack's organization, preserving all photos and album links.
  auto UngroupStack(const std::string& stack_id) -> PhotoStackChange;
  /// Persist a member as cover; unchanged cover returns a no-op. Reject nonmembers.
  auto SetCover(const std::string& stack_id, sl_element_id_t file_id) -> PhotoStackChange;
  /// Swap a member with its saved neighbor. An endpoint or unstacked File is a no-op.
  auto MoveMember(sl_element_id_t file_id, PhotoStackMove direction) -> PhotoStackChange;
  /// Read validated, saved member order; unknown stacks are errors, never empty successes.
  auto ReadMemberIds(const std::string& stack_id) -> std::vector<sl_element_id_t>;
  /// Remove memberships before File deletion on the caller's locked transaction connection.
  /// Validate affected stacks, then repair covers or dissolve them. Opens no connection or
  /// transaction; the caller must retain live File bindings until this method returns.
  static void RemoveFileMemberships(duckdb_connection&               connection,
                                    std::span<const sl_element_id_t> file_ids);

 private:
  Database& database_;
};

}  // namespace alcedo
