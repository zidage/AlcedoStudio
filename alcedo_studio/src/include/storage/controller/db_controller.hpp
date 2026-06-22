//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <duckdb.h>

#include <codecvt>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "controller_types.hpp"
#include "type/type.hpp"
#include "utils/queue/queue.hpp"

namespace alcedo {
class DBController {
 private:
  duckdb_database                       db_;
  std::shared_ptr<std::recursive_mutex> db_lock_;

  file_path_t                           db_path_;

  bool                                  initialized_;

  constexpr static const char*          init_table_query =
      "CREATE TABLE Sleeve (id BIGINT PRIMARY KEY);"
      "CREATE TABLE Image (id BIGINT PRIMARY KEY, image_path TEXT, file_name TEXT, type INTEGER, "
      "metadata JSON);"
      "CREATE TABLE SleeveRoot (id BIGINT PRIMARY KEY);"
      "CREATE TABLE Element (id BIGINT PRIMARY KEY, type INTEGER, element_name TEXT, added_time "
      "TIMESTAMP, modified_time "
      "TIMESTAMP, "
      "ref_count BIGINT);"
      "CREATE TABLE FolderContent (folder_id BIGINT NOT NULL, element_id BIGINT NOT NULL, "
      "PRIMARY KEY(folder_id, element_id));"
      "CREATE INDEX idx_folder_content_folder ON FolderContent(folder_id);"
      "CREATE INDEX idx_folder_content_element ON FolderContent(element_id);"
      "CREATE TABLE FileImage (file_id BIGINT, image_id BIGINT);"
      "CREATE TABLE ComboFolder (combo_id BIGINT, folder_id BIGINT);"
      "CREATE TABLE Filter (combo_id BIGINT, type INTEGER, data JSON);"
      "CREATE TABLE EditHistory (file_id BIGINT PRIMARY KEY, history JSON);"
      "CREATE TABLE Version (hash BIGINT PRIMARY KEY, history_id BIGINT, parent_hash BIGINT, "
      "content "
      "JSON);"
      "CREATE TABLE PipelineParam(file_id BIGINT PRIMARY KEY, param_json JSON);";

  constexpr static const char* semantic_table_query =
      "CREATE TABLE IF NOT EXISTS SemanticModel ("
      "model_key VARCHAR PRIMARY KEY,"
      "model_id VARCHAR NOT NULL,"
      "revision VARCHAR NOT NULL,"
      "embedding_dim INTEGER NOT NULL,"
      "image_size INTEGER NOT NULL,"
      "engine_id VARCHAR,"
      "profile_id VARCHAR,"
      "supported_text_languages_json JSON,"
      "prompt_config_hash VARCHAR,"
      "asset_manifest_json JSON,"
      "active BOOLEAN NOT NULL DEFAULT FALSE,"
      "created_at TIMESTAMP DEFAULT current_timestamp);"
      "CREATE TABLE IF NOT EXISTS SemanticImageEmbedding ("
      "file_id BIGINT NOT NULL,"
      "image_id BIGINT NOT NULL,"
      "model_key VARCHAR NOT NULL,"
      "embedding FLOAT[512] NOT NULL,"
      "embedding_dim INTEGER NOT NULL,"
      "thumbnail_resolution INTEGER NOT NULL,"
      "generated_at TIMESTAMP DEFAULT current_timestamp,"
      "status VARCHAR NOT NULL,"
      "error VARCHAR,"
      "PRIMARY KEY(file_id, model_key));"
      "CREATE INDEX IF NOT EXISTS idx_semantic_embedding_model_file "
      "ON SemanticImageEmbedding(model_key, file_id);"
      "CREATE TABLE IF NOT EXISTS SemanticImageEmbedding768 ("
      "file_id BIGINT NOT NULL,"
      "image_id BIGINT NOT NULL,"
      "model_key VARCHAR NOT NULL,"
      "embedding FLOAT[768] NOT NULL,"
      "embedding_dim INTEGER NOT NULL,"
      "thumbnail_resolution INTEGER NOT NULL,"
      "generated_at TIMESTAMP DEFAULT current_timestamp,"
      "status VARCHAR NOT NULL,"
      "error VARCHAR,"
      "PRIMARY KEY(file_id, model_key));"
      "CREATE INDEX IF NOT EXISTS idx_semantic_embedding768_model_file "
      "ON SemanticImageEmbedding768(model_key, file_id);"
      "CREATE TABLE IF NOT EXISTS SemanticImageLabel ("
      "file_id BIGINT NOT NULL,"
      "model_key VARCHAR NOT NULL,"
      "label VARCHAR NOT NULL,"
      "score DOUBLE NOT NULL,"
      "second_label VARCHAR,"
      "second_score DOUBLE,"
      "margin DOUBLE,"
      "confident BOOLEAN NOT NULL,"
      "top_scores JSON,"
      "updated_at TIMESTAMP DEFAULT current_timestamp,"
      "PRIMARY KEY(file_id, model_key));"
      "CREATE INDEX IF NOT EXISTS idx_semantic_label_model_label "
      "ON SemanticImageLabel(model_key, label);"
      "CREATE TABLE IF NOT EXISTS SemanticLabelQuery ("
      "prompt_config_hash VARCHAR NOT NULL,"
      "label VARCHAR NOT NULL,"
      "query_text VARCHAR NOT NULL,"
      "created_at TIMESTAMP DEFAULT current_timestamp,"
      "PRIMARY KEY(prompt_config_hash, label));"
      "CREATE TABLE IF NOT EXISTS SemanticLabelPrototype ("
      "model_key VARCHAR NOT NULL,"
      "label VARCHAR NOT NULL,"
      "prompt_config_hash VARCHAR NOT NULL,"
      "embedding FLOAT[512] NOT NULL,"
      "PRIMARY KEY(model_key, label, prompt_config_hash));"
      "CREATE TABLE IF NOT EXISTS SemanticLabelPrototype768 ("
      "model_key VARCHAR NOT NULL,"
      "label VARCHAR NOT NULL,"
      "prompt_config_hash VARCHAR NOT NULL,"
      "embedding FLOAT[768] NOT NULL,"
      "PRIMARY KEY(model_key, label, prompt_config_hash));";

  constexpr static const char* semantic_migration_query =
      "ALTER TABLE SemanticModel ADD COLUMN IF NOT EXISTS engine_id VARCHAR;"
      "ALTER TABLE SemanticModel ADD COLUMN IF NOT EXISTS profile_id VARCHAR;"
      "ALTER TABLE SemanticModel ADD COLUMN IF NOT EXISTS supported_text_languages_json JSON;"
      "ALTER TABLE SemanticModel ADD COLUMN IF NOT EXISTS active BOOLEAN DEFAULT FALSE;"
      "UPDATE SemanticModel SET active = TRUE "
      "WHERE model_key = ("
      "SELECT model_key FROM SemanticModel "
      "WHERE NOT EXISTS (SELECT 1 FROM SemanticModel WHERE active = TRUE) "
      "ORDER BY created_at DESC, model_key DESC LIMIT 1);";

  void SeedSemanticLabelQueries(duckdb_connection conn);

 public:
  explicit DBController(file_path_t& db_path);
  ~DBController();

  void InitializeDB();

  auto GetConnectionGuard() -> ConnectionGuard;
};
};  // namespace alcedo
