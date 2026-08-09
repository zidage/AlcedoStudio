//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/element/file_mapper.hpp"

#include <format>
#include <stdexcept>
#include <variant>

namespace alcedo {
auto FileMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> FileMapperParams {
  if (data.size() != FileMapper::FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for SleeveFile");
  }

  auto file_id = std::get_if<sl_element_id_t>(&data[0]);
  auto img_id  = std::get_if<image_id_t>(&data[1]);
  if (file_id == nullptr || img_id == nullptr) {
    throw std::runtime_error("Unmatching types occured when parsing the data from the DB");
  }
  return {*file_id, *img_id};
}

auto FileMapper::ToParams(const std::pair<sl_element_id_t, image_id_t>& source)
    -> FileMapperParams {
  return {source.first, source.second};
}

auto FileMapper::FromParams(FileMapperParams&& param) -> std::pair<sl_element_id_t, image_id_t> {
  return {param.file_id, param.image_id};
}

auto FileMapper::GetFileById(const sl_element_id_t id) -> std::pair<sl_element_id_t, image_id_t> {
  auto result = GetByPredicate(std::format("file_id={}", id));
  if (result.size() != 1) {
    throw std::runtime_error("FileMapper: Unable to recover a file image mapping: broken record");
  }
  return result.at(0);
}

auto FileMapper::GetBoundImageById(const sl_element_id_t id) -> image_id_t {
  return GetFileById(id).second;
}

void FileMapper::RemoveBindByFileId(const sl_element_id_t id) { RemoveById(id); }
}  // namespace alcedo
