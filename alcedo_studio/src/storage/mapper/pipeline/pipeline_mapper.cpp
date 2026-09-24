//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/pipeline/pipeline_mapper.hpp"

#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo {
auto PipelineMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> PipelineMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("[ERROR] PipelineMapper: Invalid DuckFieldDesc for PipelineParam");
  }
  auto file_id    = std::get_if<sl_element_id_t>(&data[0]);
  auto param_json = std::get_if<std::unique_ptr<std::string>>(&data[1]);

  if (file_id == nullptr || param_json == nullptr) {
    throw std::runtime_error(
        "[ERROR] PipelineMapper: Encounting unmatching types when parsing the data from the DB");
  }

  return {*file_id, std::move(*param_json)};
}

auto PipelineMapper::GetPipelineJsonByFileId(sl_element_id_t file_id)
    -> std::optional<nlohmann::json> {
  auto rows = GetParams(std::format(PipelineMapper::PrimeKeyClause(), file_id).c_str());
  if (rows.size() > 1) {
    throw std::runtime_error("PipelineMapper: multiple pipeline JSON rows for file_id " +
                             std::to_string(file_id));
  }
  if (rows.empty() || !rows.front().param_json) return std::nullopt;
  return nlohmann::json::parse(*rows.front().param_json);
}

void PipelineMapper::UpdatePipelineJsonByFileId(sl_element_id_t       file_id,
                                                const nlohmann::json& document) {
  PipelineMapperParams params{file_id, std::make_unique<std::string>(document.dump())};
  UpdateParams(file_id, params);
}

}  // namespace alcedo
