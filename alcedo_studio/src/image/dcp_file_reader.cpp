// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dcp_file_reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "image/dng_camera_matrix.hpp"
#include "image/dng_color_profile_import.hpp"
#include "image/dng_tone_curve.hpp"

namespace alcedo {
namespace {

// DNG 1.6 tag numbers that the reader uses or rejects.
namespace tag {
constexpr std::uint16_t kUniqueCameraModel           = 50708;
constexpr std::uint16_t kColorMatrix1                = 50721;
constexpr std::uint16_t kColorMatrix2                = 50722;
constexpr std::uint16_t kReductionMatrix1            = 50725;
constexpr std::uint16_t kReductionMatrix2            = 50726;
constexpr std::uint16_t kCalibrationIlluminant1      = 50778;
constexpr std::uint16_t kCalibrationIlluminant2      = 50779;
constexpr std::uint16_t kProfileCalibrationSignature = 50932;
constexpr std::uint16_t kProfileName                 = 50936;
constexpr std::uint16_t kProfileHueSatMapDims        = 50937;
constexpr std::uint16_t kProfileHueSatMapData1       = 50938;
constexpr std::uint16_t kProfileHueSatMapData2       = 50939;
constexpr std::uint16_t kProfileToneCurve            = 50940;
constexpr std::uint16_t kForwardMatrix1              = 50964;
constexpr std::uint16_t kForwardMatrix2              = 50965;
constexpr std::uint16_t kProfileLookTableDims        = 50981;
constexpr std::uint16_t kProfileLookTableData        = 50982;
constexpr std::uint16_t kProfileHueSatMapEncoding    = 51107;
constexpr std::uint16_t kProfileLookTableEncoding    = 51108;
constexpr std::uint16_t kBaselineExposureOffset      = 51109;
constexpr std::uint16_t kCalibrationIlluminant3      = 52529;
constexpr std::uint16_t kColorMatrix3                = 52531;
constexpr std::uint16_t kForwardMatrix3              = 52532;
constexpr std::uint16_t kProfileHueSatMapData3       = 52537;
constexpr std::uint16_t kReductionMatrix3            = 52538;
constexpr std::uint16_t kRgbTables                   = 52543;
}  // namespace tag

/// Tags that the render path cannot apply. A profile that holds one fails to load (P-7).
constexpr std::array<std::pair<std::uint16_t, std::string_view>, 8> kUnsupportedTags{{
    {tag::kReductionMatrix1, "ReductionMatrix1"},
    {tag::kReductionMatrix2, "ReductionMatrix2"},
    {tag::kCalibrationIlluminant3, "CalibrationIlluminant3"},
    {tag::kColorMatrix3, "ColorMatrix3"},
    {tag::kForwardMatrix3, "ForwardMatrix3"},
    {tag::kProfileHueSatMapData3, "ProfileHueSatMapData3"},
    {tag::kReductionMatrix3, "ReductionMatrix3"},
    {tag::kRgbTables, "RGBTables"},
}};

// TIFF field types (TIFF 6.0 section 2).
enum class TiffType : std::uint16_t {
  kByte      = 1,
  kAscii     = 2,
  kShort     = 3,
  kLong      = 4,
  kSRational = 10,
  kFloat     = 11,
};

constexpr std::uint16_t kDcpMagic  = 0x4352;  // "RC" in little-endian order.
constexpr std::uint16_t kTiffMagic = 42;

struct IfdEntry {
  std::uint16_t               tag   = 0;
  std::uint16_t               type  = 0;
  std::uint32_t               count = 0;
  std::array<std::uint8_t, 4> value_field{};  // Inline payload or payload offset.
};

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

auto ElementSize(TiffType type) -> std::size_t {
  switch (type) {
    case TiffType::kByte:
    case TiffType::kAscii:
      return 1;
    case TiffType::kShort:
      return 2;
    case TiffType::kLong:
    case TiffType::kFloat:
      return 4;
    case TiffType::kSRational:
      return 8;
  }
  return 0;
}

auto TypeName(TiffType type) -> std::string_view {
  switch (type) {
    case TiffType::kByte:
      return "BYTE";
    case TiffType::kAscii:
      return "ASCII";
    case TiffType::kShort:
      return "SHORT";
    case TiffType::kLong:
      return "LONG";
    case TiffType::kSRational:
      return "SRATIONAL";
    case TiffType::kFloat:
      return "FLOAT";
  }
  return "?";
}

/**
 * Reader of the first IFD of a DCP file. It reads the header and the entry table on construction
 * and one payload for each request, so an identity read touches only the string payloads.
 * Every error names the file, and the tag when one is involved.
 */
class DcpIfdReader {
 public:
  explicit DcpIfdReader(const std::filesystem::path& path) : path_(path) {
    std::error_code error;
    file_size_ = std::filesystem::file_size(path, error);
    if (error) Fail("cannot read the file size: " + error.message());
    if (file_size_ > kMaxDcpFileBytes) {
      Fail("the file has " + std::to_string(file_size_) + " bytes, above the limit of " +
           std::to_string(kMaxDcpFileBytes));
    }
    file_.open(path, std::ios::binary);
    if (!file_) Fail("cannot open the file");

    std::array<std::uint8_t, 8> header{};
    if (file_size_ < header.size()) Fail("the file is shorter than a TIFF header");
    ReadBytes(0, header.data(), header.size(), "the TIFF header");
    if (header[0] == 'I' && header[1] == 'I') {
      big_endian_ = false;
    } else if (header[0] == 'M' && header[1] == 'M') {
      big_endian_ = true;
    } else {
      Fail("the byte order mark is not II or MM");
    }
    const std::uint16_t magic = U16(header.data() + 2);
    if (magic != kDcpMagic && magic != kTiffMagic) {
      Fail("the magic number " + std::to_string(magic) + " is not 0x4352 or 42");
    }

    const std::uint64_t         ifd_offset = U32(header.data() + 4);
    std::array<std::uint8_t, 2> count_bytes{};
    if (ifd_offset + count_bytes.size() > file_size_) Fail("the IFD is outside the file");
    ReadBytes(ifd_offset, count_bytes.data(), count_bytes.size(), "the IFD entry count");
    const std::uint16_t entry_count = U16(count_bytes.data());
    const std::uint64_t table_size  = static_cast<std::uint64_t>(entry_count) * 12;
    if (ifd_offset + 2 + table_size > file_size_) {
      Fail("the IFD is truncated: " + std::to_string(entry_count) + " entries do not fit");
    }
    std::vector<std::uint8_t> table(static_cast<std::size_t>(table_size));
    ReadBytes(ifd_offset + 2, table.data(), table.size(), "the IFD entries");
    entries_.reserve(entry_count);
    for (std::size_t i = 0; i < entry_count; ++i) {
      const std::uint8_t* raw = table.data() + i * 12;
      IfdEntry            entry;
      entry.tag   = U16(raw);
      entry.type  = U16(raw + 2);
      entry.count = U32(raw + 4);
      std::copy(raw + 8, raw + 12, entry.value_field.begin());
      if (Find(entry.tag) != nullptr) Fail(TagText(entry.tag) + " appears more than once");
      entries_.push_back(entry);
    }
  }

