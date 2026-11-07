//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "libraw_metadata_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "camera_color_matrix_db.hpp"
#include "metadata_values.hpp"
#include "nikon_lens_id_table.hpp"

namespace alcedo::metadata_detail {
namespace {
void SetDisplayDimensionsFromLibRaw(const LibRaw& raw_processor, ExifDisplayMetaData& display) {
  uint32_t width  = static_cast<uint32_t>(raw_processor.imgdata.sizes.width);
  uint32_t height = static_cast<uint32_t>(raw_processor.imgdata.sizes.height);
  // The develop output is rotated for LibRaw flip 5/6. Persist the dimensions
  // of that rotated output, not the unrotated sensor rectangle.
  if (raw_processor.imgdata.sizes.flip == 5 || raw_processor.imgdata.sizes.flip == 6) {
    std::swap(width, height);
  }
  if (width > 0) display.width_ = width;
  if (height > 0) display.height_ = height;
}

/// `YYYY:MM:DD HH:MM:SS` EXIF date text as `YYYY-MM-DD HH:MM:SS`.
/// @return Empty when the text is not a complete date and time, or is the zero date that EXIF
/// uses for an unknown date.
auto NormalizeExifDateTimeText(std::string_view text) -> std::string {
  constexpr std::string_view kLayout = "dddd:dd:dd dd:dd:dd";
  if (text.size() < kLayout.size()) {
    return {};
  }
  for (std::size_t i = 0; i < kLayout.size(); ++i) {
    const bool is_valid =
        kLayout[i] == 'd' ? (text[i] >= '0' && text[i] <= '9') : text[i] == kLayout[i];
    if (!is_valid) {
      return {};
    }
  }
  const auto two_digits = [text](std::size_t pos) {
    return (text[pos] - '0') * 10 + (text[pos + 1] - '0');
  };
  const int month = two_digits(5);
  const int day   = two_digits(8);
  if (text.substr(0, 4) == "0000" || month < 1 || month > 12 || day < 1 || day > 31 ||
      two_digits(11) > 23 || two_digits(14) > 59 || two_digits(17) > 59) {
    return {};
  }
  std::string normalized(text.substr(0, kLayout.size()));
  normalized[4] = '-';
  normalized[7] = '-';
  return normalized;
}

/// Capture time of an opened RAW file as `YYYY-MM-DD HH:MM:SS`, the camera clock time.
///
/// Uses the EXIF date text: DateTimeOriginal, then DateTimeDigitized, then TIFF DateTime. A file
/// without EXIF or TIFF date tags (Canon CRW, Sigma X3F) has only the LibRaw timestamp. For these
/// formats LibRaw stores the camera clock time as seconds since 1970 without a time zone
/// conversion, so the UTC conversion `gmtime` returns the camera clock fields.
/// @return Empty when the file has no capture time.
auto FormatLibRawCaptureTime(const LibRaw& raw_processor, const LibRawCaptureTimeText& text)
    -> std::string {
  for (const std::string* exif_text :
       {&text.date_time_original_, &text.date_time_digitized_, &text.date_time_}) {
    if (!exif_text->empty()) {
      return *exif_text;
    }
  }
  const time_t timestamp = raw_processor.imgdata.other.timestamp;
  if (timestamp <= 0) {
    return {};
  }
  std::tm camera_clock{};
#if defined(_WIN32)
  if (gmtime_s(&camera_clock, &timestamp) != 0) {
    return {};
  }
#else
  if (gmtime_r(&timestamp, &camera_clock) == nullptr) {
    return {};
  }
#endif
  char buf[64] = {};
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &camera_clock);
  return buf;
}

void PopulateAsShotNeutralFromLibRawCamMul(const libraw_colordata_t& color,
                                           RawRuntimeColorContext&   ctx) {
  if (!IsFinitePositive(color.cam_mul[0]) || !IsFinitePositive(color.cam_mul[1]) ||
      !IsFinitePositive(color.cam_mul[2])) {
    return;
  }

  double green = static_cast<double>(color.cam_mul[1]);
  if (IsFinitePositive(color.cam_mul[3])) {
    green = 0.5 * (green + static_cast<double>(color.cam_mul[3]));
  }

  ctx.as_shot_neutral_[0] = green / static_cast<double>(color.cam_mul[0]);
  ctx.as_shot_neutral_[1] = 1.0;
  ctx.as_shot_neutral_[2] = green / static_cast<double>(color.cam_mul[2]);
  ctx.as_shot_neutral_valid_ =
      std::all_of(std::begin(ctx.as_shot_neutral_), std::end(ctx.as_shot_neutral_),
                  [](double value) { return std::isfinite(value) && value > 0.0; });
}
}  // namespace

