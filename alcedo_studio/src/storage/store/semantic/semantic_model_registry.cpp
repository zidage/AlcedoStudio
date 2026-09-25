//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_model_registry.hpp"

#include <array>
#include <exception>
#include <format>
#include <variant>

#include "semantic_tables.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;
using semantic_tables::Column;
using semantic_tables::SetError;

// SemanticModel columns read by GetModel and ListModels, in MapModelRow order.
constexpr const char* kModelColumns =
    "model_key, model_id, revision, embedding_dim, image_size, engine_id, profile_id, "
    "supported_text_languages_json, prompt_config_hash, asset_manifest_json, active";
const std::array<duckorm::DuckFieldDesc, 11> kModelRowFields = {
    Column("model_key", duckorm::DuckDBType::VARCHAR),
    Column("model_id", duckorm::DuckDBType::VARCHAR),
    Column("revision", duckorm::DuckDBType::VARCHAR),
    Column("embedding_dim", duckorm::DuckDBType::INT32),
    Column("image_size", duckorm::DuckDBType::INT32),
    Column("engine_id", duckorm::DuckDBType::NULLABLE_STRING),
    Column("profile_id", duckorm::DuckDBType::NULLABLE_STRING),
    Column("supported_text_languages_json", duckorm::DuckDBType::NULLABLE_STRING),
    Column("prompt_config_hash", duckorm::DuckDBType::NULLABLE_STRING),
    Column("asset_manifest_json", duckorm::DuckDBType::NULLABLE_STRING),
    Column("active", duckorm::DuckDBType::BOOLEAN)};

auto MapModelRow(const std::vector<duckorm::VarTypes>& row) -> SemanticModelRecord {
  SemanticModelRecord record;
  record.model_key_                     = duckorm::cell_text(row[0]);
  record.model_id_                      = duckorm::cell_text(row[1]);
  record.revision_                      = duckorm::cell_text(row[2]);
  record.embedding_dim_                 = std::get<int32_t>(row[3]);
  record.image_size_                    = std::get<int32_t>(row[4]);
  record.engine_id_                     = duckorm::cell_text(row[5]);
  record.profile_id_                    = duckorm::cell_text(row[6]);
  record.supported_text_languages_json_ = duckorm::cell_text(row[7]);
  record.prompt_config_hash_            = duckorm::cell_text(row[8]);
  record.asset_manifest_json_           = duckorm::cell_text(row[9]);
  record.active_                        = duckorm::cell_bool(row[10]);
  return record;
}

void UpsertModelRow(duckdb_connection conn, const SemanticModelRecord& model) {
  static constexpr std::array<duckorm::DuckFieldDesc, 11> fields = {
      FIELD_AS(SemanticModelRecord, model_key_, "model_key", STRING),
      FIELD_AS(SemanticModelRecord, model_id_, "model_id", STRING),
      FIELD_AS(SemanticModelRecord, revision_, "revision", STRING),
      FIELD_AS(SemanticModelRecord, embedding_dim_, "embedding_dim", INT32),
      FIELD_AS(SemanticModelRecord, image_size_, "image_size", INT32),
      FIELD_AS(SemanticModelRecord, engine_id_, "engine_id", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, profile_id_, "profile_id", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, supported_text_languages_json_, "supported_text_languages_json",
               NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, prompt_config_hash_, "prompt_config_hash", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, asset_manifest_json_, "asset_manifest_json", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, active_, "active", BOOLEAN)};
  duckorm::insert_or_replace(conn, "SemanticModel", &model, fields, fields.size());
}

/// The key of the active semantic model as stored, or std::nullopt when the query fails. An
/// empty string means no model is active.
auto ReadActiveModelKey(duckdb_connection conn) -> std::optional<std::string> {
  try {
    return duckorm::select_string(conn, expr::raw("SELECT model_key FROM SemanticModel WHERE "
                                                  "active = TRUE ORDER BY created_at DESC, "
                                                  "model_key DESC LIMIT 1"))
        .value_or(std::string{});
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
}  // namespace

SemanticModelRegistry::SemanticModelRegistry(Database& db_ctrl) : database_(db_ctrl) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  RefreshActiveModelKey(guard.conn_);
}

void SemanticModelRegistry::RefreshActiveModelKey(duckdb_connection conn) const {
  auto key = ReadActiveModelKey(conn);
  if (!key.has_value()) {
    return;
  }
  std::lock_guard lock(active_model_key_mutex_);
  active_model_key_ = std::move(*key);
}