  [[nodiscard]] auto Entries() const -> const std::vector<IfdEntry>& { return entries_; }

  [[nodiscard]] auto Find(std::uint16_t tag_number) const -> const IfdEntry* {
    for (const auto& entry : entries_) {
      if (entry.tag == tag_number) return &entry;
    }
    return nullptr;
  }

  /// String payload of an ASCII or BYTE (UTF-8) tag, without trailing NUL characters.
  [[nodiscard]] auto ReadString(const IfdEntry& entry) -> std::string {
    const auto type = static_cast<TiffType>(entry.type);
    if (type != TiffType::kAscii && type != TiffType::kByte) FailType(entry, "ASCII or BYTE");
    const auto  bytes = Payload(entry, type);
    std::string text(bytes.begin(), bytes.end());
    while (!text.empty() && text.back() == '\0') text.pop_back();
    if (text.find('\0') != std::string::npos) Fail(TagText(entry.tag) + " holds a NUL character");
    return text;
  }

  [[nodiscard]] auto ReadSRationals(const IfdEntry& entry) -> std::vector<double> {
    const auto          bytes = Payload(entry, RequireType(entry, TiffType::kSRational));
    std::vector<double> values(entry.count);
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto numerator   = static_cast<std::int32_t>(U32(bytes.data() + i * 8));
      const auto denominator = static_cast<std::int32_t>(U32(bytes.data() + i * 8 + 4));
      if (denominator == 0) Fail(TagText(entry.tag) + " has a zero denominator");
      values[i] = static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    return values;
  }