void CollectLibRawCaptureTimeText(void* context, int tag, int type, int len, unsigned int /*ord*/,
                                  void* ifp, INT64 /*base*/) {
  constexpr int kExifAsciiType      = 2;
  constexpr int kExifDateTimeLength = 19;
  if (type != kExifAsciiType || len < kExifDateTimeLength) {
    return;
  }
  // LibRaw passes EXIF IFD tags unchanged, TIFF IFD tags with the IFD number above bit 20, and
  // Panasonic RW2 IFD tags with 0x30000. Other values in bits 16-19 mark GPS, interoperability and
  // maker note IFDs, which have no capture date.
  const int ifd_kind = tag & 0xF0000;
  if (ifd_kind != 0 && ifd_kind != 0x30000) {
    return;
  }
  auto&        text   = *static_cast<LibRawCaptureTimeText*>(context);
  std::string* target = nullptr;
  switch (tag & 0xFFFF) {
    case 0x9003:
      target = &text.date_time_original_;
      break;
    case 0x9004:
      target = &text.date_time_digitized_;
      break;
    case 0x0132:
      target = &text.date_time_;
      break;
    default:
      return;
  }
  if (!target->empty()) {
    return;
  }
  char buf[kExifDateTimeLength] = {};
  if (static_cast<LibRaw_abstract_datastream*>(ifp)->read(buf, 1, kExifDateTimeLength) !=
      kExifDateTimeLength) {
    return;
  }
  *target = NormalizeExifDateTimeText(std::string_view(buf, kExifDateTimeLength));
}

void PopulateDngMetadataHintFromOpenLibRaw(LibRaw& raw_processor, ExifDisplayMetaData& display,
                                           RawRuntimeColorContext& ctx) {
  const std::string raw_make       = TrimAscii(raw_processor.imgdata.idata.make);
  const std::string raw_model      = TrimAscii(raw_processor.imgdata.idata.model);
  const std::string raw_lens_make  = TrimTrailingZeroPadded(raw_processor.imgdata.lens.LensMake);
  std::string       raw_lens_model = TrimTrailingZeroPadded(raw_processor.imgdata.lens.Lens);
  if (raw_lens_model.empty()) {
    raw_lens_model = TrimTrailingZeroPadded(raw_processor.imgdata.lens.makernotes.Lens);
  }

  if (!raw_make.empty()) {
    ctx.camera_make_ = raw_make;
  }
  if (!raw_model.empty()) {
    ctx.camera_model_ = raw_model;
  }
  if (!raw_lens_make.empty()) {
    ctx.lens_make_ = raw_lens_make;
  }
  if (!raw_lens_model.empty()) {
    ctx.lens_model_ = raw_lens_model;
  }

  if (!ctx.camera_make_.empty()) {
    display.make_ = ctx.camera_make_;
  }
  if (!ctx.camera_model_.empty()) {
    display.model_ = ctx.camera_model_;
  }
  if (!ctx.lens_make_.empty()) {
    display.lens_make_ = ctx.lens_make_;
  }
  if (!ctx.lens_model_.empty()) {
    display.lens_ = ctx.lens_model_;
  }

  const float focal_length_mm = raw_processor.imgdata.other.focal_len;
  if (IsFinitePositive(focal_length_mm)) {
    ctx.focal_length_mm_ = focal_length_mm;
    display.focal_       = focal_length_mm;
  }

  const float aperture_f_number = raw_processor.imgdata.other.aperture;
  if (IsFinitePositive(aperture_f_number)) {
    ctx.aperture_f_number_ = aperture_f_number;
    display.aperture_      = aperture_f_number;
  }

  const float focal_35mm_mm =
      raw_processor.imgdata.lens.FocalLengthIn35mmFormat > 0
          ? static_cast<float>(raw_processor.imgdata.lens.FocalLengthIn35mmFormat)
          : 0.0f;
  if (IsFinitePositive(focal_35mm_mm)) {
    ctx.focal_35mm_mm_  = focal_35mm_mm;
    display.focal_35mm_ = focal_35mm_mm;
  }

  const auto iso_speed = static_cast<uint64_t>(raw_processor.imgdata.other.iso_speed);
  if (iso_speed > 0) {
    display.iso_ = iso_speed;
  }

  const float shutter_sec = raw_processor.imgdata.other.shutter;
  if (std::isfinite(shutter_sec) && shutter_sec > 0.0f) {
    if (shutter_sec >= 1.0f) {
      display.shutter_speed_ = {static_cast<int>(shutter_sec), 1};
    } else {
      display.shutter_speed_ = {1, static_cast<int>(1.0f / shutter_sec + 0.5f)};
    }
  }

  // The capture time stays the EXIF date text that Exiv2 read (EXIFToDisplayMetaData).
  SetDisplayDimensionsFromLibRaw(raw_processor, display);
}

