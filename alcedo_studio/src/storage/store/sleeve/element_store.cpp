//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/sleeve/element_store.hpp"

#include <duckdb.h>

#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

auto BuildScopedFileQuery(sl_element_id_t                            folder_id,
                          const std::optional<duckorm::SqlFragment>& extra_filter)
    -> ScopedFileQuery {
  ScopedFileQuery scope;
  std::string     extra_where;
  if (extra_filter.has_value() && !extra_filter->empty()) {
    extra_where         = " AND (" + extra_filter->sql_ + ")";
    scope.binds_.sql_   = extra_filter->sql_;
    scope.binds_.binds_ = extra_filter->binds_;
  }

  if (folder_id == 0) {
    scope.from_where_ = std::format(
        "FROM Element e "
        "JOIN FileImage fi ON fi.file_id = e.id "
        "JOIN Image i ON i.id = fi.image_id "
        "WHERE e.type = {}{}",
        static_cast<uint32_t>(ElementType::FILE), extra_where);
    return scope;
  }

  scope.from_where_ = std::format(
      "FROM FolderContent fc "
      "JOIN Element e ON fc.element_id = e.id "
      "JOIN FileImage fi ON fi.file_id = e.id "
      "JOIN Image i ON i.id = fi.image_id "
      "WHERE fc.folder_id = {} AND e.type = {}{}",
      folder_id, static_cast<uint32_t>(ElementType::FILE), extra_where);
  return scope;
}

namespace {

auto RunGroupByQuery(duckdb_connection conn, const std::string& sql,
                     const duckorm::SqlFragment& binds) -> std::vector<StorageStatsBucket> {
  std::vector<StorageStatsBucket> rows;
  duckdb_result                   result;
  if (duckorm::execute_query(conn, sql, binds, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return rows;
  }

  const auto row_count = duckdb_row_count(&result);
  rows.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    char*              label_raw = duckdb_value_varchar(&result, 0, r);
    StorageStatsBucket row;
    if (label_raw) {
      row.label_ = label_raw;
      duckdb_free(label_raw);
    }
    row.count_ = static_cast<int>(duckdb_value_int64(&result, 1, r));
    rows.push_back(std::move(row));
  }

  duckdb_destroy_result(&result);
  return rows;
}

auto RunScalarInt64(duckdb_connection conn, const std::string& sql,
                    const duckorm::SqlFragment& binds) -> int64_t {
  duckdb_result result;
  if (duckorm::execute_query(conn, sql, binds, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return 0;
  }
  int64_t value = 0;
  if (duckdb_row_count(&result) > 0) {
    value = duckdb_value_int64(&result, 0, 0);
  }
  duckdb_destroy_result(&result);
  return value;
}

auto JoinIds(std::span<const sl_element_id_t> ids) -> std::string {
  std::string out;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += std::to_string(ids[i]);
  }
  return out;
}

void DeleteSemanticAndAiRowsForFiles(duckdb_connection                conn,
                                     std::span<const sl_element_id_t> file_ids) {
  if (file_ids.empty()) {
    return;
  }
  const auto    ids = JoinIds(file_ids);
  duckdb_result result;
  duckdb_query(
      conn, std::format("DELETE FROM SemanticImageEmbedding WHERE file_id IN ({});", ids).c_str(),
      &result);
  duckdb_destroy_result(&result);
  duckdb_query(
      conn,
      std::format("DELETE FROM SemanticImageEmbedding768 WHERE file_id IN ({});", ids).c_str(),
      &result);
  duckdb_destroy_result(&result);
  duckdb_query(conn,
               std::format("DELETE FROM SemanticImageLabel WHERE file_id IN ({});", ids).c_str(),
               &result);
  duckdb_destroy_result(&result);
  // Phase 5f: AI image understanding + rating rows. Routed through the duckorm `remove`
  // path (`DeleteAiAnnotationRowsForFiles`) rather than a hand-written DELETE so the AI
  // ser/deser stays ORM-faithful. This runs on the ElementStore's own connection so
  // the cleanup is atomic with element deletion; rating rows are dropped here too even
  // though they are not part of full-text search, so a re-import does not resurrect an
  // old AI rating under a new image id.
  DeleteAiAnnotationRowsForFiles(conn, file_ids);
}
}  // namespace