  [[nodiscard]] auto ReadFloats(const IfdEntry& entry) -> std::vector<float> {
    const auto         bytes = Payload(entry, RequireType(entry, TiffType::kFloat));
    std::vector<float> values(entry.count);
    for (std::size_t i = 0; i < values.size(); ++i) {
      values[i] = std::bit_cast<float>(U32(bytes.data() + i * 4));
    }
    return values;
  }

  [[nodiscard]] auto ReadLongs(const IfdEntry& entry) -> std::vector<std::uint32_t> {
    const auto                 bytes = Payload(entry, RequireType(entry, TiffType::kLong));
    std::vector<std::uint32_t> values(entry.count);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = U32(bytes.data() + i * 4);
    return values;
  }

  [[nodiscard]] auto ReadShorts(const IfdEntry& entry) -> std::vector<std::uint16_t> {
    const auto                 bytes = Payload(entry, RequireType(entry, TiffType::kShort));
    std::vector<std::uint16_t> values(entry.count);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = U16(bytes.data() + i * 2);
    return values;
  }

  [[noreturn]] void Fail(const std::string& reason) const {
    throw std::runtime_error("DCP " + PathText(path_) + ": " + reason);
  }

  void RequireCount(const IfdEntry& entry, std::uint32_t count) const {
    if (entry.count != count) {
      Fail(TagText(entry.tag) + " has " + std::to_string(entry.count) + " values, expected " +
           std::to_string(count));
    }
  }

  static auto TagText(std::uint16_t tag_number) -> std::string {
    return "tag " + std::to_string(tag_number);
  }

 private:
  auto RequireType(const IfdEntry& entry, TiffType type) const -> TiffType {
    if (entry.type != static_cast<std::uint16_t>(type)) FailType(entry, TypeName(type));
    return type;
  }

  [[noreturn]] void FailType(const IfdEntry& entry, std::string_view expected) const {
    Fail(TagText(entry.tag) + " has TIFF type " + std::to_string(entry.type) + ", expected " +
         std::string(expected));
  }

  auto Payload(const IfdEntry& entry, TiffType type) -> std::vector<std::uint8_t> {
    const std::uint64_t size = static_cast<std::uint64_t>(entry.count) * ElementSize(type);
    if (size <= entry.value_field.size()) {
      return {entry.value_field.begin(),
              entry.value_field.begin() + static_cast<std::ptrdiff_t>(size)};
    }
    const std::uint64_t offset = U32(entry.value_field.data());
    if (offset + size > file_size_) Fail(TagText(entry.tag) + " data is outside the file");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    ReadBytes(offset, bytes.data(), bytes.size(), TagText(entry.tag) + " data");
    return bytes;
  }

  void ReadBytes(std::uint64_t offset, std::uint8_t* out, std::size_t size,
                 const std::string& what) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    if (!file_ || file_.gcount() != static_cast<std::streamsize>(size)) {
      Fail("cannot read " + what);
    }
  }

  [[nodiscard]] auto U16(const std::uint8_t* bytes) const -> std::uint16_t {
    return big_endian_ ? static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1])
                       : static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
  }

  [[nodiscard]] auto U32(const std::uint8_t* bytes) const -> std::uint32_t {
    const std::uint32_t b0 = bytes[0], b1 = bytes[1], b2 = bytes[2], b3 = bytes[3];
    return big_endian_ ? (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
                       : b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
  }

  std::filesystem::path path_;
  std::ifstream         file_;
  std::uintmax_t        file_size_  = 0;
  bool                  big_endian_ = false;
  std::vector<IfdEntry> entries_;
};