void PopulateMetadataRuntimeContext(LibRaw& raw_processor, RawRuntimeColorContext& ctx) {
  const auto& color = raw_processor.imgdata.color;
  for (int i = 0; i < 3; ++i) {
    ctx.cam_mul_[i] = color.cam_mul[i];
    ctx.pre_mul_[i] = color.pre_mul[i];
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      ctx.cam_xyz_[r * 3 + c] = color.cam_xyz[r][c];
      ctx.rgb_cam_[r * 3 + c] = color.rgb_cam[r][c];
    }
  }
  PopulateAsShotNeutralFromLibRawCamMul(color, ctx);

  ctx.camera_make_  = TrimAscii(raw_processor.imgdata.idata.make);
  ctx.camera_model_ = TrimAscii(raw_processor.imgdata.idata.model);

  ctx.lens_make_    = TrimTrailingZeroPadded(raw_processor.imgdata.lens.LensMake);
  ctx.lens_model_   = TrimTrailingZeroPadded(raw_processor.imgdata.lens.Lens);
  if (ctx.lens_model_.empty()) {
    ctx.lens_model_ = TrimTrailingZeroPadded(raw_processor.imgdata.lens.makernotes.Lens);
  }

  if (IsNikonCamera(ctx.camera_make_, ctx.camera_model_)) {
    if (ctx.lens_make_.empty()) {
      ctx.lens_make_ = "Nikon";
    }
    if (ctx.lens_model_.empty()) {
      ctx.lens_model_ = ResolveNikonLensModel(raw_processor.imgdata.lens);
    }
  }

  ctx.focal_length_mm_ = raw_processor.imgdata.other.focal_len;
  if (!IsFinitePositive(ctx.focal_length_mm_)) {
    ctx.focal_length_mm_ = raw_processor.imgdata.lens.makernotes.CurFocal;
  }
  ctx.aperture_f_number_ = raw_processor.imgdata.other.aperture;
  if (!IsFinitePositive(ctx.aperture_f_number_)) {
    ctx.aperture_f_number_ = raw_processor.imgdata.lens.makernotes.CurAp;
  }
  ctx.focus_distance_m_ = 0.0f;
  if (std::isfinite(raw_processor.imgdata.lens.makernotes.FocusRangeIndex) &&
      raw_processor.imgdata.lens.makernotes.FocusRangeIndex > 0.0f) {
    ctx.focus_distance_m_ = raw_processor.imgdata.lens.makernotes.FocusRangeIndex;
  }

  ctx.focal_35mm_mm_ = 0.0f;
  if (raw_processor.imgdata.lens.FocalLengthIn35mmFormat > 0) {
    ctx.focal_35mm_mm_ = static_cast<float>(raw_processor.imgdata.lens.FocalLengthIn35mmFormat);
  } else if (raw_processor.imgdata.lens.makernotes.FocalLengthIn35mmFormat > 0) {
    ctx.focal_35mm_mm_ =
        static_cast<float>(raw_processor.imgdata.lens.makernotes.FocalLengthIn35mmFormat);
  }
  ctx.crop_factor_hint_    = ResolveCropFactorHint(ctx.focal_length_mm_, ctx.focal_35mm_mm_);

  ctx.lens_metadata_valid_ = !ctx.lens_model_.empty() && std::isfinite(ctx.focal_length_mm_) &&
                             ctx.focal_length_mm_ > 0.0f;

  // Resolve Adobe DNG colour matrices from the camera-model database once and
  // store them in the context so that downstream operators (ColorTempOp) never
  // need to repeat the lookup.
  ctx.color_matrices_valid_ = LookupCameraColorMatrices(ctx.camera_make_, ctx.camera_model_,
                                                        ctx.color_matrix_1_, ctx.color_matrix_2_);
  if (ctx.color_matrices_valid_) {
    MarkAdobeCameraMatrixDatabaseIlluminants(ctx);
  }

  // Mark as valid for downstream consumers (not an actual decode, just metadata)
  ctx.valid_                  = true;
  ctx.output_in_camera_space_ = true;  // no processing done, color data is camera-space
}

