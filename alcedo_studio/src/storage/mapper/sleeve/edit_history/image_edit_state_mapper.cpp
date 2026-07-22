//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/edit_history/image_edit_state_mapper.hpp"

#include <stdexcept>
#include <utility>

namespace alcedo {

auto ImageEditStateMapper::FromRawData(std::vector<duckorm::VarTypes>&& data)
    -> ImageEditStateMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for ImageEditState");
  }
  auto* element_id = std::get_if<std::uint32_t>(&data[0]);
  auto* root_id    = std::get_if<std::unique_ptr<std::string>>(&data[1]);
  auto* active_version_id =
      std::get_if<std::unique_ptr<std::string>>(&data[2]);
  auto* materialized_head_commit_hash =
      std::get_if<std::unique_ptr<std::string>>(&data[3]);
  auto* materialized_transaction_chain_hash =
      std::get_if<std::unique_ptr<std::string>>(&data[4]);
  auto* stored_pipeline_projection =
      std::get_if<std::unique_ptr<std::string>>(&data[5]);
  auto* project_schema_version = std::get_if<std::uint32_t>(&data[6]);

  if (element_id == nullptr || root_id == nullptr || active_version_id == nullptr ||
      materialized_head_commit_hash == nullptr ||
      materialized_transaction_chain_hash == nullptr || stored_pipeline_projection == nullptr ||
      project_schema_version == nullptr) {
    throw std::runtime_error(
        "Encountering unmatched types when parsing ImageEditState from the DB");
  }

  return {*element_id,
          std::move(*root_id),
          std::move(*active_version_id),
          std::move(*materialized_head_commit_hash),
          std::move(*materialized_transaction_chain_hash),
          std::move(*stored_pipeline_projection),
          *project_schema_version};
}

}  // namespace alcedo
