//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/image_writer.hpp"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "image/metadata.hpp"
#include "io/image/export_icc_profile_resolver.hpp"
#include "io/image/ultra_hdr_writer.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
OIIO_NAMESPACE_USING

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  return conv::ToBytes(path.wstring());
}

auto RatingPercentFor(int rating) -> uint16_t {
  switch (ExifDisplayMetaData::NormalizeRating(rating)) {
    case 1:
      return 1;
    case 2:
      return 25;
    case 3:
      return 50;
    case 4:
      return 75;
    case 5:
      return 99;
    default:
      return 0;
  }
}

auto HasMeaningfulExportMetadata(const ExifDisplayMetaData& metadata) -> bool {
  return !metadata.make_.empty() || !metadata.model_.empty() || !metadata.lens_.empty() ||
         !metadata.lens_make_.empty() || !metadata.date_time_str_.empty() ||
         metadata.aperture_ > 0.0f || metadata.focal_ > 0.0f || metadata.focal_35mm_ > 0.0f ||
         metadata.focus_distance_m_ > 0.0f || metadata.iso_ > 0 ||
         (metadata.shutter_speed_.first > 0 && metadata.shutter_speed_.second > 0) ||
         ExifDisplayMetaData::NormalizeRating(metadata.rating_) > 0;
}

auto RationalFromFloat(float value, int denominator) -> std::pair<int, int> {
  if (!std::isfinite(value) || value <= 0.0f || denominator <= 0) {
    return {0, 1};
  }
  return {std::max(1, static_cast<int>(std::lround(value * denominator))), denominator};
}

auto ExifDateTimeString(std::string value) -> std::optional<std::string> {
  if (value.size() < 19) {
    return std::nullopt;
  }
  value = value.substr(0, 19);
  if (value[4] == '-') value[4] = ':';
  if (value[7] == '-') value[7] = ':';
  return value;
}

auto ShouldResize(const ExportRecipe& recipe) -> bool {
  return recipe.resize_.mode_ != ExportResizeMode::ORIGINAL_PIXELS;
}

auto ResizeRGBA32F(const cv::Mat& rgba32f, const ExportRecipe& recipe) -> cv::Mat {
  if (!ShouldResize(recipe)) return rgba32f;
  const auto resolved =
      ResolveExportResolution(recipe.resize_, {.width_ = rgba32f.cols, .height_ = rgba32f.rows});
  if (!resolved.success_) {
    throw std::runtime_error("ImageWriter: " + resolved.message_);
  }
  if (resolved.pixels_.width_ == rgba32f.cols && resolved.pixels_.height_ == rgba32f.rows) {
    return rgba32f;
  }

  cv::Mat resized;
  cv::resize(rgba32f, resized, cv::Size(resolved.pixels_.width_, resolved.pixels_.height_), 0.0,
             0.0, cv::INTER_AREA);
  return resized;
}

auto FormatSupportsAlpha(ImageFormatType fmt) -> bool {
  switch (fmt) {
    case ImageFormatType::PNG:
    case ImageFormatType::TIFF:
    case ImageFormatType::WEBP:
    case ImageFormatType::EXR:
      return true;
    default:
      return false;
  }
}

auto IsUltraHdrTransfer(ColorUtils::EOTF eotf) -> bool {
  return eotf == ColorUtils::EOTF::ST2084 || eotf == ColorUtils::EOTF::HLG;
}

auto MakeOIIOBuffer(const cv::Mat& rgba32f, const ExportRecipe& recipe, TypeDesc& out_spec_format,
                    TypeDesc& out_input_format, int& out_channels) -> cv::Mat {
  const auto&           options    = recipe.codec_;
  const ImageFormatType fmt        = options.format_;
  const bool            want_alpha = recipe.alpha_ == ExportAlphaPolicy::PRESERVE_IF_SUPPORTED &&
                          FormatSupportsAlpha(fmt) && rgba32f.channels() == 4;

  if (fmt == ImageFormatType::JPEG || fmt == ImageFormatType::BMP) {
    out_channels = 3;
  } else {
    out_channels = want_alpha ? 4 : 3;
  }

  cv::Mat rgb_or_rgba;
  if (out_channels == 3) {
    cv::cvtColor(rgba32f, rgb_or_rgba, cv::COLOR_RGBA2RGB);
  } else {
    rgb_or_rgba = rgba32f;
  }

  if (fmt == ImageFormatType::EXR) {
    if (options.bit_depth_ == ExportFormatOptions::BIT_DEPTH::BIT_32) {
      out_spec_format  = TypeDesc::FLOAT;
      out_input_format = TypeDesc::FLOAT;
      return (rgb_or_rgba.type() == CV_32FC(out_channels) && rgb_or_rgba.isContinuous())
                 ? rgb_or_rgba
                 : rgb_or_rgba.clone();
    }

    out_spec_format  = TypeDesc::HALF;
    out_input_format = TypeDesc::FLOAT;
    return (rgb_or_rgba.type() == CV_32FC(out_channels) && rgb_or_rgba.isContinuous())
               ? rgb_or_rgba
               : rgb_or_rgba.clone();
  }

  if (fmt == ImageFormatType::JPEG || fmt == ImageFormatType::WEBP || fmt == ImageFormatType::BMP) {
    out_spec_format  = TypeDesc::UINT8;
    out_input_format = TypeDesc::UINT8;
    cv::Mat u8;
    rgb_or_rgba.convertTo(u8, CV_MAKETYPE(CV_8U, out_channels), 255.0);
    return u8.isContinuous() ? u8 : u8.clone();
  }

  if (fmt == ImageFormatType::PNG || fmt == ImageFormatType::TIFF) {
    switch (options.bit_depth_) {
      case ExportFormatOptions::BIT_DEPTH::BIT_8: {
        out_spec_format  = TypeDesc::UINT8;
        out_input_format = TypeDesc::UINT8;
        cv::Mat u8;
        rgb_or_rgba.convertTo(u8, CV_MAKETYPE(CV_8U, out_channels), 255.0);
        return u8.isContinuous() ? u8 : u8.clone();
      }
      case ExportFormatOptions::BIT_DEPTH::BIT_16: {
        out_spec_format  = TypeDesc::UINT16;
        out_input_format = TypeDesc::UINT16;
        cv::Mat u16;
        rgb_or_rgba.convertTo(u16, CV_MAKETYPE(CV_16U, out_channels), 65535.0);
        return u16.isContinuous() ? u16 : u16.clone();
      }
      case ExportFormatOptions::BIT_DEPTH::BIT_32: {
        out_spec_format  = TypeDesc::FLOAT;
        out_input_format = TypeDesc::FLOAT;
        return (rgb_or_rgba.type() == CV_32FC(out_channels) && rgb_or_rgba.isContinuous())
                   ? rgb_or_rgba
                   : rgb_or_rgba.clone();
      }
      default:
        break;
    }
  }

  out_spec_format  = TypeDesc::UINT8;
  out_input_format = TypeDesc::UINT8;
  out_channels     = 3;
  cv::Mat rgb;
  cv::cvtColor(rgba32f, rgb, cv::COLOR_RGBA2RGB);
  cv::Mat u8;
  rgb.convertTo(u8, CV_8UC3, 255.0);
  return u8.isContinuous() ? u8 : u8.clone();
}

