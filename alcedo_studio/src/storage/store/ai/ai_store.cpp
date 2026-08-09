//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/ai/ai_store.hpp"

#include <duckdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <vector>

#include "storage/mapper/duckorm/duckdb_orm.hpp"

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

// ---- understanding (AiImageUnderstanding) field descriptors ----
//
// `kInsertUnderstandingFields` lists every column EXCEPT `updated_at` (which is left to
// its DDL default `current_timestamp` and re-stamped on each upsert). Member offsets are
// real — bind_field reads them via offsetof. The STRING bind type reads a `std::string`
// member directly (the AiDescription members are plain std::string).
inline constexpr std::array<duckorm::DuckFieldDesc, 11> kInsertUnderstandingFields = {
    FIELD_AS(AiDescription, file_id_, "file_id", UINT32),
    FIELD_AS(AiDescription, task_id_, "task_id", STRING),
    FIELD_AS(AiDescription, provider_id_, "provider_id", STRING),
    FIELD_AS(AiDescription, model_id_, "model_id", STRING),
    FIELD_AS(AiDescription, prompt_profile_id_, "prompt_profile_id", STRING),
    FIELD_AS(AiDescription, rendition_kind_, "rendition_kind", STRING),
    FIELD_AS(AiDescription, caption_, "caption", STRING),
    FIELD_AS(AiDescription, tags_json_, "tags_json", STRING),
    FIELD_AS(AiDescription, scene_, "scene", STRING),
    FIELD_AS(AiDescription, confidence_, "confidence", DOUBLE),
    FIELD_AS(AiDescription, active_, "active", BOOLEAN),
};

// `kSelectUnderstandingFields` lists ALL columns in DDL order (duckorm select runs
// `SELECT *`, so the count must match `duckdb_column_count` and the order matches the
// table definition). Offsets are unused for select, so they are zero; only the type drives
// the value extraction. file_id is read as INT64 (BIGINT column, the proven read type)
// and cast to uint32; BOOLEAN/TIMESTAMP come back as varchar ("true"/"false" / date
// string), so `active` is parsed from its string and `updated_at` is ignored.
inline constexpr std::array<duckorm::DuckFieldDesc, 12> kSelectUnderstandingFields = {
    duckorm::DuckFieldDesc{"file_id", duckorm::DuckDBType::INT64, 0},
    duckorm::DuckFieldDesc{"task_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"provider_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"model_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"prompt_profile_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"rendition_kind", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"caption", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"tags_json", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"scene", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"confidence", duckorm::DuckDBType::DOUBLE, 0},
    duckorm::DuckFieldDesc{"active", duckorm::DuckDBType::BOOLEAN, 0},
    duckorm::DuckFieldDesc{"updated_at", duckorm::DuckDBType::TIMESTAMP, 0},
};

// ---- rating (AiImageRating) field descriptors ----
inline constexpr std::array<duckorm::DuckFieldDesc, 11> kInsertRatingFields = {
    FIELD_AS(AiRating, file_id_, "file_id", UINT32),
    FIELD_AS(AiRating, task_id_, "task_id", STRING),
    FIELD_AS(AiRating, provider_id_, "provider_id", STRING),
    FIELD_AS(AiRating, model_id_, "model_id", STRING),
    FIELD_AS(AiRating, prompt_profile_id_, "prompt_profile_id", STRING),
    FIELD_AS(AiRating, rendition_kind_, "rendition_kind", STRING),
    FIELD_AS(AiRating, rating_, "rating", INT32),
    FIELD_AS(AiRating, rubric_id_, "rubric_id", STRING),
    FIELD_AS(AiRating, rubric_version_, "rubric_version", STRING),
    FIELD_AS(AiRating, reasons_, "reasons", STRING),
    FIELD_AS(AiRating, active_, "active", BOOLEAN),
};

inline constexpr std::array<duckorm::DuckFieldDesc, 12> kSelectRatingFields = {
    duckorm::DuckFieldDesc{"file_id", duckorm::DuckDBType::INT64, 0},
    duckorm::DuckFieldDesc{"task_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"provider_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"model_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"prompt_profile_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"rendition_kind", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"rating", duckorm::DuckDBType::INT32, 0},
    duckorm::DuckFieldDesc{"rubric_id", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"rubric_version", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"reasons", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"active", duckorm::DuckDBType::BOOLEAN, 0},
    duckorm::DuckFieldDesc{"updated_at", duckorm::DuckDBType::TIMESTAMP, 0},
};

