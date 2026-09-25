//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "storage/store/database.hpp"
#include "storage/store/semantic/semantic_records.hpp"

namespace alcedo {

/**
 * @brief The registered semantic models (SemanticModel) and which one is active.
 *
 * Owns the active model key: the constructor loads it, and every write to SemanticModel after
 * the database is open goes through UpsertModel, SetActiveModelKey, or PurgeModel, which read it
 * again after their commit. ActiveModelKey() then answers without SQL, which the search path
 * relies on.
 *
 * Methods take the database lock for their statements. Methods with an @p error parameter
 * return false / std::nullopt / empty and write the reason there on failure.
 */
class SemanticModelRegistry {
 public:
  explicit SemanticModelRegistry(Database& db_ctrl);

  /// Insert or replace a model. An active model becomes the only active one; replacing the
  /// active model with an inactive row leaves no active model.
  [[nodiscard]] auto UpsertModel(const SemanticModelRecord& model,
                                 std::string*               error = nullptr) const -> bool;
  /// True when the model is registered. A failed query reads as not registered.
  [[nodiscard]] auto HasModel(const std::string& model_key) const -> bool;
  /// Embedding size of a registered model; std::nullopt when not registered or on failure.
  [[nodiscard]] auto GetModelEmbeddingDim(const std::string& model_key) const -> std::optional<int>;
  /// The model's supported text languages JSON; empty when not registered or on failure.
  [[nodiscard]] auto GetModelSupportedTextLanguagesJson(const std::string& model_key) const
      -> std::string;
  [[nodiscard]] auto GetModel(const std::string& model_key, std::string* error = nullptr) const
      -> std::optional<SemanticModelRecord>;
  [[nodiscard]] auto ActiveModel(std::string* error = nullptr) const
      -> std::optional<SemanticModelRecord>;
  /// Key of the active model, or empty when none is active. Returns the value recorded by the
  /// last model write: runs no SQL and does not take the database lock.
  [[nodiscard]] auto ActiveModelKey() const -> std::string;
  /// Make a registered model the only active one.
  [[nodiscard]] auto SetActiveModelKey(const std::string& model_key,
                                       std::string*       error = nullptr) const -> bool;
  /// Every registered model (regardless of the active flag), newest first. The purge path uses
  /// it to find models whose profile is no longer installed.
  [[nodiscard]] auto ListModels(std::string* error = nullptr) const
      -> std::vector<SemanticModelRecord>;
  /// Delete every row keyed by @p model_key: embeddings and prototypes of both sizes, labels,
  /// and the SemanticModel row, in one transaction. A no-op for a key with no rows. If the
  /// purged model was active, no model is active afterwards.
  [[nodiscard]] auto PurgeModel(const std::string& model_key, std::string* error = nullptr) const
      -> bool;

 private:
  // Re-read the active model key on @p conn after a SemanticModel write. Keeps the previous
  // value when the query fails. Caller holds the database lock.
  void                RefreshActiveModelKey(duckdb_connection conn) const;

  Database&           database_;
  // Key of the active model (empty when none is active); the mutex covers the string copy
  // only, because search reads it on the search worker.
  mutable std::mutex  active_model_key_mutex_;
  mutable std::string active_model_key_;
};

}  // namespace alcedo