auto SemanticModelRegistry::UpsertModel(const SemanticModelRecord& model,
                                        std::string*               error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (model.model_key_.empty()) {
    SetError(error, "Semantic model key is empty.");
    return false;
  }
  if (model.model_id_.empty()) {
    SetError(error, "Semantic model id is empty.");
    return false;
  }
  if (!semantic_tables::IsSupportedEmbeddingDim(model.embedding_dim_)) {
    SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                model.embedding_dim_));
    return false;
  }
  if (model.image_size_ <= 0) {
    SetError(error, "Semantic model image size must be positive.");
    return false;
  }

  try {
    if (model.active_) {
      duckorm::Transaction transaction(guard.conn_);
      duckorm::execute(guard.conn_, expr::raw("UPDATE SemanticModel SET active = FALSE"));
      UpsertModelRow(guard.conn_, model);
      transaction.commit();
    } else {
      // Replacing the active model's row with an inactive one leaves no active model.
      UpsertModelRow(guard.conn_, model);
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  RefreshActiveModelKey(guard.conn_);
  return true;
}

auto SemanticModelRegistry::HasModel(const std::string& model_key) const -> bool {
  return GetModelEmbeddingDim(model_key).has_value();
}

auto SemanticModelRegistry::GetModelEmbeddingDim(const std::string& model_key) const
    -> std::optional<int> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    return semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

auto SemanticModelRegistry::GetModelSupportedTextLanguagesJson(const std::string& model_key) const
    -> std::string {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw("SELECT supported_text_languages_json FROM SemanticModel WHERE ");
  query.append(expr::column_eq("model_key", model_key));
  try {
    return duckorm::select_string(guard.conn_, query).value_or(std::string{});
  } catch (const std::exception&) {
    return {};
  }
}

auto SemanticModelRegistry::GetModel(const std::string& model_key, std::string* error) const
    -> std::optional<SemanticModelRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(std::format("SELECT {} FROM SemanticModel WHERE ", kModelColumns));
  query.append(expr::column_eq("model_key", model_key));
  query.append(expr::raw(" LIMIT 1"));
  try {
    const auto rows =
        duckorm::select_by_query(guard.conn_, kModelRowFields, kModelRowFields.size(), query);
    if (rows.empty()) {
      return std::nullopt;
    }
    return MapModelRow(rows.front());
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return std::nullopt;
  }
}

auto SemanticModelRegistry::ActiveModel(std::string* error) const
    -> std::optional<SemanticModelRecord> {
  const auto key = ActiveModelKey();
  if (key.empty()) {
    return std::nullopt;
  }
  return GetModel(key, error);
}

auto SemanticModelRegistry::ActiveModelKey() const -> std::string {
  std::lock_guard lock(active_model_key_mutex_);
  return active_model_key_;
}

auto SemanticModelRegistry::SetActiveModelKey(const std::string& model_key,
                                              std::string*       error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (model_key.empty()) {
    SetError(error, "Semantic model key is empty.");
    return false;
  }
  if (!HasModel(model_key)) {
    SetError(error, "Semantic model is not registered.");
    return false;
  }
  try {
    duckorm::Transaction transaction(guard.conn_);
    duckorm::execute(guard.conn_, expr::raw("UPDATE SemanticModel SET active = FALSE"));
    auto activate = expr::raw("UPDATE SemanticModel SET active = TRUE WHERE ");
    activate.append(expr::column_eq("model_key", model_key));
    duckorm::execute(guard.conn_, activate);
    transaction.commit();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  RefreshActiveModelKey(guard.conn_);
  return true;
}

auto SemanticModelRegistry::ListModels(std::string* error) const
    -> std::vector<SemanticModelRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto rows = duckorm::select_by_query(
        guard.conn_, kModelRowFields, kModelRowFields.size(),
        expr::raw(std::format(
            "SELECT {} FROM SemanticModel ORDER BY created_at DESC, model_key DESC",
            kModelColumns)));
    std::vector<SemanticModelRecord> models;
    models.reserve(rows.size());
    for (const auto& row : rows) {
      models.push_back(MapModelRow(row));
    }
    return models;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }
}

auto SemanticModelRegistry::PurgeModel(const std::string& model_key, std::string* error) const
    -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (model_key.empty()) {
    SetError(error, "Semantic model key is empty.");
    return false;
  }
  // Embedding/prototype tables are dimension-sharded (512 vs 768); a given
  // model_key lives in only one of each pair, so deleting from both is a safe
  // no-op on the empty shard. SemanticModel holds the registry row itself.
  static constexpr std::array<const char*, 6> kTables = {
      "SemanticImageEmbedding", "SemanticImageEmbedding768", "SemanticImageLabel",
      "SemanticLabelPrototype", "SemanticLabelPrototype768", "SemanticModel",
  };
  try {
    duckorm::Transaction transaction(guard.conn_);
    const auto           where = expr::column_eq("model_key", model_key);
    for (const auto* table : kTables) {
      duckorm::remove(guard.conn_, table, where);
    }
    transaction.commit();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  RefreshActiveModelKey(guard.conn_);
  return true;
}

}  // namespace alcedo