constexpr const char* kUnderstandingTable = "AiImageUnderstanding";
constexpr const char* kRatingTable        = "AiImageRating";
constexpr const char* kFtsDocumentTable   = "AiImageFtsDocument";

// Read a VARCHAR/JSON/BOOLEAN/TIMESTAMP cell (always returned as a unique_ptr<string> by
// duckorm select). Returns "" for a null pointer (the columns are NOT NULL DEFAULT '' so
// this only guards against a hypothetical NULL).
auto                  CellString(const duckorm::VarTypes& value) -> std::string {
  const auto& ptr = std::get<std::unique_ptr<std::string>>(value);
  return ptr ? *ptr : std::string{};
}

// A BOOLEAN cell comes back as the varchar "true"/"false"; treat any string starting with
// 't' (any case) as true so the parse is robust to DuckDB's casing.
auto CellBool(const duckorm::VarTypes& value) -> bool {
  const auto& ptr = std::get<std::unique_ptr<std::string>>(value);
  return ptr && !ptr->empty() && (*ptr)[0] == 't';
}

auto MapUnderstanding(const std::vector<duckorm::VarTypes>& row) -> AiDescription {
  AiDescription d;
  d.file_id_           = static_cast<sl_element_id_t>(std::get<int64_t>(row[0]));
  d.task_id_           = CellString(row[1]);
  d.provider_id_       = CellString(row[2]);
  d.model_id_          = CellString(row[3]);
  d.prompt_profile_id_ = CellString(row[4]);
  d.rendition_kind_    = CellString(row[5]);
  d.caption_           = CellString(row[6]);
  d.tags_json_         = CellString(row[7]);
  d.scene_             = CellString(row[8]);
  d.confidence_        = std::get<double>(row[9]);
  d.active_            = CellBool(row[10]);
  // row[11] is updated_at — audit-only, not surfaced on the domain object.
  return d;
}

auto MapRating(const std::vector<duckorm::VarTypes>& row) -> AiRating {
  AiRating r;
  r.file_id_           = static_cast<sl_element_id_t>(std::get<int64_t>(row[0]));
  r.task_id_           = CellString(row[1]);
  r.provider_id_       = CellString(row[2]);
  r.model_id_          = CellString(row[3]);
  r.prompt_profile_id_ = CellString(row[4]);
  r.rendition_kind_    = CellString(row[5]);
  r.rating_            = static_cast<int>(std::get<int32_t>(row[6]));
  r.rubric_id_         = CellString(row[7]);
  r.rubric_version_    = CellString(row[8]);
  r.reasons_           = CellString(row[9]);
  r.active_            = CellBool(row[10]);
  // row[11] is updated_at — audit-only.
  return r;
}

auto JoinFileIds(std::span<const sl_element_id_t> file_ids) -> std::string {
  std::string out;
  for (size_t i = 0; i < file_ids.size(); ++i) {
    if (i > 0) {
      out += ',';
    }
    out += std::to_string(file_ids[i]);
  }
  return out;
}

auto RunQueryNoThrow(duckdb_connection conn, const std::string& sql) -> bool {
  duckdb_result result;
  const bool    ok = duckdb_query(conn, sql.c_str(), &result) == DuckDBSuccess;
  duckdb_destroy_result(&result);
  return ok;
}

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

auto LoadFtsExtension(duckdb_connection conn) -> bool {
  // `fts` is a DuckDB extension. The shipped app runs offline, so prefer the extension
  // copied next to the executable. Disable autoinstall first so a missing packaged
  // extension never stalls trying to download an optional accelerator.
  RunQueryNoThrow(conn, "SET autoinstall_known_extensions=false;");
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
    if (RunQueryNoThrow(conn, "LOAD " + SqlString(candidate.generic_string()) + ";")) {
      return true;
    }
  }

  return RunQueryNoThrow(conn, "LOAD fts;");
}

auto EnsureFtsDocumentTable(duckdb_connection conn) -> bool {
  return RunQueryNoThrow(conn,
                         "CREATE TABLE IF NOT EXISTS AiImageFtsDocument ("
                         "file_id BIGINT PRIMARY KEY,"
                         "body VARCHAR NOT NULL DEFAULT '',"
                         "updated_at TIMESTAMP DEFAULT current_timestamp);");
}

