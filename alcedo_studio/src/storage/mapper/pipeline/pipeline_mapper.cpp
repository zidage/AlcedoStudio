//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/pipeline/pipeline_mapper.hpp"

#include <format>
#include <memory>
#include <stdexcept>
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

auto PipelineMapper::ToParams(const std::shared_ptr<CPUPipelineExecutor> source)
    -> PipelineMapperParams {
  PipelineMapperParams param;
  param.file_id    = source->GetBoundFile();
  param.param_json = std::make_unique<std::string>(source->ExportPipelineParams().dump());
  return param;
}

auto PipelineMapper::FromParams(PipelineMapperParams&& param)
    -> std::shared_ptr<CPUPipelineExecutor> {
  auto pipeline = std::make_shared<CPUPipelineExecutor>();
  pipeline->SetBoundFile(param.file_id);
  if (param.param_json) {
    pipeline->ImportPipelineParams(nlohmann::json::parse(std::move(*param.param_json)));
    pipeline->SetExecutionStages();
  }
  return pipeline;
}

auto PipelineMapper::GetPipelineParamByFileId(const sl_element_id_t file_id)
    -> std::shared_ptr<CPUPipelineExecutor> {
  auto result = GetByPredicate(std::format(PipelineMapper::PrimeKeyClause(), file_id));
  if (result.size() > 1) {
    throw std::runtime_error(
        "[ERROR] PipelineMapper: Broken image database. Multiple pipeline params found for "
        "file_id " +
        std::to_string(file_id));
  }

  if (result.empty()) {
    return nullptr;
  }

  return result.front();
}

void PipelineMapper::UpdatePipelineParamByFileId(
    const sl_element_id_t file_id, const std::shared_ptr<CPUPipelineExecutor> pipeline) {
  Update(pipeline, file_id);
}
}  // namespace alcedo
