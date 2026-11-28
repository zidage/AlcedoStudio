//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/sleeve/element/element_mapper.hpp"

#include <cstdint>
#include <ctime>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/mapper/utc_timestamp_text.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto ReadMountId(const ElementMapperParams& row) -> std::optional<mount_id_t> {
  if (!row.mount_id.has_value()) {
    return std::nullopt;
  }
  if (*row.mount_id < 0 || *row.mount_id > std::numeric_limits<mount_id_t>::max()) {
    throw std::runtime_error(std::format(
        "ElementMapper: element {} has the mount id {}, which is not a valid mount id", row.id,
        *row.mount_id));
  }
  return static_cast<mount_id_t>(*row.mount_id);
}

}  // namespace

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
  auto mount_id      = std::get_if<std::optional<int64_t>>(&data[6]);

  if (id == nullptr || type == nullptr || element_name == nullptr || added_time == nullptr ||
      modified_time == nullptr || ref_count == nullptr || mount_id == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  return {*id,
          *type,
          std::move(*element_name),
          std::move(*added_time),
          std::move(*modified_time),
          *ref_count,
          *mount_id};
}

auto ElementMapper::ToParams(const std::shared_ptr<SleeveElement>& source) -> ElementMapperParams {
  std::string utf_8_str = conv::ToBytes(source->element_name_);
  return {source->element_id_,
          static_cast<uint32_t>(source->type_),
          std::make_unique<std::string>(utf_8_str),
          std::make_unique<std::string>(FormatUtcTimestampText(source->added_time_)),
          std::make_unique<std::string>(FormatUtcTimestampText(source->last_modified_time_)),
          source->ref_count_,
          source->mount_id_.has_value()
              ? std::optional<int64_t>{static_cast<int64_t>(*source->mount_id_)}
              : std::nullopt};
}

auto ElementMapper::FromParams(ElementMapperParams&& param) -> std::shared_ptr<SleeveElement> {
  auto                           id           = param.id;
  auto                           type         = static_cast<ElementType>(param.type);
  auto                           element_name = conv::FromBytes(std::move(*param.element_name));
  std::shared_ptr<SleeveElement> element;
  if (type == ElementType::FILE) {
    element = std::make_shared<SleeveFile>(id, element_name);
  } else if (type == ElementType::FOLDER) {
    element = std::make_shared<SleeveFolder>(id, element_name);
  } else {
    throw std::runtime_error("ElementMapper: Invalid ElementMapperParams");
  }

  element->added_time_ = param.added_time ? ParseUtcTimestampText(*param.added_time) : 0;
  element->last_modified_time_ =
      param.modified_time ? ParseUtcTimestampText(*param.modified_time) : 0;
  element->ref_count_ = param.ref_count;
  element->mount_id_  = ReadMountId(param);

  return element;
}

auto ElementMapper::GetElementById(const sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  auto row = GetById(id);
  if (!row.has_value()) {
    throw std::runtime_error(
        std::format("ElementMapper: No element with id {} is stored in DB", id));
  }
  return FromParams(std::move(*row));
}

void ElementMapper::RestoreFields(SleeveElement& element, const ElementMapperParams& param) {
  if (element.element_id_ != param.id || element.type_ != static_cast<ElementType>(param.type) ||
      !param.element_name) {
    throw std::runtime_error("ElementMapper: stored element identity does not match live element");
  }
  auto       name     = conv::FromBytes(*param.element_name);
  const auto added    = param.added_time ? ParseUtcTimestampText(*param.added_time) : 0;
  const auto modified = param.modified_time ? ParseUtcTimestampText(*param.modified_time) : 0;
  const auto mount_id = ReadMountId(param);
  element.element_name_       = std::move(name);
  element.added_time_         = added;
  element.last_modified_time_ = modified;
  element.ref_count_          = param.ref_count;
  element.mount_id_           = mount_id;
}

auto ElementMapper::GetElementByName(const std::wstring& name)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  return GetByPredicate(duckorm::expr::column_eq("element_name", conv::ToBytes(name)));
}

auto ElementMapper::GetElementByType(const ElementType type)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  return GetByPredicate(
      duckorm::expr::column_eq("type", static_cast<std::int64_t>(static_cast<uint32_t>(type))));
}

auto ElementMapper::GetElementsByQuery(const duckorm::SqlFragment& query)
    -> std::vector<std::shared_ptr<SleeveElement>> {
  return GetByQuery(query);
}
}  // namespace alcedo