void RefreshFtsDocumentsForFiles(duckdb_connection                conn,
                                 std::span<const sl_element_id_t> file_ids) {
  if (file_ids.empty()) {
    return;
  }
  if (!EnsureFtsDocumentTable(conn)) {
    return;
  }
  const auto ids = JoinFileIds(file_ids);
  RunQueryNoThrow(conn,
                  std::format("DELETE FROM {} WHERE file_id IN ({});", kFtsDocumentTable, ids));
  RunQueryNoThrow(
      conn,
      std::format("INSERT INTO {} (file_id, body) "
                  "SELECT file_id, string_agg(caption || ' ' || tags_json || ' ' || scene, ' ') "
                  "FROM {} WHERE active = TRUE AND file_id IN ({}) GROUP BY file_id;",
                  kFtsDocumentTable, kUnderstandingTable, ids));
}

void RebuildFtsIndex(duckdb_connection conn) {
  if (!EnsureFtsDocumentTable(conn)) {
    return;
  }
  if (!LoadFtsExtension(conn)) {
    return;
  }
  RunQueryNoThrow(conn,
                  std::format("PRAGMA create_fts_index('{}', 'file_id', 'body', overwrite=1);",
                              kFtsDocumentTable));
}

// Enforce the "file_id is a foreign key into Element(id)" contract at the write
// boundary. The AiImageUnderstanding / AiImageRating DDL declares file_id NOT
// NULL but, like the semantic embedding tables, does NOT add a SQL-level
// REFERENCES Element(id) constraint: a DDL foreign key could not be added
// migration-safely here (CREATE TABLE IF NOT EXISTS skips existing DBs, so
// enforcement would be inconsistent across fresh and pre-existing databases),
// and the codebase's established pattern is manual cascade on the
// ElementStore's connection. Instead, every upsert rejects a file_id with
// no matching Element row, so no orphan AI annotation can ever be written.
// `file_id` is an integer, so it is interpolated safely into the predicate.
auto FileExists(duckdb_connection conn, sl_element_id_t file_id) -> bool {
  duckdb_result result;
  const auto    sql = std::format("SELECT 1 FROM Element WHERE id = {} LIMIT 1;", file_id);
  if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    // Fail closed: a query failure must never allow an orphan write.
    return false;
  }
  const bool exists = duckdb_row_count(&result) > 0;
  duckdb_destroy_result(&result);
  return exists;
}

}  // namespace

AiStore::AiStore(Database& db_ctrl) : database_(db_ctrl) {}

auto AiStore::UpsertUnderstanding(const AiDescription& description) const -> bool {
  const std::span<const AiDescription> descriptions(&description, 1);
  return UpsertUnderstandings(descriptions) == 1;
}

auto AiStore::UpsertUnderstandings(std::span<const AiDescription> descriptions) const
    -> size_t {
  if (descriptions.empty()) {
    return 0;
  }
  auto                         guard = database_.GetConnectionGuard();
  auto                         lock  = guard.Lock();

  std::vector<sl_element_id_t> accepted_file_ids;
  accepted_file_ids.reserve(descriptions.size());
  EnsureFtsDocumentTable(guard.conn_);

  duckorm::begin_transaction(guard.conn_);
  try {
    for (const auto& description : descriptions) {
      if (!description.IsValid()) {
        continue;  // partial/failed result — leave no active search document
      }
      if (!FileExists(guard.conn_, description.file_id_)) {
        continue;  // no Element row for file_id — refuse the orphan annotation
      }
      duckorm::insert_or_replace(guard.conn_, kUnderstandingTable, &description,
                                 kInsertUnderstandingFields, kInsertUnderstandingFields.size());
      accepted_file_ids.push_back(description.file_id_);
    }
    if (!accepted_file_ids.empty()) {
      RefreshFtsDocumentsForFiles(guard.conn_, accepted_file_ids);
    }
    duckorm::commit_transaction(guard.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard.conn_);
    throw;
  }

  if (!accepted_file_ids.empty()) {
    RebuildFtsIndex(guard.conn_);
  }
  return accepted_file_ids.size();
}