auto ReadRequiredString(DcpIfdReader& reader, std::uint16_t tag_number, std::string_view name)
    -> std::string {
  const auto* entry = reader.Find(tag_number);
  if (entry == nullptr) {
    reader.Fail(DcpIfdReader::TagText(tag_number) + " (" + std::string(name) + ") is missing");
  }
  auto text = reader.ReadString(*entry);
  if (text.empty()) {
    reader.Fail(DcpIfdReader::TagText(tag_number) + " (" + std::string(name) + ") is empty");
  }
  return text;
}

auto ReadMatrix(DcpIfdReader& reader, std::uint16_t tag_number)
    -> std::optional<std::array<double, 9>> {
  const auto* entry = reader.Find(tag_number);
  if (entry == nullptr) return std::nullopt;
  reader.RequireCount(*entry, 9);
  const auto            values = reader.ReadSRationals(*entry);
  std::array<double, 9> matrix{};
  std::copy(values.begin(), values.end(), matrix.begin());
  return matrix;
}

/// CCT of a calibration illuminant tag. An absent tag is 0 (Unknown), which has no CCT.
auto ReadIlluminantCct(DcpIfdReader& reader, std::uint16_t tag_number) -> double {
  std::uint32_t illuminant = 0;
  if (const auto* entry = reader.Find(tag_number)) {
    reader.RequireCount(*entry, 1);
    illuminant = reader.ReadShorts(*entry).front();
  }
  const double cct = CalibrationIlluminantToCct(illuminant);
  if (!std::isfinite(cct)) {
    reader.Fail(DcpIfdReader::TagText(tag_number) + ": illuminant " + std::to_string(illuminant) +
                " has no color temperature");
  }
  return cct;
}

auto ReadUint(DcpIfdReader& reader, std::uint16_t tag_number, std::uint32_t absent)
    -> std::uint32_t {
  const auto* entry = reader.Find(tag_number);
  if (entry == nullptr) return absent;
  reader.RequireCount(*entry, 1);
  return reader.ReadLongs(*entry).front();
}

/// One table from its dimensions, data, and encoding tags. An absent data tag gives an empty map.
auto ReadMap(DcpIfdReader& reader, std::uint16_t dims_tag, std::uint16_t data_tag,
             std::uint16_t encoding_tag) -> DngHueSatMap {
  const auto* data = reader.Find(data_tag);
  if (data == nullptr) return {};
  const auto* dims = reader.Find(dims_tag);
  if (dims == nullptr) reader.Fail(DcpIfdReader::TagText(dims_tag) + " is missing");
  reader.RequireCount(*dims, 3);
  const auto                   dim_values = reader.ReadLongs(*dims);
  std::array<std::uint32_t, 3> divisions{dim_values[0], dim_values[1], dim_values[2]};
  const std::uint32_t          encoding = ReadUint(reader, encoding_tag, 0);
  if (data->count > kMaxDngTableFloats) {
    reader.Fail(DcpIfdReader::TagText(data_tag) + " has more than " +
                std::to_string(kMaxDngTableFloats) + " values");
  }
  const auto entries = reader.ReadFloats(*data);
  try {
    return MakeDngHueSatMap(divisions, encoding, entries);
  } catch (const std::runtime_error& error) {
    reader.Fail(DcpIfdReader::TagText(data_tag) + ": " + error.what());
  }
}

/// `ProfileToneCurve` as (x, y) FLOAT pairs. An absent tag gives null.
auto ReadToneCurve(DcpIfdReader& reader) -> DngToneCurvePtr {
  const auto* entry = reader.Find(tag::kProfileToneCurve);
  if (entry == nullptr) return nullptr;
  const std::string name = DcpIfdReader::TagText(tag::kProfileToneCurve) + " (ProfileToneCurve)";
  if (entry->count % 2 != 0) {
    reader.Fail(name + " has " + std::to_string(entry->count) + " values, expected (x, y) pairs");
  }
  if (entry->count > 2 * kMaxDngToneCurvePoints) {
    reader.Fail(name + " has more than " + std::to_string(kMaxDngToneCurvePoints) + " points");
  }
  const auto                     values = reader.ReadFloats(*entry);
  std::vector<DngToneCurvePoint> points(values.size() / 2);
  for (std::size_t i = 0; i < points.size(); ++i) points[i] = {values[2 * i], values[2 * i + 1]};
  try {
    return MakeDngToneCurve(std::move(points));
  } catch (const std::runtime_error& error) {
    reader.Fail(name + ": " + error.what());
  }
}

}  // namespace

