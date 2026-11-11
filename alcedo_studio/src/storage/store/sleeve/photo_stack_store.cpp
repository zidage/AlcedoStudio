//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/photo_stack_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"
#include "storage/mapper/sleeve/photo_stack_mapper.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;

/// Minimal query outputs, scoped to the operation; never written back as stored stack state.
struct FileIdRow {
  std::int64_t file_id;
  using Columns = duckorm::RowColumns<&FileIdRow::file_id>;
};
struct StackIdRow {
  std::string stack_id;
  using Columns = duckorm::RowColumns<&StackIdRow::stack_id>;
};

/// Reject noncanonical UUID text without introducing a Qt dependency in storage.
void ValidateStackId(const std::string& id) {
  if (id.size() != 36) {
    throw std::invalid_argument("Photo stack ID must be a canonical UUID");
  }
  for (std::size_t i = 0; i < id.size(); ++i) {
    const bool valid = (i == 8 || i == 13 || i == 18 || i == 23)
                           ? id[i] == '-'
                           : ((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'));
    if (!valid) {
      throw std::invalid_argument("Photo stack ID must be a canonical UUID");
    }
  }
}

/// Preserve first occurrence of explicit targets, without copying an owner-held object.
auto UniqueFiles(std::span<const sl_element_id_t> ids) -> std::vector<sl_element_id_t> {
  std::unordered_set<sl_element_id_t> seen;
  std::vector<sl_element_id_t>        result;
  for (const auto id : ids) {
    if (seen.insert(id).second) {
      result.push_back(id);
    }
  }
  return result;
}

/// Check persisted BIGINT IDs before conversion to the application's File ID type.
auto FileId(std::int64_t id) -> sl_element_id_t {
  if (id < 0 || static_cast<std::uint64_t>(id) > std::numeric_limits<sl_element_id_t>::max()) {
    throw std::runtime_error("Photo stack contains a File ID outside the supported range");
  }
  return static_cast<sl_element_id_t>(id);
}

/// Read ordered ID output only; the caller validates the header and relations first.
auto MemberIds(duckdb_connection conn, const std::string& id) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> result;
  for (const auto& row : duckorm::query<FileIdRow>(
           conn, {"SELECT file_id FROM PhotoStackMember WHERE stack_id = ? ORDER BY member_order",
                  {id}})) {
    result.push_back(FileId(row.file_id));
  }
  return result;
}

/// Require every target to be a live File with exactly one binding to an existing Image.
void ValidateFiles(duckdb_connection conn, std::span<const sl_element_id_t> ids) {
  if (ids.empty()) {
    return;
  }
  auto query = expr::raw(
      "SELECT e.id FROM Element e JOIN FileImage fi ON fi.file_id = e.id "
      "LEFT JOIN Image i ON i.id = fi.image_id WHERE ");
  query.append(expr::in_list(expr::col("e.id"), ids));
  query.append(expr::raw(" AND e.type = "));
  query.append(expr::param(static_cast<std::int64_t>(ElementType::FILE)));
  query.append(expr::raw(" GROUP BY e.id HAVING COUNT(*) = 1 AND COUNT(i.id) = 1"));
  if (duckorm::query<FileIdRow>(conn, query).size() != ids.size()) {
    throw std::invalid_argument("Photo stack target is missing or has no valid File image binding");
  }
}

/// Check the same persisted invariants for one stack or every header, with one grouped query.
/// The caller holds the connection lock and transaction; this read never repairs stored rows.
void ValidateRelations(duckdb_connection conn, const std::string* stack_id = nullptr) {
  auto query = expr::raw(
      "SELECT s.stack_id FROM PhotoStack s LEFT JOIN PhotoStackMember m "
      "ON m.stack_id = s.stack_id");
  if (stack_id) {
    query.append(expr::raw(" WHERE s.stack_id = "));
    query.append(expr::param(*stack_id));
  }
  query.append(
      expr::raw(" GROUP BY s.stack_id, s.cover_file_id HAVING COUNT(m.file_id) < 2 "
                "OR COUNT(DISTINCT m.member_order) <> COUNT(m.file_id) OR MIN(m.member_order) < 0 "
                "OR COUNT(*) FILTER (WHERE m.file_id = s.cover_file_id) <> 1 "
                "OR COUNT(*) FILTER (WHERE m.file_id < 0 OR m.file_id > "));
  query.append(expr::param(static_cast<std::int64_t>(std::numeric_limits<sl_element_id_t>::max())));
  query.append(
      expr::raw(") > 0 OR COUNT(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM Element e "
                "JOIN FileImage fi ON fi.file_id = e.id LEFT JOIN Image i ON i.id = fi.image_id "
                "WHERE e.id = m.file_id AND e.type = "));
  query.append(expr::param(static_cast<std::int64_t>(ElementType::FILE)));
  query.append(
      expr::raw(" GROUP BY e.id HAVING COUNT(*) = 1 AND COUNT(i.id) = 1)) > 0 "
                "ORDER BY s.stack_id LIMIT 1"));
  const auto invalid = duckorm::query<StackIdRow>(conn, query);
  if (!invalid.empty()) {
    throw std::runtime_error("Invalid persisted photo stack relation: " + invalid.front().stack_id);
  }
}

/// Validate the complete affected relation on this connection; return only its saved cover ID.
auto ValidateStack(duckdb_connection conn, const std::string& id) -> sl_element_id_t {
  ValidateStackId(id);
  const auto cover = duckorm::select_int64(
      conn, {"SELECT cover_file_id FROM PhotoStack WHERE stack_id = ?", {id}});
  if (!cover) {
    throw std::invalid_argument("Unknown photo stack: " + id);
  }
  ValidateRelations(conn, &id);
  return FileId(*cover);
}

/// Find and validate the source stacks of explicit Files before any mutation.
auto SourceStacks(duckdb_connection conn, std::span<const sl_element_id_t> ids)
    -> std::vector<std::string> {
  std::vector<std::string> result;
  if (!ids.empty()) {
    auto query = expr::raw("SELECT DISTINCT stack_id FROM PhotoStackMember WHERE ");
    query.append(expr::in_list(expr::col("file_id"), ids));
    query.append(expr::raw(" ORDER BY stack_id"));
    for (auto& row : duckorm::query<StackIdRow>(conn, query)) {
      ValidateStack(conn, row.stack_id);
      result.push_back(std::move(row.stack_id));
    }
  }
  return result;
}

/// Include each affected stack and File once, before a membership may disappear.
void RecordChange(duckdb_connection conn, const std::string& id, PhotoStackChange& change) {
  if (std::find(change.changed_stack_ids.begin(), change.changed_stack_ids.end(), id) !=
      change.changed_stack_ids.end()) {
    return;
  }
  change.changed_stack_ids.push_back(id);
  std::unordered_set<sl_element_id_t> seen(change.changed_file_ids.begin(),
                                           change.changed_file_ids.end());
  for (const auto file : MemberIds(conn, id)) {
    if (seen.insert(file).second) {
      change.changed_file_ids.push_back(file);
    }
  }
}

/// Repair a source after explicit removal, within the caller's still-uncommitted transaction.
void RepairStack(duckdb_connection& conn, const std::string& id) {
  const auto       count = duckorm::select_int64(
                               conn, {"SELECT COUNT(*) FROM PhotoStackMember WHERE stack_id = ?", {id}})
                               .value();
  PhotoStackMapper stacks(conn);
  if (count < 2) {
    PhotoStackMemberMapper(conn).RemoveStack(id);
    stacks.Remove(id);
  } else {
    const auto retained_cover =
        duckorm::select_int64(
            conn,
            {"SELECT COUNT(*) FROM PhotoStack s JOIN PhotoStackMember m "
             "ON m.stack_id = s.stack_id AND m.file_id = s.cover_file_id WHERE s.stack_id = ?",
             {id}})
            .value();
    if (retained_cover == 0) {
      stacks.SetCover(id,
                      FileId(duckorm::select_int64(
                                 conn, {"SELECT file_id FROM PhotoStackMember WHERE stack_id = ? "
                                        "ORDER BY member_order LIMIT 1",
                                        {id}})
                                 .value()));
    }
  }
}

/// Remove selected memberships and repair each already-validated source on one connection.
void Detach(duckdb_connection& conn, std::span<const sl_element_id_t> ids,
            const std::vector<std::string>& sources) {
  PhotoStackMemberMapper(conn).RemoveFiles(ids);
  for (const auto& source : sources) {
    RepairStack(conn, source);
  }
}

/// Scope connection, serialization, and transaction to one owner operation; publish after commit.
template <typename Operation>
auto InTransaction(Database& database, Operation operation) {
  auto                 guard = database.GetConnectionGuard();
  auto                 lock  = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  auto                 result = operation(guard.conn_);
  transaction.commit();
  return result;
}
}  // namespace

