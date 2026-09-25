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

/// One file ranked by a natural-language vector search.
struct SemanticRankedFile {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  std::string     file_name_{};
  double          score_ = 0.0;
};
}  // namespace alcedo
