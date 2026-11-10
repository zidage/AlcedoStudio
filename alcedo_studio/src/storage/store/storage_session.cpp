//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/storage_session.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

#include "storage/mapper/pipeline/pipeline_mapper.hpp"

namespace alcedo {

StorageSession::StorageSession(Database& database)
    : guard_(database.GetConnectionGuard()),
      lock_(guard_.Lock()),
      commit_graph_(guard_.conn_),
      mask_assets_(guard_.conn_) {}

auto StorageSession::CommitGraph() -> CommitGraphStore& { return commit_graph_; }

auto StorageSession::MaskAssets() -> MaskAssetStore& { return mask_assets_; }

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
