//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Private to the semantic stores: assigning a label to image embeddings that were just written
// and writing the label rows. The functions run on the caller's connection, inside the
// caller's transaction, and throw std::runtime_error on failure.

#pragma once

#include <duckdb.h>

#include <span>
#include <string>
#include <vector>

#include "storage/store/semantic/semantic_records.hpp"
#include "type/type.hpp"

namespace alcedo::semantic_label_assignment {

/// Throws when the prompt configuration hash is empty or a threshold is not finite.
void ValidateOptions(const SemanticLabelAssignmentOptions& options);

/// Checks that @p label (when not null) belongs to @p record and has a label and finite
/// scores. Writes the reason to @p error and returns false otherwise.
[[nodiscard]] auto ValidateLabel(const SemanticImageEmbeddingRecord& record,
                                 const SemanticImageLabelRecord* label, std::string* error)
    -> bool;

/// Inserts one SemanticImageLabel row.
void InsertLabel(duckdb_connection conn, const SemanticImageLabelRecord& label);

/// Inserts label rows with the DuckDB appender. An empty second label or top-score text is
/// stored as NULL.
void AppendLabels(duckdb_connection conn, std::span<const SemanticImageLabelRecord> labels);

/**
 * @brief Label of the embedding just written for @p record.
 *
 * Ranks the label prototypes of the options' prompt configuration by inner product with the
 * embedding, keeps the ranked labels above the elbow cutoff (at most the display count), and
 * builds the label record. Throws when the options are invalid, a label is empty, or the
 * model has no prototype.
 */
[[nodiscard]] auto AssignLabel(duckdb_connection conn, const SemanticImageEmbeddingRecord& record,
                               int model_dim, const SemanticLabelAssignmentOptions& options)
    -> SemanticImageLabelRecord;

/**
 * @brief Labels of a batch of embeddings just written, with one query.
 *
 * @p records share @p model_key and have unique file ids; @p file_ids are their ids in the
 * same order. Returns one label for each record, in input order. Throws when a file gets no
 * label.
 */
[[nodiscard]] auto AssignLabels(duckdb_connection                             conn,
                                std::span<const SemanticImageEmbeddingRecord> records,
                                std::span<const sl_element_id_t> file_ids,
                                const std::string& model_key, int model_dim,
                                const SemanticLabelAssignmentOptions& options)
    -> std::vector<SemanticImageLabelRecord>;

}  // namespace alcedo::semantic_label_assignment
