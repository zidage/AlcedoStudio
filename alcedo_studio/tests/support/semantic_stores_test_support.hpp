//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The four semantic stores of one project, for tests that register a model, write embeddings,
// and read labels or search results in one test body.

#pragma once

#include "sleeve/storage.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "storage/store/semantic/semantic_label_store.hpp"
#include "storage/store/semantic/semantic_model_registry.hpp"
#include "storage/store/semantic/semantic_vector_search.hpp"

namespace alcedo::semantic_test {

/// References to the semantic stores owned by one Storage; valid while the Storage lives.
struct SemanticStores {
  SemanticModelRegistry&  models_;
  SemanticEmbeddingStore& embeddings_;
  SemanticLabelStore&     labels_;
  SemanticVectorSearch&   search_;
};

inline auto StoresOf(Storage& storage) -> SemanticStores {
  return SemanticStores{.models_     = storage.GetSemanticModelRegistry(),
                        .embeddings_ = storage.GetSemanticEmbeddingStore(),
                        .labels_     = storage.GetSemanticLabelStore(),
                        .search_     = storage.GetSemanticVectorSearch()};
}

}  // namespace alcedo::semantic_test
