//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_label_store.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <variant>

#include "semantic_tables.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/sleeve/element_store.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;
using semantic_tables::Column;
using semantic_tables::LabelPrototypeTable;
using semantic_tables::SetError;

void InsertOrReplaceLabelPrototype(duckdb_connection                   conn,
                                   const SemanticLabelPrototypeRecord& record, int model_dim) {
  static constexpr std::array<duckorm::DuckFieldDesc, 4> fields = {
      FIELD_AS(SemanticLabelPrototypeRecord, model_key_, "model_key", STRING),
      FIELD_AS(SemanticLabelPrototypeRecord, label_, "label", STRING),
      FIELD_AS(SemanticLabelPrototypeRecord, prompt_config_hash_, "prompt_config_hash", STRING),
      FIELD_AS(SemanticLabelPrototypeRecord, embedding_, "embedding", FLOAT_ARRAY)};
  duckorm::insert_or_replace(conn, LabelPrototypeTable(model_dim), &record, fields,
                             fields.size());
}
}  // namespace

SemanticLabelStore::SemanticLabelStore(Database& db_ctrl) : database_(db_ctrl) {}

auto SemanticLabelStore::UpsertLabelPrototype(const SemanticLabelPrototypeRecord& record,
                                              std::string* error) const -> bool {
  return UpsertLabelPrototypes(std::span<const SemanticLabelPrototypeRecord>(&record, 1), error);
}

auto SemanticLabelStore::UpsertLabelPrototypes(
    std::span<const SemanticLabelPrototypeRecord> records, std::string* error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (records.empty()) {
    return true;
  }
  try {
    duckorm::Transaction transaction(guard.conn_);
    for (const auto& record : records) {
      if (record.model_key_.empty()) {
        SetError(error, "Semantic label prototype model key is empty.");
        return false;
      }
      if (record.label_.empty()) {
        SetError(error, "Semantic label prototype label is empty.");
        return false;
      }
      if (record.prompt_config_hash_.empty()) {
        SetError(error, "Semantic label prototype prompt config hash is empty.");
        return false;
      }
      const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, record.model_key_);
      if (!model_dim.has_value()) {
        SetError(error, "Semantic model is not registered.");
        return false;
      }
      if (!semantic_tables::ValidateEmbedding(record.embedding_, *model_dim, error)) {
        return false;
      }
      InsertOrReplaceLabelPrototype(guard.conn_, record, *model_dim);
    }
    transaction.commit();
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
}

auto SemanticLabelStore::LoadLabelPrototypes(const std::string& model_key,
                                             const std::string& prompt_config_hash,
                                             std::string*       error) const
    -> std::vector<SemanticGenerationLabelPrototype> {
  auto                                          guard   = database_.GetConnectionGuard();
  auto                                          db_lock = guard.Lock();
  std::vector<SemanticGenerationLabelPrototype> out;
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return out;
    }
    if (!semantic_tables::IsSupportedEmbeddingDim(*model_dim)) {
      SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                  *model_dim));
      return out;
    }

    // One column for each embedding element: the select decoder has no array type.
    std::string                         columns;
    std::vector<duckorm::DuckFieldDesc> fields = {Column("label", duckorm::DuckDBType::VARCHAR)};
    fields.reserve(static_cast<size_t>(*model_dim) + 1);
    for (int i = 1; i <= *model_dim; ++i) {
      columns += std::format(", embedding[{}]", i);
      fields.push_back(Column("embedding", duckorm::DuckDBType::DOUBLE));
    }
    auto query = expr::raw(
        std::format("SELECT label{} FROM {} WHERE ", columns, LabelPrototypeTable(*model_dim)));
    query.append(expr::and_({expr::column_eq("model_key", model_key),
                             expr::column_eq("prompt_config_hash", prompt_config_hash)}));
    query.append(expr::raw(" ORDER BY label"));

    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    out.reserve(rows.size());
    for (const auto& row : rows) {
      SemanticGenerationLabelPrototype prototype;
      prototype.label = duckorm::cell_text(row[0]);
      prototype.embedding.resize(static_cast<size_t>(*model_dim), 0.0F);
      for (int i = 0; i < *model_dim; ++i) {
        prototype.embedding[static_cast<size_t>(i)] =
            static_cast<float>(std::get<double>(row[static_cast<size_t>(i) + 1]));
      }
      out.push_back(std::move(prototype));
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    out.clear();
  }
  return out;
}

auto SemanticLabelStore::CountLabelPrototypes(const std::string& model_key,
                                              const std::string& prompt_config_hash) const
    -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      return 0;
    }
    auto query =
        expr::raw(std::format("SELECT COUNT(*) FROM {} WHERE ", LabelPrototypeTable(*model_dim)));
    query.append(expr::and_({expr::column_eq("model_key", model_key),
                             expr::column_eq("prompt_config_hash", prompt_config_hash)}));
    return semantic_tables::CountOrZero(guard.conn_, query);
  } catch (const std::exception&) {
    return 0;
  }
}

