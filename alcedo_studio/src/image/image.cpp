//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/image.hpp"

#include <algorithm>
#include <xxhash.h>

#include <cstdint>
#include <exception>
#include <exiv2/exif.hpp>
#include <exiv2/tags.hpp>
#include <json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo {
using json = nlohmann::json;

namespace {
constexpr auto kRawRuntimeContextJsonKey = "RawRuntimeColorContext";

template <typename T>
auto MakeJsonArray(const T* values, const int count) -> json {
  json out = json::array();
  if (!values || count <= 0) {
    return out;
  }

  for (int i = 0; i < count; ++i) {
    out.push_back(values[i]);
  }
  return out;
}

template <typename T>
void LoadJsonArray(const json& value, const char* key, T* dst, const int count) {
  if (!dst || count <= 0 || !value.contains(key) || !value[key].is_array()) {
    return;
  }

  const auto& array = value[key];
  const auto  limit = std::min<int>(count, static_cast<int>(array.size()));
  for (int i = 0; i < limit; ++i) {
    dst[i] = array[static_cast<size_t>(i)].get<T>();
  }
}
}  // namespace

auto RawColorContextToJson(const RawRuntimeColorContext& ctx) -> json {
  json value;
  value["DngColorProfile"] = DngColorProfileToJson(ctx.dng_profile_);
  value["Valid"]                      = ctx.valid_;
  value["OutputInCameraSpace"]        = ctx.output_in_camera_space_;
  value["CamMul"]              = MakeJsonArray(ctx.cam_mul_, 3);
  value["PreMul"]              = MakeJsonArray(ctx.pre_mul_, 3);
  value["CamXyz"]              = MakeJsonArray(ctx.cam_xyz_, 9);
  value["RgbCam"]              = MakeJsonArray(ctx.rgb_cam_, 9);
  value["CameraMake"]          = ctx.camera_make_;
  value["CameraModel"]         = ctx.camera_model_;
  value["LensMetadataValid"]   = ctx.lens_metadata_valid_;
  value["LensMake"]            = ctx.lens_make_;
  value["LensModel"]           = ctx.lens_model_;
  value["FocalLengthMm"]       = ctx.focal_length_mm_;
  value["ApertureFNumber"]     = ctx.aperture_f_number_;
  value["FocusDistanceM"]             = ctx.focus_distance_m_;
  value["Focal35mmMm"]                = ctx.focal_35mm_mm_;
  value["CropFactorHint"]             = ctx.crop_factor_hint_;
  value["DngWarpRectilinearPresent"] = ctx.dng_warp_rectilinear_present_;
  value["DngWarpRectilinearApplied"]  = ctx.dng_warp_rectilinear_applied_;
  value["ColorMatricesValid"]         = ctx.color_matrices_valid_;
  value["ColorMatrix1"]               = MakeJsonArray(ctx.color_matrix_1_, 9);
  value["ColorMatrix2"]               = MakeJsonArray(ctx.color_matrix_2_, 9);
  value["ForwardMatricesValid"] = ctx.forward_matrices_valid_;
  value["ForwardMatrix1"]       = MakeJsonArray(ctx.forward_matrix_1_, 9);
  value["ForwardMatrix2"]       = MakeJsonArray(ctx.forward_matrix_2_, 9);
  value["AsShotNeutralValid"]  = ctx.as_shot_neutral_valid_;
  value["AsShotNeutral"]       = MakeJsonArray(ctx.as_shot_neutral_, 3);
  value["CalibrationIlluminantsValid"] = ctx.calibration_illuminants_valid_;
  value["ColorMatrix1Cct"]     = ctx.color_matrix_1_cct_;
  value["ColorMatrix2Cct"]     = ctx.color_matrix_2_cct_;
  return value;
}

