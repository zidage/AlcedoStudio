//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/jpeg_exif_app1.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "alcedo_version.hpp"

namespace alcedo {
namespace {

constexpr uint16_t kExifIfdTag          = 0x8769;
constexpr uint16_t kGpsIfdTag           = 0x8825;
constexpr size_t   kMaxApp1PayloadBytes = 65533;

auto               RatingPercentFor(int rating) -> uint16_t {
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

auto RationalFromFloat(float value, int denominator) -> std::pair<uint32_t, uint32_t> {
  if (!std::isfinite(value) || value <= 0.0f || denominator <= 0) {
    return {0, 1};
  }
  return {static_cast<uint32_t>(std::max(1L, std::lround(value * denominator))),
          static_cast<uint32_t>(denominator)};
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

void WriteU32LE(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

auto MakeAsciiTag(uint16_t tag, const std::string& text) -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 2, .count_ = 0, .value_ = {text.begin(), text.end()}};
  result.value_.push_back(0);
  result.count_ = static_cast<uint32_t>(result.value_.size());
  return result;
}

auto MakeUndefinedTag(uint16_t tag, std::vector<uint8_t> bytes) -> ExifIfdTag {
  const auto count = static_cast<uint32_t>(bytes.size());
  return ExifIfdTag{.tag_ = tag, .type_ = 7, .count_ = count, .value_ = std::move(bytes)};
}

auto MakeShortTag(uint16_t tag, uint16_t value) -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 3, .count_ = 1, .value_ = {}};
  AppendU16LE(result.value_, value);
  return result;
}

auto MakeLongTag(uint16_t tag, uint32_t value) -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 4, .count_ = 1, .value_ = {}};
  AppendU32LE(result.value_, value);
  return result;
}

auto MakeRationalTag(uint16_t tag, std::pair<uint32_t, uint32_t> value) -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 5, .count_ = 1, .value_ = {}};
  AppendU32LE(result.value_, value.first);
  AppendU32LE(result.value_, value.second);
  return result;
}

auto FindTag(std::vector<ExifIfdTag>& tags, uint16_t tag) -> std::vector<ExifIfdTag>::iterator {
  return std::find_if(tags.begin(), tags.end(),
                      [tag](const ExifIfdTag& entry) { return entry.tag_ == tag; });
}

/// True when @p tag is an ASCII value of only NUL and space characters.
auto IsBlankAsciiTag(const ExifIfdTag& tag) -> bool {
  return tag.type_ == 2 && std::all_of(tag.value_.begin(), tag.value_.end(),
                                       [](uint8_t byte) { return byte == 0 || byte == ' '; });
}

/// Write @p tag into @p tags; it replaces an entry with the same tag number.
void SetTag(std::vector<ExifIfdTag>& tags, ExifIfdTag tag) {
  if (auto it = FindTag(tags, tag.tag_); it != tags.end()) {
    *it = std::move(tag);
  } else {
    tags.push_back(std::move(tag));
  }
}

/// Write @p tag into @p tags when they have no value for its tag number (or only a blank text).
void SetTagWhenMissing(std::vector<ExifIfdTag>& tags, ExifIfdTag tag) {
  if (auto it = FindTag(tags, tag.tag_); it != tags.end()) {
    if (IsBlankAsciiTag(*it)) *it = std::move(tag);
    return;
  }
  tags.push_back(std::move(tag));
}

void SortByTag(std::vector<ExifIfdTag>& tags) {
  std::stable_sort(tags.begin(), tags.end(),
                   [](const ExifIfdTag& a, const ExifIfdTag& b) { return a.tag_ < b.tag_; });
}

auto WritableTags(const std::vector<ExifIfdTag>& tags) -> std::vector<ExifIfdTag> {
  std::vector<ExifIfdTag> writable;
  for (const auto& tag : tags) {
    if (IsWritableExifIfdTag(tag)) writable.push_back(tag);
  }
  return writable;
}

