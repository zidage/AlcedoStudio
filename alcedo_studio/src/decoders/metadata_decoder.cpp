//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/metadata_decoder.hpp"

#include <libraw/libraw.h>

#include <chrono>
#include <exiv2/basicio.hpp>
#include <exiv2/exif.hpp>
#include <exiv2/image.hpp>
#include <exiv2/tags.hpp>
#include <exiv2/types.hpp>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>

#include "image/metadata_extractor.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {
auto RationalToFloat(const Exiv2::Rational& value) -> float {
  if (value.second == 0) {
    return 0.0f;
  }
  return static_cast<float>(value.first) / static_cast<float>(value.second);
}

auto ReadRatingValue(int64_t rating) -> int {
  if (rating < ExifDisplayMetaData::kMinRating || rating > ExifDisplayMetaData::kMaxRating) {
    return ExifDisplayMetaData::kMinRating;
  }
  return static_cast<int>(rating);
}

auto ReadRatingFromXmp(const Exiv2::XmpData& xmp_data) -> std::optional<int> {
  const auto it = xmp_data.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
  if (it == xmp_data.end()) {
    return std::nullopt;
  }
  return ReadRatingValue(it->toInt64());
}

auto ReadRatingFromExif(const Exiv2::ExifData& exif_data) -> std::optional<int> {
  const auto it = exif_data.findKey(Exiv2::ExifKey("Exif.Image.Rating"));
  if (it == exif_data.end()) {
    return std::nullopt;
  }
  return ReadRatingValue(it->toInt64());
}

void PopulateStandardRating(const Exiv2::Image& exif_image, ExifDisplayMetaData& display_metadata) {
  if (const auto xmp_rating = ReadRatingFromXmp(exif_image.xmpData()); xmp_rating.has_value()) {
    display_metadata.rating_ = *xmp_rating;
    return;
  }
  if (const auto exif_rating = ReadRatingFromExif(exif_image.exifData()); exif_rating.has_value()) {
    display_metadata.rating_ = *exif_rating;
  }
}
}  // namespace

void GetDisplayMetadataFromExif(Exiv2::ExifData& exif_data, ExifDisplayMetaData& display_metadata) {
  if (exif_data.empty()) {
    return;
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.Make")) != exif_data.end()) {
    display_metadata.make_ = exif_data["Exif.Image.Make"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.Model")) != exif_data.end()) {
    display_metadata.model_ = exif_data["Exif.Image.Model"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.LensModel")) != exif_data.end()) {
    display_metadata.lens_ = exif_data["Exif.Photo.LensModel"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.LensMake")) != exif_data.end()) {
    display_metadata.lens_make_ = exif_data["Exif.Photo.LensMake"].toString();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.FNumber")) != exif_data.end()) {
    display_metadata.aperture_ = RationalToFloat(exif_data["Exif.Photo.FNumber"].toRational());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.FocalLength")) != exif_data.end()) {
    display_metadata.focal_ = RationalToFloat(exif_data["Exif.Photo.FocalLength"].toRational());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.FocalLengthIn35mmFilm")) != exif_data.end()) {
    display_metadata.focal_35mm_ =
        static_cast<float>(exif_data["Exif.Photo.FocalLengthIn35mmFilm"].toInt64());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.SubjectDistance")) != exif_data.end()) {
    display_metadata.focus_distance_m_ =
        RationalToFloat(exif_data["Exif.Photo.SubjectDistance"].toRational());
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Photo.ISOSpeedRatings")) != exif_data.end()) {
    display_metadata.iso_ = exif_data["Exif.Photo.ISOSpeedRatings"].toInt64();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ShutterSpeedValue")) != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Image.ShutterSpeedValue"].toRational();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ImageLength")) != exif_data.end()) {
    display_metadata.height_ = exif_data["Exif.Image.ImageLength"].toUint32();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth")) != exif_data.end()) {
    display_metadata.width_ = exif_data["Exif.Image.ImageWidth"].toUint32();
  }
  if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.DateTime")) != exif_data.end()) {
    display_metadata.date_time_str_ = exif_data["Exif.Image.DateTime"].toString();
    if (display_metadata.date_time_str_.size() >= 10) {
      display_metadata.date_time_str_[4] =
          '-';  // Change from "YYYY:MM:DD HH:MM:SS" to "YYYY-MM-DD HH:MM:SS"
      display_metadata.date_time_str_[7] = '-';
    }
  }
}

namespace {
void PopulateDisplayMetadata(Image& image, const std::filesystem::path& file_path) {
  ExifDisplayMetaData display{};
  if (image.exif_data_ && !image.exif_data_->exifData().empty()) {
    GetDisplayMetadataFromExif(image.exif_data_->exifData(), display);
  }
  if (image.exif_data_) {
    PopulateStandardRating(*image.exif_data_, display);
  }
  display.is_hdr_ = MetadataExtractor::DetectHdrDisplayMetadata(file_path, image.exif_data_.get());
  image.SetExifDisplayMetaData(std::move(display));
}
}  // namespace

/**
 * @brief A callback used to parse the basic information of a image file
 *
 * @param buffer
 * @param file_path
 * @param result
 * @param id
 * @param promise
 */
void MetadataDecoder::Decode(std::vector<char> buffer, std::filesystem::path file_path,
                             std::shared_ptr<BufferQueue> result, image_id_t id,
                             std::shared_ptr<std::promise<image_id_t>> promise) {
  try {
    std::shared_ptr<Image> img =
        std::make_shared<Image>(id, file_path, file_path.filename().wstring(), ImageType::DEFAULT);

    img->exif_data_ = Exiv2::ImageFactory::open((Exiv2::byte*)buffer.data(), buffer.size());
    img->exif_data_->readMetadata();
    img->has_exif_ = !img->exif_data_->exifData().empty();

    PopulateDisplayMetadata(*img, file_path);

    result->push(img);
    promise->set_value(id);

    return;
  } catch (std::exception& e) {
    // TODO: Append error message to log
    std::cout << e.what() << std::endl;
  }
  // If it fails to read metadata, produce a plain image with minimum metadata
  std::shared_ptr<Image> img =
      std::make_shared<Image>(id, file_path, file_path.filename().wstring(), ImageType::DEFAULT);
  result->push(img);
  promise->set_value(id);
}

void MetadataDecoder::Decode(std::vector<char> buffer, std::shared_ptr<Image> source_img,
                             std::shared_ptr<BufferQueue>              result,
                             std::shared_ptr<std::promise<image_id_t>> promise) {
  try {
    source_img->exif_data_ = Exiv2::ImageFactory::open((Exiv2::byte*)buffer.data(), buffer.size());
    source_img->exif_data_->readMetadata();
    source_img->has_exif_ = !source_img->exif_data_->exifData().empty();
    PopulateDisplayMetadata(*source_img, source_img->image_path_);
    result->push(source_img);
    promise->set_value(source_img->image_id_);

    return;
  } catch (std::exception& e) {
    // TODO: Append error message to log
    std::cout << e.what() << std::endl;
  }
  // If it fails to read metadata, produce a plain image with minimum metadata
  result->push(source_img);
  promise->set_value(source_img->image_id_);
}

};  // namespace alcedo