/**
 * @brief Construct a new Element Controller:: Element Controller object
 *
 * @param guard
 */
ElementStore::ElementStore(ConnectionGuard&& guard)
    : guard_(std::move(guard)),
      element_mapper_(guard_.conn_),
      element_id_mapper_(guard_.conn_),
      file_mapper_(guard_.conn_),
      folder_mapper_(guard_.conn_),
      history_mapper_(guard_.conn_),
      pipeline_mapper_(guard_.conn_),
      edit_history_mapper_(guard_.conn_) {}
/**
 * @brief Add an element to the database.
 *
 * @param element
 */
void ElementStore::AddElement(const std::shared_ptr<SleeveElement> element) {
  auto db_lock = guard_.Lock();
  InsertElementRows(element);
  element->sync_flag_ = SyncFlag::SYNCED;
}

void ElementStore::InsertElementRows(const std::shared_ptr<SleeveElement>& element) {
  element_mapper_.Insert(element);
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    file_mapper_.Insert({file->element_id_, file->image_id_});
    if (file->GetEditHistory() != nullptr) {
      auto history = file->GetEditHistory();
      history_mapper_.Insert(history);
    }
  } else if (element->type_ == ElementType::FOLDER) {
    auto  folder   = std::static_pointer_cast<SleeveFolder>(element);
    auto& contents = folder->ListElements();
    for (auto& content_id : contents) {
      folder_mapper_.Insert({folder->element_id_, content_id});
    }
  }
}

void ElementStore::AddElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto db_lock = guard_.Lock();
  duckorm::begin_transaction(guard_.conn_);
  try {
    for (const auto& element : elements) {
      InsertElementRows(element);
    }
    duckorm::commit_transaction(guard_.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard_.conn_);
    throw;
  }
  for (const auto& element : elements) {
    element->sync_flag_ = SyncFlag::SYNCED;
  }
}

/**
 * @brief Add a content to a folder in the database.
 *
 * @param folder_id
 * @param content_id
 */
void ElementStore::AddFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id) {
  auto db_lock = guard_.Lock();
  folder_mapper_.Insert({folder_id, content_id});
}

void ElementStore::RemoveFolderContent(sl_element_id_t folder_id, sl_element_id_t content_id) {
  auto db_lock = guard_.Lock();
  folder_mapper_.RemoveFolderContent(folder_id, content_id);
}

/**
 * @brief Get an element by its ID from the database.
 *
 * @param id
 * @return std::shared_ptr<SleeveElement>
 */
auto ElementStore::GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  auto db_lock = guard_.Lock();
  auto result  = element_mapper_.GetElementById(id);
  if (result->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(result);
    try {
      file->image_id_ = file_mapper_.GetBoundImageById(file->element_id_);
    } catch (...) {
      file->image_id_ = 0;
    }
    auto history = history_mapper_.GetEditHistoryByFileId(file->element_id_);
    file->SetEditHistory(history);
  }
  result->SetSyncFlag(SyncFlag::SYNCED);
  return result;
}

/**
 * @brief Get the content of a folder by its ID from the database.
 *
 * @param folder_id
 * @return std::vector<sl_element_id_t>
 */
auto ElementStore::GetFolderContent(const sl_element_id_t folder_id)
    -> std::vector<sl_element_id_t> {
  auto db_lock = guard_.Lock();
  return folder_mapper_.GetFolderContent(folder_id);
}

/**
 * @brief Remove an element by its ID from the database, only be called when the ref count to the
 * element is 0.
 *
 * Low-level row delete: removes the Element row only. It does NOT cascade
 * AI / semantic / history / pipeline / file-binding rows — that orchestration
 * lives at the service layer (SleeveServiceImpl::DeleteElement flows through
 * Write -> Sync -> RemoveElements / RemoveElement(shared_ptr), which call
 * DeleteSemanticAndAiRowsForFiles on the same connection). Keep this primitive
 * a pure row delete so the storage-controller layer does not reach back up
 * into element-fetch / cascade orchestration.
 *
 * @param id
 */
void ElementStore::RemoveElement(const sl_element_id_t id) {
  auto db_lock = guard_.Lock();
  element_mapper_.RemoveById(id);
}

