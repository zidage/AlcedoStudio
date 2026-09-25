//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/database.hpp"

#include <duckdb.h>
#include <utf8.h>

#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
#include "utf8/checked.h"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
// Run a multi-statement DDL string (for example `ai_annotation_table_query`) and throw on the
// first failing statement. Both open paths run the CREATE ... IF NOT EXISTS strings, so an
// existing database gains new tables in place.
void RunDdl(duckdb_connection conn, const char* query) {
  duckorm::execute(conn, duckorm::expr::raw(query));
}
}  // namespace

/**
 * @brief Construct a new Database::Database object
 *
 * @param db_path
 */
Database::Database(file_path_t& db_path)
    : db_lock_(std::make_shared<std::recursive_mutex>()), db_path_(db_path), initialized_(false) {
  if (std::filesystem::exists(db_path)) {
    initialized_ = true;
  }
  InitializeDB();
}

/**
 * @brief Destroy the Database::Database object
 *
 */
Database::~Database() { duckdb_close(&db_); }

/**
 * @brief Get a connection guard for the database.
 *
 * @return ConnectionGuard
 */
auto Database::GetConnectionGuard() -> ConnectionGuard {
  ConnectionGuard guard{{}, db_lock_};

  if (duckdb_connect(db_, &guard.conn_) != DuckDBSuccess) {
    throw std::runtime_error("DB cannot be connected");
  }

  return guard;
}

/**
 * @brief Initialize the database by creating necessary tables.
 *
 */
void Database::InitializeDB() {
  // SQL query to create the necessary tables

  std::string utf8_str = conv::ToBytes(db_path_.wstring());
  auto        state    = duckdb_open(utf8_str.c_str(), &db_);
  if (state != DuckDBSuccess) {
    throw std::runtime_error("DB cannot be opened or created");
  }

  auto guard   = GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (!initialized_) {
    RunDdl(guard.conn_, init_table_query);
  }
  RunDdl(guard.conn_, semantic_table_query);
  RunDdl(guard.conn_, semantic_migration_query);
  RunDdl(guard.conn_, ai_annotation_table_query);
  RunDdl(guard.conn_, commit_graph_table_query);
  PopulateSemanticLabelQueries(guard.conn_);
  initialized_ = true;
}

void Database::PopulateSemanticLabelQueries(duckdb_connection conn) {
  namespace expr = duckorm::expr;
  duckorm::Transaction transaction(conn);
  const auto           insert_label_queries = [&conn](SemanticLabelLanguage language) {
    const std::string prompt_hash = SemanticPromptConfigHashForLanguage(language);
    for (const auto& label_query : DefaultSemanticPhotographyLabelQueries(language)) {
      auto statement = expr::raw(
          "INSERT OR REPLACE INTO SemanticLabelQuery (prompt_config_hash, label, query_text) "
          "VALUES (");
      statement.append(expr::param(prompt_hash));
      statement.append(expr::raw(", "));
      statement.append(expr::param(std::string(label_query.label)));
      statement.append(expr::raw(", "));
      statement.append(expr::param(std::string(label_query.query)));
      statement.append(expr::raw(")"));
      duckorm::execute(conn, statement);
    }
  };
  insert_label_queries(SemanticLabelLanguage::kEnglish);
  insert_label_queries(SemanticLabelLanguage::kChinese);
  transaction.commit();
}

};  // namespace alcedo