auto ReadDcpProfileIdentity(const std::filesystem::path& path) -> DcpProfileIdentity {
  DcpIfdReader reader(path);
  return {ReadRequiredString(reader, tag::kUniqueCameraModel, "UniqueCameraModel"),
          ReadRequiredString(reader, tag::kProfileName, "ProfileName")};
}

auto ReadDcpCameraProfile(const std::filesystem::path& path) -> DcpCameraProfilePtr {
  DcpIfdReader reader(path);
  for (const auto& [tag_number, name] : kUnsupportedTags) {
    if (reader.Find(tag_number) != nullptr) {
      reader.Fail(DcpIfdReader::TagText(tag_number) + " (" + std::string(name) +
                  ") is not supported: the render path has two illuminants and three channels");
    }
  }

  DcpCameraProfile profile;
  profile.unique_camera_model =
      ReadRequiredString(reader, tag::kUniqueCameraModel, "UniqueCameraModel");
  profile.profile_name      = ReadRequiredString(reader, tag::kProfileName, "ProfileName");

  const auto color_matrix_1 = ReadMatrix(reader, tag::kColorMatrix1);
  if (!color_matrix_1) {
    reader.Fail(DcpIfdReader::TagText(tag::kColorMatrix1) + " (ColorMatrix1) is missing");
  }
  const auto color_matrix_2  = ReadMatrix(reader, tag::kColorMatrix2);
  profile.color_matrix_1     = *color_matrix_1;
  profile.color_matrix_1_cct = ReadIlluminantCct(reader, tag::kCalibrationIlluminant1);
  if (color_matrix_2) {
    profile.color_matrix_2     = *color_matrix_2;
    profile.color_matrix_2_cct = ReadIlluminantCct(reader, tag::kCalibrationIlluminant2);
  } else {
    // One ColorMatrix fills both slots, as the embedded DNG import does.
    profile.color_matrix_2     = profile.color_matrix_1;
    profile.color_matrix_2_cct = profile.color_matrix_1_cct;
  }

  const auto forward_matrix_1 = ReadMatrix(reader, tag::kForwardMatrix1);
  const auto forward_matrix_2 = ReadMatrix(reader, tag::kForwardMatrix2);
  if (forward_matrix_1 || forward_matrix_2) {
    profile.forward_matrices_valid = true;
    profile.forward_matrix_1       = forward_matrix_1 ? *forward_matrix_1 : *forward_matrix_2;
    profile.forward_matrix_2       = forward_matrix_2 ? *forward_matrix_2 : *forward_matrix_1;
  }

  if (const auto* signature = reader.Find(tag::kProfileCalibrationSignature)) {
    profile.profile_calibration_signature = reader.ReadString(*signature);
  }

  DngColorProfile tables;
  tables.name          = profile.profile_name;
  tables.hue_sat_map_1 = ReadMap(reader, tag::kProfileHueSatMapDims, tag::kProfileHueSatMapData1,
                                 tag::kProfileHueSatMapEncoding);
  tables.hue_sat_map_2 = ReadMap(reader, tag::kProfileHueSatMapDims, tag::kProfileHueSatMapData2,
                                 tag::kProfileHueSatMapEncoding);
  tables.look_table    = ReadMap(reader, tag::kProfileLookTableDims, tag::kProfileLookTableData,
                                 tag::kProfileLookTableEncoding);
  if (const auto* offset = reader.Find(tag::kBaselineExposureOffset)) {
    reader.RequireCount(*offset, 1);
    tables.baseline_exposure_offset = reader.ReadSRationals(*offset).front();
  }
  profile.profile_tone_curve = ReadToneCurve(reader);

  try {
    profile.tables = MakeDngColorProfile(std::move(tables));
    return MakeDcpCameraProfile(std::move(profile));
  } catch (const std::runtime_error& error) {
    reader.Fail(error.what());
  }
}

}  // namespace alcedo