/// Append an IFD of @p tags (sorted by tag) to @p tiff, with value data after the entries.
/// Returns the position of the IFD in @p tiff.
auto EncodeIfd(std::vector<uint8_t>& tiff, const std::vector<ExifIfdTag>& tags) -> size_t {
  // IFD offsets must be word aligned; an odd-length value can end the previous IFD on an odd
  // offset.
  if (tiff.size() % 2 != 0) tiff.push_back(0);
  const size_t ifd_pos = tiff.size();
  AppendU16LE(tiff, static_cast<uint16_t>(tags.size()));
  const size_t entries_pos = tiff.size();
  tiff.resize(tiff.size() + tags.size() * 12u + 4u, 0);

  for (size_t i = 0; i < tags.size(); ++i) {
    const auto& tag  = tags[i];
    const auto  slot = entries_pos + i * 12u;
    tiff[slot]       = static_cast<uint8_t>(tag.tag_ & 0xFF);
    tiff[slot + 1]   = static_cast<uint8_t>((tag.tag_ >> 8) & 0xFF);
    tiff[slot + 2]   = static_cast<uint8_t>(tag.type_ & 0xFF);
    tiff[slot + 3]   = static_cast<uint8_t>((tag.type_ >> 8) & 0xFF);
    WriteU32LE(tiff.data() + slot + 4, tag.count_);
    if (tag.value_.size() <= 4) {
      std::copy(tag.value_.begin(), tag.value_.end(), tiff.begin() + slot + 8);
    } else {
      // Value offsets must be word aligned.
      if (tiff.size() % 2 != 0) tiff.push_back(0);
      WriteU32LE(tiff.data() + slot + 8, static_cast<uint32_t>(tiff.size()));
      tiff.insert(tiff.end(), tag.value_.begin(), tag.value_.end());
    }
  }
  // The next-IFD offset stays 0: the export writes no IFD1 (thumbnail).
  return ifd_pos;
}

/// Write @p offset into the value of the IFD entry @p tag (an IFD pointer of type LONG).
void PatchIfdPointer(std::vector<uint8_t>& tiff, size_t ifd_pos, uint16_t tag, uint32_t offset) {
  const uint16_t count =
      static_cast<uint16_t>(tiff[ifd_pos] | (static_cast<uint16_t>(tiff[ifd_pos + 1]) << 8));
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t*       slot = tiff.data() + ifd_pos + 2 + i * 12;
    const uint16_t entry_tag =
        static_cast<uint16_t>(slot[0] | (static_cast<uint16_t>(slot[1]) << 8));
    if (entry_tag == tag) {
      WriteU32LE(slot + 8, offset);
      return;
    }
  }
}

/// Encode the TIFF header, IFD0, and the EXIF and GPS IFDs that IFD0 points to.
auto EncodeTiffBlock(const ExifIfdEntries& entries) -> std::vector<uint8_t> {
  std::vector<ExifIfdTag> ifd0 = WritableTags(entries.ifd0_);
  std::vector<ExifIfdTag> exif = WritableTags(entries.exif_);
  std::vector<ExifIfdTag> gps  = WritableTags(entries.gps_);
  if (!exif.empty()) SetTag(ifd0, MakeLongTag(kExifIfdTag, 0));
  if (!gps.empty()) SetTag(ifd0, MakeLongTag(kGpsIfdTag, 0));
  SortByTag(ifd0);
  SortByTag(exif);
  SortByTag(gps);

  std::vector<uint8_t> tiff;
  tiff.push_back('I');
  tiff.push_back('I');
  AppendU16LE(tiff, 42);
  AppendU32LE(tiff, 8);

  const size_t ifd0_pos = EncodeIfd(tiff, ifd0);
  if (!exif.empty()) {
    const size_t exif_pos = EncodeIfd(tiff, exif);
    PatchIfdPointer(tiff, ifd0_pos, kExifIfdTag, static_cast<uint32_t>(exif_pos));
  }
  if (!gps.empty()) {
    const size_t gps_pos = EncodeIfd(tiff, gps);
    PatchIfdPointer(tiff, ifd0_pos, kGpsIfdTag, static_cast<uint32_t>(gps_pos));
  }
  return tiff;
}

auto MakeApp1Payload(const ExifIfdEntries& entries) -> std::vector<uint8_t> {
  constexpr uint8_t          kExifPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
  const std::vector<uint8_t> tiff          = EncodeTiffBlock(entries);
  std::vector<uint8_t>       payload(std::begin(kExifPrefix), std::end(kExifPrefix));
  payload.insert(payload.end(), tiff.begin(), tiff.end());
  return payload;
}

auto IsSameTag(const ExifIfdTag& a, const ExifIfdTag& b) -> bool {
  return a.tag_ == b.tag_ && a.type_ == b.type_ && a.count_ == b.count_ && a.value_ == b.value_;
}

