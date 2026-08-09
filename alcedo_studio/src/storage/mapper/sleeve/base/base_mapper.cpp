//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/base/base_mapper.hpp"

#include <stdexcept>

#include "type/type.hpp"

namespace alcedo {
auto BaseMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> BaseMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for Base");
  }
  auto id = std::get_if<sleeve_id_t>(&data[0]);

  if (id == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  return {*id};
}

auto BaseMapper::ToParams(const sleeve_id_t source) -> BaseMapperParams { return {source}; }

auto BaseMapper::FromParams(BaseMapperParams&& param) -> sleeve_id_t { return param.id; }
}  // namespace alcedo
