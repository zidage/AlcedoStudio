//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/exif_ifd_tags.hpp"

#include <algorithm>
#include <array>
#include <exiv2/exiv2.hpp>
#include <fstream>
#include <stdexcept>
#include <string>

namespace alcedo {
namespace {

constexpr uint16_t kExifIfdTag    = 0x8769;
constexpr uint16_t kGpsIfdTag     = 0x8825;
constexpr uint16_t kInteropIfdTag = 0xA005;
constexpr uint16_t kSubIfdsTag    = 0x014A;
constexpr uint16_t kTiffLongType  = 4;

void               AppendU16LE(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendU32LE(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
}

auto ReadU16LE(const uint8_t* bytes) -> uint16_t {
  return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

auto ReadU32LE(const uint8_t* bytes) -> uint32_t {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

auto SortedByTag(std::vector<ExifIfdTag> tags) -> std::vector<ExifIfdTag> {
  std::stable_sort(tags.begin(), tags.end(),
                   [](const ExifIfdTag& a, const ExifIfdTag& b) { return a.tag_ < b.tag_; });
  return tags;
}

/// IFD0 tags that describe the source file or that the export writes itself.
auto IsExcludedSourceIfd0Tag(uint16_t tag) -> bool {
  if (IsExifImageStructureTag(tag)) return true;
  switch (tag) {
    case 0x0112:  // Orientation: the exported pixels are upright.
    case 0x0131:  // Software: the export writes Alcedo.
    case 0x02BC:  // XMP packet
    case 0x4746:  // Rating: the export writes the current rating.
    case 0x4749:  // RatingPercent
    case 0x83BB:  // IPTC-NAA
    case 0x8649:  // Photoshop image resources
    case 0xC4A5:  // PrintIM
    case 0xEA1C:  // Microsoft padding
    case 0xEA1D:  // Microsoft offset schema
      return true;
    default:
      break;
  }
  // TIFF/EP copies of EXIF fields in IFD0 (CFA layout, exposure, capture time, sensing method).
  // The EXIF IFD holds the EXIF versions of these fields. Copyright (0x8298) is kept.
  if (tag >= 0x828D && tag <= 0x9217 && tag != 0x8298) return true;
  // DNG tags, including the camera serial number and the private data of the camera maker.
  return (tag >= 0xC612 && tag <= 0xC7FF) || (tag >= 0xCD00 && tag <= 0xCDFF);
}

/// EXIF IFD tags that describe the source file or that the export writes itself.
auto IsExcludedSourceExifTag(uint16_t tag, bool include_device_serials) -> bool {
  switch (tag) {
    case 0x9000:  // ExifVersion: the export writes the version of its EXIF IFD.
    case 0x9102:  // CompressedBitsPerPixel of the source compression
    case 0x927C:  // MakerNote: its offsets are relative to the source file.
    case 0xA000:  // FlashpixVersion: the export writes it.
    case 0xA002:  // PixelXDimension: the export writes the exported size.
    case 0xA003:  // PixelYDimension
    case 0xA005:  // Interoperability IFD pointer
    case 0xEA1C:  // Microsoft padding
    case 0xEA1D:  // Microsoft offset schema
      return true;
    case 0xA431:  // BodySerialNumber
    case 0xA435:  // LensSerialNumber
      return !include_device_serials;
    default:
      return false;
  }
}

/// One 12-byte IFD entry: tag, type, count, and the value or the offset of the value.
struct TiffIfdEntryBytes {
  uint16_t                tag_ = 0;
  std::array<uint8_t, 12> bytes_{};
};

/// The entry of @p tag. A value longer than 4 bytes is appended to @p out, which starts at file
/// offset @p out_offset, and the entry holds its offset.
auto AppendTagEntry(std::vector<uint8_t>& out, uint64_t out_offset, const ExifIfdTag& tag)
    -> TiffIfdEntryBytes {
  std::vector<uint8_t> bytes;
  AppendU16LE(bytes, tag.tag_);
  AppendU16LE(bytes, tag.type_);
  AppendU32LE(bytes, tag.count_);
  if (tag.value_.size() <= 4) {
    std::vector<uint8_t> inline_value = tag.value_;
    inline_value.resize(4, 0);
    bytes.insert(bytes.end(), inline_value.begin(), inline_value.end());
  } else {
    if (out.size() % 2 != 0) out.push_back(0);
    AppendU32LE(bytes, static_cast<uint32_t>(out_offset + out.size()));
    out.insert(out.end(), tag.value_.begin(), tag.value_.end());
  }
  TiffIfdEntryBytes entry{.tag_ = tag.tag_};
  std::copy(bytes.begin(), bytes.end(), entry.bytes_.begin());
  return entry;
}

/// Append an IFD of @p entries to @p out, which starts at file offset @p out_offset, and return
/// the file offset of the IFD.
auto AppendIfd(std::vector<uint8_t>& out, uint64_t out_offset,
               std::vector<TiffIfdEntryBytes> entries, uint32_t next_ifd_offset) -> uint64_t {
  std::stable_sort(
      entries.begin(), entries.end(),
      [](const TiffIfdEntryBytes& a, const TiffIfdEntryBytes& b) { return a.tag_ < b.tag_; });
  if (out.size() % 2 != 0) out.push_back(0);
  const uint64_t ifd_offset = out_offset + out.size();
  AppendU16LE(out, static_cast<uint16_t>(entries.size()));
  for (const auto& entry : entries) {
    out.insert(out.end(), entry.bytes_.begin(), entry.bytes_.end());
  }
  AppendU32LE(out, next_ifd_offset);
  return ifd_offset;
}

/// The entries of an existing IFD, with the entries of @p tags replacing those with the same tag.
/// The values of the existing entries stay where they are in the file.
auto MergeIfdEntries(std::vector<uint8_t>& out, uint64_t out_offset,
                     const std::vector<TiffIfdEntryBytes>& existing,
                     const std::vector<ExifIfdTag>&        tags) -> std::vector<TiffIfdEntryBytes> {
  std::vector<TiffIfdEntryBytes> merged;
  for (const auto& entry : existing) {
    const bool replaced = std::any_of(tags.begin(), tags.end(), [&entry](const ExifIfdTag& tag) {
      return tag.tag_ == entry.tag_;
    });
    if (!replaced) merged.push_back(entry);
  }
  for (const auto& tag : tags) {
    merged.push_back(AppendTagEntry(out, out_offset, tag));
  }
  return merged;
}

auto WritableTags(const std::vector<ExifIfdTag>& tags) -> std::vector<ExifIfdTag> {
  std::vector<ExifIfdTag> writable;
  for (const auto& tag : tags) {
    if (IsWritableExifIfdTag(tag)) writable.push_back(tag);
  }
  return SortedByTag(std::move(writable));
}

void AppendU32BE(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
}

auto ReadU32BE(const uint8_t* bytes) -> uint32_t {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

/// CRC-32 (ISO 3309) of @p bytes, as PNG chunks store it.
auto PngCrc32(const uint8_t* bytes, size_t size) -> uint32_t {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

/// All bytes of @p path in one read. @p file_role names the file in the error message.
auto ReadFileBytes(const std::filesystem::path& path, const std::string& file_role)
    -> std::vector<uint8_t> {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    throw std::runtime_error("cannot open the " + file_role + " file to read its EXIF metadata");
  }
  const std::streamoff size = input.tellg();
  if (size < 0) {
    throw std::runtime_error("cannot read the " + file_role + " file to read its EXIF metadata");
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input) {
    throw std::runtime_error("cannot read the " + file_role + " file to read its EXIF metadata");
  }
  return bytes;
}

auto MakeIfdPointerTag(uint16_t tag, uint64_t offset) -> ExifIfdTag {
  ExifIfdTag pointer{.tag_ = tag, .type_ = kTiffLongType, .count_ = 1, .value_ = {}};
  AppendU32LE(pointer.value_, static_cast<uint32_t>(offset));
  return pointer;
}

}  // namespace

auto ExifTiffTypeSize(uint16_t type) -> uint32_t {
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
      return 4;
    case 5:   // RATIONAL
    case 10:  // SRATIONAL
    case 12:  // DOUBLE
      return 8;
    default:
      return 0;
  }
}

auto IsWritableExifIfdTag(const ExifIfdTag& tag) -> bool {
  const uint32_t type_size = ExifTiffTypeSize(tag.type_);
  return type_size != 0 && tag.count_ > 0 &&
         static_cast<uint64_t>(tag.count_) * type_size == tag.value_.size();
}

auto IsExifImageStructureTag(uint16_t tag) -> bool {
  switch (tag) {
    case 0x00FE:  // NewSubfileType
    case 0x00FF:  // SubfileType
    case 0x0100:  // ImageWidth
    case 0x0101:  // ImageLength
    case 0x0102:  // BitsPerSample
    case 0x0103:  // Compression
    case 0x0106:  // PhotometricInterpretation
    case 0x0107:  // Threshholding
    case 0x0108:  // CellWidth
    case 0x0109:  // CellLength
    case 0x010A:  // FillOrder
    case 0x0111:  // StripOffsets
    case 0x0115:  // SamplesPerPixel
    case 0x0116:  // RowsPerStrip
    case 0x0117:  // StripByteCounts
    case 0x0118:  // MinSampleValue
    case 0x0119:  // MaxSampleValue
    case 0x011A:  // XResolution
    case 0x011B:  // YResolution
    case 0x011C:  // PlanarConfiguration
    case 0x011E:  // XPosition
    case 0x011F:  // YPosition
    case 0x0120:  // FreeOffsets
    case 0x0121:  // FreeByteCounts
    case 0x0122:  // GrayResponseUnit
    case 0x0123:  // GrayResponseCurve
    case 0x0124:  // T4Options
    case 0x0125:  // T6Options
    case 0x0128:  // ResolutionUnit
    case 0x0129:  // PageNumber
    case 0x012D:  // TransferFunction
    case 0x013D:  // Predictor
    case 0x013E:  // WhitePoint
    case 0x013F:  // PrimaryChromaticities
    case 0x8769:  // EXIF IFD pointer
    case 0x8773:  // ICC profile
    case 0x8825:  // GPS IFD pointer
    case 0xA005:  // Interoperability IFD pointer
      return true;
    default:
      break;
  }
  // ColorMap, halftone, tile layout, SubIFDs, ink, extra samples, sample format, JPEG tables,
  // and the JPEG and YCbCr layout fields (0x0140-0x015F, 0x0200-0x0214).
  return (tag >= 0x0140 && tag <= 0x015F) || (tag >= 0x0200 && tag <= 0x0214);
}

auto ReadExifIfdEntries(const std::filesystem::path& image_path) -> ExifIfdEntries {
  // Read the bytes through std::filesystem so that paths outside the ANSI code page open on
  // Windows; Exiv2 opens file names as narrow strings.
  const std::vector<uint8_t> bytes = ReadFileBytes(image_path, "source");

  ExifIfdEntries             entries;
  try {
    auto image =
        Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte*>(bytes.data()), bytes.size());
    image->readMetadata();
    for (const auto& datum : image->exifData()) {
      std::vector<ExifIfdTag>* target = nullptr;
      switch (datum.ifdId()) {
        case Exiv2::IfdId::ifd0Id:
          target = &entries.ifd0_;
          break;
        case Exiv2::IfdId::exifId:
          target = &entries.exif_;
          break;
        case Exiv2::IfdId::gpsId:
          target = &entries.gps_;
          break;
        default:
          continue;
      }
      const uint16_t tag_number = datum.tag();
      if (tag_number == kExifIfdTag || tag_number == kGpsIfdTag || tag_number == kInteropIfdTag ||
          tag_number == kSubIfdsTag) {
        continue;  // IFD pointers: their offsets belong to the source file.
      }
      const auto     type      = static_cast<uint16_t>(datum.typeId());
      const uint32_t type_size = ExifTiffTypeSize(type);
      const size_t   size      = datum.size();
      if (type_size == 0 || size == 0 || size % type_size != 0) {
        continue;
      }
      ExifIfdTag tag;
      tag.tag_  = tag_number;
      tag.type_ = type;
      tag.value_.resize(size);
      datum.copy(tag.value_.data(), Exiv2::littleEndian);
      tag.count_ = static_cast<uint32_t>(size / type_size);
      target->push_back(std::move(tag));
    }
  } catch (const Exiv2::Error& error) {
    // Exiv2 reports an unknown container (for example CR3 in a build without BMFF support) with
    // either code; such a file has no EXIF entries that Exiv2 can read.
    if (error.code() == Exiv2::ErrorCode::kerFileContainsUnknownImageType ||
        error.code() == Exiv2::ErrorCode::kerMemoryContainsUnknownImageType) {
      return {};
    }
    throw std::runtime_error(std::string("cannot read the EXIF metadata of the source file: ") +
                             error.what());
  }
  entries.ifd0_ = SortedByTag(std::move(entries.ifd0_));
  entries.exif_ = SortedByTag(std::move(entries.exif_));
  entries.gps_  = SortedByTag(std::move(entries.gps_));
  return entries;
}

auto ReadExifGpsTags(const std::filesystem::path& image_path) -> std::vector<ExifIfdTag> {
  return ReadExifIfdEntries(image_path).gps_;
}

auto SelectExportExifIfdEntries(const ExifIfdEntries& source, bool include_location,
                                bool include_device_serials) -> ExifIfdEntries {
  ExifIfdEntries selected;
  for (const auto& tag : source.ifd0_) {
    if (IsWritableExifIfdTag(tag) && !IsExcludedSourceIfd0Tag(tag.tag_)) {
      selected.ifd0_.push_back(tag);
    }
  }
  for (const auto& tag : source.exif_) {
    if (IsWritableExifIfdTag(tag) && !IsExcludedSourceExifTag(tag.tag_, include_device_serials)) {
      selected.exif_.push_back(tag);
    }
  }
  if (include_location) {
    for (const auto& tag : source.gps_) {
      if (IsWritableExifIfdTag(tag)) selected.gps_.push_back(tag);
    }
  }
  return selected;
}

void WriteTiffExifIfdEntries(const std::filesystem::path& tiff_path,
                             const ExifIfdEntries&        entries) {
  std::vector<ExifIfdTag> ifd0_tags;
  for (const auto& tag : WritableTags(entries.ifd0_)) {
    if (!IsExifImageStructureTag(tag.tag_)) ifd0_tags.push_back(tag);
  }
  const std::vector<ExifIfdTag> exif_tags = WritableTags(entries.exif_);
  const std::vector<ExifIfdTag> gps_tags  = WritableTags(entries.gps_);
  if (ifd0_tags.empty() && exif_tags.empty() && gps_tags.empty()) {
    return;
  }

  std::fstream file(tiff_path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file.is_open()) {
    throw std::runtime_error("cannot open the exported TIFF file to write its EXIF metadata");
  }
  const auto read_exact = [&file](std::streamoff offset, uint8_t* out, size_t size) {
    file.seekg(offset);
    file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    if (!file) {
      throw std::runtime_error("the exported TIFF file ends inside one of its IFDs");
    }
  };
  // The entries of the IFD at @p ifd_offset; @p next_ifd_offset receives its next-IFD offset.
  const auto read_ifd = [&read_exact](uint32_t ifd_offset, uint32_t& next_ifd_offset) {
    std::array<uint8_t, 2> count_bytes{};
    read_exact(ifd_offset, count_bytes.data(), count_bytes.size());
    const uint16_t       entry_count = ReadU16LE(count_bytes.data());
    std::vector<uint8_t> ifd(static_cast<size_t>(entry_count) * 12u + 4u);
    read_exact(static_cast<std::streamoff>(ifd_offset) + 2, ifd.data(), ifd.size());
    next_ifd_offset = ReadU32LE(ifd.data() + static_cast<size_t>(entry_count) * 12u);
    std::vector<TiffIfdEntryBytes> ifd_entries(entry_count);
    for (uint16_t i = 0; i < entry_count; ++i) {
      std::copy_n(ifd.begin() + static_cast<std::ptrdiff_t>(i) * 12, 12,
                  ifd_entries[i].bytes_.begin());
      ifd_entries[i].tag_ = ReadU16LE(ifd_entries[i].bytes_.data());
    }
    return ifd_entries;
  };
  const auto pointer_of = [](const std::vector<TiffIfdEntryBytes>& ifd_entries, uint16_t tag) {
    for (const auto& entry : ifd_entries) {
      if (entry.tag_ == tag) return ReadU32LE(entry.bytes_.data() + 8);
    }
    return uint32_t{0};
  };

  std::array<uint8_t, 8> header{};
  read_exact(0, header.data(), header.size());
  if (header[0] != 'I' || header[1] != 'I' || ReadU16LE(header.data() + 2) != 42) {
    throw std::runtime_error(
        "the exported file is not a little-endian classic TIFF; its EXIF metadata was not "
        "written");
  }
  uint32_t                             next_ifd_offset = 0;
  const std::vector<TiffIfdEntryBytes> ifd0 =
      read_ifd(ReadU32LE(header.data() + 4), next_ifd_offset);
  uint32_t                       ignored_next_offset = 0;
  std::vector<TiffIfdEntryBytes> old_exif;
  if (const uint32_t offset = pointer_of(ifd0, kExifIfdTag); offset != 0) {
    old_exif = read_ifd(offset, ignored_next_offset);
  }
  std::vector<TiffIfdEntryBytes> old_gps;
  if (const uint32_t offset = pointer_of(ifd0, kGpsIfdTag); offset != 0) {
    old_gps = read_ifd(offset, ignored_next_offset);
  }

  file.seekp(0, std::ios::end);
  const uint64_t       end_offset = static_cast<uint64_t>(file.tellp());
  std::vector<uint8_t> appended;
  if (end_offset % 2 != 0) appended.push_back(0);
  const uint64_t appended_offset = end_offset;

  // The new EXIF and GPS IFDs keep the entries that OpenImageIO wrote unless @p entries replaces
  // them; the new IFD0 points to them.
  if (!exif_tags.empty()) {
    const auto exif_entries = MergeIfdEntries(appended, appended_offset, old_exif, exif_tags);
    ifd0_tags.push_back(
        MakeIfdPointerTag(kExifIfdTag, AppendIfd(appended, appended_offset, exif_entries, 0)));
  }
  if (!gps_tags.empty()) {
    const auto gps_entries = MergeIfdEntries(appended, appended_offset, old_gps, gps_tags);
    ifd0_tags.push_back(
        MakeIfdPointerTag(kGpsIfdTag, AppendIfd(appended, appended_offset, gps_entries, 0)));
  }
  const auto     ifd0_entries = MergeIfdEntries(appended, appended_offset, ifd0, ifd0_tags);
  const uint64_t new_ifd0_offset =
      AppendIfd(appended, appended_offset, ifd0_entries, next_ifd_offset);

  if (end_offset + appended.size() > 0xFFFFFFFFull) {
    throw std::runtime_error("the exported TIFF file is too large to add its EXIF metadata");
  }
  file.seekp(static_cast<std::streamoff>(end_offset));
  file.write(reinterpret_cast<const char*>(appended.data()),
             static_cast<std::streamsize>(appended.size()));
  std::vector<uint8_t> header_offset;
  AppendU32LE(header_offset, static_cast<uint32_t>(new_ifd0_offset));
  file.seekp(4);
  file.write(reinterpret_cast<const char*>(header_offset.data()), 4);
  file.flush();
  if (!file) {
    throw std::runtime_error("cannot write the EXIF metadata into the exported TIFF file");
  }
}

void WritePngExifChunk(const std::filesystem::path& png_path,
                       const std::vector<uint8_t>&  exif_tiff_block) {
  if (exif_tiff_block.empty()) {
    return;
  }
  const std::vector<uint8_t>       bytes         = ReadFileBytes(png_path, "exported PNG");
  constexpr std::array<uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (bytes.size() < kPngSignature.size() ||
      !std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin())) {
    throw std::runtime_error(
        "the exported file is not a PNG file; its EXIF metadata was not written");
  }
  if (exif_tiff_block.size() > 0x7FFFFFFFu) {
    throw std::runtime_error("the EXIF metadata is too large for a PNG chunk");
  }

  std::vector<uint8_t> exif_chunk;
  AppendU32BE(exif_chunk, static_cast<uint32_t>(exif_tiff_block.size()));
  exif_chunk.insert(exif_chunk.end(), {'e', 'X', 'I', 'f'});
  exif_chunk.insert(exif_chunk.end(), exif_tiff_block.begin(), exif_tiff_block.end());
  AppendU32BE(exif_chunk, PngCrc32(exif_chunk.data() + 4, exif_chunk.size() - 4));

  std::vector<uint8_t> out(kPngSignature.begin(), kPngSignature.end());
  bool                 exif_written = false;
  for (size_t pos = kPngSignature.size(); pos < bytes.size();) {
    if (pos + 12 > bytes.size()) {
      throw std::runtime_error("the exported PNG file ends inside a chunk");
    }
    const uint64_t chunk_size = 12ull + ReadU32BE(bytes.data() + pos);
    if (pos + chunk_size > bytes.size()) {
      throw std::runtime_error("the exported PNG file ends inside a chunk");
    }
    const std::string type(bytes.begin() + static_cast<std::ptrdiff_t>(pos) + 4,
                           bytes.begin() + static_cast<std::ptrdiff_t>(pos) + 8);
    if (type == "IDAT" && !exif_written) {
      out.insert(out.end(), exif_chunk.begin(), exif_chunk.end());
      exif_written = true;
    }
    if (type != "eXIf") {
      out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                 bytes.begin() + static_cast<std::ptrdiff_t>(pos + chunk_size));
    }
    pos += chunk_size;
  }
  if (!exif_written) {
    throw std::runtime_error("the exported PNG file has no image data chunk");
  }

  std::ofstream output(png_path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!output) {
    throw std::runtime_error("cannot write the EXIF metadata into the exported PNG file");
  }
}

}  // namespace alcedo
