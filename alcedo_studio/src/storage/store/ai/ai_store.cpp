//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/ai/ai_store.hpp"

#include <duckdb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/duckdb_extension.hpp"
#include "utils/string/search_text.hpp"


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
inline constexpr std::array<duckorm::DuckFieldDesc, 14> kSelectUnderstandingFields = {
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
    duckorm::DuckFieldDesc{"caption_search_text", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"tags_search_text", duckorm::DuckDBType::VARCHAR, 0},
    duckorm::DuckFieldDesc{"updated_at", duckorm::DuckDBType::TIMESTAMP, 0},
};

auto JoinFolded(const std::vector<std::string>& parts) -> std::string {
  std::string out;
  for (const auto& part : parts) {
    const auto folded = FoldSearchTextUtf8(part);
    if (folded.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += folded;
  }
  return out;
}

// Set the folded search text columns of the (file_id, task_id) row that the caller has just
// written: caption_search_text = folded caption and scene, tags_search_text = folded tags with
// one space between tags. Search reads these columns and never folds caption, scene, or
// tags_json in SQL. A plain UPDATE is used because an INSERT ... ON CONFLICT DO UPDATE of a
// row already written in the same transaction resets the columns it does not list (seen
// with DuckDB in LibrarySearchColumnsTest). Throws on a DuckDB error.
void WriteUnderstandingSearchText(duckdb_connection conn, const AiDescription& description) {
  namespace expr = duckorm::expr;
  auto statement = expr::raw("UPDATE AiImageUnderstanding SET caption_search_text = ");
  statement.append(expr::param(JoinFolded({description.caption_, description.scene_})));
  statement.append(expr::raw(", tags_search_text = "));
  statement.append(expr::param(JoinFolded(description.Tags())));
  statement.append(expr::raw(" WHERE file_id = "));
  statement.append(expr::param(static_cast<int64_t>(description.file_id_)));
  statement.append(expr::raw(" AND task_id = "));
  statement.append(expr::param(description.task_id_));
  duckorm::execute(conn, statement);
}

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
constexpr const char* kSearchTextTable    = "AiImageSearchText";

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
  // row[11..12] are the derived search text columns; row[13] is updated_at — audit-only.
  // Neither is surfaced on the domain object.
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

auto FileIdColumnIn(std::span<const sl_element_id_t> file_ids) -> duckorm::SqlFragment {
  return duckorm::expr::in_list(duckorm::expr::col("file_id"), file_ids);
}

auto FileIdEquals(sl_element_id_t file_id) -> duckorm::SqlFragment {
  return duckorm::expr::eq(duckorm::expr::col("file_id"),
                           duckorm::expr::param(static_cast<int64_t>(file_id)));
}

auto ActiveRowOfFile(sl_element_id_t file_id) -> duckorm::SqlFragment {
  return duckorm::expr::and_({FileIdEquals(file_id), duckorm::expr::raw("active = TRUE")});
}

/// Runs a statement of the optional BM25 index path (document table, index build). The index
/// is an accelerator: without the fts extension, or with a damaged document table, search
/// matches the folded text only. Returns false when DuckDB rejects the statement.
auto ExecuteFtsStatement(duckdb_connection conn, const duckorm::SqlFragment& statement) -> bool {
  try {
    duckorm::execute(conn, statement);
    return true;
  } catch (const std::runtime_error&) {
    return false;
  }
}

auto EnsureFtsDocumentTable(duckdb_connection conn) -> bool {
  return ExecuteFtsStatement(conn, duckorm::expr::raw("CREATE TABLE IF NOT EXISTS AiImageFtsDocument ("
                                                      "file_id BIGINT PRIMARY KEY,"
                                                      "body VARCHAR NOT NULL DEFAULT '',"
                                                      "updated_at TIMESTAMP DEFAULT "
                                                      "current_timestamp);"));
}

/**
 * @brief Rewrite the per-file search rows of the given files, or of all files.
 *
 * @p file_ids is std::nullopt for all files. Each file with at least one active understanding
 * gets one `AiImageSearchText` row (the folded caption and tag text of its understandings, in
 * task_id order) and one `AiImageFtsDocument` row (the BM25 body). Files without an active
 * understanding get no rows. The search text statements throw on error, so the caller's
 * transaction rolls back; the FTS document statements are best-effort because the BM25 index
 * is optional.
 */
void RewriteSearchDocuments(duckdb_connection                                conn,
                            std::optional<std::span<const sl_element_id_t>> file_ids) {
  namespace expr         = duckorm::expr;
  const auto files_where = [&file_ids](const char* keyword) {
    if (!file_ids.has_value()) {
      return duckorm::SqlFragment{};
    }
    auto where = expr::raw(keyword);
    where.append(FileIdColumnIn(*file_ids));
    return where;
  };
  const auto statement = [](std::string sql, duckorm::SqlFragment where, const char* tail) {
    auto out = expr::raw(sql);
    out.append(std::move(where));
    out.append(expr::raw(tail));
    return out;
  };

  duckorm::execute(conn, statement(std::format("DELETE FROM {}", kSearchTextTable),
                                   files_where(" WHERE "), ";"));
  duckorm::execute(
      conn, statement(std::format("INSERT INTO {} (file_id, caption_search_text, tags_search_text) "
                                  "SELECT file_id, string_agg(caption_search_text, ' ' ORDER BY "
                                  "task_id), string_agg(tags_search_text, ' ' ORDER BY task_id) "
                                  "FROM {} WHERE active = TRUE",
                                  kSearchTextTable, kUnderstandingTable),
                      files_where(" AND "), " GROUP BY file_id;"));
  if (!EnsureFtsDocumentTable(conn)) {
    return;
  }
  ExecuteFtsStatement(conn, statement(std::format("DELETE FROM {}", kFtsDocumentTable),
                                      files_where(" WHERE "), ";"));
  ExecuteFtsStatement(
      conn, statement(std::format("INSERT INTO {} (file_id, body) SELECT file_id, "
                                  "string_agg(caption || ' ' || tags_json || ' ' || scene, ' ') "
                                  "FROM {} WHERE active = TRUE",
                                  kFtsDocumentTable, kUnderstandingTable),
                      files_where(" AND "), " GROUP BY file_id;"));
}

/**
 * @brief Recreate the BM25 index over `AiImageFtsDocument`.
 *
 * DuckDB does not maintain FTS indexes when the source table changes, so every writer calls
 * this after its commit. Returns true when `create_fts_index` succeeded, which is when the
 * `match_bm25` macro exists. Without the fts extension it returns false on every call; with
 * it, the index exists from the first build on (an empty table is indexed too), so a later
 * rebuild does not change the result.
 */
auto RebuildFtsIndex(duckdb_connection conn) -> bool {
  if (!EnsureFtsDocumentTable(conn)) {
    return false;
  }
  if (!LoadPackagedDuckDbExtension(conn, "fts")) {
    return false;
  }
  return ExecuteFtsStatement(
      conn, duckorm::expr::raw(std::format(
                "PRAGMA create_fts_index('{}', 'file_id', 'body', overwrite=1);",
                kFtsDocumentTable)));
}

// Enforce the "file_id is a foreign key into Element(id)" rule at the write boundary. The
// AiImageUnderstanding / AiImageRating DDL declares file_id NOT NULL but, like the semantic
// embedding tables, does NOT add a SQL-level REFERENCES Element(id) constraint: a DDL foreign
// key could not be added migration-safely here (CREATE TABLE IF NOT EXISTS skips existing DBs,
// so enforcement would be inconsistent across fresh and pre-existing databases), and the
// codebase's established pattern is manual cascade on the ElementStore's connection. Instead,
// every upsert rejects a file_id with no matching Element row, so no orphan AI annotation can
// ever be written. A query failure throws, so it never allows an orphan write either.
auto FileExists(duckdb_connection conn, sl_element_id_t file_id) -> bool {
  auto query = duckorm::expr::raw("SELECT 1 FROM Element WHERE id = ");
  query.append(duckorm::expr::param(static_cast<int64_t>(file_id)));
  query.append(duckorm::expr::raw(" LIMIT 1"));
  return duckorm::select_int64(conn, query).has_value();
}

}  // namespace

