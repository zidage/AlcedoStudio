// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "storage/store/sleeve/project_maintenance_store.hpp"

#include <format>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "edit/history/pipeline_document_checkpoint.hpp"
#include "edit/mask/ai_mask_asset_references.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/image/source_file_mapper.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/mapper/sleeve/element/file_mapper.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/store/mask_asset/mask_asset_store.hpp"
#include "storage/store/semantic/semantic_embedding_store.hpp"
#include "utils/file/source_file.hpp"
#include "utils/file/source_image_status.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
/// Read a bounded projection, keeping query errors explicit and NULL cells intact.
auto ReadRows(duckdb_connection& connection, const std::string& query) -> nlohmann::json {
  return nlohmann::json::parse(
      duckorm::select_string(
          connection,
          duckorm::expr::raw("SELECT coalesce(to_json(list(t)), '[]') FROM (" + query + ") t"))
          .value());
}
}  // namespace

auto ProjectMaintenanceStore::ListEntryIds(sl_element_id_t after) -> std::vector<sl_element_id_t> {
  auto                         guard = database_.GetConnectionGuard();
  auto                         lock  = guard.Lock();
  std::vector<sl_element_id_t> result;
  for (const auto& row : ReadRows(
           guard.conn_,
           std::format(
               "SELECT id FROM (SELECT id FROM Element WHERE type=0 UNION SELECT file_id AS id "
               "FROM FileImage UNION SELECT element_id FROM ImageEditState WHERE NOT EXISTS "
               "(SELECT 1 FROM Element WHERE id=element_id AND type=0) UNION SELECT element_id "
               "FROM VersionRef WHERE NOT EXISTS (SELECT 1 FROM Element WHERE id=element_id AND "
               "type=0) "
               "UNION SELECT element_id FROM PipelineRoot WHERE NOT EXISTS "
               "(SELECT 1 FROM Element WHERE id=element_id AND type=0) UNION SELECT file_id "
               "FROM PipelineParam WHERE NOT EXISTS (SELECT 1 FROM Element WHERE id=file_id AND "
               "type=0) "
               "UNION SELECT element_id FROM FolderContent WHERE NOT EXISTS "
               "(SELECT 1 FROM Element WHERE id=element_id)) candidates "
               "WHERE id>{} AND NOT EXISTS (SELECT 1 FROM Element mounted "
               "WHERE mounted.id=candidates.id AND mounted.mount_id IS NOT NULL) "
               "ORDER BY id LIMIT 64",
               after))) {
    result.push_back(row.at("id").get<sl_element_id_t>());
  }
  return result;
}

auto ProjectMaintenanceStore::ListUnboundImages(image_id_t after)
    -> std::vector<std::pair<image_id_t, image_path_t>> {
  auto                                             guard = database_.GetConnectionGuard();
  auto                                             lock  = guard.Lock();
  std::vector<std::pair<image_id_t, image_path_t>> result;
  for (const auto& row : ReadRows(
           guard.conn_,
           std::format("SELECT id,image_path FROM Image WHERE id>{} AND NOT EXISTS "
                       "(SELECT 1 FROM FileImage WHERE image_id=Image.id) ORDER BY id LIMIT 64",
                       after))) {
    result.emplace_back(row.at("id").get<image_id_t>(),
                        row["image_path"].is_string()
                            ? std::filesystem::u8path(row["image_path"].get<std::string>())
                            : image_path_t{});
  }
  return result;
}

