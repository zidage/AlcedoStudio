//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/edit_history/version_ref_mapper.hpp"

#include <stdexcept>
#include <utility>

namespace alcedo {

auto VersionRefMapper::FromRawData(std::vector<duckorm::VarTypes>&& data)
    -> VersionRefMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for VersionRef");
  }
  auto* version_id       = std::get_if<std::unique_ptr<std::string>>(&data[0]);
  auto* element_id       = std::get_if<std::uint32_t>(&data[1]);
  auto* display_name     = std::get_if<std::unique_ptr<std::string>>(&data[2]);
  auto* head_commit_hash = std::get_if<std::unique_ptr<std::string>>(&data[3]);
  auto* created_at_unix  = std::get_if<std::int64_t>(&data[4]);
  auto* updated_at_unix  = std::get_if<std::int64_t>(&data[5]);

  if (version_id == nullptr || element_id == nullptr || display_name == nullptr ||
      head_commit_hash == nullptr || created_at_unix == nullptr || updated_at_unix == nullptr) {
    throw std::runtime_error("Encountering unmatched types when parsing VersionRef from the DB");
  }

  return {std::move(*version_id), *element_id,      std::move(*display_name),
          std::move(*head_commit_hash), *created_at_unix, *updated_at_unix};
}

}  // namespace alcedo
