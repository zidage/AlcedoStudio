//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "image/image.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "storage/mapper/mapper.hpp"
#include "type/type.hpp"

namespace alcedo {
/**
 * @brief One Image row (see the `Image` table in database.hpp).
 *
 * The first five fields are the stored Image. The other fields are search columns that
 * ImageMapper::ToParams derives from the Image on every insert and update (see
 * image_search_columns.hpp). Library search, library stats, and the thumbnail filter read
 * these columns instead of the metadata JSON. FromParams ignores them.
 */
struct ImageMapperParams {
  image_id_t                   id;
  std::unique_ptr<std::string> image_path;
  std::unique_ptr<std::string> file_name;
  uint32_t                     type;
  std::unique_ptr<std::string> metadata;

  std::string                  file_stem_;
  std::string                  file_ext_;
  std::string                  capture_at_;    ///< `YYYY-MM-DD HH:MM:SS`; empty writes NULL.
  std::string                  capture_date_;  ///< `YYYY-MM-DD`; empty writes NULL.
  std::string                  camera_make_;
  std::string                  camera_model_;
  std::string                  lens_;
  std::optional<int64_t>       iso_;
  std::optional<double>        focal_mm_;
  std::optional<double>        aperture_;
  int32_t                      rating_ = 0;
  std::optional<int64_t>       pixel_count_;
  std::string                  file_search_text_;
  std::string                  exif_search_text_;
  std::string                  exif_search_words_;
};

/**
 * @brief Single-table mapper for Image rows and domain Image objects.
 */
class ImageMapper
    : public Mapper<ImageMapper, std::shared_ptr<Image>, ImageMapperParams, image_id_t>,
      public FieldReflectable<ImageMapper> {
 private:
  static constexpr uint32_t                                         field_count_      = 20;
  static constexpr const char*                                      table_name_       = "Image";
  static constexpr const char*                                      prime_key_clause_ = "id={}";
  // Order matches the `Image` table DDL, because duckorm select reads `SELECT *`.
  static constexpr std::array<duckorm::DuckFieldDesc, field_count_> field_descs_      = {
      FIELD(ImageMapperParams, id, UINT32),
      FIELD(ImageMapperParams, image_path, VARCHAR),
      FIELD(ImageMapperParams, file_name, VARCHAR),
      FIELD(ImageMapperParams, type, UINT32),
      FIELD(ImageMapperParams, metadata, VARCHAR),
      FIELD_AS(ImageMapperParams, file_stem_, "file_stem", STRING),
      FIELD_AS(ImageMapperParams, file_ext_, "file_ext", STRING),
      FIELD_AS(ImageMapperParams, capture_at_, "capture_at", NULLABLE_STRING),
      FIELD_AS(ImageMapperParams, capture_date_, "capture_date", NULLABLE_STRING),
      FIELD_AS(ImageMapperParams, camera_make_, "camera_make", STRING),
      FIELD_AS(ImageMapperParams, camera_model_, "camera_model", STRING),
      FIELD_AS(ImageMapperParams, lens_, "lens", STRING),
      FIELD_AS(ImageMapperParams, iso_, "iso", NULLABLE_INT64),
      FIELD_AS(ImageMapperParams, focal_mm_, "focal_mm", NULLABLE_DOUBLE),
      FIELD_AS(ImageMapperParams, aperture_, "aperture", NULLABLE_DOUBLE),
      FIELD_AS(ImageMapperParams, rating_, "rating", INT32),
      FIELD_AS(ImageMapperParams, pixel_count_, "pixel_count", NULLABLE_INT64),
      FIELD_AS(ImageMapperParams, file_search_text_, "file_search_text", STRING),
      FIELD_AS(ImageMapperParams, exif_search_text_, "exif_search_text", STRING),
      FIELD_AS(ImageMapperParams, exif_search_words_, "exif_search_words", STRING)};

 public:
  static auto FromRawData(std::vector<duckorm::VarTypes>&& data) -> ImageMapperParams;
  /// Build the row for @p source, including the derived search columns. Reads the Image
  /// display metadata; the caller keeps @p source alive and serializes writers to it.
  static auto ToParams(const std::shared_ptr<Image> source) -> ImageMapperParams;
  static auto FromParams(ImageMapperParams&& param) -> std::shared_ptr<Image>;

  auto        GetImageById(const image_id_t id) -> std::vector<std::shared_ptr<Image>>;
  auto        GetImageByName(const std::wstring& name) -> std::vector<std::shared_ptr<Image>>;
  auto GetImageByPath(const std::filesystem::path path) -> std::vector<std::shared_ptr<Image>>;
  auto GetImageByType(const ImageType type) -> std::vector<std::shared_ptr<Image>>;

  friend struct FieldReflectable<ImageMapper>;
  using Mapper::Mapper;
};
}  // namespace alcedo
