//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/raster_container_reader.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>

namespace alcedo {
namespace {

/// Upper bound for an inflated iCCP profile. Real profiles are below 1 MB.
constexpr std::size_t kMaxInflatedIccBytes = 64u << 20;

auto                  ToU8(std::byte value) -> uint8_t { return std::to_integer<uint8_t>(value); }

void RequireRange(std::span<const std::byte> bytes, std::size_t offset, std::size_t length,
                  const char* what) {
  if (offset > bytes.size() || length > bytes.size() - offset) {
    throw RasterContainerError(std::string(what) + " lies outside the file");
  }
}

auto ReadBe16(std::span<const std::byte> bytes, std::size_t offset) -> uint16_t {
  RequireRange(bytes, offset, 2, "16-bit field");
  return static_cast<uint16_t>((ToU8(bytes[offset]) << 8) | ToU8(bytes[offset + 1]));
}

auto ReadBe32(std::span<const std::byte> bytes, std::size_t offset) -> uint32_t {
  RequireRange(bytes, offset, 4, "32-bit field");
  return (static_cast<uint32_t>(ToU8(bytes[offset])) << 24) |
         (static_cast<uint32_t>(ToU8(bytes[offset + 1])) << 16) |
         (static_cast<uint32_t>(ToU8(bytes[offset + 2])) << 8) |
         static_cast<uint32_t>(ToU8(bytes[offset + 3]));
}

auto ReadLe32(std::span<const std::byte> bytes, std::size_t offset) -> uint32_t {
  RequireRange(bytes, offset, 4, "32-bit field");
  return static_cast<uint32_t>(ToU8(bytes[offset])) |
         (static_cast<uint32_t>(ToU8(bytes[offset + 1])) << 8) |
         (static_cast<uint32_t>(ToU8(bytes[offset + 2])) << 16) |
         (static_cast<uint32_t>(ToU8(bytes[offset + 3])) << 24);
}

auto ReadLeFloat(std::span<const std::byte> bytes, std::size_t offset) -> float {
  const uint32_t bits  = ReadLe32(bytes, offset);
  float          value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

auto HasPrefix(std::span<const std::byte> bytes, std::string_view prefix) -> bool {
  if (bytes.size() < prefix.size()) {
    return false;
  }
  return std::memcmp(bytes.data(), prefix.data(), prefix.size()) == 0;
}

auto CopyBytes(std::span<const std::byte> bytes) -> std::vector<std::byte> {
  return {bytes.begin(), bytes.end()};
}

// -------------------------------------------------------------------------------------------
// TIFF image file directories
// -------------------------------------------------------------------------------------------

struct TiffEntry {
  uint16_t tag_          = 0;
  uint16_t type_         = 0;
  uint32_t count_        = 0;
  /// Offset of the value bytes in the file: the entry's value field when the value fits in
  /// four bytes, else the offset that the entry stores.
  uint32_t value_offset_ = 0;
};

auto TiffTypeSize(uint16_t type) -> std::size_t {
  switch (type) {
    case 1:  // BYTE
    case 2:  // ASCII
    case 6:  // SBYTE
    case 7:  // UNDEFINED
      return 1;
    case 3:  // SHORT
    case 8:  // SSHORT
      return 2;
    case 4:   // LONG
    case 9:   // SLONG
    case 11:  // FLOAT
    case 13:  // IFD
      return 4;
    case 5:   // RATIONAL
    case 10:  // SRATIONAL
    case 12:  // DOUBLE
      return 8;
    default:
      return 0;
  }
}

/// Classic (32-bit offset) TIFF structure in either byte order.
class TiffStructure {
 public:
  explicit TiffStructure(std::span<const std::byte> bytes) : bytes_(bytes) {
    RequireRange(bytes_, 0, 8, "TIFF header");
    if (HasPrefix(bytes_, "II")) {
      little_endian_ = true;
    } else if (HasPrefix(bytes_, "MM")) {
      little_endian_ = false;
    } else {
      throw RasterContainerError("TIFF byte-order mark is invalid");
    }
    const uint16_t magic = U16(2);
    if (magic == 43) {
      throw RasterContainerError("BigTIFF files are not supported");
    }
    if (magic != 42) {
      throw RasterContainerError("TIFF magic number is invalid");
    }
    first_ifd_offset_ = U32(4);
  }

  [[nodiscard]] auto FirstIfdOffset() const -> uint32_t { return first_ifd_offset_; }

  [[nodiscard]] auto ReadIfd(uint32_t offset) const -> std::map<uint16_t, TiffEntry> {
    const uint16_t count = U16(offset);
    RequireRange(bytes_, static_cast<std::size_t>(offset) + 2, std::size_t{count} * 12u,
                 "TIFF directory");
    std::map<uint16_t, TiffEntry> entries;
    for (uint16_t i = 0; i < count; ++i) {
      const std::size_t base = static_cast<std::size_t>(offset) + 2 + std::size_t{i} * 12u;
      TiffEntry         entry;
      entry.tag_          = U16(base);
      entry.type_         = U16(base + 2);
      entry.count_        = U32(base + 4);
      const auto size     = TiffTypeSize(entry.type_);
      const auto payload  = static_cast<uint64_t>(size) * entry.count_;
      entry.value_offset_ = payload <= 4 ? static_cast<uint32_t>(base + 8) : U32(base + 8);
      entries.emplace(entry.tag_, entry);
    }
    return entries;
  }

  /// Raw value bytes of @p entry.
  [[nodiscard]] auto ValueBytes(const TiffEntry& entry) const -> std::span<const std::byte> {
    const auto size = TiffTypeSize(entry.type_);
    if (size == 0) {
      throw RasterContainerError("TIFF entry has an unknown type");
    }
    const auto length = static_cast<uint64_t>(size) * entry.count_;
    if (length > bytes_.size()) {
      throw RasterContainerError("TIFF entry value is larger than the file");
    }
    RequireRange(bytes_, entry.value_offset_, static_cast<std::size_t>(length), "TIFF value");
    return bytes_.subspan(entry.value_offset_, static_cast<std::size_t>(length));
  }

  /// Unsigned integer value @p index of a BYTE, SHORT, LONG or IFD entry.
  [[nodiscard]] auto Uint(const TiffEntry& entry, uint32_t index = 0) const -> uint32_t {
    if (index >= entry.count_) {
      throw RasterContainerError("TIFF entry index is out of range");
    }
    const auto value_bytes = ValueBytes(entry);
    switch (entry.type_) {
      case 1:
      case 7:
        return ToU8(value_bytes[index]);
      case 3:
        return U16(entry.value_offset_ + index * 2u);
      case 4:
      case 13:
        return U32(entry.value_offset_ + index * 4u);
      default:
        throw RasterContainerError("TIFF entry is not an unsigned integer");
    }
  }

  [[nodiscard]] auto Ascii(const TiffEntry& entry) const -> std::string {
    const auto  value_bytes = ValueBytes(entry);
    std::string text;
    for (const auto byte : value_bytes) {
      if (ToU8(byte) == 0) {
        break;
      }
      text.push_back(static_cast<char>(ToU8(byte)));
    }
    return text;
  }

 private:
  [[nodiscard]] auto U16(std::size_t offset) const -> uint16_t {
    RequireRange(bytes_, offset, 2, "TIFF field");
    const auto a = ToU8(bytes_[offset]);
    const auto b = ToU8(bytes_[offset + 1]);
    return little_endian_ ? static_cast<uint16_t>(a | (b << 8))
                          : static_cast<uint16_t>((a << 8) | b);
  }

  [[nodiscard]] auto U32(std::size_t offset) const -> uint32_t {
    return little_endian_ ? ReadLe32(bytes_, offset) : ReadBe32(bytes_, offset);
  }

  std::span<const std::byte> bytes_;
  bool                       little_endian_    = true;
  uint32_t                   first_ifd_offset_ = 0;
};

auto FindEntry(const std::map<uint16_t, TiffEntry>& entries, uint16_t tag) -> const TiffEntry* {
  const auto it = entries.find(tag);
  return it == entries.end() ? nullptr : &it->second;
}

// -------------------------------------------------------------------------------------------
// zlib
// -------------------------------------------------------------------------------------------

auto InflateZlib(std::span<const std::byte> compressed) -> std::vector<std::byte> {
  z_stream stream{};
  if (inflateInit(&stream) != Z_OK) {
    return {};
  }
  stream.next_in  = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());
  std::vector<std::byte>   output;
  std::array<Bytef, 16384> chunk{};
  int                      status = Z_OK;
  while (status == Z_OK) {
    stream.next_out  = chunk.data();
    stream.avail_out = static_cast<uInt>(chunk.size());
    status           = inflate(&stream, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END) {
      inflateEnd(&stream);
      return {};
    }
    const auto produced = chunk.size() - stream.avail_out;
    if (output.size() + produced > kMaxInflatedIccBytes) {
      inflateEnd(&stream);
      return {};
    }
    const auto* begin = reinterpret_cast<const std::byte*>(chunk.data());
    output.insert(output.end(), begin, begin + produced);
    if (status == Z_OK && stream.avail_in == 0 && produced == 0) {
      break;
    }
  }
  inflateEnd(&stream);
  return status == Z_STREAM_END ? output : std::vector<std::byte>{};
}

}  // namespace

// -------------------------------------------------------------------------------------------
// JPEG
// -------------------------------------------------------------------------------------------

auto ReadJpegContainer(std::span<const std::byte> file_bytes) -> JpegContainerInfo {
  if (file_bytes.size() < 4 || ToU8(file_bytes[0]) != 0xFF || ToU8(file_bytes[1]) != 0xD8) {
    throw RasterContainerError("JPEG SOI marker is missing");
  }
  JpegContainerInfo                             info;
  std::map<uint8_t, std::span<const std::byte>> icc_segments;
  uint8_t                                       icc_segment_count = 0;

  std::size_t                                   pos               = 2;
  while (pos < file_bytes.size()) {
    if (ToU8(file_bytes[pos]) != 0xFF) {
      throw RasterContainerError("JPEG marker is expected");
    }
    while (pos < file_bytes.size() && ToU8(file_bytes[pos]) == 0xFF) {
      ++pos;  // Fill bytes.
    }
    if (pos >= file_bytes.size()) {
      break;
    }
    const uint8_t marker = ToU8(file_bytes[pos++]);
    if (marker == 0xD9) {
      break;  // EOI
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;  // Markers without a length field.
    }
    const uint16_t length = ReadBe16(file_bytes, pos);
    if (length < 2) {
      throw RasterContainerError("JPEG segment length is invalid");
    }
    RequireRange(file_bytes, pos, length, "JPEG segment");
    const auto segment = file_bytes.subspan(pos + 2, length - 2u);
    pos += length;

    if (marker == 0xE1 && HasPrefix(segment, std::string_view("Exif\0\0", 6))) {
      info.exif_tiff_ = CopyBytes(segment.subspan(6));
    } else if (marker == 0xE2 && HasPrefix(segment, std::string_view("ICC_PROFILE\0", 12)) &&
               segment.size() >= 14) {
      const uint8_t sequence = ToU8(segment[12]);
      icc_segment_count      = ToU8(segment[13]);
      icc_segments.emplace(sequence, segment.subspan(14));
    } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
               marker != 0xCC) {
      if (segment.size() < 6) {
        throw RasterContainerError("JPEG SOF segment is too short");
      }
      info.component_count_ = ToU8(segment[5]);
    } else if (marker == 0xDA) {
      break;  // Entropy-coded data follows; no color markers after the first scan matter.
    }
  }
  if (info.component_count_ == 0) {
    throw RasterContainerError("JPEG has no SOF segment");
  }

