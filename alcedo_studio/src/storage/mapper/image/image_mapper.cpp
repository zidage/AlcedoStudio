//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/image/image_mapper.hpp"

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "image/image.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
auto ImageMapper::FromRawData(std::vector<duckorm::VarTypes>&& data) -> ImageMapperParams {
  if (data.size() != FieldCount()) {
    throw std::runtime_error("Invalid DuckFieldDesc for Image");
  }
  auto id         = std::get_if<sl_element_id_t>(&data[0]);
  auto image_path = std::get_if<std::unique_ptr<std::string>>(&data[1]);
  auto file_name  = std::get_if<std::unique_ptr<std::string>>(&data[2]);
  auto type       = std::get_if<uint32_t>(&data[3]);
  auto metadata   = std::get_if<std::unique_ptr<std::string>>(&data[4]);

  if (id == nullptr || image_path == nullptr || file_name == nullptr || type == nullptr ||
      metadata == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  return {*id, std::move(*image_path), std::move(*file_name), *type, std::move(*metadata)};
}

auto ImageMapper::ToParams(const std::shared_ptr<Image> source) -> ImageMapperParams {
  std::string utf8_path     = conv::ToBytes(source->image_path_.wstring());
  std::string utf8_img_name = conv::ToBytes(source->image_name_);
  return {source->image_id_, std::make_unique<std::string>(utf8_path),
          std::make_unique<std::string>(utf8_img_name), static_cast<uint32_t>(source->image_type_),
          std::make_unique<std::string>(source->ExifToJson())};
}

auto ImageMapper::FromParams(ImageMapperParams&& param) -> std::shared_ptr<Image> {
  auto recovered = std::make_shared<Image>(
      param.id, std::filesystem::path(conv::FromBytes(std::move(*param.image_path))),
      conv::FromBytes(std::move(*param.file_name)), static_cast<ImageType>(param.type));
  recovered->JsonToExif(std::move(*param.metadata));
  return recovered;
}

auto ImageMapper::GetImageById(const image_id_t id) -> std::vector<std::shared_ptr<Image>> {
  std::string predicate = std::format("id={}", id);
  return GetByPredicate(std::move(predicate));
}

auto ImageMapper::GetImageByName(const std::wstring& name) -> std::vector<std::shared_ptr<Image>> {
  std::wstring predicate_w = std::format(L"file_name={}", name);
  return GetByPredicate(conv::ToBytes(predicate_w));
}

auto ImageMapper::GetImageByPath(const std::filesystem::path path)
    -> std::vector<std::shared_ptr<Image>> {
  std::wstring predicate_w = std::format(L"image_path={}", path.wstring());
  return GetByPredicate(conv::ToBytes(predicate_w));
}

auto ImageMapper::GetImageByType(const ImageType type) -> std::vector<std::shared_ptr<Image>> {
  std::string predicate = std::format("type={}", static_cast<uint32_t>(type));
  return GetByPredicate(std::move(predicate));
}
}  // namespace alcedo