auto RawColorContextFromJson(const json& value, RawRuntimeColorContext& ctx) -> bool {
  if (!value.is_object()) {
    return false;
  }

  ctx.dng_profile_ = DngColorProfileFromJson(value.value("DngColorProfile", json(nullptr)));
  ctx.valid_                  = value.value("Valid", false);
  ctx.output_in_camera_space_ = value.value("OutputInCameraSpace", false);
  LoadJsonArray(value, "CamMul", ctx.cam_mul_, 3);
  LoadJsonArray(value, "PreMul", ctx.pre_mul_, 3);
  LoadJsonArray(value, "CamXyz", ctx.cam_xyz_, 9);
  LoadJsonArray(value, "RgbCam", ctx.rgb_cam_, 9);
  ctx.camera_make_            = value.value("CameraMake", std::string{});
  ctx.camera_model_           = value.value("CameraModel", std::string{});
  ctx.lens_metadata_valid_    = value.value("LensMetadataValid", false);
  ctx.lens_make_              = value.value("LensMake", std::string{});
  ctx.lens_model_             = value.value("LensModel", std::string{});
  ctx.focal_length_mm_        = value.value("FocalLengthMm", 0.0f);
  ctx.aperture_f_number_      = value.value("ApertureFNumber", 0.0f);
  ctx.focus_distance_m_              = value.value("FocusDistanceM", 0.0f);
  ctx.focal_35mm_mm_                 = value.value("Focal35mmMm", 0.0f);
  ctx.crop_factor_hint_              = value.value("CropFactorHint", 0.0f);
  ctx.dng_warp_rectilinear_present_ = value.value("DngWarpRectilinearPresent", false);
  ctx.dng_warp_rectilinear_applied_ = value.value("DngWarpRectilinearApplied", false);
  ctx.color_matrices_valid_          = value.value("ColorMatricesValid", false);
  LoadJsonArray(value, "ColorMatrix1", ctx.color_matrix_1_, 9);
  LoadJsonArray(value, "ColorMatrix2", ctx.color_matrix_2_, 9);
  ctx.forward_matrices_valid_ = value.value("ForwardMatricesValid", false);
  LoadJsonArray(value, "ForwardMatrix1", ctx.forward_matrix_1_, 9);
  LoadJsonArray(value, "ForwardMatrix2", ctx.forward_matrix_2_, 9);
  ctx.as_shot_neutral_valid_  = value.value("AsShotNeutralValid", false);
  LoadJsonArray(value, "AsShotNeutral", ctx.as_shot_neutral_, 3);
  ctx.calibration_illuminants_valid_ = value.value("CalibrationIlluminantsValid", false);
  ctx.color_matrix_1_cct_     = value.value("ColorMatrix1Cct", 2856.0);
  ctx.color_matrix_2_cct_     = value.value("ColorMatrix2Cct", 6504.0);

  return ctx.valid_ || ctx.color_matrices_valid_ || ctx.forward_matrices_valid_ ||
         !ctx.camera_make_.empty() ||
         !ctx.camera_model_.empty();
}

Image::Image(image_id_t image_id) : image_id_(image_id) {}

/**
 * @brief Construct a new Image object
 *
 * @param image_id the interal uid given to the new image
 * @param image_path the disk location of the image
 * @param image_type the type of the image
 */
Image::Image(image_id_t image_id, image_path_t image_path, ImageType image_type)
    : image_id_(image_id), image_path_(image_path), image_type_(image_type) {}

Image::Image(image_id_t image_id, image_path_t image_path, file_name_t image_name,
             ImageType image_type)
    : image_id_(image_id),
      image_path_(image_path),
      image_name_(image_name),
      image_type_(image_type) {}

Image::Image(image_path_t image_path, ImageType image_type)
    : image_path_(image_path), image_type_(image_type) {}

Image::Image(Image&& other)
    : image_id_(other.image_id_),
      image_path_(std::move(other.image_path_)),
      exif_data_(std::move(other.exif_data_)),
      image_data_(std::move(other.image_data_)),
      thumbnail_(std::move(other.thumbnail_)),
      image_type_(other.image_type_) {}

std::wostream& operator<<(std::wostream& os, const Image& img) {
  os << "img_id: " << img.image_id_ << "\timage_path: " << img.image_path_.wstring()
     << L"\tAdded Time: ";
  return os;
}

