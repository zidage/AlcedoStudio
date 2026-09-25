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
#include "image/metadata.hpp"
#include "storage/mapper/image/image_search_columns.hpp"
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
  // The search columns (data[5..]) are derived from the metadata; FromParams does not use
  // them, so they are not read back.
  ImageMapperParams params;
  params.id         = *id;
  params.image_path = std::move(*image_path);
  params.file_name  = std::move(*file_name);
  params.type       = *type;
  params.metadata   = std::move(*metadata);
  return params;
}

auto ImageMapper::ToParams(const std::shared_ptr<Image> source) -> ImageMapperParams {
  ImageMapperParams params;
  params.id         = source->image_id_;
  params.image_path = std::make_unique<std::string>(conv::ToBytes(source->image_path_.wstring()));
  params.file_name  = std::make_unique<std::string>(conv::ToBytes(source->image_name_));
  params.type       = static_cast<uint32_t>(source->image_type_);
  params.metadata   = std::make_unique<std::string>(source->ExifToJson());

  // ExifToJson has refreshed exif_json_ from the display metadata when the display metadata
  // is present, so this reads the same values that the metadata column stores.
  if (source->has_exif_display_.load()) {
    FillImageSearchColumns(source->image_name_, source->image_path_, source->exif_display_, params);
  } else {
    ExifDisplayMetaData display;
    display.FromJson(source->exif_json_);
    FillImageSearchColumns(source->image_name_, source->image_path_, display, params);
  }
  return params;
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
