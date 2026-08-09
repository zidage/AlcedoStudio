//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/element/element_mapper.hpp"

#include <cstdint>
#include <ctime>
#include <format>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
auto ElementMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> ElementMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for SleeveElement");
  }
  auto id            = std::get_if<sl_element_id_t>(&data[0]);
  auto type          = std::get_if<uint32_t>(&data[1]);
  auto element_name  = std::get_if<std::unique_ptr<std::string>>(&data[2]);
  auto added_time    = std::get_if<std::unique_ptr<std::string>>(&data[3]);
  auto modified_time = std::get_if<std::unique_ptr<std::string>>(&data[4]);
  auto ref_count     = std::get_if<uint32_t>(&data[5]);

  if (id == nullptr || type == nullptr || element_name == nullptr || added_time == nullptr ||
      modified_time == nullptr || ref_count == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  return {
      *id,       *type, std::move(*element_name), std::move(*added_time), std::move(*modified_time),
      *ref_count};
}

auto ElementMapper::ToParams(const std::shared_ptr<SleeveElement>& source) -> ElementMapperParams {
  char added_time[32];
  char modified_time[32];
  std::strftime(added_time, sizeof(added_time), "%Y-%m-%d %H:%M:%S",
                std::gmtime(&source->added_time_));
  std::strftime(modified_time, sizeof(modified_time), "%Y-%m-%d %H:%M:%S",
                std::gmtime(&source->last_modified_time_));

  std::string utf_8_str = conv::ToBytes(source->element_name_);
  return {source->element_id_,
          static_cast<uint32_t>(source->type_),
          std::make_unique<std::string>(utf_8_str),
          std::make_unique<std::string>(added_time),
          std::make_unique<std::string>(modified_time),
          source->ref_count_};
}

auto ElementMapper::FromParams(ElementMapperParams&& param) -> std::shared_ptr<SleeveElement> {
  auto               id                = param.id;
  auto               type              = static_cast<ElementType>(param.type);
  auto               element_name      = conv::FromBytes(std::move(*param.element_name));
  auto               added_time_str    = std::move(*param.added_time);
  auto               modified_time_str = std::move(*param.modified_time);
  auto               ref_count         = param.ref_count;

  std::tm            tm_added{};
  std::tm            tm_modified{};

  std::istringstream a_ss(added_time_str);
  std::istringstream m_ss(modified_time_str);
  a_ss >> std::get_time(&tm_added, "%Y-%m-%d %H:%M:%S");
  m_ss >> std::get_time(&tm_modified, "%Y-%m-%d %H:%M:%S");
  std::shared_ptr<SleeveElement> element;
  if (type == ElementType::FILE) {
    element = std::make_shared<SleeveFile>(id, element_name);
  } else if (type == ElementType::FOLDER) {
    element = std::make_shared<SleeveFolder>(id, element_name);
  } else {
    throw std::runtime_error("ElementMapper: Invalid ElementMapperParams");
  }

  element->added_time_         = std::mktime(&tm_added);
  element->last_modified_time_ = std::mktime(&tm_modified);
  element->ref_count_          = ref_count;

  return element;
}

auto ElementMapper::GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  auto result = GetByPredicate(std::format("id={}", id));
  if (result.size() > 1) {
    throw std::runtime_error("ElementMapper: Sleeve element id is not unique. Broken DB file");
  }
  if (result.size() == 0) {
    throw std::runtime_error(
        std::format("ElementMapper: No element with id {} is stored in DB", id));
  }
  return result[0];
}

auto ElementMapper::GetElementByName(const std::wstring& name)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  std::string predicate = conv::ToBytes(std::format(L"element_name={}", name));
  return GetByPredicate(std::move(predicate));
}

auto ElementMapper::GetElementByType(const ElementType type)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  std::string predicate = std::format("type={}", static_cast<uint32_t>(type));
  return GetByPredicate(std::move(predicate));
}

auto ElementMapper::GetElementsInFolderByFilter(const std::wstring& filter_sql)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  std::string filter_sql_u8 = conv::ToBytes(filter_sql);
  return GetByQuery(std::move(filter_sql_u8));
}
}  // namespace alcedo
