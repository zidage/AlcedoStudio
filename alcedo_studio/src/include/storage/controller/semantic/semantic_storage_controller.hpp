//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "storage/controller/db_controller.hpp"
#include "storage/controller/semantic/semantic_label_config.hpp"
#include "type/type.hpp"

namespace alcedo {
inline constexpr int kSemanticEmbeddingDim    = 512;
inline constexpr int kSemanticEmbeddingDim768 = 768;

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

struct SemanticImageEmbeddingRecord {
  sl_element_id_t    file_id_  = 0;
  image_id_t         image_id_ = 0;
  std::string        model_key_{};
  std::vector<float> embedding_{};
  int                thumbnail_resolution_ = 256;
};

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

struct SemanticLabelPrototypeRecord {
  std::string        model_key_{};
  std::string        label_{};
  std::string        prompt_config_hash_{};
  std::vector<float> embedding_{};
};

struct SemanticLabelAssignmentOptions {
  std::string prompt_config_hash_{kDefaultSemanticPhotographyPromptConfigHash};
  double      confidence_score_threshold_{kDefaultSemanticLabelConfidenceThreshold};
  double      confidence_margin_threshold_{kDefaultSemanticLabelMarginThreshold};
  size_t      top_score_count_{kDefaultSemanticLabelTopScoreCount};
};

struct SemanticLabelQueryRecord {
  std::string prompt_config_hash_{};
  std::string label_{};
  std::string query_text_{};
};

struct SemanticRankedFile {
  sl_element_id_t file_id_  = 0;
  image_id_t      image_id_ = 0;
  std::string     file_name_{};
  double          score_ = 0.0;
};

class SemanticStorageController {
 private:
  DBController& db_ctrl_;

 public:
  explicit SemanticStorageController(DBController& db_ctrl);

  [[nodiscard]] auto UpsertModel(const SemanticModelRecord& model,
                                 std::string*               error = nullptr) const -> bool;
  [[nodiscard]] auto HasModel(const std::string& model_key) const -> bool;
  [[nodiscard]] auto GetModelEmbeddingDim(const std::string& model_key) const -> std::optional<int>;
  [[nodiscard]] auto GetModelSupportedTextLanguagesJson(const std::string& model_key) const
      -> std::string;
  [[nodiscard]] auto GetModel(const std::string& model_key, std::string* error = nullptr) const
      -> std::optional<SemanticModelRecord>;
  [[nodiscard]] auto ActiveModel(std::string* error = nullptr) const
      -> std::optional<SemanticModelRecord>;
  [[nodiscard]] auto ActiveModelKey() const -> std::string;
  [[nodiscard]] auto SetActiveModelKey(const std::string& model_key,
                                       std::string*       error = nullptr) const -> bool;
  [[nodiscard]] auto LatestModelKey() const -> std::string;

  [[nodiscard]] auto UpsertImageEmbedding(const SemanticImageEmbeddingRecord& record,
                                          std::string* error = nullptr) const -> bool;
  [[nodiscard]] auto UpsertImageEmbeddingWithLabel(const SemanticImageEmbeddingRecord& record,
                                                   const SemanticImageLabelRecord*     label,
                                                   std::string* error = nullptr) const -> bool;
  [[nodiscard]] auto UpsertImageEmbeddingAndAssignLabel(
      const SemanticImageEmbeddingRecord&   record,
      const SemanticLabelAssignmentOptions& assignment_options,
      SemanticImageLabelRecord* assigned_label = nullptr, std::string* error = nullptr) const
      -> bool;
  // Batched variant: persists a whole embedding-batch worth of records in a single
  // DuckDB transaction using the Appender bulk-insert path, and assigns labels for
  // every record with one windowed SQL query. `assigned_labels` (if provided) is
  // resized to `records.size()` and filled in input order. Per-row transactions are
  // the dominant cost for DuckDB, so callers with more than one record should prefer
  // this over the single-row upsert.
  [[nodiscard]] auto UpsertImageEmbeddingsAndAssignLabels(
      std::span<const SemanticImageEmbeddingRecord> records,
      const SemanticLabelAssignmentOptions&         assignment_options,
      std::vector<SemanticImageLabelRecord>*        assigned_labels = nullptr,
      std::string*                                  error           = nullptr) const -> bool;
  [[nodiscard]] auto UpsertLabelPrototype(const SemanticLabelPrototypeRecord& record,
                                          std::string* error = nullptr) const -> bool;
  [[nodiscard]] auto UpsertLabelPrototypes(std::span<const SemanticLabelPrototypeRecord> records,
                                           std::string* error = nullptr) const -> bool;
  void               DeleteImageEmbeddingsForFiles(std::span<const sl_element_id_t> file_ids) const;
  [[nodiscard]] auto CountImageEmbeddings(const std::string& model_key) const -> size_t;
  [[nodiscard]] auto CountImageEmbeddingsForFile(sl_element_id_t    file_id,
                                                 const std::string& model_key) const -> size_t;
  [[nodiscard]] auto HasReadyImageEmbedding(sl_element_id_t file_id, image_id_t image_id,
                                            const std::string& model_key,
                                            bool               require_label = false) const -> bool;
  [[nodiscard]] auto CountImageLabelsForFile(sl_element_id_t    file_id,
                                             const std::string& model_key) const -> size_t;
  [[nodiscard]] auto CountImageLabelsInFolder(sl_element_id_t    folder_id,
                                              const std::string& model_key) const -> size_t;
  [[nodiscard]] auto CountLabelPrototypes(const std::string& model_key,
                                          const std::string& prompt_config_hash) const -> size_t;
  [[nodiscard]] auto CountLabelQueries(const std::string& prompt_config_hash) const -> size_t;
  [[nodiscard]] auto ListLabelQueries(const std::string& prompt_config_hash,
                                      std::string*       error = nullptr) const
      -> std::vector<SemanticLabelQueryRecord>;
  [[nodiscard]] auto LoadLabelPrototypes(const std::string& model_key,
                                         const std::string& prompt_config_hash,
                                         std::string*       error = nullptr) const
      -> std::vector<SemanticGenerationLabelPrototype>;
  [[nodiscard]] auto GetImageLabelForFile(sl_element_id_t file_id, const std::string& model_key,
                                          std::string* error = nullptr) const
      -> std::optional<SemanticImageLabelRecord>;

  [[nodiscard]] auto SearchImageEmbeddings(sl_element_id_t folder_id, const std::string& model_key,
                                           std::span<const float> query_embedding, size_t offset,
                                           size_t limit, std::string* error = nullptr) const
      -> std::vector<SemanticRankedFile>;

  [[nodiscard]] auto EnsureVectorSearchIndex(const std::string& model_key,
                                             std::string*       error = nullptr) const -> bool;
};
}  // namespace alcedo
