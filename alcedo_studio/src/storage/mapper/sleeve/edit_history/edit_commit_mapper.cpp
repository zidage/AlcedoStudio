//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/edit_history/edit_commit_mapper.hpp"

#include <stdexcept>
#include <utility>

namespace alcedo {

auto EditCommitMapper::FromRawData(std::vector<duckorm::VarTypes>&& data)
    -> EditCommitMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for EditCommit");
  }
  auto* commit_hash        = std::get_if<std::unique_ptr<std::string>>(&data[0]);
  auto* root_id            = std::get_if<std::unique_ptr<std::string>>(&data[1]);
  auto* first_parent_hash  = std::get_if<std::unique_ptr<std::string>>(&data[2]);
  auto* second_parent_hash = std::get_if<std::unique_ptr<std::string>>(&data[3]);
  auto* created_at_ns      = std::get_if<std::uint64_t>(&data[4]);
  auto* kind               = std::get_if<std::uint32_t>(&data[5]);
  auto* edit_payload       = std::get_if<std::unique_ptr<std::string>>(&data[6]);

  if (commit_hash == nullptr || root_id == nullptr || first_parent_hash == nullptr ||
      second_parent_hash == nullptr || created_at_ns == nullptr || kind == nullptr ||
      edit_payload == nullptr) {
    throw std::runtime_error("Encountering unmatched types when parsing EditCommit from the DB");
  }

  return {std::move(*commit_hash),
          std::move(*root_id),
          std::move(*first_parent_hash),
          std::move(*second_parent_hash),
          *created_at_ns,
          *kind,
          std::move(*edit_payload)};
}

}  // namespace alcedo
