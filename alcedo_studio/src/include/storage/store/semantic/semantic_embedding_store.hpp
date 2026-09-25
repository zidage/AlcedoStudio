//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "storage/store/database.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief Image embeddings of files (SemanticImageEmbedding, SemanticImageEmbedding768) and the
 *        labels written together with them.
 *
 * An upsert replaces the file's embedding and label rows for the model in one transaction, so
 * a file never keeps a label of an older embedding. The model must be registered; its
 * embedding size selects the table. Methods take the database lock for their statements.
 * Upserts return false and write the reason to @p error on failure; count methods read a
 * failed query as 0.
 */
class SemanticEmbeddingStore {
 public:
  explicit SemanticEmbeddingStore(Database& db_ctrl);

  /// Replace the file's embedding for the model and drop its label.
  [[nodiscard]] auto UpsertImageEmbedding(const SemanticImageEmbeddingRecord& record,
                                          std::string* error = nullptr) const -> bool;
  /// Replace the file's embedding and, when @p label is set, its label.
  [[nodiscard]] auto UpsertImageEmbeddingWithLabel(const SemanticImageEmbeddingRecord& record,
                                                   const SemanticImageLabelRecord*     label,
                                                   std::string* error = nullptr) const -> bool;
  /// Replace the file's embedding and assign its label from the model's label prototypes in
  /// the same transaction.
  [[nodiscard]] auto UpsertImageEmbeddingAndAssignLabel(
      const SemanticImageEmbeddingRecord&   record,
      const SemanticLabelAssignmentOptions& assignment_options,
      SemanticImageLabelRecord* assigned_label = nullptr, std::string* error = nullptr) const
      -> bool;
  /// Batched variant: writes a whole embedding batch with the DuckDB appender in one
  /// transaction and assigns every label with one windowed query. `assigned_labels` (if
  /// provided) is filled in input order. Per-row transactions are the dominant cost for
  /// DuckDB, so callers with more than one record should prefer this over the single-row
  /// upsert.
  [[nodiscard]] auto UpsertImageEmbeddingsAndAssignLabels(
      std::span<const SemanticImageEmbeddingRecord> records,
      const SemanticLabelAssignmentOptions&         assignment_options,
      std::vector<SemanticImageLabelRecord>*        assigned_labels = nullptr,
      std::string*                                  error           = nullptr) const -> bool;

  [[nodiscard]] auto CountImageEmbeddings(const std::string& model_key) const -> size_t;
  [[nodiscard]] auto CountImageEmbeddingsForFile(sl_element_id_t    file_id,
                                                 const std::string& model_key) const -> size_t;
  /// True when the file has a ready embedding of @p image_id for the model and, with
  /// @p require_label, a non-empty label.
  [[nodiscard]] auto HasReadyImageEmbedding(sl_element_id_t file_id, image_id_t image_id,
                                            const std::string& model_key,
                                            bool               require_label = false) const -> bool;

 private:
  Database& database_;
};

// Delete every embedding row (both embedding sizes) and every label row of the given files, for
// all models, on the supplied connection. The element deletion cascade passes its own connection
// so the rows go in the caller's transaction. Throws std::runtime_error when a delete fails. A
// no-op for an empty list.
void DeleteSemanticRowsForFiles(duckdb_connection conn, std::span<const sl_element_id_t> file_ids);

}  // namespace alcedo