  bool complete = icc_segment_count > 0 && icc_segments.size() == icc_segment_count;
  for (uint8_t sequence = 1; complete && sequence <= icc_segment_count; ++sequence) {
    complete = icc_segments.contains(sequence);
  }
  if (complete) {
    for (const auto& [sequence, data] : icc_segments) {
      info.icc_profile_.insert(info.icc_profile_.end(), data.begin(), data.end());
    }
  }
  return info;
}

// -------------------------------------------------------------------------------------------
// PNG
// -------------------------------------------------------------------------------------------

auto ReadPngContainer(std::span<const std::byte> file_bytes) -> PngContainerInfo {
  constexpr std::string_view kSignature("\x89PNG\r\n\x1a\n", 8);
  if (!HasPrefix(file_bytes, kSignature)) {
    throw RasterContainerError("PNG signature is invalid");
  }
  PngContainerInfo info;
  std::size_t      pos       = kSignature.size();
  bool             have_ihdr = false;
  while (true) {
    const uint32_t length = ReadBe32(file_bytes, pos);
    RequireRange(file_bytes, pos + 4, 4, "PNG chunk type");
    const std::string_view type(reinterpret_cast<const char*>(file_bytes.data() + pos + 4), 4);
    RequireRange(file_bytes, pos + 8, static_cast<std::size_t>(length) + 4u, "PNG chunk");
    const auto data = file_bytes.subspan(pos + 8, length);
    pos += 12u + length;

    if (!have_ihdr) {
      if (type != "IHDR" || length != 13) {
        throw RasterContainerError("PNG must start with a 13-byte IHDR chunk");
      }
      info.bit_depth_  = ToU8(data[8]);
      info.color_type_ = ToU8(data[9]);
      have_ihdr        = true;
      continue;
    }
    if (type == "IDAT" || type == "IEND") {
      break;
    }
    if (type == "cICP" && length == 4) {
      info.cicp_ =
          std::array<uint8_t, 4>{ToU8(data[0]), ToU8(data[1]), ToU8(data[2]), ToU8(data[3])};
    } else if (type == "iCCP") {
      info.has_iccp_      = true;
      const auto name_end = std::find(
          data.begin(), data.begin() + std::min<std::size_t>(data.size(), 80), std::byte{0});
      const auto name_length = static_cast<std::size_t>(name_end - data.begin());
      // Profile name, NUL, compression method 0, then the zlib stream.
      if (name_end != data.end() && name_length + 2 <= data.size() &&
          ToU8(data[name_length + 1]) == 0) {
        info.icc_profile_ = InflateZlib(data.subspan(name_length + 2));
      }
    } else if (type == "sRGB" && length == 1) {
      info.has_srgb_ = true;
    } else if (type == "gAMA" && length == 4) {
      info.gama_ = ReadBe32(data, 0);
    } else if (type == "cHRM" && length == 32) {
      std::array<uint32_t, 8> values{};
      for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = ReadBe32(data, i * 4);
      }
      info.chrm_ = values;
    } else if (type == "mDCv" && length == 24) {
      info.mastering_max_luminance_nits_ = static_cast<float>(ReadBe32(data, 16)) * 0.0001f;
    } else if (type == "cLLI" && length == 8) {
      info.max_content_light_level_nits_ = static_cast<float>(ReadBe32(data, 0)) * 0.0001f;
    } else if (type == "eXIf") {
      info.exif_tiff_ = CopyBytes(data);
    }
  }
  return info;
}

