//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "storage/store/semantic/semantic_label_config.hpp"
#include "type/type.hpp"

namespace alcedo {
inline constexpr int kSemanticEmbeddingDim    = 512;
inline constexpr int kSemanticEmbeddingDim768 = 768;

/// One row of SemanticModel: a registered CLIP-style model. At most one model is active.
struct SemanticModelRecord {
  std::string model_key_{};
  std::string model_id_{};
  std::string revision_{};
  int         embedding_dim_ = kSemanticEmbeddingDim;
  int         image_size_    = 256;
  std::string engine_id_{};
  std::string profile_id_{};
  std::string supported_text_languages_json_{};
  std::string prompt_config_hash_{};
  std::string asset_manifest_json_{};
  bool        active_ = true;
};

/// One image embedding of a file for one model (SemanticImageEmbedding / ...768).
struct SemanticImageEmbeddingRecord {
  sl_element_id_t    file_id_  = 0;
  image_id_t         image_id_ = 0;
  std::string        model_key_{};
  std::vector<float> embedding_{};
  int                thumbnail_resolution_ = 256;
};

/// The label assigned to a file for one model (SemanticImageLabel).
struct SemanticImageLabelRecord {
  sl_element_id_t       file_id_ = 0;
  std::string           model_key_{};
  std::string           label_{};
  double                score_ = 0.0;
  std::string           second_label_{};
  std::optional<double> second_score_{};
  double                margin_    = 0.0;
  bool                  confident_ = false;
  std::string           top_scores_json_{};
};

/// The text embedding of one label prompt for one model (SemanticLabelPrototype / ...768).
struct SemanticLabelPrototypeRecord {
  std::string        model_key_{};
  std::string        label_{};
  std::string        prompt_config_hash_{};
  std::vector<float> embedding_{};
};

/// Thresholds for assigning a label to an image embedding.
struct SemanticLabelAssignmentOptions {
  std::string prompt_config_hash_{kDefaultSemanticPhotographyPromptConfigHash};
  double      confidence_score_threshold_{kDefaultSemanticLabelConfidenceThreshold};
  double      confidence_margin_threshold_{kDefaultSemanticLabelMarginThreshold};
  size_t      top_score_count_{kDefaultSemanticLabelTopScoreCount};
};

/// One label prompt of a prompt configuration (SemanticLabelQuery).
struct SemanticLabelQueryRecord {
  std::string prompt_config_hash_{};
  std::string label_{};
  std::string query_text_{};
};

/**
 * @brief The text embeddings of one natural-language query for one model.
 *
 * The result of an embedding operation (SemanticSearchProvider::EmbedQuery), not a copy of an
 * owner's state. `query_` is the embedding of the trimmed query text and `neutral_` the
 * embedding of the fixed neutral text (SemanticVectorSearch::kNeutralText); both have
 * `dimension_` values. A search caller keeps the value while the search is active, so later
 * page reads do not embed the text again, and releases it when the search ends. No code writes
 * it back to an owner.
 */
struct SemanticQueryEmbedding {
  std::string        model_key_{};
  int                dimension_ = 0;
  std::vector<float> query_{};
  std::vector<float> neutral_{};
};

/// One file kept by the natural-language relevance rule (SemanticVectorSearch).
struct SemanticRankedFile {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  /// Query similarity minus neutral similarity.
  double          relevance_ = 0.0;
};

/// One page of the kept files, in relevance order, and the number of all kept files.
struct SemanticRankedPage {
  std::vector<SemanticRankedFile> rows_{};
  size_t                          total_ = 0;
};

/// The relevance values of one scored file of a scope, before the keep rule is applied.
struct SemanticRelevanceScore {
  sl_element_id_t file_id_            = 0;
  image_id_t      image_id_           = 0;
  std::string     file_name_{};
  double          query_similarity_   = 0.0;  ///< c: cosine similarity to the query text.
  double          neutral_similarity_ = 0.0;  ///< b: cosine similarity to the neutral text.
  double          relevance_          = 0.0;  ///< r = c - b.
  /// (r - mean) / population standard deviation over the scope; empty when the deviation is 0.
  std::optional<double> relevance_z_{};
  bool                  kept_ = false;  ///< True when the keep rule keeps the file.
};
}  // namespace alcedo