auto PhotoStackStore::CreateStack(const std::string&               stack_id,
                                  std::span<const sl_element_id_t> file_ids,
                                  sl_element_id_t cover_file_id) -> PhotoStackChange {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    ValidateStackId(stack_id);
    const auto ids = UniqueFiles(file_ids);
    if (ids.size() < 2 || std::find(ids.begin(), ids.end(), cover_file_id) == ids.end()) {
      throw std::invalid_argument("A photo stack requires two unique Files and a selected cover");
    }
    ValidateFiles(conn, ids);
    if (duckorm::select_int64(conn,
                              {"SELECT COUNT(*) FROM PhotoStack WHERE stack_id = ?", {stack_id}})
            .value() != 0) {
      throw std::invalid_argument("Photo stack ID already exists: " + stack_id);
    }
    const auto sources = SourceStacks(conn, ids);
    if (sources.size() == 1) {
      const auto                                current = MemberIds(conn, sources.front());
      const std::unordered_set<sl_element_id_t> current_ids(current.begin(), current.end());
      if (current.size() == ids.size() &&
          std::all_of(ids.begin(), ids.end(), [&](auto id) { return current_ids.contains(id); })) {
        return PhotoStackChange{.stack_id = sources.front()};
      }
    }
    PhotoStackChange change{.stack_id = stack_id};
    for (const auto& source : sources) {
      RecordChange(conn, source, change);
    }
    Detach(conn, ids, sources);
    PhotoStackMapper(conn).Insert(stack_id, cover_file_id);
    PhotoStackMemberMapper members(conn);
    for (std::size_t i = 0; i < ids.size(); ++i) {
      members.Insert(ids[i], stack_id, static_cast<std::int64_t>(i));
    }
    RecordChange(conn, stack_id, change);
    return change;
  });
}

