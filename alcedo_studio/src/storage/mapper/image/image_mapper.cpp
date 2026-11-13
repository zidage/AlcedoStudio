//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/image/image_mapper.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "image/color_label.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/image/image_search_columns.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
// Positions of the library mark columns in field_descs_ (the end of the `Image` table).
constexpr size_t kColorLabelsColumn = 20;
constexpr size_t kDeleteMarkColumn  = 21;
}  // namespace

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
  // The library marks are the last two columns. duckorm reads a BOOLEAN cell as its text.
  auto color_labels = std::get_if<int32_t>(&data[kColorLabelsColumn]);
  auto delete_mark  = std::get_if<std::unique_ptr<std::string>>(&data[kDeleteMarkColumn]);
  if (color_labels == nullptr || delete_mark == nullptr) {
    throw std::runtime_error("Encounting unmatching types when parsing the data from the DB");
  }
  // The search columns (data[5..19]) are derived from the metadata; FromParams does not use
  // them, so they are not read back.
  ImageMapperParams params;
  params.id            = *id;
  params.image_path    = std::move(*image_path);
  params.file_name     = std::move(*file_name);
  params.type          = *type;
  params.metadata      = std::move(*metadata);
  params.color_labels_ = *color_labels;
  params.delete_mark_  = *delete_mark != nullptr && **delete_mark == "true";
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
  params.color_labels_ = static_cast<int32_t>(source->color_labels_ & kColorLabelMaskAll);
  params.delete_mark_  = source->delete_mark_;
  return params;
}

auto ImageMapper::FromParams(ImageMapperParams&& param) -> std::shared_ptr<Image> {
  auto recovered = std::make_shared<Image>(
      param.id, std::filesystem::path(conv::FromBytes(std::move(*param.image_path))),
      conv::FromBytes(std::move(*param.file_name)), static_cast<ImageType>(param.type));
  recovered->JsonToExif(std::move(*param.metadata));
  recovered->color_labels_ =
      static_cast<ColorLabelMask>(param.color_labels_) & kColorLabelMaskAll;
  recovered->delete_mark_ = param.delete_mark_;
  return recovered;
}

auto ImageMapper::GetImageById(const image_id_t id) -> std::vector<std::shared_ptr<Image>> {
  return GetByPredicate(KeyEquals(id));
}

auto ImageMapper::GetImageByName(const std::wstring& name) -> std::vector<std::shared_ptr<Image>> {
  return GetByPredicate(duckorm::expr::column_eq("file_name", conv::ToBytes(name)));
}

auto ImageMapper::GetImageByPath(const std::filesystem::path path)
    -> std::vector<std::shared_ptr<Image>> {
  // ToParams stores conv::ToBytes(path.wstring()), so the bound text equals the stored text.
  return GetByPredicate(duckorm::expr::column_eq("image_path", conv::ToBytes(path.wstring())));
}

auto ImageMapper::GetImageByType(const ImageType type) -> std::vector<std::shared_ptr<Image>> {
  return GetByPredicate(
      duckorm::expr::column_eq("type", static_cast<std::int64_t>(static_cast<uint32_t>(type))));
}
}  // namespace alcedo