// -------------------------------------------------------------------------------------------
// TIFF and EXIF
// -------------------------------------------------------------------------------------------

auto ReadTiffContainer(std::span<const std::byte> file_bytes) -> TiffContainerInfo {
  const TiffStructure tiff(file_bytes);
  const auto          entries = tiff.ReadIfd(tiff.FirstIfdOffset());
  TiffContainerInfo   info;
  if (const auto* entry = FindEntry(entries, 256)) {
    info.width_ = tiff.Uint(*entry);
  }
  if (const auto* entry = FindEntry(entries, 257)) {
    info.height_ = tiff.Uint(*entry);
  }
  if (const auto* entry = FindEntry(entries, 258)) {
    info.bits_per_sample_ = static_cast<uint16_t>(tiff.Uint(*entry));
  }
  if (const auto* entry = FindEntry(entries, 262)) {
    info.photometric_ = static_cast<uint16_t>(tiff.Uint(*entry));
  }
  if (const auto* entry = FindEntry(entries, 271)) {
    info.make_ = tiff.Ascii(*entry);
  }
  if (const auto* entry = FindEntry(entries, 274)) {
    info.orientation_ = static_cast<uint16_t>(tiff.Uint(*entry));
  }
  if (const auto* entry = FindEntry(entries, 277)) {
    info.samples_per_pixel_ = static_cast<uint16_t>(tiff.Uint(*entry));
  }
  if (const auto* entry = FindEntry(entries, 339)) {
    info.sample_format_ = static_cast<uint16_t>(tiff.Uint(*entry));
  }
  if (const auto* entry = FindEntry(entries, 34675)) {
    info.icc_profile_ = CopyBytes(tiff.ValueBytes(*entry));
  }
  info.has_dng_version_ = FindEntry(entries, 50706) != nullptr;
  return info;
}

