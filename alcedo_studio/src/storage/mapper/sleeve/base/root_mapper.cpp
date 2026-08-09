//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/base/base_mapper.hpp"

#include <stdexcept>

namespace alcedo {
auto RootMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> RootMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for Base");
  }
  auto id = std::get_if<sl_element_id_t>(&data[0]);

  if (id == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  return {*id};
}

auto RootMapper::ToParams(const sl_element_id_t source) -> RootMapperParams { return {source}; }

auto RootMapper::FromParams(RootMapperParams&& param) -> sl_element_id_t { return param.id_; }
}  // namespace alcedo