auto AiStore::GetUnderstanding(sl_element_id_t    file_id,
                                           const std::string& task_id) const
    -> std::optional<AiDescription> {
  const auto where = std::format("file_id = {}", file_id);
  auto       guard = database_.GetConnectionGuard();
  auto       lock  = guard.Lock();
  // Query by file_id (an integer, safely interpolated) and match task_id in C++ so no
  // string is interpolated into the predicate.
  auto       rows  = duckorm::select(guard.conn_, kUnderstandingTable, kSelectUnderstandingFields,
                                     kSelectUnderstandingFields.size(), where.c_str());
  for (auto& row : rows) {
    auto candidate = MapUnderstanding(row);
    if (candidate.task_id_ == task_id) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto AiStore::GetActiveUnderstanding(sl_element_id_t file_id) const
    -> std::optional<AiDescription> {
  const auto where = std::format("file_id = {} AND active = TRUE", file_id);
  auto       guard = database_.GetConnectionGuard();
  auto       lock  = guard.Lock();
  auto       rows  = duckorm::select(guard.conn_, kUnderstandingTable, kSelectUnderstandingFields,
                                     kSelectUnderstandingFields.size(), where.c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return MapUnderstanding(rows.front());
}

auto AiStore::HasUnderstandingFtsIndex() const -> bool {
  auto                  guard = database_.GetConnectionGuard();
  auto                  lock  = guard.Lock();
  duckdb_result         result;
  constexpr const char* kQuery =
      "SELECT COUNT(*) FROM duckdb_functions() "
      "WHERE schema_name = 'fts_main_AiImageFtsDocument' "
      "AND function_name = 'match_bm25';";
  if (duckdb_query(guard.conn_, kQuery, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return false;
  }
  const bool available = duckdb_row_count(&result) > 0 && duckdb_column_count(&result) > 0 &&
                         !duckdb_value_is_null(&result, 0, 0) &&
                         duckdb_value_int64(&result, 0, 0) > 0;
  duckdb_destroy_result(&result);
  return available;
}

auto AiStore::UpsertRating(const AiRating& rating) const -> bool {
  if (!rating.IsValid()) {
    return false;  // rating 0 (unset) or missing identity — never persisted
  }
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  if (!FileExists(guard.conn_, rating.file_id_)) {
    return false;  // no Element row for file_id — refuse the orphan annotation
  }
  duckorm::insert_or_replace(guard.conn_, kRatingTable, &rating, kInsertRatingFields,
                             kInsertRatingFields.size());
  return true;
}

auto AiStore::UpsertRatingReasons(const AiRating& rating) const -> bool {
  // Phase 7a: reasons-only row. The caller sets `rating_ = 0` as a sentinel (the real
  // star is the EXIF/metadata `Rating` value); `IsValidReasonsOnly` ignores the rating
  // value and requires file key + provider/model identity + non-empty reasons. Reuses
  // `kInsertRatingFields` and the same `(file_id, task_id)` PK + `FileExists` guard as
  // `UpsertRating` — no DDL change (the `rating` column is `NOT NULL DEFAULT 0`).
  if (!rating.IsValidReasonsOnly()) {
    return false;  // missing identity or empty reasons — never persisted
  }
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  if (!FileExists(guard.conn_, rating.file_id_)) {
    return false;  // no Element row for file_id — refuse the orphan annotation
  }
  duckorm::insert_or_replace(guard.conn_, kRatingTable, &rating, kInsertRatingFields,
                             kInsertRatingFields.size());
  return true;
}

auto AiStore::GetRating(sl_element_id_t file_id, const std::string& task_id) const
    -> std::optional<AiRating> {
  const auto where = std::format("file_id = {}", file_id);
  auto       guard = database_.GetConnectionGuard();
  auto       lock  = guard.Lock();
  auto       rows  = duckorm::select(guard.conn_, kRatingTable, kSelectRatingFields,
                                     kSelectRatingFields.size(), where.c_str());
  for (auto& row : rows) {
    auto candidate = MapRating(row);
    if (candidate.task_id_ == task_id) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto AiStore::GetActiveRating(sl_element_id_t file_id) const
    -> std::optional<AiRating> {
  const auto where = std::format("file_id = {} AND active = TRUE", file_id);
  auto       guard = database_.GetConnectionGuard();
  auto       lock  = guard.Lock();
  auto       rows  = duckorm::select(guard.conn_, kRatingTable, kSelectRatingFields,
                                     kSelectRatingFields.size(), where.c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return MapRating(rows.front());
}

void AiStore::DeleteForFiles(std::span<const sl_element_id_t> file_ids) const {
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  DeleteAiAnnotationRowsForFiles(guard.conn_, file_ids);
}

void DeleteAiAnnotationRowsForFiles(duckdb_connection                conn,
                                    std::span<const sl_element_id_t> file_ids) {
  if (file_ids.empty()) {
    return;
  }
  const auto where = std::format("file_id IN ({})", JoinFileIds(file_ids));
  // duckorm::remove builds `DELETE FROM <table> WHERE <where>`; we supply only the
  // integer IN-list predicate, so no raw DELETE statement is written here.
  duckorm::remove(conn, kUnderstandingTable, where.c_str());
  duckorm::remove(conn, kRatingTable, where.c_str());
  duckorm::remove(conn, kFtsDocumentTable, where.c_str());
  RebuildFtsIndex(conn);
}

}  // namespace alcedo
