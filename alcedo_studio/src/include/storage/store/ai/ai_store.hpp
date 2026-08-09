//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"
#include "storage/store/database.hpp"
#include "type/type.hpp"

namespace alcedo {

// Persists the remote image-analysis sidecar's results for a file (Phase 5f):
// `AiDescription` (the searchable understanding — caption, tags, scene, confidence) and
// `AiRating` (the 1..5 integer rating, kept out of full-text search). Both bind to
// `file_id` (the Sleeve element id / inode), the same key the CLIP embeddings bind to, so
// deleting a file cascades cleanly and a re-import under a new image id recovers prior
// annotations.
//
// All serialization/deserialization goes through the duckorm layer (`insert_or_replace`
// and `select`); no raw INSERT/SELECT is written here. The `(file_id, task_id)` primary
// key makes `insert_or_replace` enforce "at most one row per pair" — hence at most one
// active-for-search understanding per (file_id, task_id); a re-run for the same pair
// replaces the prior row in place. `prompt_profile_id` (understanding) and
// `rubric_id` / `rubric_version` (rating) are stored per row so a prompt/profile or
// rubric change is never silently reinterpreted as the old row's score.
class AiStore {
 private:
  Database& database_;

 public:
  explicit AiStore(Database& db_ctrl);

  // Persist a successful image-understanding result. `insert_or_replace` on the table's
  // PRIMARY KEY (file_id, task_id) replaces the prior row for that pair, so there is at
  // most one row — hence at most one active-for-search understanding — per
  // (file_id, task_id). Returns false and writes nothing when `IsValid()` is false, so a
  // partial/failed remote call leaves no active search document (the primary guard is the
  // caller only reaching here on a complete successful describe; IsValid is the
  // storage-layer backstop). Throws on a DuckDB error.
  [[nodiscard]] auto UpsertUnderstanding(const AiDescription& description) const -> bool;

  // Persist multiple successful image-understanding results in one DB transaction. Valid
  // rows are upserted together, then the derived FTS document table and DuckDB FTS index
  // are refreshed once after commit. Invalid rows or orphan file_ids are skipped; the
  // return value is the number of rows accepted by storage.
  [[nodiscard]] auto UpsertUnderstandings(std::span<const AiDescription> descriptions) const
      -> size_t;

  // Read the row for an exact (file_id, task_id) pair — a deterministic primary-key
  // lookup. Returns std::nullopt when no such row exists.
  [[nodiscard]] auto GetUnderstanding(sl_element_id_t file_id, const std::string& task_id) const
      -> std::optional<AiDescription>;

  // Read the active-for-search understanding for a file (the first active row). The host
  // uses a single task_id slot per file, so this is the row `UpsertUnderstanding` most
  // recently wrote. Returns std::nullopt when no active row exists.
  [[nodiscard]] auto GetActiveUnderstanding(sl_element_id_t file_id) const
      -> std::optional<AiDescription>;

  // True when the derived AI-description FTS index exists and exposes its DuckDB
  // `match_bm25` function. Search callers use this to add the BM25 predicate only when
  // it is safe; old project files or runtimes without the fts extension keep using the
  // compatibility LIKE search path.
  [[nodiscard]] auto HasUnderstandingFtsIndex() const -> bool;

  // Persist a successful image-rating result (1..5 integer). Same upsert/identity
  // contract as `UpsertUnderstanding`; rejected when `IsValid()` is false (a rating of 0
  // is "unset" and never persisted, so a scored image is never confused with an unrated
  // one). Throws on a DuckDB error.
  [[nodiscard]] auto UpsertRating(const AiRating& rating) const -> bool;

  // Phase 7a: persist a rating's *reasons* only (rationale text + provider/model/prompt/
  // rubric identity), with `rating_ = 0` as a sentinel. The product star rating is the
  // EXIF/metadata `Rating` value written through the star-rating path; this row exists so
  // the AI rationale and identity survive alongside it. The caller MUST set `rating_ = 0`
  // (the truth is the EXIF star, not this column); `GetActiveRating` consumers must not
  // treat the stored `rating_` as the real score. Validated via `IsValidReasonsOnly()`
  // (rating ignored; file key + provider/model identity + non-empty reasons required), so
  // it is rejected if the reasons are empty. Same `(file_id, task_id)` PK + `FileExists`
  // guard as `UpsertRating`; reuses `kInsertRatingFields` (no DDL change). Throws on a
  // DuckDB error.
  [[nodiscard]] auto UpsertRatingReasons(const AiRating& rating) const -> bool;

  [[nodiscard]] auto GetRating(sl_element_id_t file_id, const std::string& task_id) const
      -> std::optional<AiRating>;
  [[nodiscard]] auto GetActiveRating(sl_element_id_t file_id) const -> std::optional<AiRating>;

  // Drop all AI annotation rows for the given files using the controller's own
  // connection. The file-deletion cascade (element_controller) calls the free function
  // below on its own connection so the cleanup is atomic with element deletion; this
  // convenience overload is for non-cascade callers and tests. A no-op for an empty list.
  void               DeleteForFiles(std::span<const sl_element_id_t> file_ids) const;
};

// Delete every `AiImageUnderstanding` and `AiImageRating` row for the given files on the
// supplied connection, via the duckorm `remove` path (this function does not write raw
// DELETE statements — only the `file_id IN (...)` predicate). The element-deletion
// cascade passes its own connection so the AI row cleanup shares the caller's
// transaction. A no-op for an empty file list.
void DeleteAiAnnotationRowsForFiles(duckdb_connection                conn,
                                    std::span<const sl_element_id_t> file_ids);

}  // namespace alcedo