auto PhotoStackStore::AddMembers(const std::string&               stack_id,
                                 std::span<const sl_element_id_t> file_ids) -> PhotoStackChange {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    ValidateStack(conn, stack_id);
    auto ids = UniqueFiles(file_ids);
    ValidateFiles(conn, ids);
    const auto                                current = MemberIds(conn, stack_id);
    const std::unordered_set<sl_element_id_t> current_ids(current.begin(), current.end());
    std::erase_if(ids, [&](auto id) { return current_ids.contains(id); });
    PhotoStackChange change{.stack_id = stack_id};
    if (ids.empty()) {
      return change;
    }
    const auto sources = SourceStacks(conn, ids);
    const auto last =
        duckorm::select_int64(
            conn, {"SELECT MAX(member_order) FROM PhotoStackMember WHERE stack_id = ?", {stack_id}})
            .value();
    if (ids.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - last)) {
      throw std::invalid_argument("Photo stack member order exceeds the supported range");
    }
    RecordChange(conn, stack_id, change);
    for (const auto& source : sources) {
      RecordChange(conn, source, change);
    }
    Detach(conn, ids, sources);
    PhotoStackMemberMapper              members(conn);
    auto                                order = last;
    std::unordered_set<sl_element_id_t> seen(change.changed_file_ids.begin(),
                                             change.changed_file_ids.end());
    for (const auto id : ids) {
      members.Insert(id, stack_id, ++order);
      if (seen.insert(id).second) {
        change.changed_file_ids.push_back(id);
      }
    }
    return change;
  });
}

auto PhotoStackStore::RemoveMembers(std::span<const sl_element_id_t> file_ids) -> PhotoStackChange {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    const auto ids = UniqueFiles(file_ids);
    ValidateFiles(conn, ids);
    const auto       sources = SourceStacks(conn, ids);
    PhotoStackChange change;
    for (const auto& source : sources) {
      RecordChange(conn, source, change);
    }
    Detach(conn, ids, sources);
    return change;
  });
}