auto ReadExifColorTags(std::span<const std::byte> exif_tiff) -> ExifColorTags {
  ExifColorTags tags;
  try {
    const TiffStructure tiff(exif_tiff);
    const auto          ifd0 = tiff.ReadIfd(tiff.FirstIfdOffset());
    if (const auto* entry = FindEntry(ifd0, 274)) {
      tags.orientation_ = static_cast<uint16_t>(tiff.Uint(*entry));
    }
    const auto* exif_pointer = FindEntry(ifd0, 0x8769);
    if (exif_pointer == nullptr) {
      return tags;
    }
    const auto exif_ifd = tiff.ReadIfd(tiff.Uint(*exif_pointer));
    if (const auto* entry = FindEntry(exif_ifd, 0xA001)) {
      tags.color_space_ = static_cast<uint16_t>(tiff.Uint(*entry));
    }
    if (const auto* interop_pointer = FindEntry(exif_ifd, 0xA005)) {
      const auto interop = tiff.ReadIfd(tiff.Uint(*interop_pointer));
      if (const auto* entry = FindEntry(interop, 0x0001)) {
        tags.interop_index_ = tiff.Ascii(*entry);
      }
    }
  } catch (const RasterContainerError&) {
    // EXIF is optional; the values read before the malformed structure are kept.
  }
  return tags;
}