void ElementStore::RemoveElement(const std::shared_ptr<SleeveElement> element) {
  auto db_lock = guard_.Lock();
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    DeleteSemanticAndAiRowsForFiles(guard_.conn_,
                                    std::span<const sl_element_id_t>(&file->element_id_, 1));
    history_mapper_.RemoveById(file->element_id_);
    pipeline_mapper_.RemoveById(file->element_id_);
    file_mapper_.RemoveById(file->element_id_);
    folder_mapper_.RemoveContentById(file->element_id_);
  } else if (element->type_ == ElementType::FOLDER) {
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    folder_mapper_.RemoveById(folder->element_id_);
  }
  element_mapper_.RemoveById(element->element_id_);
}

void ElementStore::RemoveElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto                         db_lock = guard_.Lock();

  std::vector<sl_element_id_t> file_ids;
  std::vector<sl_element_id_t> folder_ids;
  std::vector<sl_element_id_t> element_ids;
  file_ids.reserve(elements.size());
  folder_ids.reserve(elements.size());
  element_ids.reserve(elements.size());

  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(elements.size() * 2 + 1);
  for (const auto& element : elements) {
    if (!element || !seen.insert(element->element_id_).second) {
      continue;
    }

    element_ids.push_back(element->element_id_);
    if (element->type_ == ElementType::FILE) {
      file_ids.push_back(element->element_id_);
    } else if (element->type_ == ElementType::FOLDER) {
      folder_ids.push_back(element->element_id_);
    }
  }

  if (!file_ids.empty()) {
    DeleteSemanticAndAiRowsForFiles(guard_.conn_, file_ids);
    history_mapper_.RemoveByIds(file_ids);
    pipeline_mapper_.RemoveByIds(file_ids);
    file_mapper_.RemoveByIds(file_ids);
    folder_mapper_.RemoveContentByIds(file_ids);
  }
  if (!folder_ids.empty()) {
    folder_mapper_.RemoveByIds(folder_ids);
  }
  if (!element_ids.empty()) {
    element_mapper_.RemoveByIds(element_ids);
  }
}

/**
 * @brief Update an element in the database.
 *
 * @param element
 */
void ElementStore::UpdateElement(const std::shared_ptr<SleeveElement> element) {
  auto db_lock = guard_.Lock();
  UpdateElementRows(element);
  element->sync_flag_ = SyncFlag::SYNCED;
}

void ElementStore::UpdateElementRows(const std::shared_ptr<SleeveElement>& element) {
  element_mapper_.Update(element, element->element_id_);
  if (element->type_ == ElementType::FILE) {
    auto file = std::static_pointer_cast<SleeveFile>(element);
    file_mapper_.Update({file->element_id_, file->image_id_}, file->image_id_);
    if (file->GetEditHistory() != nullptr) {
      history_mapper_.Update(file->GetEditHistory(), file->element_id_);
    }
  } else if (element->type_ == ElementType::FOLDER) {
    auto folder = std::static_pointer_cast<SleeveFolder>(element);
    folder_mapper_.RemoveById(folder->element_id_);
    for (auto& content_id : folder->ListElements()) {
      AddFolderContent(folder->element_id_, content_id);
    }
  }
}

void ElementStore::UpdateElements(std::span<const std::shared_ptr<SleeveElement>> elements) {
  if (elements.empty()) {
    return;
  }
  auto db_lock = guard_.Lock();
  duckorm::begin_transaction(guard_.conn_);
  try {
    for (const auto& element : elements) {
      UpdateElementRows(element);
    }
    duckorm::commit_transaction(guard_.conn_);
  } catch (...) {
    duckorm::rollback_transaction(guard_.conn_);
    throw;
  }
  for (const auto& element : elements) {
    element->sync_flag_ = SyncFlag::SYNCED;
  }
}

auto ElementStore::GetElementsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                               const sl_element_id_t              folder_id)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  auto       db_lock    = guard_.Lock();
  const auto where_frag = FilterSQLCompiler::Compile(filter->GetRoot());
  const auto where      = where_frag.empty() ? std::optional<duckorm::SqlFragment>{} : where_frag;
  // Always resolve ids through the prepared-bind list path so SqlFragment
  // binds stay valid. Then load full elements by primary key.
  const auto ids        = ListFilteredFileIds(folder_id, where);
  std::vector<std::shared_ptr<SleeveElement>> out;
  out.reserve(ids.size());
  for (const auto id : ids) {
    if (auto element = element_mapper_.GetElementById(id)) {
      out.push_back(std::move(element));
    }
  }
  return out;
}

