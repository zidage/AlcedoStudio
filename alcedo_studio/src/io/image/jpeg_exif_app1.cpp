//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/jpeg_exif_app1.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "alcedo_version.hpp"

namespace alcedo {
namespace {

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
  std::vector<uint8_t> inline_or_offset;
  std::vector<uint8_t> overflow;
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

}  // namespace

auto BuildJpegExifApp1Payload(const JpegExifApp1Options& options) -> std::vector<uint8_t> {
  const bool has_metadata =
      options.metadata_.has_value() && HasMeaningfulExportMetadata(*options.metadata_);
  if (!options.include_software_ && !has_metadata && options.width_ <= 0 && options.height_ <= 0) {
    return {};
  }

  std::vector<ExifIfdEntry> exif_ifd;
  std::vector<ExifIfdEntry> ifd0;
  ifd0.push_back(MakeShortEntry(0x0112, 1));  // Orientation = upright

  if (options.include_software_) {
    ifd0.push_back(MakeAsciiEntry(0x0131, AlcedoSoftwareExifString()));  // Software
  }

  if (options.width_ > 0) {
    ifd0.push_back(MakeLongEntry(0x0100, static_cast<uint32_t>(options.width_)));  // ImageWidth
    exif_ifd.push_back(
        MakeLongEntry(0xA002, static_cast<uint32_t>(options.width_)));  // PixelXDimension
  }
  if (options.height_ > 0) {
    ifd0.push_back(MakeLongEntry(0x0101, static_cast<uint32_t>(options.height_)));  // ImageLength
    exif_ifd.push_back(
        MakeLongEntry(0xA003, static_cast<uint32_t>(options.height_)));  // PixelYDimension
  }

  if (has_metadata) {
    const auto& metadata = *options.metadata_;
    if (const auto exif_dt = ExifDateTimeString(metadata.date_time_str_); exif_dt.has_value()) {
      exif_ifd.push_back(MakeAsciiEntry(0x9003, *exif_dt));  // DateTimeOriginal
      exif_ifd.push_back(MakeAsciiEntry(0x9004, *exif_dt));  // DateTimeDigitized
      ifd0.push_back(MakeAsciiEntry(0x0132, *exif_dt));      // DateTime
    }
    if (!metadata.lens_make_.empty()) {
      exif_ifd.push_back(MakeAsciiEntry(0xA433, metadata.lens_make_));
    }
    if (!metadata.lens_.empty()) {
      exif_ifd.push_back(MakeAsciiEntry(0xA434, metadata.lens_));
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
    if (metadata.focal_35mm_ > 0.0f) {
      exif_ifd.push_back(MakeShortEntry(
          0xA405, static_cast<uint16_t>(std::lround(metadata.focal_35mm_))));  // FocalLengthIn35mmFilm
    }
    if (metadata.focus_distance_m_ > 0.0f) {
      const auto rational = RationalFromFloat(metadata.focus_distance_m_, 1000);
      exif_ifd.push_back(MakeRationalEntry(0x9206, static_cast<uint32_t>(rational.first),
                                           static_cast<uint32_t>(rational.second)));
    }
    if (metadata.iso_ > 0) {
      exif_ifd.push_back(MakeShortEntry(
          0x8827, static_cast<uint16_t>(std::min<uint64_t>(metadata.iso_, 65535))));
    }
    if (metadata.shutter_speed_.first > 0 && metadata.shutter_speed_.second > 0) {
      exif_ifd.push_back(MakeRationalEntry(0x829A, static_cast<uint32_t>(metadata.shutter_speed_.first),
                                           static_cast<uint32_t>(metadata.shutter_speed_.second)));
    }
    if (!metadata.make_.empty()) {
      ifd0.push_back(MakeAsciiEntry(0x010F, metadata.make_));
    }
    if (!metadata.model_.empty()) {
      ifd0.push_back(MakeAsciiEntry(0x0110, metadata.model_));
    }
    const int normalized_rating = ExifDisplayMetaData::NormalizeRating(metadata.rating_);
    if (normalized_rating > 0) {
      ifd0.push_back(MakeShortEntry(0x4746, static_cast<uint16_t>(normalized_rating)));
      ifd0.push_back(MakeShortEntry(0x4749, RatingPercentFor(normalized_rating)));
    }
  }

  constexpr uint16_t kExifIfdTag   = 0x8769;
  const bool         need_exif_ifd = !exif_ifd.empty();
  if (need_exif_ifd) {
    ifd0.push_back(MakeLongEntry(kExifIfdTag, 0));
  }

  std::sort(ifd0.begin(), ifd0.end(),
            [](const ExifIfdEntry& a, const ExifIfdEntry& b) { return a.tag < b.tag; });
  std::sort(exif_ifd.begin(), exif_ifd.end(),
            [](const ExifIfdEntry& a, const ExifIfdEntry& b) { return a.tag < b.tag; });

  std::vector<uint8_t> tiff;
  tiff.push_back('I');
  tiff.push_back('I');
  AppendU16LE(tiff, 42);
  AppendU32LE(tiff, 8);

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

auto ReplaceJpegExifApp1Segment(const std::filesystem::path& path,
                                const std::vector<uint8_t>&  exif_payload) -> bool {
  if (exif_payload.empty() || exif_payload.size() > 65533) {
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

auto ApplyJpegExifApp1Metadata(const std::filesystem::path& path,
                               const JpegExifApp1Options&   options) -> bool {
  const std::vector<uint8_t> exif_payload = BuildJpegExifApp1Payload(options);
  if (exif_payload.empty()) {
    return false;
  }
  return ReplaceJpegExifApp1Segment(path, exif_payload);
}

}  // namespace alcedo
