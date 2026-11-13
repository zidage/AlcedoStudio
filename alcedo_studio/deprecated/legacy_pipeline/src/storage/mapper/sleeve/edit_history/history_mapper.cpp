//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/edit_history/history_mapper.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace alcedo {
auto EditHistoryMapper::FromRawData(std::vector<duckorm::VarTypes>&& data)
    -> EditHistoryMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for EditHistory");
  }
  auto file_id = std::get_if<sl_element_id_t>(&data[0]);
  auto history = std::get_if<std::unique_ptr<std::string>>(&data[1]);

  if (file_id == nullptr || history == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  return {*file_id, std::move(*history)};
}

auto EditHistoryMapper::ToParams(const std::shared_ptr<EditHistory> source)
    -> EditHistoryMapperParams {
  EditHistoryMapperParams param;
  param.file_id = source->GetBoundImage();
  param.history = std::make_unique<std::string>(source->ToJSON().dump());
  return param;
}

auto EditHistoryMapper::FromParams(EditHistoryMapperParams&& param)
    -> std::shared_ptr<EditHistory> {
  auto history = std::make_shared<EditHistory>(param.file_id);
  if (param.history) {
    history->FromJSON(nlohmann::json::parse(std::move(*param.history)));
  }
  return history;
}

auto EditHistoryMapper::GetEditHistoryByFileId(const sl_element_id_t file_id)
    -> std::shared_ptr<EditHistory> {
  auto result = GetByPredicate(std::format("file_id={}", file_id));
  if (result.size() > 1) {
    throw std::runtime_error("EditHistoryMapper: Multiple edit history found for file_id " +
                             std::to_string(file_id));
  }
  if (result.empty()) {
    return nullptr;
  }
  return result.front();
}

void EditHistoryMapper::UpdateEditHistory(const std::shared_ptr<EditHistory> history) {
  Update(history, history->GetBoundImage());
}
}  // namespace alcedo
