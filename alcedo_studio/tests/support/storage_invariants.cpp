//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage_invariants.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "storage_test_access.hpp"

namespace alcedo::test {
namespace {

/// One violating row of a rule query: its key columns as text.
struct ViolationRow {
  std::string row;
  using Columns = duckorm::RowColumns<&ViolationRow::row>;
};

/// A rule and the query that selects the rows that break it (one VARCHAR column).
struct InvariantRule {
  std::string name;
  std::string violating_rows_sql;
};

auto FileType() -> int { return static_cast<int>(ElementType::FILE); }
auto FolderType() -> int { return static_cast<int>(ElementType::FOLDER); }

auto InvariantRules() -> std::vector<InvariantRule> {
  const auto is_file = std::format("SELECT id FROM Element WHERE type = {}", FileType());
  // Tables whose rows belong to one file element; file removal deletes them.
  const std::string_view file_side_tables[] = {
      "PipelineParam",      "SemanticImageEmbedding", "SemanticImageEmbedding768",
      "SemanticImageLabel", "AiImageUnderstanding",   "AiImageRating",
      "AiImageFtsDocument", "AiImageSearchText"};

  std::vector<InvariantRule> rules{
      // Library.
      {"file has exactly one image binding",
       std::format("SELECT 'file ' || e.id || ': ' || COUNT(fi.file_id) || ' bindings' "
                   "FROM Element e LEFT JOIN FileImage fi ON fi.file_id = e.id "
                   "WHERE e.type = {} GROUP BY e.id HAVING COUNT(fi.file_id) <> 1",
                   FileType())},
      {"image binding names an existing Image row",
       "SELECT 'file ' || fi.file_id || ' image ' || fi.image_id FROM FileImage fi "
       "WHERE fi.image_id NOT IN (SELECT id FROM Image)"},
      {"image binding names a file element",
       std::format("SELECT 'file ' || fi.file_id || ' image ' || fi.image_id FROM FileImage fi "
                   "WHERE fi.file_id NOT IN ({})",
                   is_file)},
      {"Image row is bound to a file",
       "SELECT 'image ' || i.id FROM Image i WHERE i.id NOT IN (SELECT image_id FROM FileImage)"},
      {"file is listed in the library root folder",
       std::format("SELECT 'file ' || e.id FROM Element e WHERE e.type = {} AND e.id NOT IN "
                   "(SELECT element_id FROM FolderContent WHERE folder_id = 0)",
                   FileType())},
      {"folder content row names a folder element",
       std::format("SELECT 'folder ' || fc.folder_id || ' element ' || fc.element_id "
                   "FROM FolderContent fc WHERE fc.folder_id NOT IN "
                   "(SELECT id FROM Element WHERE type = {})",
                   FolderType())},
      {"folder content row names an existing element",
       "SELECT 'folder ' || fc.folder_id || ' element ' || fc.element_id FROM FolderContent fc "
       "WHERE fc.element_id NOT IN (SELECT id FROM Element)"},

      // Edit history.
      {"file has a history root",
       std::format("SELECT 'file ' || e.id FROM Element e WHERE e.type = {} AND e.id NOT IN "
                   "(SELECT element_id FROM ImageEditState)",
                   FileType())},
      {"history belongs to a file element",
       std::format("SELECT 'element ' || s.element_id FROM ImageEditState s "
                   "WHERE s.element_id NOT IN ({})",
                   is_file)},
      {"history has the PipelineRoot row of its root",
       "SELECT 'element ' || s.element_id || ' root ' || s.root_id FROM ImageEditState s "
       "WHERE NOT EXISTS (SELECT 1 FROM PipelineRoot r "
       "WHERE r.root_id = s.root_id AND r.element_id = s.element_id)"},
      {"PipelineRoot row belongs to a history",
       "SELECT 'element ' || r.element_id || ' root ' || r.root_id FROM PipelineRoot r "
       "WHERE NOT EXISTS (SELECT 1 FROM ImageEditState s "
       "WHERE s.root_id = r.root_id AND s.element_id = r.element_id)"},
      {"active Version ref exists",
       "SELECT 'element ' || s.element_id || ' version ' || s.active_version_id "
       "FROM ImageEditState s WHERE NOT EXISTS (SELECT 1 FROM VersionRef v "
       "WHERE v.version_id = s.active_version_id AND v.element_id = s.element_id)"},
      {"Version ref belongs to a history",
       "SELECT 'version ' || v.version_id || ' element ' || v.element_id FROM VersionRef v "
       "WHERE v.element_id NOT IN (SELECT element_id FROM ImageEditState)"},
      {"Version head commit exists in the root of its image",
       "SELECT 'version ' || v.version_id || ' head ' || v.head_commit_hash FROM VersionRef v "
       "JOIN ImageEditState s ON s.element_id = v.element_id "
       "WHERE COALESCE(v.head_commit_hash, '') <> '' AND NOT EXISTS (SELECT 1 FROM EditCommit c "
       "WHERE c.commit_hash = v.head_commit_hash AND c.root_id = s.root_id)"},
      {"parent commit exists in the same root",
       "SELECT 'commit ' || c.commit_hash FROM EditCommit c WHERE "
       "(COALESCE(c.first_parent_hash, '') <> '' AND NOT EXISTS (SELECT 1 FROM EditCommit p "
       "WHERE p.commit_hash = c.first_parent_hash AND p.root_id = c.root_id)) OR "
       "(COALESCE(c.second_parent_hash, '') <> '' AND NOT EXISTS (SELECT 1 FROM EditCommit p "
       "WHERE p.commit_hash = c.second_parent_hash AND p.root_id = c.root_id))"},
      {"commit belongs to a stored history",
       "SELECT 'commit ' || c.commit_hash || ' root ' || c.root_id FROM EditCommit c "
       "WHERE c.root_id NOT IN (SELECT root_id FROM ImageEditState)"},
      {"attribution names an existing commit",
       "SELECT 'commit ' || a.commit_hash FROM EditCommitAttribution a "
       "WHERE a.commit_hash NOT IN (SELECT commit_hash FROM EditCommit)"},
  };
  for (const auto table : file_side_tables) {
    rules.push_back(
        InvariantRule{.name               = std::format("{} row names a file element", table),
                      .violating_rows_sql = std::format("SELECT 'file ' || t.file_id FROM {} t "
                                                        "WHERE t.file_id NOT IN ({})",
                                                        table, is_file)});
  }
  return rules;
}

/// ImageEditState element ids, read on @p conn.
struct HistoryElementRow {
  std::int64_t element_id = 0;
  using Columns           = duckorm::RowColumns<&HistoryElementRow::element_id>;
};

}  // namespace

auto FindStorageInvariantViolations(Storage& storage) -> std::vector<StorageInvariantViolation> {
  namespace expr = duckorm::expr;
  auto guard     = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
  auto lock      = guard.Lock();

  std::vector<StorageInvariantViolation> violations;
  for (const auto& rule : InvariantRules()) {
    for (auto& row :
         duckorm::query<ViolationRow>(guard.conn_, expr::raw(rule.violating_rows_sql))) {
      violations.push_back(StorageInvariantViolation{.rule = rule.name, .row = std::move(row.row)});
    }
  }

  // Version refs, commits, and the materialized head and chain of each history must load as
  // one CommitGraph (CommitGraph::FromParts checks the parent and head rules).
  const auto history_rows = duckorm::query<HistoryElementRow>(
      guard.conn_, expr::raw("SELECT element_id FROM ImageEditState ORDER BY element_id"));
  CommitGraphStore graphs(guard.conn_);
  for (const auto& history : history_rows) {
    const auto element_id = static_cast<sl_element_id_t>(history.element_id);
    try {
      if (!graphs.LoadGraph(element_id).has_value()) {
        violations.push_back({.rule = "history loads as a commit graph",
                              .row  = std::format("element {}: no graph", element_id)});
      }
    } catch (const std::exception& error) {
      violations.push_back({.rule = "history loads as a commit graph",
                            .row  = std::format("element {}: {}", element_id, error.what())});
    }
  }
  return violations;
}

void ExpectStoredStorageInvariants(const std::filesystem::path& db_path) {
  if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped()) {
    return;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(db_path, ec)) {
    return;
  }
  Storage storage(db_path);
  const auto violations = FindStorageInvariantViolations(storage);
  EXPECT_TRUE(violations.empty()) << "Stored project " << db_path.string() << ": "
                                  << FormatStorageInvariantViolations(violations);
}

auto FormatStorageInvariantViolations(const std::vector<StorageInvariantViolation>& violations)
    -> std::string {
  std::string text = std::format("{} storage invariant violation(s)", violations.size());
  for (const auto& violation : violations) {
    text += std::format("\n  {}: {}", violation.rule, violation.row);
  }
  return text;
}

}  // namespace alcedo::test
