//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/storage_session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

struct ImportedFileLocationRow {
  int64_t                    element_id = 0;
  int64_t                    image_id   = 0;
  std::optional<std::string> image_path;
  using Columns =
      duckorm::RowColumns<&ImportedFileLocationRow::element_id, &ImportedFileLocationRow::image_id,
                          &ImportedFileLocationRow::image_path>;
};

}  // namespace

StorageSession::StorageSession(Database& database)
    : guard_(database.GetConnectionGuard()),
      lock_(guard_.Lock()),
      commit_graph_(guard_.conn_),
      mask_assets_(guard_.conn_) {}

auto StorageSession::CommitGraph() -> CommitGraphStore& { return commit_graph_; }

auto StorageSession::MaskAssets() -> MaskAssetStore& { return mask_assets_; }

auto StorageSession::ReadImportedFileLocations() -> std::vector<FileLocationRecord> {
  auto query = duckorm::expr::raw(
      "SELECT e.id, fi.image_id, i.image_path FROM PipelineRoot r "
      "JOIN Element e ON e.id = r.element_id JOIN FileImage fi ON fi.file_id = e.id "
      "JOIN Image i ON i.id = fi.image_id WHERE e.type = ");
  query.append(duckorm::expr::param(static_cast<int64_t>(ElementType::FILE)));
  query.append(duckorm::expr::raw(" ORDER BY e.id"));
  std::vector<FileLocationRecord> locations;
  for (const auto& row : duckorm::query<ImportedFileLocationRow>(guard_.conn_, query)) {
    locations.push_back({static_cast<sl_element_id_t>(row.element_id),
                         static_cast<image_id_t>(row.image_id),
                         std::filesystem::path(conv::FromBytes(row.image_path.value_or("")))});
  }
  return locations;
}

void StorageSession::InsertImageRoots(std::span<const EncodedRootPipeline> roots,
                                      std::vector<std::string>             element_pipeline_json) {
  if (roots.size() != element_pipeline_json.size()) {
    throw std::invalid_argument(
        "StorageSession: each image root needs one element pipeline JSON value");
  }
  std::vector<PipelineMapperParams> pipeline_rows;
  pipeline_rows.reserve(roots.size());
  for (std::size_t index = 0; index < roots.size(); ++index) {
    pipeline_rows.push_back(PipelineMapperParams{
        roots[index].element_id,
        std::make_unique<std::string>(std::move(element_pipeline_json[index]))});
  }
  PipelineMapper pipeline_mapper(guard_.conn_);
  // The element pipeline JSON commits in the same transaction as the root rows.
  commit_graph_.InsertRootPipelines(roots, [&pipeline_rows, &pipeline_mapper]() {
    pipeline_mapper.UpsertParamsRows(pipeline_rows);
  });
}

}  // namespace alcedo