auto ElementStore::GetElementIdsInFolderByFilter(const std::shared_ptr<FilterCombo> filter,
                                                 const sl_element_id_t              folder_id)
    -> std::vector<sl_element_id_t> {
  auto       db_lock    = guard_.Lock();
  const auto where_frag = FilterSQLCompiler::Compile(filter->GetRoot());
  const auto where      = where_frag.empty() ? std::optional<duckorm::SqlFragment>{} : where_frag;
  return ListFilteredFileIds(folder_id, where);
}

auto ElementStore::BuildFolderStats(sl_element_id_t                            folder_id,
                                    const std::optional<duckorm::SqlFragment>& extra_filter,
                                    const std::string& active_semantic_model_key)
    -> FolderStatsView {
  auto            db_lock = guard_.Lock();
  FolderStatsView out;

  const auto      base_query = BuildScopedFileQuery(folder_id, extra_filter);
  const auto&     base_join  = base_query.from_where_;
  const auto&     binds      = base_query.binds_;

  out.total_photo_count_     = static_cast<int>(
      RunScalarInt64(guard_.conn_, std::format("SELECT COUNT(*) {}", base_join), binds));

  out.date_stats_ = RunGroupByQuery(
      guard_.conn_,
      std::format(
          "SELECT TRY_CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::VARCHAR AS d, "
          "COUNT(*) AS c {} "
          "GROUP BY d ORDER BY d DESC",
          base_join),
      binds);

  out.camera_stats_ = RunGroupByQuery(
      guard_.conn_,
      std::format(
          "SELECT COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') "
          "AS m, COUNT(*) AS c {} "
          "GROUP BY m ORDER BY c DESC",
          base_join),
      binds);

  out.lens_stats_ = RunGroupByQuery(
      guard_.conn_,
      std::format(
          "SELECT COALESCE(NULLIF(json_extract_string(i.metadata, '$.Lens'), ''), '(unknown)') "
          "AS l, COUNT(*) AS c {} "
          "GROUP BY l ORDER BY c DESC",
          base_join),
      binds);

  if (!active_semantic_model_key.empty()) {
    duckorm::SqlFragment label_binds = binds;
    label_binds.binds_.push_back(duckorm::BindValue{active_semantic_model_key});
    out.label_stats_ = RunGroupByQuery(
        guard_.conn_,
        std::format("WITH scoped AS (SELECT e.id AS file_id {}) "
                    "SELECT sl.label AS l, COUNT(DISTINCT scoped.file_id) AS c "
                    "FROM scoped "
                    "JOIN SemanticImageLabel sl ON sl.file_id = scoped.file_id "
                    "WHERE sl.model_key = ? AND sl.label IS NOT NULL AND sl.label <> '' "
                    "GROUP BY sl.label ORDER BY c DESC, sl.label",
                    base_join),
        label_binds);
  }

  out.rating_stats_ = RunGroupByQuery(
      guard_.conn_,
      std::format("SELECT json_extract(i.metadata, '$.Rating')::VARCHAR AS r, COUNT(*) AS c {} "
                  "GROUP BY r ORDER BY r DESC",
                  base_join),
      binds);

  return out;
}

auto ElementStore::ListFilesInFolder(sl_element_id_t folder_id) const
    -> std::vector<FileListEntry> {
  auto db_lock = guard_.Lock();
  return ListFilesInFolderPage(folder_id, 0, 0);
}