/// Remove the largest entry of @p entries that is an unchanged copy of a @p source entry.
/// Returns false when no such entry with a value larger than 4 bytes exists.
auto RemoveLargestSourceEntry(ExifIfdEntries& entries, const ExifIfdEntries& source) -> bool {
  std::vector<ExifIfdTag>* largest_list = nullptr;
  size_t                   largest_pos  = 0;
  size_t                   largest_size = 4;
  const auto scan = [&](std::vector<ExifIfdTag>& list, const std::vector<ExifIfdTag>& from) {
    for (size_t i = 0; i < list.size(); ++i) {
      const bool from_source = std::any_of(
          from.begin(), from.end(), [&](const ExifIfdTag& tag) { return IsSameTag(tag, list[i]); });
      if (from_source && list[i].value_.size() > largest_size) {
        largest_list = &list;
        largest_pos  = i;
        largest_size = list[i].value_.size();
      }
    }
  };
  scan(entries.ifd0_, source.ifd0_);
  scan(entries.exif_, source.exif_);
  scan(entries.gps_, source.gps_);
  if (largest_list == nullptr) return false;
  largest_list->erase(largest_list->begin() + static_cast<std::ptrdiff_t>(largest_pos));
  return true;
}

}  // namespace

auto ExifColorSpaceFor(const ExportColorProfileConfig& profile) -> uint16_t {
  const bool srgb_curve = profile.encoding_eotf == ColorUtils::EOTF::SRGB_PIECEWISE ||
                          profile.encoding_eotf == ColorUtils::EOTF::GAMMA_2_2;
  return profile.encoding_space == ColorUtils::ColorSpace::REC709 && srgb_curve ? 1 : 0xFFFF;
}

auto BuildExportExifIfdEntries(const ExportExifOptions& options) -> ExifIfdEntries {
  ExifIfdEntries entries;
  entries.ifd0_ = WritableTags(options.source_entries_.ifd0_);
  entries.exif_ = WritableTags(options.source_entries_.exif_);
  entries.gps_  = WritableTags(options.source_entries_.gps_);
  // The source image data does not describe the exported file.
  std::erase_if(entries.ifd0_,
                [](const ExifIfdTag& tag) { return IsExifImageStructureTag(tag.tag_); });

  auto& ifd0 = entries.ifd0_;
  auto& exif = entries.exif_;
  SetTag(ifd0, MakeShortTag(0x0112, 1));  // Orientation: the exported pixels are upright.
  if (options.include_software_) {
    SetTag(ifd0, MakeAsciiTag(0x0131, AlcedoSoftwareExifString()));  // Software
  }
  SetTag(exif, MakeUndefinedTag(0x9000, {'0', '2', '3', '2'}));  // ExifVersion
  SetTag(exif, MakeUndefinedTag(0xA000, {'0', '1', '0', '0'}));  // FlashpixVersion
  if (options.color_space_.has_value()) {
    SetTag(exif, MakeShortTag(0xA001, *options.color_space_));  // ColorSpace
  }
  if (options.width_ > 0) {
    SetTag(exif, MakeLongTag(0xA002, static_cast<uint32_t>(options.width_)));  // PixelXDimension
  }
  if (options.height_ > 0) {
    SetTag(exif, MakeLongTag(0xA003, static_cast<uint32_t>(options.height_)));  // PixelYDimension
  }

  if (options.metadata_.has_value() && HasMeaningfulExportMetadata(*options.metadata_)) {
    const auto& metadata = *options.metadata_;
    // The capture time and the rating are the current values of the photo.
    if (const auto exif_dt = ExifDateTimeString(metadata.date_time_str_); exif_dt.has_value()) {
      SetTag(ifd0, MakeAsciiTag(0x0132, *exif_dt));  // DateTime
      SetTag(exif, MakeAsciiTag(0x9003, *exif_dt));  // DateTimeOriginal
      SetTag(exif, MakeAsciiTag(0x9004, *exif_dt));  // DateTimeDigitized
    }
    const int normalized_rating = ExifDisplayMetaData::NormalizeRating(metadata.rating_);
    if (normalized_rating > 0) {
      SetTag(ifd0, MakeShortTag(0x4746, static_cast<uint16_t>(normalized_rating)));  // Rating
      SetTag(ifd0, MakeShortTag(0x4749, RatingPercentFor(normalized_rating)));  // RatingPercent
    }

    // The source values of the camera and the exposure are exact; the display values are
    // normalized names and rounded numbers. Use the display values only where the source has no
    // value.
    if (!metadata.make_.empty()) SetTagWhenMissing(ifd0, MakeAsciiTag(0x010F, metadata.make_));
    if (!metadata.model_.empty()) SetTagWhenMissing(ifd0, MakeAsciiTag(0x0110, metadata.model_));
    if (!metadata.lens_make_.empty()) {
      SetTagWhenMissing(exif, MakeAsciiTag(0xA433, metadata.lens_make_));  // LensMake
    }
    if (!metadata.lens_.empty()) {
      SetTagWhenMissing(exif, MakeAsciiTag(0xA434, metadata.lens_));  // LensModel
    }
    if (metadata.aperture_ > 0.0f) {
      SetTagWhenMissing(exif, MakeRationalTag(0x829D, RationalFromFloat(metadata.aperture_, 100)));
    }
    if (metadata.focal_ > 0.0f) {
      SetTagWhenMissing(exif, MakeRationalTag(0x920A, RationalFromFloat(metadata.focal_, 100)));
    }
    if (metadata.focal_35mm_ > 0.0f) {
      SetTagWhenMissing(exif, MakeShortTag(0xA405, static_cast<uint16_t>(std::lround(
                                                       metadata.focal_35mm_))));  // 35 mm focal
    }
    if (metadata.focus_distance_m_ > 0.0f) {
      SetTagWhenMissing(
          exif, MakeRationalTag(0x9206, RationalFromFloat(metadata.focus_distance_m_, 1000)));
    }
    if (metadata.iso_ > 0) {
      SetTagWhenMissing(
          exif,
          MakeShortTag(0x8827, static_cast<uint16_t>(std::min<uint64_t>(metadata.iso_, 65535))));
    }
    if (metadata.shutter_speed_.first > 0 && metadata.shutter_speed_.second > 0) {
      SetTagWhenMissing(
          exif, MakeRationalTag(0x829A, {static_cast<uint32_t>(metadata.shutter_speed_.first),
                                         static_cast<uint32_t>(metadata.shutter_speed_.second)}));
    }
  }

  SortByTag(entries.ifd0_);
  SortByTag(entries.exif_);
  SortByTag(entries.gps_);
  return entries;
}