// -------------------------------------------------------------------------------------------
// OpenEXR
// -------------------------------------------------------------------------------------------

auto ReadExrHeader(std::span<const std::byte> file_bytes) -> ExrHeaderInfo {
  if (file_bytes.size() < 8 || ToU8(file_bytes[0]) != 0x76 || ToU8(file_bytes[1]) != 0x2F ||
      ToU8(file_bytes[2]) != 0x31 || ToU8(file_bytes[3]) != 0x01) {
    throw RasterContainerError("OpenEXR magic number is invalid");
  }
  auto read_string = [&](std::size_t& pos) {
    std::string text;
    while (true) {
      RequireRange(file_bytes, pos, 1, "OpenEXR attribute string");
      const auto ch = ToU8(file_bytes[pos++]);
      if (ch == 0) {
        return text;
      }
      if (text.size() >= 255) {
        throw RasterContainerError("OpenEXR attribute string is too long");
      }
      text.push_back(static_cast<char>(ch));
    }
  };

  ExrHeaderInfo info;
  std::size_t   pos = 8;
  while (true) {
    const auto name = read_string(pos);
    if (name.empty()) {
      break;
    }
    const auto     type = read_string(pos);
    const uint32_t size = ReadLe32(file_bytes, pos);
    pos += 4;
    RequireRange(file_bytes, pos, size, "OpenEXR attribute value");
    if (name == "chromaticities" && type == "chromaticities" && size == 32) {
      std::array<float, 8> values{};
      for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = ReadLeFloat(file_bytes, pos + i * 4);
      }
      info.chromaticities_ = values;
    } else if (name == "acesImageContainerFlag" && type == "int" && size == 4) {
      info.aces_container_ = ReadLe32(file_bytes, pos) == 1;
    }
    pos += size;
  }
  return info;
}

}  // namespace alcedo