auto ProjectMaintenanceStore::Inspect(sl_element_id_t id) -> MaintenanceEntryEvidence {
  MaintenanceEntryEvidence result;
  result.entry_id = id;
  auto guard      = database_.GetConnectionGuard();
  auto lock       = guard.Lock();
  try {
    const auto rows = ReadRows(
        guard.conn_,
        std::format("SELECT e.id,e.type,e.mount_id,epoch_us(e.added_time) AS "
                    "added_time,i.image_path,i.rating,"
                    "i.metadata,i.color_labels,i.delete_mark,"
                    "EXISTS(SELECT 1 FROM SourceFileChanged c WHERE c.element_id=requested.id) "
                    "AS source_changed,"
                    "(SELECT count(*) FROM FileImage f WHERE f.image_id=i.id) AS bindings "
                    "FROM (SELECT {} AS id) requested LEFT JOIN Element e ON e.id=requested.id "
                    "LEFT JOIN FileImage fi ON fi.file_id=requested.id LEFT JOIN Image i ON "
                    "i.id=fi.image_id",
                    id));
    if (rows.size() == 1 && !rows[0]["mount_id"].is_null()) {
      result.mounted = true;
      result.protection_reasons.emplace_back("Mounted entry");
      return result;
    }
    if (rows.size() != 1 || rows[0]["id"].is_null() || rows[0]["type"] != 0 ||
        rows[0]["image_path"].is_null()) {
      throw std::runtime_error("Missing or ambiguous Element, FileImage, or Image reference");
    }
    const auto& row    = rows[0];
    result.source_path = std::filesystem::u8path(row.at("image_path").get<std::string>());
    result.source_changed = row.at("source_changed").get<bool>();
    if (const auto source = SourceFileStore(database_).ReadSourceFile(id)) {
      result.recorded_fingerprint =
          SourceFileFingerprint{source->fingerprint, source->fingerprint_version};
    }
    if (!row["added_time"].is_null()) result.added_time = row["added_time"].get<int64_t>();
    result.rating = row.at("rating").get<int>();
    if (row["metadata"].is_null() || !row["metadata"].is_object() ||
        !row["metadata"].contains("Rating")) {
      throw std::runtime_error("Missing metadata rating evidence");
    }
    if (result.rating != row["metadata"]["Rating"].get<int>()) {
      result.valid_references = false;
      result.protection_reasons.emplace_back("Image and metadata ratings disagree");
    }
    if (result.rating != 0) result.protection_reasons.emplace_back("Rated entry");
    if (row.at("color_labels").get<int>() != 0)
      result.protection_reasons.emplace_back("Color-labeled entry");
    if (row.at("delete_mark").get<bool>())
      result.protection_reasons.emplace_back("Deletion-marked entry");
    if (row["bindings"] != 1) result.protection_reasons.emplace_back("Shared Image binding");
    if (duckorm::select_int64(
            guard.conn_,
            duckorm::expr::raw(std::format(
                "SELECT (SELECT count(*) FROM AiImageUnderstanding WHERE file_id={0}) + "
                "(SELECT count(*) FROM AiImageRating WHERE file_id={0})",
                id)))
            .value() != 0) {
      result.protection_reasons.emplace_back("Owner-managed annotations");
    }
    if (duckorm::select_int64(
            guard.conn_,
            duckorm::expr::raw(std::format(
                "SELECT count(*) FROM FolderContent fc LEFT JOIN Element f ON f.id=fc.folder_id "
                "WHERE fc.element_id={} AND (f.id IS NULL OR f.type<>1)",
                id)))
            .value() != 0) {
      result.protection_reasons.emplace_back("Invalid album reference");
      result.valid_references = false;
    }
    CommitGraphStore history(guard.conn_);
    const auto       graph = history.LoadGraph(id);
    if (!graph) throw std::runtime_error("Missing history graph");
    result.commit_count  = history.CountCommitsForRoot(graph->GetRootId());
    result.version_count = graph->GetAllVersionRefs().size();
    if (result.commit_count != 0) result.protection_reasons.emplace_back("Stored editing commits");
    const auto& version = graph->GetActiveVersionRef();
    if (result.version_count != 1 || version.display_name != "Default" ||
        version.head_commit_hash || version.created_at != version.updated_at) {
      result.protection_reasons.emplace_back("Additional, renamed, or changed Version");
    }
    const auto  root_json = history.GetRootSerializedPipelineState(id, graph->GetRootId());
    const auto& state     = graph->GetImageEditState();
    if (!root_json || !state.serialized_pipeline_state) {
      throw std::runtime_error("Missing immutable root or document checkpoint");
    }
    const auto root       = DecodePipelineRootState(*root_json);
    const auto checkpoint = DecodePipelineDocumentCheckpoint(*state.serialized_pipeline_state);
    if (root.element_id != id ||
        ComputeRootId(id, root.document, root.raw_color_context) != graph->GetRootId() ||
        checkpoint.root_id != graph->GetRootId() ||
        checkpoint.head_commit_hash != state.materialized_head_commit_hash ||
        checkpoint.transaction_chain_hash != state.materialized_transaction_chain_hash) {
      throw std::runtime_error("Inconsistent root or checkpoint identity");
    }
    PipelineMapper pipelines(guard.conn_);
    const auto     pipeline = pipelines.GetPipelineJsonByFileId(id);
    if (!pipeline) throw std::runtime_error("Missing PipelineParam reference");
    const auto root_document = root.document.ToJson();
    if (checkpoint.document.ToJson() != root_document || *pipeline != root_document) {
      result.protection_reasons.emplace_back("Pipeline differs from immutable import root");
    }
    std::set<MaskAssetKey> assets;
    CollectAiMaskAssetKeys(*root_json, assets);
    CollectAiMaskAssetKeys(*state.serialized_pipeline_state, assets);
    CollectAiMaskAssetKeys(*pipeline, assets);
    for (const auto& [hash, commit] : graph->GetAllCommits()) {
      CollectAiMaskAssetKeys(commit.GetPayloadJSON(), assets);
    }
    MaskAssetStore asset_store(guard.conn_);
    for (const auto& key : assets) (void)asset_store.Read(key);
  } catch (const std::exception& error) {
    result.valid_references = false;
    result.protection_reasons.emplace_back(std::string("Invalid project evidence: ") +
                                           error.what());
  }
  return result;
}

