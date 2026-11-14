//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/photo_stack_mapper.hpp"

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"

namespace alcedo {

void PhotoStackMapper::Insert(const std::string& stack_id, sl_element_id_t cover_file_id) {
  duckorm::Statement statement(connection_,
                               "INSERT INTO PhotoStack (stack_id, cover_file_id) VALUES (?, ?)");
  statement.Bind(1, stack_id);
  statement.Bind(2, static_cast<std::int64_t>(cover_file_id));
  statement.Execute();
}

void PhotoStackMapper::SetCover(const std::string& stack_id, sl_element_id_t cover_file_id) {
  duckorm::Statement statement(connection_,
                               "UPDATE PhotoStack SET cover_file_id = ? WHERE stack_id = ?");
  statement.Bind(1, static_cast<std::int64_t>(cover_file_id));
  statement.Bind(2, stack_id);
  statement.Execute();
}

void PhotoStackMapper::Remove(const std::string& stack_id) {
  duckorm::remove(connection_, "PhotoStack", duckorm::expr::column_eq("stack_id", stack_id));
}

void PhotoStackMemberMapper::Insert(sl_element_id_t file_id, const std::string& stack_id,
                                    std::int64_t member_order) {
  duckorm::Statement statement(
      connection_,
      "INSERT INTO PhotoStackMember (file_id, stack_id, member_order) VALUES (?, ?, ?)");
  statement.Bind(1, static_cast<std::int64_t>(file_id));
  statement.Bind(2, stack_id);
  statement.Bind(3, member_order);
  statement.Execute();
}

void PhotoStackMemberMapper::RemoveFiles(std::span<const sl_element_id_t> file_ids) {
  if (!file_ids.empty()) {
    duckorm::remove(connection_, "PhotoStackMember",
                    duckorm::expr::in_list(duckorm::expr::col("file_id"), file_ids));
  }
}

void PhotoStackMemberMapper::RemoveStack(const std::string& stack_id) {
  duckorm::remove(connection_, "PhotoStackMember", duckorm::expr::column_eq("stack_id", stack_id));
}

}  // namespace alcedo