auto ElementStore::ListFilesInFolderPage(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<duckorm::SqlFragment>& extra_filter) const -> std::vector<FileListEntry> {
  auto                       db_lock = guard_.Lock();
  std::vector<FileListEntry> out;
  const auto                 scope = BuildScopedFileQuery(folder_id, extra_filter);
  auto                       sql =
      std::format("SELECT e.id, fi.image_id, e.element_name {} ORDER BY e.id", scope.from_where_);
  if (limit > 0) {
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
  }

  duckdb_result     result;
  duckdb_connection conn = guard_.conn_;
  if (duckorm::execute_query(conn, sql, scope.binds_, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return out;
  }

  const auto row_count = duckdb_row_count(&result);
  out.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    FileListEntry entry;
    entry.file_id_  = static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, r));
    entry.image_id_ = static_cast<image_id_t>(duckdb_value_int64(&result, 1, r));
    char* name_raw  = duckdb_value_varchar(&result, 2, r);
    if (name_raw) {
      entry.file_name_ = name_raw;
      duckdb_free(name_raw);
    }
    out.push_back(std::move(entry));
  }

  duckdb_destroy_result(&result);
  return out;
}

auto ElementStore::CountFilesInFolder(sl_element_id_t                            folder_id,
                                      const std::optional<duckorm::SqlFragment>& extra_filter) const
    -> size_t {
  auto              db_lock = guard_.Lock();
  const auto        scope   = BuildScopedFileQuery(folder_id, extra_filter);
  duckdb_connection conn    = guard_.conn_;
  return static_cast<size_t>(
      RunScalarInt64(conn, std::format("SELECT COUNT(*) {}", scope.from_where_), scope.binds_));
}

auto ElementStore::ListFilteredFileIds(
    sl_element_id_t folder_id, const std::optional<duckorm::SqlFragment>& extra_filter) const
    -> std::vector<sl_element_id_t> {
  auto                         db_lock = guard_.Lock();
  std::vector<sl_element_id_t> out;
  const auto                   scope = BuildScopedFileQuery(folder_id, extra_filter);
  const auto                   sql = std::format("SELECT e.id {} ORDER BY e.id", scope.from_where_);

  duckdb_result                result;
  duckdb_connection            conn = guard_.conn_;
  if (duckorm::execute_query(conn, sql, scope.binds_, &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return out;
  }

  const auto row_count = duckdb_row_count(&result);
  out.reserve(static_cast<size_t>(row_count));
  for (idx_t r = 0; r < row_count; ++r) {
    out.push_back(static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, r)));
  }

  duckdb_destroy_result(&result);
  return out;
}

auto ElementStore::GetPipelineByElementId(const sl_element_id_t element_id)
    -> std::shared_ptr<CPUPipelineExecutor> {
  auto db_lock = guard_.Lock();
  return pipeline_mapper_.GetPipelineParamByFileId(element_id);
}

auto ElementStore::UpdatePipelineByElementId(const sl_element_id_t                      element_id,
                                             const std::shared_ptr<CPUPipelineExecutor> pipeline)
    -> void {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.UpdatePipelineParamByFileId(element_id, pipeline);
}

auto ElementStore::GetPipelineJsonByElementId(sl_element_id_t element_id)
    -> std::optional<nlohmann::json> {
  auto db_lock = guard_.Lock();
  return pipeline_mapper_.GetPipelineJsonByFileId(element_id);
}

void ElementStore::UpdatePipelineJsonByElementId(sl_element_id_t       element_id,
                                                 const nlohmann::json& document) {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.UpdatePipelineJsonByFileId(element_id, document);
}

auto ElementStore::RemovePipelineByElementId(const sl_element_id_t element_id) -> void {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.RemoveById(element_id);
}

auto ElementStore::RemovePipelinesByElementIds(std::span<const sl_element_id_t> element_ids)
    -> void {
  auto db_lock = guard_.Lock();
  pipeline_mapper_.RemoveByIds(element_ids);
}

auto ElementStore::GetEditHistoryByFileId(const sl_element_id_t file_id)
    -> std::shared_ptr<EditHistory> {
  auto db_lock = guard_.Lock();
  return history_mapper_.GetEditHistoryByFileId(file_id);
}

auto ElementStore::UpdateEditHistoryByFileId(const sl_element_id_t              file_id,
                                             const std::shared_ptr<EditHistory> history) -> void {
  auto db_lock = guard_.Lock();
  history_mapper_.Update(history, file_id);
}

auto ElementStore::RemoveEditHistoryByFileId(const sl_element_id_t file_id) -> void {
  auto db_lock = guard_.Lock();
  history_mapper_.RemoveById(file_id);
}