auto BuildJpegExifApp1Payload(const ExportExifOptions& options) -> std::vector<uint8_t> {
  ExifIfdEntries entries = BuildExportExifIfdEntries(options);

  // The IFD0 fields of a JPEG file with EXIF data. The JPEG frame header holds the image size;
  // EXIF does not allow ImageWidth and ImageLength in JPEG files.
  auto&          ifd0    = entries.ifd0_;
  const double   dpi     = std::isfinite(options.resolution_dpi_) && options.resolution_dpi_ > 0.0
                               ? options.resolution_dpi_
                               : 72.0;
  const auto     dpi_rational = RationalFromFloat(static_cast<float>(dpi), 100);
  SetTag(ifd0, MakeRationalTag(0x011A, dpi_rational));            // XResolution
  SetTag(ifd0, MakeRationalTag(0x011B, dpi_rational));            // YResolution
  SetTag(ifd0, MakeShortTag(0x0128, 2));                          // ResolutionUnit: inches
  SetTag(ifd0, MakeShortTag(0x0213, 1));                          // YCbCrPositioning: centered
  SetTag(entries.exif_, MakeUndefinedTag(0x9101, {1, 2, 3, 0}));  // ComponentsConfiguration: YCbCr

  std::vector<uint8_t> payload = MakeApp1Payload(entries);
  while (payload.size() > kMaxApp1PayloadBytes &&
         RemoveLargestSourceEntry(entries, options.source_entries_)) {
    payload = MakeApp1Payload(entries);
  }
  if (payload.size() > kMaxApp1PayloadBytes) {
    return {};
  }
  return payload;
}

auto BuildExifTiffBlock(const ExifIfdEntries& entries) -> std::vector<uint8_t> {
  ExifIfdEntries block = entries;
  std::erase_if(block.ifd0_,
                [](const ExifIfdTag& tag) { return IsExifImageStructureTag(tag.tag_); });
  if (block.ifd0_.empty() && block.exif_.empty() && block.gps_.empty()) {
    return {};
  }
  return EncodeTiffBlock(block);
}

auto ReplaceJpegExifApp1Segment(const std::filesystem::path& path,
                                const std::vector<uint8_t>&  exif_payload) -> bool {
  if (exif_payload.empty() || exif_payload.size() > kMaxApp1PayloadBytes) {
    return false;
  }

  std::vector<uint8_t> bytes = ReadFileBytes(path);
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
      return WriteFileBytes(path, out);
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
  return WriteFileBytes(path, out);
}

auto ApplyJpegExifApp1Metadata(const std::filesystem::path& path, const ExportExifOptions& options)
    -> bool {
  const std::vector<uint8_t> exif_payload = BuildJpegExifApp1Payload(options);
  if (exif_payload.empty()) {
    return false;
  }
  return ReplaceJpegExifApp1Segment(path, exif_payload);
}

}  // namespace alcedo
