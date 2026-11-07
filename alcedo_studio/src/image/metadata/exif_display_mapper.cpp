//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "exif_display_mapper.hpp"

#include <cstdint>
#include <exiv2/xmp_exiv2.hpp>
#include <optional>

#include "metadata_values.hpp"

namespace alcedo::metadata_detail {
namespace {
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
}  // namespace

void GetDisplayMetadataFromExif(Exiv2::ExifData& exif_data, ExifDisplayMetaData& display_metadata) {
  if (exif_data.empty()) {
    return;
  }
  const auto find_key = [&exif_data](const char* key) {
    return exif_data.findKey(Exiv2::ExifKey(key));
  };
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
  } else if (find_key("Exif.Photo.ISOSpeed") != exif_data.end()) {
    display_metadata.iso_ = exif_data["Exif.Photo.ISOSpeed"].toInt64();
  }
  if (find_key("Exif.Photo.ExposureTime") != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Photo.ExposureTime"].toRational();
  } else if (find_key("Exif.Photo.ShutterSpeedValue") != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Photo.ShutterSpeedValue"].toRational();
  } else if (find_key("Exif.Image.ShutterSpeedValue") != exif_data.end()) {
    display_metadata.shutter_speed_ = exif_data["Exif.Image.ShutterSpeedValue"].toRational();
  }
  if (find_key("Exif.Photo.PixelYDimension") != exif_data.end()) {
    display_metadata.height_ = exif_data["Exif.Photo.PixelYDimension"].toUint32();
  } else if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ImageLength")) != exif_data.end()) {
    display_metadata.height_ = exif_data["Exif.Image.ImageLength"].toUint32();
  }
  if (find_key("Exif.Photo.PixelXDimension") != exif_data.end()) {
    display_metadata.width_ = exif_data["Exif.Photo.PixelXDimension"].toUint32();
  } else if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.ImageWidth")) != exif_data.end()) {
    display_metadata.width_ = exif_data["Exif.Image.ImageWidth"].toUint32();
  }
  if (find_key("Exif.Photo.DateTimeOriginal") != exif_data.end()) {
    display_metadata.date_time_str_ = exif_data["Exif.Photo.DateTimeOriginal"].toString();
  } else if (exif_data.findKey(Exiv2::ExifKey("Exif.Image.DateTime")) != exif_data.end()) {
    display_metadata.date_time_str_ = exif_data["Exif.Image.DateTime"].toString();
  }
  if (display_metadata.date_time_str_.size() >= 10) {
    display_metadata.date_time_str_[4] =
        '-';  // Change from "YYYY:MM:DD HH:MM:SS" to "YYYY-MM-DD HH:MM:SS"
    display_metadata.date_time_str_[7] = '-';
  }
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

}  // namespace alcedo::metadata_detail
