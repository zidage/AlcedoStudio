//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/store/database.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Natural-language search over the image embeddings of one folder scope.
 *
 * Owns the relevance rule: one exact scan of the scope computes, for each file with a `ready`
 * embedding of the model, the query similarity `c`, the neutral similarity `b`, and the
 * relevance `r = c - b`. A file is kept when `c >= kMinQuerySimilarity` and either the scope has
 * fewer than kMinScopeStatisticsCount scored files or the z value of `r` over the scope is at
 * least kMinRelevanceZ. Kept files are ordered by `r` descending, then by file id ascending.
 * The rule uses no vector index. Methods take the database lock for their statements.
 */
class SemanticVectorSearch {
 public:
  /// Minimum z value of the relevance over the scope for a file to be kept. Calibrated with
  /// SemanticRelevanceCalibrationTest (global_search_redesign_plan.md, Phase GS1): a query whose
  /// subject is in about 40 % of the scope has z values near 1.1 for its clear matches.
  static constexpr double      kMinRelevanceZ           = 1.0;
  /// Minimum cosine similarity between the image and the query text for a file to be kept.
  /// Calibrated with SemanticRelevanceCalibrationTest on SigLIP2 embeddings of a varied project:
  /// the midpoint between the highest similarity of a nonsense-query file with
  /// z >= kMinRelevanceZ (0.0816) and the lowest similarity of a listed match (0.0894).
  static constexpr double      kMinQuerySimilarity      = 0.0855;
  /// Below this number of scored files the scope statistics are not used.
  static constexpr size_t      kMinScopeStatisticsCount = 30;
  /// The text whose embedding gives the neutral similarity `b`.
  static constexpr const char* kNeutralText             = "a photo";

  explicit SemanticVectorSearch(Database& db_ctrl);

  /**
   * @brief The page [offset, offset + limit) of the kept files and the number of kept files.
   *
   * Every page of one query reads the same ordered result, so pages neither repeat nor skip
   * files. Returns an empty page and writes the reason to @p error on failure. Limit 0 reads
   * only the total.
   */
  [[nodiscard]] auto SearchImageEmbeddings(sl_element_id_t               folder_id,
                                           const SemanticQueryEmbedding& embedding, size_t offset,
                                           size_t limit, std::string* error = nullptr) const
      -> SemanticRankedPage;

  /**
   * @brief The relevance values of every scored file of the folder, kept or not.
   *
   * Ordered by relevance descending, then by file id. For calibration of the constants and for
   * diagnostics. Returns an empty list and writes the reason to @p error on failure.
   */
  [[nodiscard]] auto ReadRelevanceScores(sl_element_id_t               folder_id,
                                         const SemanticQueryEmbedding& embedding,
                                         std::string* error = nullptr) const
      -> std::vector<SemanticRelevanceScore>;

 private:
  Database& database_;
};

/**
 * @brief SELECT of the files of @p folder_id (0 for the whole library) that the relevance rule
 *        of SemanticVectorSearch keeps for @p embedding.
 *
 * The statement returns the columns `file_id`, `image_id`, and `relevance`, with no order.
 * The fragment carries its binds and is valid for one statement on the database of @p conn.
 * The caller holds the connection lock of @p conn (ElementStore runs it inside its read).
 * @throws std::runtime_error when the model is not registered, when a vector does not have
 *         the model dimension, or when a vector has a non-finite value or a zero norm.
 */
[[nodiscard]] auto BuildSemanticRelevanceSelect(duckorm::Connection conn, sl_element_id_t folder_id,
                                                const SemanticQueryEmbedding& embedding)
    -> duckorm::SqlFragment;

}  // namespace alcedo