auto ProjectMaintenanceStore::UpdateSourceChangeMarks(std::span<const SourceChangeMark> changes)
    -> bool {
  return SourceFileStore(database_).UpdateChangeMarks(changes);
}

auto ProjectMaintenanceStore::RemovePristineProjectEntries(
    std::span<const MaintenanceRemoval>        removals,
    const std::function<void(size_t, size_t)>& progress) -> std::vector<sl_element_id_t> {
  if (removals.empty()) throw std::invalid_argument("Explicit removal IDs are required");
  // File I/O precedes the transaction and does not hold the database lock. The staged
  // database has one writer; the locked preservation pass below still rereads every entry.
  std::map<image_path_t::string_type, std::optional<SourceFileFingerprint>> fingerprints;
  for (const auto& removal : removals) {
    const auto entry  = Inspect(removal.entry_id);
    if (entry.mounted) throw std::invalid_argument("Maintenance excludes mounted entries");
    const auto source = CheckSourceImageStatus(entry.source_path);
    if (removal.retained_id ? source.status != SourceImageStatus::Readable
                            : source.status != SourceImageStatus::Missing) {
      throw std::runtime_error("Selected source status changed: " + source.reason);
    }
    if (removal.retained_id) {
      auto [current, inserted] = fingerprints.try_emplace(SourcePathKey(entry.source_path));
      if (inserted) current->second = ReadMaintenanceFingerprint(entry.source_path);
      if (!current->second || current->second != entry.recorded_fingerprint) {
        throw std::runtime_error("Selected source fingerprint changed or is unknown");
      }
    }
  }
  auto                                guard = database_.GetConnectionGuard();
  auto                                lock  = guard.Lock();
  std::unordered_set<sl_element_id_t> ids;
  for (const auto& removal : removals) {
    if (removal.entry_id == 0 || !ids.insert(removal.entry_id).second) {
      throw std::invalid_argument("Removal IDs must be unique and nonzero");
    }
  }
  // Inspect uses scoped connections under the same recursive database lock. No statements
  // have mutated the database yet, and no other owner can change the evidence between reads.
  for (const auto& removal : removals) {
    const auto evidence = Inspect(removal.entry_id);
    if (!evidence.protection_reasons.empty()) {
      throw std::runtime_error("Selected entry is protected: " + std::to_string(removal.entry_id) +
                               ": " + evidence.protection_reasons.front());
    }
    if (removal.retained_id) {
      if (ids.contains(*removal.retained_id)) {
        throw std::invalid_argument("A retained entry cannot be in the removal set");
      }
      const auto retained = Inspect(*removal.retained_id);
      const auto key      = SourcePathKey(evidence.source_path);
      const auto current  = fingerprints.find(key);
      if (retained.mounted || !retained.valid_references || retained.source_path.empty() ||
          key != SourcePathKey(retained.source_path) || current == fingerprints.end() ||
          current->second != evidence.recorded_fingerprint ||
          current->second != retained.recorded_fingerprint) {
        throw std::invalid_argument("Duplicate pair lacks a verified common source");
      }
    }
  }
  duckorm::Transaction         transaction(guard.conn_);
  std::vector<sl_element_id_t> removed;
  std::vector<image_id_t>      images;
  if (progress) progress(0, removals.size());
  for (const auto& removal : removals) {
    const auto id = removal.entry_id;
    images.push_back(static_cast<image_id_t>(
        duckorm::select_int64(
            guard.conn_,
            duckorm::expr::raw(std::format("SELECT image_id FROM FileImage WHERE file_id={}", id)))
            .value()));
    if (removal.retained_id) {
      duckorm::execute(guard.conn_, duckorm::expr::raw(
                                        std::format("INSERT INTO FolderContent SELECT folder_id,{} "
                                                    "FROM FolderContent WHERE element_id={} "
                                                    "ON CONFLICT DO NOTHING",
                                                    *removal.retained_id, id)));
    }
    CommitGraphStore(guard.conn_).DeleteGraphForElementInTransaction(id);
    PipelineMapper(guard.conn_).RemoveById(id);
    FileMapper(guard.conn_).RemoveById(id);
    duckorm::execute(guard.conn_, duckorm::expr::raw(std::format(
                                      "DELETE FROM FolderContent WHERE element_id={}", id)));
    ElementMapper(guard.conn_).RemoveById(id);
    if (removal.retained_id) {
      duckorm::execute(guard.conn_, duckorm::expr::raw(
                                        std::format("UPDATE Element SET ref_count=(SELECT count(*) "
                                                    "FROM FolderContent WHERE element_id={0}) "
                                                    "WHERE id={0}",
                                                    *removal.retained_id)));
    }
    removed.push_back(id);
    if (progress) progress(removed.size(), removals.size());
  }
  DeleteSemanticRowsForFiles(guard.conn_, removed);
  // The annotation owner rebuilds its full-text index. Rebuild once for the batch: dropping
  // and recreating its stopword table twice in one DuckDB transaction cannot commit.
  DeleteAiAnnotationRowsForFiles(guard.conn_, removed);
  SourceFileMapper(guard.conn_).RemoveByIds(removed);
  ImageStore::RemoveUnreferencedImagesInTransaction(guard.conn_, images);
  transaction.commit();
  return removed;
}

