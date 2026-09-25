//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "storage/store/database.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief The semantic label vocabulary and the labels assigned to files.
 *
 * Owns the label prompts (SemanticLabelQuery, filled when the database opens), the prompt text
 * embeddings of each model (SemanticLabelPrototype, SemanticLabelPrototype768), and reads of
 * the assigned labels (SemanticImageLabel; SemanticEmbeddingStore writes them with the image
 * embeddings). Methods take the database lock for their statements. Methods with an @p error
 * parameter write the reason there on failure; count methods read a failed query as 0.
 */
class SemanticLabelStore {
 public:
  explicit SemanticLabelStore(Database& db_ctrl);

  [[nodiscard]] auto UpsertLabelPrototype(const SemanticLabelPrototypeRecord& record,
                                          std::string* error = nullptr) const -> bool;
  /// Insert or replace the prototypes in one transaction; none is written when one is invalid.
  [[nodiscard]] auto UpsertLabelPrototypes(std::span<const SemanticLabelPrototypeRecord> records,
                                           std::string* error = nullptr) const -> bool;
  [[nodiscard]] auto LoadLabelPrototypes(const std::string& model_key,
                                         const std::string& prompt_config_hash,
                                         std::string*       error = nullptr) const
      -> std::vector<SemanticGenerationLabelPrototype>;
  [[nodiscard]] auto CountLabelPrototypes(const std::string& model_key,
                                          const std::string& prompt_config_hash) const -> size_t;

  [[nodiscard]] auto CountLabelQueries(const std::string& prompt_config_hash) const -> size_t;
  [[nodiscard]] auto ListLabelQueries(const std::string& prompt_config_hash,
                                      std::string*       error = nullptr) const
      -> std::vector<SemanticLabelQueryRecord>;

  [[nodiscard]] auto GetImageLabelForFile(sl_element_id_t file_id, const std::string& model_key,
                                          std::string* error = nullptr) const
      -> std::optional<SemanticImageLabelRecord>;
  [[nodiscard]] auto CountImageLabelsForFile(sl_element_id_t    file_id,
                                             const std::string& model_key) const -> size_t;
  /// Files of the folder (0 for the whole library) that have a non-empty label for the model.
  [[nodiscard]] auto CountImageLabelsInFolder(sl_element_id_t    folder_id,
                                              const std::string& model_key) const -> size_t;

 private:
  Database& database_;
};

}  // namespace alcedo
