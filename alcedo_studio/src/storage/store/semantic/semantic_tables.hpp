//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Private to the semantic stores: the embedding-size-dependent table names and the checks
// that every semantic store applies. Statements run through duckorm.

#pragma once

#include <duckdb.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"

namespace alcedo::semantic_tables {

/// True for the embedding sizes that have tables (512 and 768).
[[nodiscard]] auto IsSupportedEmbeddingDim(int dim) -> bool;
/// SemanticImageEmbedding or SemanticImageEmbedding768.
[[nodiscard]] auto ImageEmbeddingTable(int dim) -> const char*;
/// SemanticLabelPrototype or SemanticLabelPrototype768.
[[nodiscard]] auto LabelPrototypeTable(int dim) -> const char*;
/// Name of the HNSW index on the image embedding table of @p dim.
[[nodiscard]] auto ImageEmbeddingIndex(int dim) -> const char*;

/// Field descriptor of a selected column (only the type is used when decoding).
[[nodiscard]] auto Column(const char* name, duckorm::DuckDBType type) -> duckorm::DuckFieldDesc;

/// Embedding size of a registered model, or std::nullopt when it is not registered.
/// @throws std::runtime_error when the query fails.
[[nodiscard]] auto ModelEmbeddingDim(duckdb_connection conn, const std::string& model_key)
    -> std::optional<int>;

/// Value of a COUNT(*) query. A failed query reads as 0: the stores' count methods report no
/// error to their callers.
[[nodiscard]] auto CountOrZero(duckdb_connection conn, const duckorm::SqlFragment& query)
    -> size_t;

/// Writes @p message to @p error when @p error is not null.
void SetError(std::string* error, const std::string& message);

/// Checks that @p embedding has @p expected_dim finite values and a non-zero norm, and that the
/// size has tables. Writes the reason to @p error and returns false otherwise.
[[nodiscard]] auto ValidateEmbedding(std::span<const float> embedding, int expected_dim,
                                     std::string* error) -> bool;

}  // namespace alcedo::semantic_tables
