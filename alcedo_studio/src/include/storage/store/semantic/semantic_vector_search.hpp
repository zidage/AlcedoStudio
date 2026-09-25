//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "storage/store/database.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Natural-language search over the image embeddings with the DuckDB vss HNSW index.
 *
 * Owns the HNSW index of each embedding table: EnsureVectorSearchIndex loads the packaged vss
 * extension and creates the index when missing. Methods take the database lock for their
 * statements and write the reason to @p error on failure.
 */
class SemanticVectorSearch {
 public:
  explicit SemanticVectorSearch(Database& db_ctrl);

  /**
   * @brief Files of the folder (0 for the whole library) nearest to @p query_embedding.
   *
   * Reads the nearest candidates of the model through the HNSW index (at least 512, enough
   * for the page), keeps those at or above the elbow score cutoff, and returns the page
   * [offset, offset + limit) ordered by score. Empty for limit 0 or on failure.
   */
  [[nodiscard]] auto SearchImageEmbeddings(sl_element_id_t folder_id, const std::string& model_key,
                                           std::span<const float> query_embedding, size_t offset,
                                           size_t limit, std::string* error = nullptr) const
      -> std::vector<SemanticRankedFile>;

  /// Load the vss extension and create the HNSW index of the model's embedding table.
  [[nodiscard]] auto EnsureVectorSearchIndex(const std::string& model_key,
                                             std::string*       error = nullptr) const -> bool;

 private:
  Database& database_;
};

}  // namespace alcedo