auto ProjectMaintenanceStore::ImageIds(std::span<const MaintenanceRemoval> removals)
    -> std::vector<image_id_t> {
  auto                    guard = database_.GetConnectionGuard();
  auto                    lock  = guard.Lock();
  std::vector<image_id_t> result;
  for (const auto& item : removals) {
    result.push_back(static_cast<image_id_t>(
        duckorm::select_int64(
            guard.conn_, duckorm::expr::raw(std::format(
                             "SELECT image_id FROM FileImage WHERE file_id={}", item.entry_id)))
            .value()));
  }
  return result;
}

auto ProjectMaintenanceStore::RetainedRowDigests(std::span<const MaintenanceRemoval> removals,
                                                 std::span<const image_id_t>         removed_images,
                                                 bool                                before_removal)
    -> std::map<std::string, std::string> {
  auto        guard = database_.GetConnectionGuard();
  auto        lock  = guard.Lock();
  std::string ids = "-1", images = "-1", retained = "-1";
  for (const auto& item : removals) {
    ids += "," + std::to_string(item.entry_id);
    if (item.retained_id) retained += "," + std::to_string(*item.retained_id);
  }
  for (auto image : removed_images) images += "," + std::to_string(image);
  std::string memberships = "SELECT * FROM FolderContent WHERE element_id NOT IN (" + ids + ")";
  if (before_removal) {
    for (const auto& item : removals)
      if (item.retained_id) {
        memberships += std::format(
            " UNION SELECT folder_id,{} AS element_id FROM FolderContent "
            "WHERE element_id={}",
            *item.retained_id, item.entry_id);
      }
  }
  std::map<std::string, std::string> result;
  for (const auto& row : ReadRows(guard.conn_,
                                  "SELECT table_name FROM information_schema.tables WHERE "
                                  "table_schema='main' ORDER BY table_name")) {
    const auto table = row["table_name"].get<std::string>();
    // Names are read from the catalog, never supplied by command clients.
    if (table.find('"') != std::string::npos) throw std::runtime_error("Invalid stored table name");
    std::string query = "SELECT * FROM \"" + table + "\"";
    if (table == "FolderContent")
      query = memberships;
    else if (table == "Element") {
      query = std::format(
          "SELECT * REPLACE(CASE WHEN id IN ({}) THEN "
          "(SELECT count(*) FROM ({}) memberships WHERE element_id=Element.id) "
          "ELSE ref_count END AS ref_count) FROM Element WHERE id NOT IN ({})",
          retained, memberships, ids);
    } else if (table == "Image")
      query += " WHERE id NOT IN (" + images + ")";
    else if (table == "ImageSourceSize")
      query += " WHERE image_id NOT IN (" + images + ")";
    else if (table == "FileImage" || table == "PipelineParam" ||
             table.starts_with("SemanticImage") || table.starts_with("AiImage"))
      query += " WHERE file_id NOT IN (" + ids + ")";
    else if (table == "PipelineRoot" || table == "VersionRef" || table == "ImageEditState" ||
             table == "SourceFile" || table == "SourceFileChanged") {
      query += " WHERE element_id NOT IN (" + ids + ")";
    }
    result[table] =
        duckorm::select_string(
            guard.conn_,
            duckorm::expr::raw(
                "SELECT sha256(coalesce(string_agg(digest, '' ORDER BY digest), '')) FROM "
                "(SELECT sha256(to_json(t)::VARCHAR) AS digest FROM (" +
                query + ") t) hashes"))
            .value();
  }
  return result;
}
}  // namespace alcedo