AiStore::AiStore(Database& db_ctrl) : database_(db_ctrl) {
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  {
    duckorm::Transaction transaction(guard.conn_);
    RewriteSearchDocuments(guard.conn_, std::nullopt);
    transaction.commit();
  }
  understanding_fts_index_ready_.store(RebuildFtsIndex(guard.conn_));
}

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
  // DDL outside the transaction: a dropped document table is created again.
  EnsureFtsDocumentTable(guard.conn_);

  {
    duckorm::Transaction transaction(guard.conn_);
    for (const auto& description : descriptions) {
      if (!description.IsValid()) {
        continue;  // partial/failed result — leave no active search document
      }
      if (!FileExists(guard.conn_, description.file_id_)) {
        continue;  // no Element row for file_id — refuse the orphan annotation
      }
      duckorm::insert_or_replace(guard.conn_, kUnderstandingTable, &description,
                                 kInsertUnderstandingFields, kInsertUnderstandingFields.size());
      WriteUnderstandingSearchText(guard.conn_, description);
      accepted_file_ids.push_back(description.file_id_);
    }
    if (!accepted_file_ids.empty()) {
      RewriteSearchDocuments(guard.conn_, std::span<const sl_element_id_t>(accepted_file_ids));
    }
    transaction.commit();
  }

  if (!accepted_file_ids.empty()) {
    understanding_fts_index_ready_.store(RebuildFtsIndex(guard.conn_));
  }
  return accepted_file_ids.size();
}

auto AiStore::GetUnderstanding(sl_element_id_t    file_id,
                                           const std::string& task_id) const
    -> std::optional<AiDescription> {
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  auto rows  = duckorm::select(guard.conn_, kUnderstandingTable, kSelectUnderstandingFields,
                               kSelectUnderstandingFields.size(), FileIdEquals(file_id));
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
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  auto rows  = duckorm::select(guard.conn_, kUnderstandingTable, kSelectUnderstandingFields,
                               kSelectUnderstandingFields.size(), ActiveRowOfFile(file_id));
  if (rows.empty()) {
    return std::nullopt;
  }
  return MapUnderstanding(rows.front());
}

auto AiStore::HasUnderstandingFtsIndex() const -> bool {
  return understanding_fts_index_ready_.load();
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
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  auto rows  = duckorm::select(guard.conn_, kRatingTable, kSelectRatingFields,
                               kSelectRatingFields.size(), FileIdEquals(file_id));
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
  auto guard = database_.GetConnectionGuard();
  auto lock  = guard.Lock();
  auto rows  = duckorm::select(guard.conn_, kRatingTable, kSelectRatingFields,
                               kSelectRatingFields.size(), ActiveRowOfFile(file_id));
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
  const auto where = FileIdColumnIn(file_ids);
  duckorm::remove(conn, kUnderstandingTable, where);
  duckorm::remove(conn, kRatingTable, where);
  duckorm::remove(conn, kSearchTextTable, where);
  duckorm::remove(conn, kFtsDocumentTable, where);
  // The result is not needed: a rebuild does not change whether the index exists.
  RebuildFtsIndex(conn);
}

}  // namespace alcedo