auto ElementStore::RemoveEditHistoriesByFileIds(std::span<const sl_element_id_t> file_ids) -> void {
  auto db_lock = guard_.Lock();
  history_mapper_.RemoveByIds(file_ids);
}

namespace {
auto ToRecoveryMapperParams(const EditorRecoveryMetadata& metadata)
    -> EditorRecoveryMetadataMapperParams {
  EditorRecoveryMetadataMapperParams params;
  params.file_id            = metadata.element_id;
  params.version_id         = std::make_unique<std::string>(metadata.version_id.ToString());
  params.journal_generation = metadata.journal_generation;
  params.materialized_operation_sequence = metadata.materialized_operation_sequence;
  params.transaction_chain_hash =
      std::make_unique<std::string>(metadata.transaction_chain_hash.ToString());
  params.pipeline_parameter_hash =
      std::make_unique<std::string>(metadata.pipeline_parameter_hash.ToString());
  return params;
}

auto FromRecoveryMapperParams(EditorRecoveryMetadataMapperParams&& params)
    -> EditorRecoveryMetadata {
  EditorRecoveryMetadata metadata;
  metadata.element_id = params.file_id;
  if (params.version_id && !params.version_id->empty()) {
    metadata.version_id = Hash128::FromString(*params.version_id);
  }
  metadata.journal_generation              = params.journal_generation;
  metadata.materialized_operation_sequence = params.materialized_operation_sequence;
  if (params.transaction_chain_hash && !params.transaction_chain_hash->empty()) {
    metadata.transaction_chain_hash = Hash128::FromString(*params.transaction_chain_hash);
  }
  if (params.pipeline_parameter_hash && !params.pipeline_parameter_hash->empty()) {
    metadata.pipeline_parameter_hash = Hash128::FromString(*params.pipeline_parameter_hash);
  }
  return metadata;
}
}  // namespace

auto ElementStore::MaterializeEditorState(const std::shared_ptr<EditHistory>&         history,
                                          const std::shared_ptr<CPUPipelineExecutor>& pipeline,
                                          const EditorRecoveryMetadata& recovery_metadata,
                                          std::string*                  error) -> bool {
  if (!history || !pipeline) {
    if (error) {
      *error = "MaterializeEditorState requires history and pipeline";
    }
    return false;
  }
  if (history->GetBoundImage() != recovery_metadata.element_id ||
      pipeline->GetBoundFile() != recovery_metadata.element_id) {
    if (error) {
      *error = "MaterializeEditorState identity mismatch";
    }
    return false;
  }

  auto db_lock = guard_.Lock();
  try {
    if (duckorm::begin_transaction(guard_.conn_) != DuckDBSuccess) {
      if (error) {
        *error = "failed to begin editor materialize transaction";
      }
      return false;
    }
    try {
      history_mapper_.UpdateEditHistory(history);
      pipeline_mapper_.UpdatePipelineParamByFileId(recovery_metadata.element_id, pipeline);
      EditorRecoveryMetadataMapper mapper(guard_.conn_);
      mapper.Update(recovery_metadata.element_id, ToRecoveryMapperParams(recovery_metadata));
      if (materialize_pre_commit_hook_) {
        // Test seam: throw here to prove the three writes roll back together.
        materialize_pre_commit_hook_();
      }
      if (duckorm::commit_transaction(guard_.conn_) != DuckDBSuccess) {
        duckorm::rollback_transaction(guard_.conn_);
        if (error) {
          *error = "failed to commit editor materialize transaction";
        }
        return false;
      }
    } catch (const std::exception& ex) {
      duckorm::rollback_transaction(guard_.conn_);
      if (error) {
        *error = ex.what();
      }
      return false;
    }
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  }
  return true;
}

auto ElementStore::GetEditorRecoveryMetadata(sl_element_id_t file_id)
    -> std::optional<EditorRecoveryMetadata> {
  auto                         db_lock = guard_.Lock();
  EditorRecoveryMetadataMapper mapper(guard_.conn_);
  auto                         rows = mapper.Get(std::format("file_id={}", file_id).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  if (rows.size() > 1) {
    throw std::runtime_error("multiple EditorRecoveryMetadata rows for file_id " +
                             std::to_string(file_id));
  }
  return FromRecoveryMapperParams(std::move(rows.front()));
}

};  // namespace alcedo