void PopulateDisplayMetadataFromLibRaw(LibRaw& raw_processor, const RawRuntimeColorContext& ctx,
                                       const LibRawCaptureTimeText& capture_time_text,
                                       ExifDisplayMetaData&         display) {
  display.make_             = ctx.camera_make_;
  display.model_            = ctx.camera_model_;
  display.lens_             = ctx.lens_model_;
  display.lens_make_        = ctx.lens_make_;
  display.focal_            = ctx.focal_length_mm_;
  display.focal_35mm_       = ctx.focal_35mm_mm_;
  display.aperture_         = ctx.aperture_f_number_;
  display.focus_distance_m_ = ctx.focus_distance_m_;

  display.iso_              = static_cast<uint64_t>(raw_processor.imgdata.other.iso_speed);

  const float shutter_sec   = raw_processor.imgdata.other.shutter;
  if (std::isfinite(shutter_sec) && shutter_sec > 0.0f) {
    if (shutter_sec >= 1.0f) {
      display.shutter_speed_ = {static_cast<int>(shutter_sec), 1};
    } else {
      display.shutter_speed_ = {1, static_cast<int>(1.0f / shutter_sec + 0.5f)};
    }
  }

  SetDisplayDimensionsFromLibRaw(raw_processor, display);

  if (auto capture_time = FormatLibRawCaptureTime(raw_processor, capture_time_text);
      !capture_time.empty()) {
    display.date_time_str_ = std::move(capture_time);
  }
}

void MergeMetadataHint(const ExifDisplayMetaData* metadata_hint, RawRuntimeColorContext& ctx) {
  if (!metadata_hint) {
    return;
  }

  const std::string hint_make      = TrimAscii(metadata_hint->make_);
  const std::string hint_model     = TrimAscii(metadata_hint->model_);
  const std::string hint_lens_make = TrimAscii(metadata_hint->lens_make_);
  const std::string hint_lens      = TrimAscii(metadata_hint->lens_);

  if (ctx.camera_make_.empty() && !hint_make.empty()) {
    ctx.camera_make_ = hint_make;
  }
  if (ctx.camera_model_.empty() && !hint_model.empty()) {
    ctx.camera_model_ = hint_model;
  }
  if (ctx.lens_make_.empty() && !hint_lens_make.empty()) {
    ctx.lens_make_ = hint_lens_make;
  }
  if (ctx.lens_model_.empty() && !hint_lens.empty()) {
    ctx.lens_model_ = hint_lens;
  }

  if (!IsFinitePositive(ctx.focal_length_mm_) && IsFinitePositive(metadata_hint->focal_)) {
    ctx.focal_length_mm_ = metadata_hint->focal_;
  }
  if (!IsFinitePositive(ctx.aperture_f_number_) && IsFinitePositive(metadata_hint->aperture_)) {
    ctx.aperture_f_number_ = metadata_hint->aperture_;
  }
  if (!IsFinitePositive(ctx.focus_distance_m_) &&
      IsFinitePositive(metadata_hint->focus_distance_m_)) {
    ctx.focus_distance_m_ = metadata_hint->focus_distance_m_;
  }
  if (!IsFinitePositive(ctx.focal_35mm_mm_) && IsFinitePositive(metadata_hint->focal_35mm_)) {
    ctx.focal_35mm_mm_ = metadata_hint->focal_35mm_;
  }

  if (!IsFinitePositive(ctx.crop_factor_hint_)) {
    ctx.crop_factor_hint_ = ResolveCropFactorHint(ctx.focal_length_mm_, ctx.focal_35mm_mm_);
  }
}

}  // namespace alcedo::metadata_detail
