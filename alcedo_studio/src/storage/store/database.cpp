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

#include "storage/store/semantic/semantic_label_config.hpp"
#include "utf8/checked.h"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
auto SqlString(const std::string& value) -> std::string {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char ch : value) {
    if (ch == '\'') {
      out.push_back('\'');
    }
    out.push_back(ch);
  }
  out.push_back('\'');
  return out;
}

// Run a multi-statement DDL string (e.g. `ai_annotation_table_query`) and throw on the
// first failing statement. Used for the Phase 5f AI annotation tables on both DB init
// paths so existing databases gain the tables in place.
void RunDdlChecked(duckdb_connection conn, const char* query) {
  duckdb_result result;
  if (duckdb_query(conn, query, &result) != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message ? error_message : "DuckDB DDL query failed");
  }
  duckdb_destroy_result(&result);
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

  // SQL query to create the tables
  auto          guard   = GetConnectionGuard();
  auto          db_lock = guard.Lock();
  duckdb_result result;
  if (initialized_) {
    if (duckdb_query(guard.conn_, semantic_table_query, &result) != DuckDBSuccess) {
      auto error_message = duckdb_result_error(&result);
      duckdb_destroy_result(&result);
      throw std::runtime_error(error_message);
    }
    duckdb_destroy_result(&result);
    if (duckdb_query(guard.conn_, semantic_migration_query, &result) != DuckDBSuccess) {
      auto error_message = duckdb_result_error(&result);
      duckdb_destroy_result(&result);
      throw std::runtime_error(error_message);
    }
    duckdb_destroy_result(&result);
    RunDdlChecked(guard.conn_, ai_annotation_table_query);
    RunDdlChecked(guard.conn_, commit_graph_table_query);
    PopulateSemanticLabelQueries(guard.conn_);
    return;
  }

  // Run the SQL query to create the tables
  if (duckdb_query(guard.conn_, init_table_query, &result) != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message);
  }
  duckdb_destroy_result(&result);

  if (duckdb_query(guard.conn_, semantic_table_query, &result) != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message);
  }
  duckdb_destroy_result(&result);
  if (duckdb_query(guard.conn_, semantic_migration_query, &result) != DuckDBSuccess) {
    auto error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message);
  }
  duckdb_destroy_result(&result);
  RunDdlChecked(guard.conn_, ai_annotation_table_query);
  RunDdlChecked(guard.conn_, commit_graph_table_query);
  PopulateSemanticLabelQueries(guard.conn_);
  initialized_ = true;
}

void Database::PopulateSemanticLabelQueries(duckdb_connection conn) {
  duckdb_result result;
  if (duckdb_query(conn, "BEGIN TRANSACTION;", &result) != DuckDBSuccess) {
    std::string error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message);
  }
  duckdb_destroy_result(&result);

  const auto seed_queries = [&](SemanticLabelLanguage language) {
    const auto* prompt_hash = SemanticPromptConfigHashForLanguage(language);
    for (const auto& label_query : DefaultSemanticPhotographyLabelQueries(language)) {
      const auto sql =
          "INSERT OR REPLACE INTO SemanticLabelQuery "
          "(prompt_config_hash, label, query_text) VALUES (" +
          SqlString(prompt_hash) + ", " + SqlString(label_query.label) + ", " +
          SqlString(label_query.query) + ");";
      if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
        std::string error_message = duckdb_result_error(&result);
        duckdb_destroy_result(&result);
        duckdb_result rollback_result;
        duckdb_query(conn, "ROLLBACK;", &rollback_result);
        duckdb_destroy_result(&rollback_result);
        throw std::runtime_error(error_message);
      }
      duckdb_destroy_result(&result);
    }
  };
  seed_queries(SemanticLabelLanguage::kEnglish);
  seed_queries(SemanticLabelLanguage::kChinese);

  if (duckdb_query(conn, "COMMIT;", &result) != DuckDBSuccess) {
    std::string error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error_message);
  }
  duckdb_destroy_result(&result);
}

};  // namespace alcedo
