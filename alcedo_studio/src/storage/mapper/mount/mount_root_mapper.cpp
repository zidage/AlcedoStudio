//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/mount/mount_root_mapper.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "storage/mapper/utc_timestamp_text.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {

auto MountRootMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> MountRootMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("MountRootMapper: the row does not have the MountRoot columns");
  }
  const auto* id              = std::get_if<uint32_t>(&data[0]);
  const auto* root_folder_id  = std::get_if<uint32_t>(&data[1]);
  auto*       volume_identity = std::get_if<std::unique_ptr<std::string>>(&data[2]);
  auto*       root_path       = std::get_if<std::unique_ptr<std::string>>(&data[3]);
  auto*       last_scan_at    = std::get_if<std::unique_ptr<std::string>>(&data[4]);
  auto*       volume_name     = std::get_if<std::unique_ptr<std::string>>(&data[5]);
  if (id == nullptr || root_folder_id == nullptr || volume_identity == nullptr ||
      root_path == nullptr || last_scan_at == nullptr || volume_name == nullptr ||
      *root_path == nullptr) {
    throw std::runtime_error("MountRootMapper: a MountRoot cell has an unexpected type");
  }
  return {*id,
          *root_folder_id,
          *volume_identity ? std::move(**volume_identity) : std::string{},
          std::move(**root_path),
          *last_scan_at ? std::move(**last_scan_at) : std::string{},
          *volume_name ? std::move(**volume_name) : std::string{}};
}

auto MountRootMapper::ToParams(const MountRootRecord& source) -> MountRootMapperParams {
  return {source.id,
          source.root_folder_id,
          source.volume_identity,
          conv::ToBytes(source.root_path.wstring()),
          source.last_scan_at.has_value() ? FormatUtcTimestampText(*source.last_scan_at)
                                          : std::string{},
          source.volume_name};
}

auto MountRootMapper::FromParams(MountRootMapperParams&& param) -> MountRootRecord {
  MountRootRecord record;
  record.id              = param.id;
  record.root_folder_id  = param.root_folder_id;
  record.volume_identity = std::move(param.volume_identity);
  record.root_path       = std::filesystem::path(conv::FromBytes(param.root_path));
  if (!param.last_scan_at.empty()) {
    record.last_scan_at = ParseUtcTimestampText(param.last_scan_at);
  }
  record.volume_name = std::move(param.volume_name);
  return record;
}

}  // namespace alcedo