auto PhotoStackStore::UngroupStack(const std::string& stack_id) -> PhotoStackChange {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    ValidateStack(conn, stack_id);
    PhotoStackChange change{.stack_id = stack_id};
    RecordChange(conn, stack_id, change);
    PhotoStackMemberMapper(conn).RemoveStack(stack_id);
    PhotoStackMapper(conn).Remove(stack_id);
    return change;
  });
}

auto PhotoStackStore::SetCover(const std::string& stack_id, sl_element_id_t file_id)
    -> PhotoStackChange {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    const auto cover = ValidateStack(conn, stack_id);
    const auto ids   = MemberIds(conn, stack_id);
    if (std::find(ids.begin(), ids.end(), file_id) == ids.end()) {
      throw std::invalid_argument("Photo stack cover must belong to that stack");
    }
    PhotoStackChange change{.stack_id = stack_id};
    if (cover != file_id) {
      RecordChange(conn, stack_id, change);
      PhotoStackMapper(conn).SetCover(stack_id, file_id);
    }
    return change;
  });
}

auto PhotoStackStore::MoveMember(sl_element_id_t file_id, PhotoStackMove direction)
    -> PhotoStackChange {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    if (direction != PhotoStackMove::kEarlier && direction != PhotoStackMove::kLater) {
      throw std::invalid_argument("Unknown photo stack move direction");
    }
    const std::array ids{file_id};
    ValidateFiles(conn, ids);
    const auto       sources = SourceStacks(conn, ids);
    PhotoStackChange change;
    if (sources.empty()) {
      return change;
    }
    change.stack_id    = sources.front();
    const auto ordered = MemberIds(conn, change.stack_id);
    auto       index = static_cast<std::size_t>(std::find(ordered.begin(), ordered.end(), file_id) -
                                                ordered.begin());
    if ((direction == PhotoStackMove::kEarlier && index == 0) ||
        (direction == PhotoStackMove::kLater && index + 1 == ordered.size())) {
      return change;
    }
    const auto neighbor = ordered[direction == PhotoStackMove::kEarlier ? index - 1 : index + 1];
    const auto order_of = [&](sl_element_id_t id) {
      return duckorm::select_int64(conn,
                                   {"SELECT member_order FROM PhotoStackMember WHERE file_id = ?",
                                    {static_cast<std::int64_t>(id)}})
          .value();
    };
    const auto first_order  = order_of(file_id);
    const auto second_order = order_of(neighbor);
    RecordChange(conn, change.stack_id, change);
    const std::array       pair{file_id, neighbor};
    PhotoStackMemberMapper members(conn);
    members.RemoveFiles(pair);
    members.Insert(file_id, change.stack_id, second_order);
    members.Insert(neighbor, change.stack_id, first_order);
    return change;
  });
}

auto PhotoStackStore::ReadMemberIds(const std::string& stack_id) -> std::vector<sl_element_id_t> {
  return InTransaction(database_, [&](duckdb_connection& conn) {
    ValidateStack(conn, stack_id);
    return MemberIds(conn, stack_id);
  });
}

void PhotoStackStore::ValidateAll() {
  auto                 guard = database_.GetConnectionGuard();
  auto                 lock  = guard.Lock();
  duckorm::Transaction transaction(guard.conn_);
  const auto           orphan = duckorm::query<StackIdRow>(
      guard.conn_, expr::raw("SELECT m.stack_id FROM PhotoStackMember m LEFT JOIN PhotoStack s "
                             "ON s.stack_id = m.stack_id WHERE s.stack_id IS NULL LIMIT 1"));
  if (!orphan.empty()) {
    throw std::runtime_error("Photo stack membership has no saved stack: " +
                             orphan.front().stack_id);
  }
  for (const auto& row :
       duckorm::query<StackIdRow>(guard.conn_, expr::raw("SELECT stack_id FROM PhotoStack"))) {
    try {
      ValidateStackId(row.stack_id);
    } catch (const std::invalid_argument& error) {
      throw std::runtime_error("Invalid persisted photo stack ID: " + row.stack_id + ": " +
                               error.what());
    }
  }
  ValidateRelations(guard.conn_);
  transaction.commit();
}

void PhotoStackStore::RemoveFileMemberships(duckdb_connection&               connection,
                                            std::span<const sl_element_id_t> file_ids) {
  const auto sources = SourceStacks(connection, file_ids);
  Detach(connection, file_ids, sources);
}

}  // namespace alcedo