auto SemanticLabelStore::CountLabelQueries(const std::string& prompt_config_hash) const
    -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw("SELECT COUNT(*) FROM SemanticLabelQuery WHERE ");
  query.append(expr::column_eq("prompt_config_hash", prompt_config_hash));
  return semantic_tables::CountOrZero(guard.conn_, query);
}

auto SemanticLabelStore::ListLabelQueries(const std::string& prompt_config_hash,
                                          std::string*       error) const
    -> std::vector<SemanticLabelQueryRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      "SELECT prompt_config_hash, label, query_text FROM SemanticLabelQuery WHERE ");
  query.append(expr::column_eq("prompt_config_hash", prompt_config_hash));
  query.append(expr::raw(" ORDER BY label"));
  static const std::array<duckorm::DuckFieldDesc, 3> fields = {
      Column("prompt_config_hash", duckorm::DuckDBType::VARCHAR),
      Column("label", duckorm::DuckDBType::VARCHAR),
      Column("query_text", duckorm::DuckDBType::VARCHAR)};

  std::vector<SemanticLabelQueryRecord> out;
  try {
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    out.reserve(rows.size());
    for (const auto& row : rows) {
      out.push_back(SemanticLabelQueryRecord{.prompt_config_hash_ = duckorm::cell_text(row[0]),
                                             .label_              = duckorm::cell_text(row[1]),
                                             .query_text_         = duckorm::cell_text(row[2])});
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    out.clear();
  }
  return out;
}

auto SemanticLabelStore::GetImageLabelForFile(sl_element_id_t    file_id,
                                              const std::string& model_key,
                                              std::string*       error) const
    -> std::optional<SemanticImageLabelRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      "SELECT file_id, model_key, label, score, second_label, second_score, margin, confident, "
        "top_scores FROM SemanticImageLabel WHERE ");
  query.append(expr::and_({expr::column_eq("file_id", static_cast<int64_t>(file_id)),
                           expr::column_eq("model_key", model_key)}));
  static const std::array<duckorm::DuckFieldDesc, 9> fields = {
      Column("file_id", duckorm::DuckDBType::INT64),
      Column("model_key", duckorm::DuckDBType::VARCHAR),
      Column("label", duckorm::DuckDBType::VARCHAR),
      Column("score", duckorm::DuckDBType::DOUBLE),
      Column("second_label", duckorm::DuckDBType::NULLABLE_STRING),
      Column("second_score", duckorm::DuckDBType::NULLABLE_DOUBLE),
      Column("margin", duckorm::DuckDBType::DOUBLE),
      Column("confident", duckorm::DuckDBType::BOOLEAN),
      Column("top_scores", duckorm::DuckDBType::NULLABLE_STRING)};
  try {
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    if (rows.empty()) {
      return std::nullopt;
    }
    const auto&              row = rows.front();
    SemanticImageLabelRecord record;
    record.file_id_         = static_cast<sl_element_id_t>(std::get<int64_t>(row[0]));
    record.model_key_       = duckorm::cell_text(row[1]);
    record.label_           = duckorm::cell_text(row[2]);
    record.score_           = std::get<double>(row[3]);
    record.second_label_    = duckorm::cell_text(row[4]);
    record.second_score_    = std::get<std::optional<double>>(row[5]);
    record.margin_          = std::get<double>(row[6]);
    record.confident_       = duckorm::cell_bool(row[7]);
    record.top_scores_json_ = duckorm::cell_text(row[8]);
    return record;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return std::nullopt;
  }
}

auto SemanticLabelStore::CountImageLabelsForFile(sl_element_id_t    file_id,
                                                 const std::string& model_key) const -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw("SELECT COUNT(*) FROM SemanticImageLabel WHERE ");
  query.append(expr::and_({expr::column_eq("file_id", static_cast<int64_t>(file_id)),
                           expr::column_eq("model_key", model_key)}));
  return semantic_tables::CountOrZero(guard.conn_, query);
}

auto SemanticLabelStore::CountImageLabelsInFolder(sl_element_id_t    folder_id,
                                                  const std::string& model_key) const -> size_t {
  if (model_key.empty()) {
    return 0;
  }
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(folder_id);
  auto       query   = expr::raw(std::format(
      "SELECT COUNT(*) FROM SemanticImageLabel sl "
                "JOIN (SELECT e.id AS file_id {}) scoped ON scoped.file_id = sl.file_id WHERE ",
      scope.from_where_));
  query.append(expr::column_eq("sl.model_key", model_key));
  query.append(expr::raw(" AND sl.label IS NOT NULL AND sl.label <> ''"));
  return semantic_tables::CountOrZero(guard.conn_, query);
}

}  // namespace alcedo
