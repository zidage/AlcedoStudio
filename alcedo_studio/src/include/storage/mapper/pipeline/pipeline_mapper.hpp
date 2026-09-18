//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "edit/pipeline/pipeline_cpu.hpp"
#include "json.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
// CREATE TABLE PipelineParam (file_id BIGINT PRIMARY KEY, param_json JSON);
struct PipelineMapperParams {
  sl_element_id_t              file_id;
  std::unique_ptr<std::string> param_json;
};

/**
 * @brief Single-table mapper for PipelineParam rows and CPUPipelineExecutor snapshots.
 */
class PipelineMapper : public Mapper<PipelineMapper, std::shared_ptr<CPUPipelineExecutor>,
                                     PipelineMapperParams, sl_element_id_t>,
                       public FieldReflectable<PipelineMapper> {
 private:
  static constexpr uint32_t    field_count_                                      = 2;
  static constexpr const char* table_name_                                       = "PipelineParam";
  static constexpr const char* prime_key_clause_                                 = "file_id={}";
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_ = {
      FIELD(PipelineMapperParams, file_id, UINT32),
      FIELD(PipelineMapperParams, param_json, VARCHAR)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> PipelineMapperParams;
  static auto ToParams(const std::shared_ptr<CPUPipelineExecutor> source) -> PipelineMapperParams;
  static auto FromParams(PipelineMapperParams&& param) -> std::shared_ptr<CPUPipelineExecutor>;

  auto        GetPipelineParamByFileId(const sl_element_id_t file_id)
      -> std::shared_ptr<CPUPipelineExecutor>;
  void               UpdatePipelineParamByFileId(const sl_element_id_t                      file_id,
                                                 const std::shared_ptr<CPUPipelineExecutor> pipeline);

  /** @brief Read the stored pipeline JSON without constructing a legacy executor. */
  [[nodiscard]] auto GetPipelineJsonByFileId(sl_element_id_t file_id)
      -> std::optional<nlohmann::json>;
  /** @brief Persist the authoritative format-version-2 pipeline document JSON. */
  void UpdatePipelineJsonByFileId(sl_element_id_t file_id, const nlohmann::json& document);

  friend struct FieldReflectable<PipelineMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
