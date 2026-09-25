//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "semantic_tables.hpp"

#include <cmath>
#include <exception>
#include <format>

#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/semantic/semantic_records.hpp"

namespace alcedo::semantic_tables {

auto IsSupportedEmbeddingDim(int dim) -> bool {
  return dim == kSemanticEmbeddingDim || dim == kSemanticEmbeddingDim768;
}

auto ImageEmbeddingTable(int dim) -> const char* {
  return dim == kSemanticEmbeddingDim768 ? "SemanticImageEmbedding768" : "SemanticImageEmbedding";
}

auto LabelPrototypeTable(int dim) -> const char* {
  return dim == kSemanticEmbeddingDim768 ? "SemanticLabelPrototype768" : "SemanticLabelPrototype";
}

auto ImageEmbeddingIndex(int dim) -> const char* {
  return dim == kSemanticEmbeddingDim768 ? "idx_semantic_image_embedding768_hnsw"
                                         : "idx_semantic_image_embedding_hnsw";
}

auto Column(const char* name, duckorm::DuckDBType type) -> duckorm::DuckFieldDesc {
  return duckorm::DuckFieldDesc{name, type, 0};
}

auto ModelEmbeddingDim(duckdb_connection conn, const std::string& model_key)
    -> std::optional<int> {
  auto query = duckorm::expr::raw("SELECT embedding_dim FROM SemanticModel WHERE ");
  query.append(duckorm::expr::column_eq("model_key", model_key));
  const auto value = duckorm::select_int64(conn, query);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<int>(*value);
}

auto CountOrZero(duckdb_connection conn, const duckorm::SqlFragment& query) -> size_t {
  try {
    const auto count = duckorm::select_int64(conn, query);
    return count.has_value() ? static_cast<size_t>(*count) : 0U;
  } catch (const std::exception&) {
    return 0U;
  }
}

void SetError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

auto ValidateEmbedding(std::span<const float> embedding, int expected_dim, std::string* error)
    -> bool {
  if (!IsSupportedEmbeddingDim(expected_dim)) {
    SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                expected_dim));
    return false;
  }
  if (embedding.size() != static_cast<size_t>(expected_dim)) {
    SetError(error, std::format("Embedding dimension mismatch: expected {}, got {}.", expected_dim,
                                embedding.size()));
    return false;
  }

  double norm_sq = 0.0;
  for (const float value : embedding) {
    if (!std::isfinite(value)) {
      SetError(error, "Embedding contains NaN or infinity.");
      return false;
    }
    norm_sq += static_cast<double>(value) * static_cast<double>(value);
  }
  if (norm_sq <= 0.0) {
    SetError(error, "Embedding norm is zero.");
    return false;
  }
  return true;
}

}  // namespace alcedo::semantic_tables
