//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/controller/db_controller.hpp"

#include <duckdb.h>
#include <utf8.h>

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "storage/controller/semantic/semantic_label_config.hpp"
#include "utf8/checked.h"
#include "utils/string/convert.hpp"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

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

void RunQueryBestEffort(duckdb_connection conn, const char* query) {
  duckdb_result result;
  duckdb_query(conn, query, &result);
  duckdb_destroy_result(&result);
}

void RunQueryBestEffort(duckdb_connection conn, const std::string& query) {
  RunQueryBestEffort(conn, query.c_str());
}

auto ExecutableDirectory() -> std::filesystem::path {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD        size = 0;
  while (true) {
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return {};
    }
    if (size < buffer.size() - 1) {
      buffer.resize(size);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str())).parent_path();
#else
  std::string buffer(PATH_MAX, '\0');
  const auto  size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size <= 0) {
    return {};
  }
  buffer.resize(static_cast<size_t>(size));
  return std::filesystem::path(buffer).parent_path();
#endif
}

auto EnvironmentVariable(const char* name) -> std::string {
#ifdef _WIN32
  char*  raw = nullptr;
  size_t len = 0;
  if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
    return {};
  }
  std::string value(raw, len > 0 ? len - 1 : 0);
  std::free(raw);
  return value;
#else
  const char* raw = std::getenv(name);
  return raw != nullptr ? std::string(raw) : std::string{};
#endif
}

void LoadFtsExtensionBestEffort(duckdb_connection conn) {
  RunQueryBestEffort(conn, "SET autoinstall_known_extensions=false;");

  std::vector<std::filesystem::path> candidates;
  if (const auto env_path = EnvironmentVariable("ALCEDO_DUCKDB_FTS_EXTENSION"); !env_path.empty()) {
    candidates.emplace_back(env_path);
  }

  const auto exe_dir = ExecutableDirectory();
  if (!exe_dir.empty()) {
#ifdef __APPLE__
    candidates.push_back(exe_dir.parent_path() / "Resources" / "duckdb_extensions" /
                         "fts.duckdb_extension");
#endif
    candidates.push_back(exe_dir / "duckdb_extensions" / "fts.duckdb_extension");
    candidates.push_back(exe_dir / "extensions" / "fts.duckdb_extension");
  }

  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
      continue;
    }
    RunQueryBestEffort(conn, "LOAD " + SqlString(candidate.generic_string()) + ";");
    return;
  }

  RunQueryBestEffort(conn, "LOAD fts;");
}

void RefreshAiUnderstandingFtsBestEffort(duckdb_connection conn) {
  // DuckDB FTS indexes are not maintained automatically when the source table changes.
  // On project open, rebuild the derived AI-description document table from the
  // authoritative annotation table and then try to refresh the FTS index. Loading/using
  // the fts extension is best-effort so old installations without it can still open and
  // use the compatibility LIKE search path. Disable extension autoinstall first so an
  // offline app never stalls trying to download an optional accelerator.
  RunQueryBestEffort(conn, "DELETE FROM AiImageFtsDocument;");
  RunQueryBestEffort(conn,
                     "INSERT INTO AiImageFtsDocument (file_id, body) "
                     "SELECT file_id, string_agg(caption || ' ' || tags_json || ' ' || scene, ' ') "
                     "FROM AiImageUnderstanding WHERE active = TRUE GROUP BY file_id;");
  LoadFtsExtensionBestEffort(conn);
  RunQueryBestEffort(
      conn, "PRAGMA create_fts_index('AiImageFtsDocument', 'file_id', 'body', overwrite=1);");
}
}  // namespace

/**
 * @brief Construct a new DBController::DBController object
 *
 * @param db_path
 */
DBController::DBController(file_path_t& db_path)
    : db_lock_(std::make_shared<std::recursive_mutex>()), db_path_(db_path), initialized_(false) {
  if (std::filesystem::exists(db_path)) {
    initialized_ = true;
  }
  InitializeDB();
}

/**
 * @brief Destroy the DBController::DBController object
 *
 */
DBController::~DBController() { duckdb_close(&db_); }

/**
 * @brief Get a connection guard for the database.
 *
 * @return ConnectionGuard
 */
auto DBController::GetConnectionGuard() -> ConnectionGuard {
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
void DBController::InitializeDB() {
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
    RunDdlChecked(guard.conn_, editor_recovery_metadata_table_query);
    RefreshAiUnderstandingFtsBestEffort(guard.conn_);
    SeedSemanticLabelQueries(guard.conn_);
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
  RunDdlChecked(guard.conn_, editor_recovery_metadata_table_query);
  RefreshAiUnderstandingFtsBestEffort(guard.conn_);
  SeedSemanticLabelQueries(guard.conn_);
  initialized_ = true;
}

void DBController::SeedSemanticLabelQueries(duckdb_connection conn) {
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
