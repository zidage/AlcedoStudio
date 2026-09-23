//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/edit_history/recovery_metadata_mapper.hpp"

#include <stdexcept>

namespace alcedo {

auto EditorRecoveryMetadataMapper::FromRawData(std::vector<duckorm::VarTypes>&& data)
    -> EditorRecoveryMetadataMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for EditorRecoveryMetadata");
  }
  auto* file_id = std::get_if<sl_element_id_t>(&data[0]);
  auto* version_id = std::get_if<std::unique_ptr<std::string>>(&data[1]);
  auto* journal_generation = std::get_if<std::uint64_t>(&data[2]);
  auto* materialized_operation_sequence = std::get_if<std::uint64_t>(&data[3]);
  auto* transaction_chain_hash = std::get_if<std::unique_ptr<std::string>>(&data[4]);
  auto* pipeline_parameter_hash = std::get_if<std::unique_ptr<std::string>>(&data[5]);

  if (file_id == nullptr || version_id == nullptr || journal_generation == nullptr ||
      materialized_operation_sequence == nullptr || transaction_chain_hash == nullptr ||
      pipeline_parameter_hash == nullptr) {
    throw std::runtime_error(
        "Encountering unmatching types when parsing EditorRecoveryMetadata from the DB");
  }
  return {*file_id,
          std::move(*version_id),
          *journal_generation,
          *materialized_operation_sequence,
          std::move(*transaction_chain_hash),
          std::move(*pipeline_parameter_hash)};
}

}  // namespace alcedo
