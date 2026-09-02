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

#include "storage/store/store_types.hpp"
#include "type/type.hpp"
#include "utils/queue/queue.hpp"

namespace alcedo {
class Database {
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
      "CREATE TABLE PipelineParam(file_id BIGINT PRIMARY KEY, param_json JSON);"
      // Mini-Git commit graph: immutable commits, Version refs, and per-image edit state.
      "CREATE TABLE EditCommit ("
      "commit_hash VARCHAR PRIMARY KEY,"
      "root_id VARCHAR NOT NULL,"
      "first_parent_hash VARCHAR,"
      "second_parent_hash VARCHAR,"
      "created_at_ns UBIGINT NOT NULL,"
      "kind INTEGER NOT NULL,"
      "edit_payload JSON NOT NULL);"
      "CREATE INDEX idx_edit_commit_first_parent ON EditCommit(first_parent_hash);"
      "CREATE INDEX idx_edit_commit_second_parent ON EditCommit(second_parent_hash);"
      "CREATE INDEX idx_edit_commit_root ON EditCommit(root_id);"
      "CREATE TABLE VersionRef ("
      "version_id VARCHAR PRIMARY KEY,"
      "element_id BIGINT NOT NULL,"
      "display_name VARCHAR NOT NULL,"
      "head_commit_hash VARCHAR,"
      "created_at_unix BIGINT NOT NULL,"
      "updated_at_unix BIGINT NOT NULL);"
      "CREATE INDEX idx_version_ref_element ON VersionRef(element_id);"
      "CREATE TABLE ImageEditState ("
      "element_id BIGINT PRIMARY KEY,"
      "root_id VARCHAR NOT NULL,"
      "active_version_id VARCHAR NOT NULL,"
      "materialized_head_commit_hash VARCHAR,"
      "materialized_transaction_chain_hash VARCHAR NOT NULL,"
      "serialized_pipeline_state JSON,"
      "project_schema_version INTEGER NOT NULL);"
      "CREATE TABLE PipelineRoot ("
      "root_id VARCHAR PRIMARY KEY,"
      "element_id BIGINT UNIQUE NOT NULL,"
      "serialized_pipeline_state JSON NOT NULL);";

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

  // Phase 5f: AI image understanding + rating annotation tables. CREATE IF NOT EXISTS
  // so this runs migration-safely on BOTH the fresh-DB and existing-DB paths (see
  // InitializeDB). Foreign key is file_id = the Sleeve element id / inode, the same key
  // the CLIP embeddings bind to (not the image id). PRIMARY KEY (file_id, task_id)
  // makes insert_or_replace enforce "at most one row per pair", hence at most one
  // active-for-search understanding per (file_id, task_id). Every text column is NOT
  // NULL DEFAULT '' so the duckorm select path (which would turn a NULL cell into
  // string(nullptr) and crash) never sees a NULL text value; updated_at is excluded from
  // inserts and re-stamped on each upsert, so it doubles as last-write time. Rating is
  // NOT part of full-text search: it is stored here only, and the search-document
  // builder (sleeve_filter_service) intentionally reads the understanding table alone.
  // Editor recovery metadata for atomic history/pipeline materialization.
  // CREATE IF NOT EXISTS so existing project databases gain the table in place.
  constexpr static const char* editor_recovery_metadata_table_query =
      "CREATE TABLE IF NOT EXISTS EditorRecoveryMetadata ("
      "file_id BIGINT PRIMARY KEY,"
      "version_id VARCHAR NOT NULL DEFAULT '',"
      "journal_generation UBIGINT NOT NULL DEFAULT 0,"
      "materialized_operation_sequence UBIGINT NOT NULL DEFAULT 0,"
      "transaction_chain_hash VARCHAR NOT NULL DEFAULT '',"
      "pipeline_parameter_hash VARCHAR NOT NULL DEFAULT '');";