auto ApplyOIIOFormatOptions(ImageSpec& spec, const ExportFormatOptions& options) -> void {
  // Drop any compression/quality copied from the source so export options win.
  spec.extra_attribs.remove("compression", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("Compression", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("CompressionQuality", TypeDesc::UNKNOWN, false);

  switch (options.format_) {
    case ImageFormatType::JPEG: {
      const int         quality  = std::clamp(options.quality_, 1, 100);
      const std::string compress = "jpeg:" + std::to_string(quality);
      // OIIO JPEG writer reads quality from compression="jpeg:N" (CompressionQuality
      // is deprecated since 2.1 and is ignored by current jpegoutput).
      spec.attribute("compression", compress);
      spec.attribute("Compression", compress);
      spec.attribute("CompressionQuality", quality);
      break;
    }
    case ImageFormatType::WEBP: {
      const int         quality  = std::clamp(options.quality_, 0, 100);
      const std::string compress = "webp:" + std::to_string(quality);
      spec.attribute("compression", compress);
      spec.attribute("Compression", compress);
      spec.attribute("CompressionQuality", quality);
      break;
    }
    case ImageFormatType::PNG:
      spec.attribute("CompressionLevel", options.compression_level_);
      spec.attribute("png:compressionLevel", options.compression_level_);
      break;
    case ImageFormatType::TIFF: {
      const int tiff_compress = static_cast<int>(options.tiff_compress_);
      spec.attribute("tiff:compression", tiff_compress);
      std::string compress_str = "none";
      if (options.tiff_compress_ == ExportFormatOptions::TIFF_COMPRESS::LZW) {
        compress_str = "lzw";
      } else if (options.tiff_compress_ == ExportFormatOptions::TIFF_COMPRESS::ZIP) {
        compress_str = "zip";
      }
      spec.attribute("compression", compress_str);
      spec.attribute("Compression", compress_str);
      break;
    }
    default:
      break;
  }
}

auto ForceUprightOrientation(ImageSpec& spec) -> void {
  // The pipeline already applies any required orientation to pixel data.
  // If we preserve the source Orientation metadata, some viewers will rotate again.
  //
  // OpenImageIO uses the standard "Orientation" metadata key (1 = normal/upright).
  spec.extra_attribs.remove("Exif:Orientation", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("EXIF:Orientation", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("exif:Orientation", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("tiff:Orientation", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("TIFF:Orientation", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("Orientation", TypeDesc::UNKNOWN, false);
  spec.attribute("Orientation", 1);
}

void RemoveEmbeddedColorProfileMetadata(ImageSpec& spec) {
  spec.extra_attribs.remove("ICCProfile", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("icc_profile", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("Exif:ColorSpace", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("EXIF:ColorSpace", TypeDesc::UNKNOWN, false);
  spec.extra_attribs.remove("exif:ColorSpace", TypeDesc::UNKNOWN, false);
}

void ApplyExportColorProfile(ImageSpec&                                     spec,
                             const std::optional<ExportColorProfileConfig>& color_profile) {
  if (!color_profile.has_value()) {
    return;
  }

  const std::vector<uint8_t> icc_bytes =
      ExportIccProfileResolver::ResolveIccProfileBytes(*color_profile);
  if (icc_bytes.empty()) {
    return;
  }

  RemoveEmbeddedColorProfileMetadata(spec);
  spec.attribute("oiio:ColorSpace", ColorUtils::ColorSpaceToString(color_profile->encoding_space) +
                                        ":" +
                                        ColorUtils::EOTFToString(color_profile->encoding_eotf));
  spec.attribute("ICCProfile",
                 TypeDesc(TypeDesc::UINT8, TypeDesc::SCALAR, TypeDesc::NOSEMANTICS,
                          static_cast<int>(icc_bytes.size())),
                 icc_bytes.data());
}

void ApplyExportMetadataToOIIO(ImageSpec&                                spec,
                               const std::optional<ExifDisplayMetaData>& export_metadata,
                               const ExportMetadataPolicy&               policy) {
  if (policy.mode_ == ExportMetadataMode::NONE || !policy.include_exif_) return;
  if (!export_metadata.has_value() || !HasMeaningfulExportMetadata(*export_metadata)) {
    return;
  }

  const ExifDisplayMetaData& metadata = *export_metadata;
  // Stamp both the Exif:* and bare names — JPEG writers across OIIO versions
  // accept one or the other; the production path must not depend on Exiv2
  // post-write (SEH on Windows MSVC).
  if (!metadata.make_.empty()) {
    spec.attribute("Exif:Make", metadata.make_);
    spec.attribute("Make", metadata.make_);
  }
  if (!metadata.model_.empty()) {
    spec.attribute("Exif:Model", metadata.model_);
    spec.attribute("Model", metadata.model_);
  }
  if (!metadata.lens_.empty()) {
    spec.attribute("Exif:LensModel", metadata.lens_);
    spec.attribute("LensModel", metadata.lens_);
  }
  if (!metadata.lens_make_.empty()) {
    spec.attribute("Exif:LensMake", metadata.lens_make_);
    spec.attribute("LensMake", metadata.lens_make_);
  }
  if (metadata.aperture_ > 0.0f) spec.attribute("FNumber", metadata.aperture_);
  if (metadata.focal_ > 0.0f) spec.attribute("Exif:FocalLength", metadata.focal_);
  if (metadata.focal_35mm_ > 0.0f) {
    spec.attribute("Exif:FocalLengthIn35mmFilm",
                   static_cast<int>(std::lround(metadata.focal_35mm_)));
  }
  if (metadata.focus_distance_m_ > 0.0f) {
    spec.attribute("Exif:SubjectDistance", metadata.focus_distance_m_);
  }
  if (metadata.iso_ > 0) {
    const int iso =
        static_cast<int>(std::min<uint64_t>(metadata.iso_, std::numeric_limits<int>::max()));
    spec.attribute("Exif:SensitivityType", 1);
    spec.attribute("Exif:StandardOutputSensitivity", iso);
    spec.attribute("Exif:ISOSpeed", iso);
  }
  if (metadata.shutter_speed_.first > 0 && metadata.shutter_speed_.second > 0) {
    spec.attribute("ExposureTime", static_cast<float>(metadata.shutter_speed_.first) /
                                       static_cast<float>(metadata.shutter_speed_.second));
  }
  if (const auto exif_dt = ExifDateTimeString(metadata.date_time_str_); exif_dt.has_value()) {
    if (!spec.find_attribute("Exif:DateTime")) spec.attribute("Exif:DateTime", *exif_dt);
    if (!spec.find_attribute("Exif:DateTimeOriginal"))
      spec.attribute("Exif:DateTimeOriginal", *exif_dt);
    if (!spec.find_attribute("Exif:DateTimeDigitized"))
      spec.attribute("Exif:DateTimeDigitized", *exif_dt);
    if (!spec.find_attribute("DateTime")) spec.attribute("DateTime", *exif_dt);
    if (!spec.find_attribute("DateTimeOriginal")) spec.attribute("DateTimeOriginal", *exif_dt);
  }

  const int normalized_rating = ExifDisplayMetaData::NormalizeRating(metadata.rating_);
  if (normalized_rating > 0) {
    spec.attribute("Exif:Rating", normalized_rating);
    spec.attribute("Rating", normalized_rating);
    spec.attribute("Exif:RatingPercent", static_cast<int>(RatingPercentFor(normalized_rating)));
    spec.attribute("XMP:xmp:Rating", normalized_rating);
  }
}

auto IsPermittedSourceMetadata(std::string name, const ExportMetadataPolicy& policy) -> bool {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  const bool exif_group = name.rfind("exif:", 0) == 0 || name == "make" || name == "model" ||
                          name.rfind("datetime", 0) == 0;
  const bool xmp_group  = name.rfind("xmp:", 0) == 0;
  const bool iptc_group = name.rfind("iptc:", 0) == 0 || name == "artist" || name == "copyright";
  const bool supported_group = (exif_group && policy.include_exif_) ||
                               (xmp_group && policy.include_xmp_) ||
                               (iptc_group && policy.include_iptc_);
  if (!supported_group) return false;
  if ((name.find("gps") != std::string::npos || name.find("location") != std::string::npos) &&
      !policy.include_location_) {
    return false;
  }
  if ((name.find("serialnumber") != std::string::npos ||
       name.find("cameraserial") != std::string::npos) &&
      !policy.include_device_serials_) {
    return false;
  }
  if ((name.find("darktable") != std::string::npos || name.find("xmp:crs:") != std::string::npos ||
       name.find("history") != std::string::npos) &&
      !policy.include_editing_history_) {
    return false;
  }
  return name.find("orientation") == std::string::npos &&
         name.find("pixelxdimension") == std::string::npos &&
         name.find("pixelydimension") == std::string::npos &&
         name.find("imagewidth") == std::string::npos &&
         name.find("imageheight") == std::string::npos &&
         name.find("xresolution") == std::string::npos &&
         name.find("yresolution") == std::string::npos &&
         name.find("resolutionunit") == std::string::npos &&
         name.find("colorspace") == std::string::npos;
}

void CopyPermittedSourceMetadata(const image_path_t& source_path, ImageSpec& output_spec,
                                 const ExportMetadataPolicy& policy) {
  if (policy.mode_ == ExportMetadataMode::NONE) return;
  try {
    if (auto input = ImageInput::open(PathToUtf8(source_path))) {
      for (const auto& attribute : input->spec().extra_attribs) {
        const auto name = attribute.name().string();
        if (IsPermittedSourceMetadata(name, policy)) {
          output_spec.attribute(name, attribute.type(), attribute.data());
        }
      }
      input->close();
    }
  } catch (const std::exception&) {
    // Display metadata remains available as the normalized fallback.
  }
}

void ApplyExportResolutionToOIIO(ImageSpec& spec, const ExportResizeSpec& resize) {
  if (!std::isfinite(resize.dpi_) || resize.dpi_ <= 0.0) return;
  spec.attribute("XResolution", static_cast<float>(resize.dpi_));
  spec.attribute("YResolution", static_cast<float>(resize.dpi_));
  spec.attribute("ResolutionUnit", "in");
}

auto ReadFileBytes(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

auto WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) -> bool {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

auto IsJpegBytes(const std::vector<uint8_t>& bytes) -> bool {
  return bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8;
}

auto IsExifApp1Segment(const std::vector<uint8_t>& bytes, size_t marker_pos, size_t segment_length)
    -> bool {
  constexpr uint8_t kExifPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
  const size_t      payload_pos   = marker_pos + 4;
  return segment_length >= 2 + sizeof(kExifPrefix) &&
         payload_pos + sizeof(kExifPrefix) <= bytes.size() &&
         std::equal(std::begin(kExifPrefix), std::end(kExifPrefix), bytes.begin() + payload_pos);
}

auto AppendU16LE(std::vector<uint8_t>& out, uint16_t value) -> void {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

auto AppendU32LE(std::vector<uint8_t>& out, uint32_t value) -> void {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

struct ExifIfdEntry {
  uint16_t             tag   = 0;
  uint16_t             type  = 0;  // 2=ASCII, 3=SHORT, 4=LONG, 5=RATIONAL
  uint32_t             count = 1;
  std::vector<uint8_t> inline_or_offset;  // 4-byte value/offset field contents when no overflow
  std::vector<uint8_t> overflow;          // payload when larger than 4 bytes
};

auto MakeAsciiEntry(uint16_t tag, const std::string& text) -> ExifIfdEntry {
  ExifIfdEntry entry;
  entry.tag  = tag;
  entry.type = 2;
  std::vector<uint8_t> bytes(text.begin(), text.end());
  bytes.push_back(0);
  entry.count = static_cast<uint32_t>(bytes.size());
  if (bytes.size() <= 4) {
    entry.inline_or_offset = bytes;
    entry.inline_or_offset.resize(4, 0);
  } else {
    entry.overflow = std::move(bytes);
  }
  return entry;
}

auto MakeShortEntry(uint16_t tag, uint16_t value) -> ExifIfdEntry {
  ExifIfdEntry entry;
  entry.tag   = tag;
  entry.type  = 3;
  entry.count = 1;
  AppendU16LE(entry.inline_or_offset, value);
  entry.inline_or_offset.resize(4, 0);
  return entry;
}

auto MakeLongEntry(uint16_t tag, uint32_t value) -> ExifIfdEntry {
  ExifIfdEntry entry;
  entry.tag   = tag;
  entry.type  = 4;
  entry.count = 1;
  AppendU32LE(entry.inline_or_offset, value);
  return entry;
}

auto MakeRationalEntry(uint16_t tag, uint32_t numerator, uint32_t denominator) -> ExifIfdEntry {
  ExifIfdEntry entry;
  entry.tag   = tag;
  entry.type  = 5;
  entry.count = 1;
  AppendU32LE(entry.overflow, numerator);
  AppendU32LE(entry.overflow, denominator);
  return entry;
}

auto EncodeIfd(std::vector<uint8_t>& tiff, const std::vector<ExifIfdEntry>& entries,
               uint32_t next_ifd_offset) -> void {
  AppendU16LE(tiff, static_cast<uint16_t>(entries.size()));
  const size_t entries_pos = tiff.size();
  tiff.resize(tiff.size() + entries.size() * 12u + 4u, 0);

  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    uint8_t*    slot  = tiff.data() + entries_pos + i * 12u;
    slot[0]           = static_cast<uint8_t>(entry.tag & 0xFF);
    slot[1]           = static_cast<uint8_t>((entry.tag >> 8) & 0xFF);
    slot[2]           = static_cast<uint8_t>(entry.type & 0xFF);
    slot[3]           = static_cast<uint8_t>((entry.type >> 8) & 0xFF);
    slot[4]           = static_cast<uint8_t>(entry.count & 0xFF);
    slot[5]           = static_cast<uint8_t>((entry.count >> 8) & 0xFF);
    slot[6]           = static_cast<uint8_t>((entry.count >> 16) & 0xFF);
    slot[7]           = static_cast<uint8_t>((entry.count >> 24) & 0xFF);

    if (entry.overflow.empty()) {
      for (size_t b = 0; b < entry.inline_or_offset.size() && b < 4; ++b) {
        slot[8 + b] = entry.inline_or_offset[b];
      }
    } else {
      const uint32_t offset = static_cast<uint32_t>(tiff.size());
      slot[8]               = static_cast<uint8_t>(offset & 0xFF);
      slot[9]               = static_cast<uint8_t>((offset >> 8) & 0xFF);
      slot[10]              = static_cast<uint8_t>((offset >> 16) & 0xFF);
      slot[11]              = static_cast<uint8_t>((offset >> 24) & 0xFF);
      tiff.insert(tiff.end(), entry.overflow.begin(), entry.overflow.end());
    }
  }

  uint8_t* next = tiff.data() + entries_pos + entries.size() * 12u;
  next[0]       = static_cast<uint8_t>(next_ifd_offset & 0xFF);
  next[1]       = static_cast<uint8_t>((next_ifd_offset >> 8) & 0xFF);
  next[2]       = static_cast<uint8_t>((next_ifd_offset >> 16) & 0xFF);
  next[3]       = static_cast<uint8_t>((next_ifd_offset >> 24) & 0xFF);
}

/// Build a JPEG APP1 Exif payload without Exiv2. Exiv2 encode/writeMetadata SEH
/// on this Windows MSVC toolchain when given real camera metadata.
auto BuildJpegExifPayloadNoExiv(const std::optional<ExifDisplayMetaData>& export_metadata)
    -> std::vector<uint8_t> {
  if (!export_metadata.has_value() || !HasMeaningfulExportMetadata(*export_metadata)) {
    return {};
  }
  const auto&               metadata = *export_metadata;

  std::vector<ExifIfdEntry> exif_ifd;
  if (const auto exif_dt = ExifDateTimeString(metadata.date_time_str_); exif_dt.has_value()) {
    exif_ifd.push_back(MakeAsciiEntry(0x9003, *exif_dt));  // DateTimeOriginal
    exif_ifd.push_back(MakeAsciiEntry(0x9004, *exif_dt));  // DateTimeDigitized
  }
  if (!metadata.lens_make_.empty()) {
    exif_ifd.push_back(MakeAsciiEntry(0xA433, metadata.lens_make_));  // LensMake
  }
  if (!metadata.lens_.empty()) {
    exif_ifd.push_back(MakeAsciiEntry(0xA434, metadata.lens_));  // LensModel
  }
  if (metadata.aperture_ > 0.0f) {
    const auto rational = RationalFromFloat(metadata.aperture_, 100);
    exif_ifd.push_back(MakeRationalEntry(0x829D, static_cast<uint32_t>(rational.first),
                                         static_cast<uint32_t>(rational.second)));
  }
  if (metadata.focal_ > 0.0f) {
    const auto rational = RationalFromFloat(metadata.focal_, 100);
    exif_ifd.push_back(MakeRationalEntry(0x920A, static_cast<uint32_t>(rational.first),
                                         static_cast<uint32_t>(rational.second)));
  }
  if (metadata.iso_ > 0) {
    exif_ifd.push_back(MakeShortEntry(0x8827, static_cast<uint16_t>(std::min<uint64_t>(
                                                  metadata.iso_, 65535))));  // ISOSpeedRatings
  }

  std::vector<ExifIfdEntry> ifd0;
  ifd0.push_back(MakeShortEntry(0x0112, 1));  // Orientation = upright
  if (!metadata.make_.empty()) {
    ifd0.push_back(MakeAsciiEntry(0x010F, metadata.make_));
  }
  if (!metadata.model_.empty()) {
    ifd0.push_back(MakeAsciiEntry(0x0110, metadata.model_));
  }
  if (const auto exif_dt = ExifDateTimeString(metadata.date_time_str_); exif_dt.has_value()) {
    ifd0.push_back(MakeAsciiEntry(0x0132, *exif_dt));  // DateTime
  }
  const int normalized_rating = ExifDisplayMetaData::NormalizeRating(metadata.rating_);
  if (normalized_rating > 0) {
    ifd0.push_back(MakeShortEntry(0x4746, static_cast<uint16_t>(normalized_rating)));  // Rating
    ifd0.push_back(MakeShortEntry(0x4749, RatingPercentFor(normalized_rating)));  // RatingPercent
  }

  constexpr uint16_t kExifIfdTag   = 0x8769;
  const bool         need_exif_ifd = !exif_ifd.empty();
  if (need_exif_ifd) {
    ifd0.push_back(MakeLongEntry(kExifIfdTag, 0));  // patched below
  }

  std::sort(ifd0.begin(), ifd0.end(),
            [](const ExifIfdEntry& a, const ExifIfdEntry& b) { return a.tag < b.tag; });
  std::sort(exif_ifd.begin(), exif_ifd.end(),
            [](const ExifIfdEntry& a, const ExifIfdEntry& b) { return a.tag < b.tag; });

  std::vector<uint8_t> tiff;
  tiff.push_back('I');
  tiff.push_back('I');
  AppendU16LE(tiff, 42);
  AppendU32LE(tiff, 8);  // offset to IFD0

  const size_t ifd0_start = tiff.size();
  EncodeIfd(tiff, ifd0, 0);
  if (need_exif_ifd) {
    const uint32_t exif_ifd_offset = static_cast<uint32_t>(tiff.size());
    EncodeIfd(tiff, exif_ifd, 0);
    const uint16_t count = static_cast<uint16_t>(
        tiff[ifd0_start] | (static_cast<uint16_t>(tiff[ifd0_start + 1]) << 8));
    for (uint16_t i = 0; i < count; ++i) {
      uint8_t*       slot = tiff.data() + ifd0_start + 2 + i * 12;
      const uint16_t tag  = static_cast<uint16_t>(slot[0] | (static_cast<uint16_t>(slot[1]) << 8));
      if (tag == kExifIfdTag) {
        slot[8]  = static_cast<uint8_t>(exif_ifd_offset & 0xFF);
        slot[9]  = static_cast<uint8_t>((exif_ifd_offset >> 8) & 0xFF);
        slot[10] = static_cast<uint8_t>((exif_ifd_offset >> 16) & 0xFF);
        slot[11] = static_cast<uint8_t>((exif_ifd_offset >> 24) & 0xFF);
        break;
      }
    }
  }

  constexpr uint8_t    kExifPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
  std::vector<uint8_t> payload(std::begin(kExifPrefix), std::end(kExifPrefix));
  payload.insert(payload.end(), tiff.begin(), tiff.end());
  if (payload.size() > 65533) {
    return {};
  }
  return payload;
}

auto BuildJpegExifPayload(const std::optional<ExifDisplayMetaData>& export_metadata)
    -> std::vector<uint8_t> {
  // Prefer the Exiv2-free builder: Exiv2::ExifParser::encode SEHs on MSVC with
  // real camera metadata and throws Invalid key '' for sparse ExifData.
  return BuildJpegExifPayloadNoExiv(export_metadata);
}

auto ReplaceJpegExifSegment(const std::filesystem::path& export_path,
                            const std::vector<uint8_t>&  exif_payload) -> bool {
  if (exif_payload.empty() || exif_payload.size() > 65533) {
    return false;
  }

  std::vector<uint8_t> bytes = ReadFileBytes(export_path);
  if (!IsJpegBytes(bytes)) {
    return false;
  }

  std::vector<uint8_t> segment;
  const uint16_t       segment_length = static_cast<uint16_t>(exif_payload.size() + 2);
  segment.reserve(exif_payload.size() + 4);
  segment.push_back(0xFF);
  segment.push_back(0xE1);
  segment.push_back(static_cast<uint8_t>((segment_length >> 8) & 0xFF));
  segment.push_back(static_cast<uint8_t>(segment_length & 0xFF));
  segment.insert(segment.end(), exif_payload.begin(), exif_payload.end());

  size_t insert_pos = 2;
  for (size_t pos = 2; pos + 4 <= bytes.size();) {
    if (bytes[pos] != 0xFF) {
      break;
    }
    while (pos < bytes.size() && bytes[pos] == 0xFF) {
      ++pos;
    }
    if (pos >= bytes.size()) {
      break;
    }
    const uint8_t marker = bytes[pos++];
    if (marker == 0xDA || marker == 0xD9) {
      insert_pos = pos - 2;
      break;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      insert_pos = pos;
      continue;
    }
    if (pos + 2 > bytes.size()) {
      break;
    }
    const size_t length = (static_cast<size_t>(bytes[pos]) << 8) | bytes[pos + 1];
    if (length < 2 || pos + length > bytes.size()) {
      break;
    }
    const size_t marker_pos = pos - 2;
    const size_t next_pos   = pos + length;
    if (marker == 0xE1 && IsExifApp1Segment(bytes, marker_pos, length)) {
      std::vector<uint8_t> out;
      out.reserve(bytes.size() - (next_pos - marker_pos) + segment.size());
      out.insert(out.end(), bytes.begin(), bytes.begin() + marker_pos);
      out.insert(out.end(), segment.begin(), segment.end());
      out.insert(out.end(), bytes.begin() + next_pos, bytes.end());
      return WriteFileBytes(export_path, out);
    }
    if (marker == 0xE0 && insert_pos == marker_pos) {
      insert_pos = next_pos;
    }
    pos = next_pos;
  }

  std::vector<uint8_t> out;
  out.reserve(bytes.size() + segment.size());
  out.insert(out.end(), bytes.begin(), bytes.begin() + insert_pos);
  out.insert(out.end(), segment.begin(), segment.end());
  out.insert(out.end(), bytes.begin() + insert_pos, bytes.end());
  return WriteFileBytes(export_path, out);
}

auto IsJpegExportPath(const std::filesystem::path& path) -> bool {
  const auto ext = path.extension().string();
  return ext == ".jpg" || ext == ".JPG" || ext == ".jpeg" || ext == ".JPEG" || ext == ".jpe" ||
         ext == ".JPE";
}

void ApplyJpegExportMetadata(const std::filesystem::path&              export_path,
                             const std::optional<ExifDisplayMetaData>& export_metadata) {
  const bool has_metadata =
      export_metadata.has_value() && HasMeaningfulExportMetadata(*export_metadata);
  if (!has_metadata) {
    return;
  }

  const std::vector<uint8_t> exif_payload = BuildJpegExifPayload(export_metadata);
  if (!exif_payload.empty()) {
    (void)ReplaceJpegExifSegment(export_path, exif_payload);
  }
}

auto TryWriteWithOpenImageIO(const image_path_t& src_path, const std::filesystem::path& export_path,
                             const cv::Mat& rgba32f, const ExportRecipe& recipe,
                             const std::optional<ExportColorProfileConfig>& color_profile,
                             const std::optional<ExifDisplayMetaData>&      export_metadata,
                             std::string&                                   out_error) -> bool {
  const std::string dst          = PathToUtf8(export_path);

  TypeDesc          spec_format  = TypeDesc::UINT8;
  TypeDesc          input_format = TypeDesc::UINT8;
  int               channels     = 0;
  const auto&       options      = recipe.codec_;
  cv::Mat           pixels = MakeOIIOBuffer(rgba32f, recipe, spec_format, input_format, channels);

  ImageSpec         outspec(pixels.cols, pixels.rows, channels, spec_format);
  if (channels == 3) outspec.channelnames = {"R", "G", "B"};
  if (channels == 4) outspec.channelnames = {"R", "G", "B", "A"};

  CopyPermittedSourceMetadata(src_path, outspec, recipe.metadata_);

  ForceUprightOrientation(outspec);
  ApplyOIIOFormatOptions(outspec, options);
  ApplyExportMetadataToOIIO(outspec, export_metadata, recipe.metadata_);
  ApplyExportResolutionToOIIO(outspec, recipe.resize_);
  if (recipe.metadata_.mode_ == ExportMetadataMode::STANDARD && recipe.metadata_.include_exif_) {
    outspec.attribute("Exif:PixelXDimension", pixels.cols);
    outspec.attribute("Exif:PixelYDimension", pixels.rows);
    outspec.attribute("Software", "Alcedo Studio");
  }
  ApplyExportColorProfile(outspec, color_profile);

  // OIIO v3 exports ImageOutput::create(string_view, ...) (and a UTF-16 helper).
  // Avoid the deprecated create(std::string, std::string) overload, which can
  // lead to unresolved externals on MSVC when linking against the DLL.
  std::unique_ptr<ImageOutput> out = ImageOutput::create(dst);
  if (!out) {
    out_error = "OpenImageIO: failed to create ImageOutput";
    return false;
  }

  if (!out->open(dst, outspec)) {
    out_error = "OpenImageIO: failed to open output: " + out->geterror();
    return false;
  }

  const stride_t xstride = static_cast<stride_t>(pixels.elemSize());
  const stride_t ystride = static_cast<stride_t>(pixels.step);
  if (!out->write_image(input_format, pixels.data, xstride, ystride, AutoStride)) {
    out_error = "OpenImageIO: failed to write image: " + out->geterror();
    out->close();
    return false;
  }

  out->close();
  return true;
}

auto TryWriteWithOpenCV(const std::filesystem::path& export_path, const cv::Mat& rgba32f,
                        const ExportRecipe& recipe, std::string& out_error) -> bool {
  const std::string dst        = PathToUtf8(export_path);
  const auto&       options    = recipe.codec_;

  const bool        want_alpha = recipe.alpha_ == ExportAlphaPolicy::PRESERVE_IF_SUPPORTED &&
                          FormatSupportsAlpha(options.format_);
  const int channels = want_alpha ? 4 : 3;

  cv::Mat   bgr_or_bgra;
  if (channels == 3) {
    cv::cvtColor(rgba32f, bgr_or_bgra, cv::COLOR_RGBA2BGR);
  } else {
    cv::cvtColor(rgba32f, bgr_or_bgra, cv::COLOR_RGBA2BGRA);
  }

  cv::Mat encoded;
  if (options.format_ == ImageFormatType::EXR ||
      (options.format_ == ImageFormatType::TIFF &&
       options.bit_depth_ == ExportFormatOptions::BIT_DEPTH::BIT_32)) {
    encoded = bgr_or_bgra;
  } else if (options.format_ == ImageFormatType::PNG || options.format_ == ImageFormatType::TIFF) {
    if (options.bit_depth_ == ExportFormatOptions::BIT_DEPTH::BIT_16) {
      bgr_or_bgra.convertTo(encoded, CV_MAKETYPE(CV_16U, channels), 65535.0);
    } else {
      bgr_or_bgra.convertTo(encoded, CV_MAKETYPE(CV_8U, channels), 255.0);
    }
  } else {
    bgr_or_bgra.convertTo(encoded, CV_MAKETYPE(CV_8U, channels), 255.0);
  }

  std::vector<int> params;
  switch (options.format_) {
    case ImageFormatType::JPEG:
      params = {cv::IMWRITE_JPEG_QUALITY, options.quality_};
      break;
    case ImageFormatType::WEBP:
      params = {cv::IMWRITE_WEBP_QUALITY, options.quality_};
      break;
    case ImageFormatType::PNG:
      params = {cv::IMWRITE_PNG_COMPRESSION, options.compression_level_};
      break;
    case ImageFormatType::TIFF:
      params = {cv::IMWRITE_TIFF_COMPRESSION, static_cast<int>(options.tiff_compress_)};
      break;
    case ImageFormatType::EXR:
      params = {cv::IMWRITE_EXR_TYPE, (options.bit_depth_ == ExportFormatOptions::BIT_DEPTH::BIT_32)
                                          ? cv::IMWRITE_EXR_TYPE_FLOAT
                                          : cv::IMWRITE_EXR_TYPE_HALF};
      break;
    default:
      break;
  }

  try {
    if (!cv::imwrite(dst, encoded, params)) {
      out_error = "OpenCV: imwrite returned false";
      return false;
    }
    return true;
  } catch (const cv::Exception& e) {
    out_error = std::string("OpenCV: ") + e.what();
    return false;
  }
}

}  // namespace

auto ImageWriter::ShouldWriteUltraHdr(const ExportFormatOptions&                     options,
                                      const std::optional<ExportColorProfileConfig>& color_profile)
    -> bool {
  return options.format_ == ImageFormatType::JPEG && color_profile.has_value() &&
         options.hdr_export_mode_ == ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR &&
         IsUltraHdrTransfer(color_profile->encoding_eotf);
}

void ImageWriter::WriteImageToPath(const image_path_t&                     src_path,
                                   std::shared_ptr<ImageBuffer>            image_data,
                                   ExportFormatOptions                     options,
                                   std::optional<ExportColorProfileConfig> color_profile,
                                   std::optional<ExifDisplayMetaData>      export_metadata) {
  WriteImageToPath(src_path, std::move(image_data), ExportRecipe::FromLegacyOptions(options),
                   std::move(color_profile), std::move(export_metadata));
}

void ImageWriter::WriteImageToPath(const image_path_t&                     src_path,
                                   std::shared_ptr<ImageBuffer>            image_data,
                                   const ExportRecipe&                     recipe,
                                   std::optional<ExportColorProfileConfig> color_profile,
                                   std::optional<ExifDisplayMetaData>      export_metadata) {
  const auto& options = recipe.codec_;
  if (!image_data) {
    throw std::runtime_error("ImageWriter: image_data is null");
  }
  if (options.export_path_.empty()) {
    throw std::runtime_error("ImageWriter: export_path is empty");
  }

  const auto export_path = options.export_path_;
  if (export_path.has_parent_path()) {
    std::filesystem::create_directories(export_path.parent_path());
  }

  if (!image_data->cpu_data_valid_) {
    if (image_data->gpu_data_valid_) {
      image_data->SyncToCPU();
    } else {
      throw std::runtime_error("ImageWriter: image_data has no valid CPU/GPU data");
    }
  }

  // Use GetCPUData() to acquire the actual image data. Expected: CV_32FC4 in [0,1].
  const cv::Mat& src_rgba32f = image_data->GetCPUData();
  if (src_rgba32f.empty()) {
    throw std::runtime_error("ImageWriter: CPU image data is empty");
  }
  if (src_rgba32f.type() != CV_32FC4) {
    throw std::runtime_error("ImageWriter: expected image data type CV_32FC4");
  }

  // Resize without an unconditional full-frame clone: FULL_RES_EXPORT buffers
  // can be multi-hundred MB; cloning before downscale doubles peak memory.
  cv::Mat working = ShouldResize(recipe) ? ResizeRGBA32F(src_rgba32f, recipe) : src_rgba32f.clone();

  if (recipe.metadata_.mode_ == ExportMetadataMode::NONE || !recipe.metadata_.include_exif_) {
    export_metadata.reset();
  }
  const auto embedded_profile = recipe.icc_ == ExportIccPolicy::EMBED_OUTPUT_PROFILE
                                    ? color_profile
                                    : std::optional<ExportColorProfileConfig>{};

  if (ShouldWriteUltraHdr(options, color_profile)) {
#if defined(ALCEDO_HAS_ULTRAHDR)
    UltraHdrWriter::WriteImageToPath(
        src_path, export_path, working, options, *color_profile, export_metadata,
        recipe.metadata_.mode_ == ExportMetadataMode::STANDARD && recipe.metadata_.include_exif_,
        recipe.icc_ == ExportIccPolicy::EMBED_OUTPUT_PROFILE);
    return;
#else
    throw std::runtime_error(
        "ImageWriter: JPEG HDR export requires Ultra HDR support in this build.");
#endif
  }

  if (color_profile.has_value() && IsUltraHdrTransfer(color_profile->encoding_eotf)) {
    throw std::runtime_error("ImageWriter: HDR export requires Ultra HDR JPEG output.");
  }

  std::string oiio_err;
  try {
    if (TryWriteWithOpenImageIO(src_path, export_path, working, recipe, embedded_profile,
                                export_metadata, oiio_err)) {
      if (IsJpegExportPath(export_path)) {
        ApplyJpegExportMetadata(export_path, export_metadata);
      }
      return;
    }
  } catch (const std::exception& e) {
    oiio_err = e.what();
  }

  std::string cv_err;
  if (TryWriteWithOpenCV(export_path, working, recipe, cv_err)) {
    if (IsJpegExportPath(export_path)) {
      ApplyJpegExportMetadata(export_path, export_metadata);
      return;
    }
    if ((export_metadata.has_value() && recipe.metadata_.mode_ == ExportMetadataMode::STANDARD) ||
        embedded_profile.has_value() || recipe.resize_.dpi_ > 0.0) {
      throw std::runtime_error(
          "ImageWriter: OpenCV fallback cannot satisfy metadata, ICC, or DPI settings. OIIO: " +
          oiio_err);
    }
    return;
  }

  throw std::runtime_error("ImageWriter: export failed. OIIO: " + oiio_err +
                           " | OpenCV: " + cv_err);
}
};  // namespace alcedo