/**
 * @brief Load image data into an image object
 *
 * @param image_data
 */
void Image::LoadOriginalData(ImageBuffer&& load_image) {
  image_data_   = std::move(load_image);
  has_full_img_ = true;
}

void Image::LoadThumbnailData(ImageBuffer&& thumbnail) {
  thumbnail_     = std::move(thumbnail);
  has_thumbnail_ = true;
}

void Image::ClearData() {
  image_data_.ReleaseCPUData();
  has_full_img_ = false;
}

void Image::ClearThumbnail() {
  thumbnail_.ReleaseCPUData();
  has_thumbnail_ = false;
}

auto Image::ExifToJson() -> std::string {
  if (has_exif_display_) {
    exif_json_ = exif_display_.ToJson();
  } else if (!has_exif_json_) {
    exif_json_ = exif_display_.ToJson();
  }

  if (has_raw_color_context_) {
    exif_json_[kRawRuntimeContextJsonKey] = RawColorContextToJson(raw_color_context_);
  }

  has_exif_json_ = true;
  return nlohmann::to_string(exif_json_);
}

void Image::JsonToExif(std::string json_str) {
  try {
    exif_json_     = nlohmann::json::parse(json_str);
    has_exif_json_ = true;
    exif_display_.FromJson(exif_json_);
    has_exif_display_ = true;
    raw_color_context_ = {};
    has_raw_color_context_ =
        exif_json_.contains(kRawRuntimeContextJsonKey) &&
        RawColorContextFromJson(exif_json_[kRawRuntimeContextJsonKey], raw_color_context_);
  } catch (nlohmann::json::parse_error& e) {
    throw std::runtime_error("[ERROR] Image: JSON parse error, " + std::string(e.what()));
  } catch (std::exception& e) {
    throw std::runtime_error("[ERROR] Image: JSON to Exif conversion error, " +
                             std::string(e.what()));
  }
}

void Image::SetId(image_id_t image_id) { image_id_ = image_id; }

void Image::SetExifDisplayMetaData(ExifDisplayMetaData&& exif_display) {
  exif_display_     = std::move(exif_display);
  has_exif_display_ = true;
  has_exif_json_    = false;
  if (sync_state_.load() == ImageSyncState::SYNCED) {
    sync_state_ = ImageSyncState::MODIFIED;
  }
}

void Image::SetHdrDisplayMetadata(bool is_hdr) {
  if (has_exif_json_ && exif_json_.is_object() && !has_exif_display_) {
    exif_display_.FromJson(exif_json_);
    has_exif_display_ = true;
  }
  exif_display_.is_hdr_ = is_hdr;
  has_exif_display_    = true;
  has_exif_json_       = false;
  if (sync_state_.load() == ImageSyncState::SYNCED) {
    sync_state_ = ImageSyncState::MODIFIED;
  }
}

void Image::SetRawColorContext(RawRuntimeColorContext&& ctx) {
  raw_color_context_     = std::move(ctx);
  has_raw_color_context_ = true;
  has_exif_json_         = false;
  if (sync_state_.load() == ImageSyncState::SYNCED) {
    sync_state_ = ImageSyncState::MODIFIED;
  }
}

auto Image::GetRawColorContext() const -> const RawRuntimeColorContext& {
  return raw_color_context_;
}

auto Image::HasRawColorContext() const -> bool { return has_raw_color_context_.load(); }

void Image::ComputeChecksum() { checksum_ = XXH3_64bits(this, sizeof(*this)); }

auto Image::GetImageData() -> cv::Mat& { return image_data_.GetCPUData(); }

auto Image::GetThumbnailData() -> cv::Mat& { return thumbnail_.GetCPUData(); }

auto Image::GetThumbnailBuffer() -> ImageBuffer& { return thumbnail_; }

void Image::MarkSyncState(ImageSyncState state) { sync_state_ = state; }

auto Image::GetSyncState() -> ImageSyncState { return sync_state_.load(); }
};  // namespace alcedo