  // Mini-Git commit-graph tables. CREATE IF NOT EXISTS keeps the existing-DB open path
  // aligned with fresh projects; incompatible older project packages are rejected by
  // project_file_version 0.4.0 before history is loaded. Root and checkpoint JSON
  // store full PipelineDocument envelopes, not CPU parameter tables.
  constexpr static const char* commit_graph_table_query =
      "CREATE TABLE IF NOT EXISTS EditCommit ("
      "commit_hash VARCHAR PRIMARY KEY,"
      "root_id VARCHAR NOT NULL,"
      "first_parent_hash VARCHAR,"
      "second_parent_hash VARCHAR,"
      "created_at_ns UBIGINT NOT NULL,"
      "kind INTEGER NOT NULL,"
      "edit_payload JSON NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_edit_commit_first_parent ON EditCommit(first_parent_hash);"
      "CREATE INDEX IF NOT EXISTS idx_edit_commit_second_parent ON EditCommit(second_parent_hash);"
      "CREATE INDEX IF NOT EXISTS idx_edit_commit_root ON EditCommit(root_id);"
      "CREATE TABLE IF NOT EXISTS VersionRef ("
      "version_id VARCHAR PRIMARY KEY,"
      "element_id BIGINT NOT NULL,"
      "display_name VARCHAR NOT NULL,"
      "head_commit_hash VARCHAR,"
      "created_at_unix BIGINT NOT NULL,"
      "updated_at_unix BIGINT NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_version_ref_element ON VersionRef(element_id);"
      "CREATE TABLE IF NOT EXISTS ImageEditState ("
      "element_id BIGINT PRIMARY KEY,"
      "root_id VARCHAR NOT NULL,"
      "active_version_id VARCHAR NOT NULL,"
      "materialized_head_commit_hash VARCHAR,"
      "materialized_transaction_chain_hash VARCHAR NOT NULL,"
      "serialized_pipeline_state JSON,"
      "project_schema_version INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS PipelineRoot ("
      "root_id VARCHAR PRIMARY KEY,"
      "element_id BIGINT UNIQUE NOT NULL,"
      "serialized_pipeline_state JSON NOT NULL);";

  constexpr static const char* ai_annotation_table_query =
      "CREATE TABLE IF NOT EXISTS AiImageUnderstanding ("
      "file_id BIGINT NOT NULL,"
      "task_id VARCHAR NOT NULL DEFAULT '',"
      "provider_id VARCHAR NOT NULL DEFAULT '',"
      "model_id VARCHAR NOT NULL DEFAULT '',"
      "prompt_profile_id VARCHAR NOT NULL DEFAULT '',"
      "rendition_kind VARCHAR NOT NULL DEFAULT '',"
      "caption VARCHAR NOT NULL DEFAULT '',"
      "tags_json VARCHAR NOT NULL DEFAULT '',"
      "scene VARCHAR NOT NULL DEFAULT '',"
      "confidence DOUBLE NOT NULL DEFAULT 0.0,"
      "active BOOLEAN NOT NULL DEFAULT TRUE,"
      "updated_at TIMESTAMP DEFAULT current_timestamp,"
      "PRIMARY KEY (file_id, task_id));"
      "CREATE INDEX IF NOT EXISTS idx_ai_understanding_file_active "
      "ON AiImageUnderstanding(file_id, active);"
      "CREATE TABLE IF NOT EXISTS AiImageFtsDocument ("
      "file_id BIGINT PRIMARY KEY,"
      "body VARCHAR NOT NULL DEFAULT '',"
      "updated_at TIMESTAMP DEFAULT current_timestamp);"
      "CREATE TABLE IF NOT EXISTS AiImageRating ("
      "file_id BIGINT NOT NULL,"
      "task_id VARCHAR NOT NULL DEFAULT '',"
      "provider_id VARCHAR NOT NULL DEFAULT '',"
      "model_id VARCHAR NOT NULL DEFAULT '',"
      "prompt_profile_id VARCHAR NOT NULL DEFAULT '',"
      "rendition_kind VARCHAR NOT NULL DEFAULT '',"
      "rating INTEGER NOT NULL DEFAULT 0,"
      "rubric_id VARCHAR NOT NULL DEFAULT '',"
      "rubric_version VARCHAR NOT NULL DEFAULT '',"
      "reasons VARCHAR NOT NULL DEFAULT '',"
      "active BOOLEAN NOT NULL DEFAULT TRUE,"
      "updated_at TIMESTAMP DEFAULT current_timestamp,"
      "PRIMARY KEY (file_id, task_id));"
      "CREATE INDEX IF NOT EXISTS idx_ai_rating_file_active "
      "ON AiImageRating(file_id, active);";

  void SeedSemanticLabelQueries(duckdb_connection conn);

 public:
  explicit Database(file_path_t& db_path);
  ~Database();

  void InitializeDB();

  auto GetConnectionGuard() -> ConnectionGuard;
};
};  // namespace alcedo
