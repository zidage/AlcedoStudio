// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace alcedo::test {

/**
 * @brief Writes synthetic DCP files (TIFF container, one IFD) for reader tests.
 *
 * Each tag holds its TIFF type and its values. @ref Bytes lays out the header, the IFD at offset
 * 8, and the payloads after the IFD, in the requested byte order. Tests use it so that no vendor
 * profile bytes enter the repository.
 */
class DcpTestFileWriter {
 public:
  enum class ByteOrder { kLittleEndian, kBigEndian };
  enum class EntryOrder { kAscending, kDescending };

  static constexpr std::uint16_t kAscii     = 2;
  static constexpr std::uint16_t kShort     = 3;
  static constexpr std::uint16_t kLong      = 4;
  static constexpr std::uint16_t kSRational = 10;
  static constexpr std::uint16_t kFloat     = 11;

  /// ASCII tag. The writer appends the terminating NUL.
  auto Ascii(std::uint16_t tag, const std::string& text) -> DcpTestFileWriter& {
    Field field{kAscii, {}};
    for (const char c : text) field.units.push_back(static_cast<std::uint8_t>(c));
    field.units.push_back(0);
    fields_[tag] = std::move(field);
    return *this;
  }

  /// SRATIONAL tag from numerator and denominator pairs.
  auto SRationalPairs(std::uint16_t                                             tag,
                      const std::vector<std::pair<std::int32_t, std::int32_t>>& values)
      -> DcpTestFileWriter& {
    Field field{kSRational, {}};
    for (const auto& [numerator, denominator] : values) {
      field.units.push_back(static_cast<std::uint32_t>(numerator));
      field.units.push_back(static_cast<std::uint32_t>(denominator));
    }
    fields_[tag] = std::move(field);
    return *this;
  }

  /// SRATIONAL tag with denominator 10000. Each value must be a multiple of 1/10000.
  auto SRationals(std::uint16_t tag, const std::vector<double>& values) -> DcpTestFileWriter& {
    std::vector<std::pair<std::int32_t, std::int32_t>> pairs;
    for (const double value : values) {
      pairs.emplace_back(static_cast<std::int32_t>(std::lround(value * 10000.0)), 10000);
    }
    return SRationalPairs(tag, pairs);
  }

  auto Floats(std::uint16_t tag, const std::vector<float>& values) -> DcpTestFileWriter& {
    Field field{kFloat, {}};
    for (const float value : values) field.units.push_back(std::bit_cast<std::uint32_t>(value));
    fields_[tag] = std::move(field);
    return *this;
  }

  auto Longs(std::uint16_t tag, const std::vector<std::uint32_t>& values) -> DcpTestFileWriter& {
    fields_[tag] = Field{kLong, values};
    return *this;
  }

  auto Shorts(std::uint16_t tag, const std::vector<std::uint16_t>& values) -> DcpTestFileWriter& {
    Field field{kShort, {}};
    for (const auto value : values) field.units.push_back(value);
    fields_[tag] = std::move(field);
    return *this;
  }

  auto Remove(std::uint16_t tag) -> DcpTestFileWriter& {
    fields_.erase(tag);
    return *this;
  }

  /// Writes @p offset into the entry of @p tag instead of the real payload offset.
  auto PayloadOffsetOverride(std::uint16_t tag, std::uint32_t offset) -> DcpTestFileWriter& {
    offset_overrides_[tag] = offset;
    return *this;
  }

  auto Magic(std::uint16_t magic) -> DcpTestFileWriter& {
    magic_ = magic;
    return *this;
  }

  [[nodiscard]] auto Bytes(ByteOrder  order       = ByteOrder::kLittleEndian,
                           EntryOrder entry_order = EntryOrder::kAscending) const
      -> std::vector<std::uint8_t> {
    const bool                                          big = order == ByteOrder::kBigEndian;

    std::vector<std::pair<std::uint16_t, const Field*>> entries;
    for (const auto& [tag, field] : fields_) entries.emplace_back(tag, &field);
    if (entry_order == EntryOrder::kDescending) std::reverse(entries.begin(), entries.end());

    // Payloads follow the header (8), the entry count (2), the entries, and the next IFD offset.
    const std::uint32_t payload_start = 8 + 2 + static_cast<std::uint32_t>(entries.size()) * 12 + 4;
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> payload_area;
    out.push_back(big ? 'M' : 'I');
    out.push_back(big ? 'M' : 'I');
    Put16(out, magic_, big);
    Put32(out, 8, big);
    Put16(out, static_cast<std::uint16_t>(entries.size()), big);
    for (const auto& [tag, field] : entries) {
      std::vector<std::uint8_t> bytes;
      for (const auto unit : field->units) {
        switch (UnitSize(field->type)) {
          case 1:
            bytes.push_back(static_cast<std::uint8_t>(unit));
            break;
          case 2:
            Put16(bytes, static_cast<std::uint16_t>(unit), big);
            break;
          default:
            Put32(bytes, unit, big);
            break;
        }
      }
      Put16(out, tag, big);
      Put16(out, field->type, big);
      Put32(out, static_cast<std::uint32_t>(field->units.size() / UnitsPerValue(field->type)), big);
      const auto override_it = offset_overrides_.find(tag);
      if (bytes.size() <= 4 && override_it == offset_overrides_.end()) {
        bytes.resize(4, 0);
        out.insert(out.end(), bytes.begin(), bytes.end());
      } else {
        Put32(out,
              override_it != offset_overrides_.end()
                  ? override_it->second
                  : payload_start + static_cast<std::uint32_t>(payload_area.size()),
              big);
        payload_area.insert(payload_area.end(), bytes.begin(), bytes.end());
        if (payload_area.size() % 2 != 0) payload_area.push_back(0);
      }
    }
    Put32(out, 0, big);
    out.insert(out.end(), payload_area.begin(), payload_area.end());
    return out;
  }

  void Write(const std::filesystem::path& path, ByteOrder order = ByteOrder::kLittleEndian,
             EntryOrder entry_order = EntryOrder::kAscending) const {
    WriteBytes(path, Bytes(order, entry_order));
  }

  static void WriteBytes(const std::filesystem::path&     path,
                         const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write test DCP file");
  }

 private:
  struct Field {
    std::uint16_t              type = 0;
    std::vector<std::uint32_t> units;  // One byte, short, or long each; SRATIONAL uses two.
  };

  static void Put16(std::vector<std::uint8_t>& out, std::uint16_t value, bool big) {
    const auto high = static_cast<std::uint8_t>(value >> 8);
    const auto low  = static_cast<std::uint8_t>(value);
    out.push_back(big ? high : low);
    out.push_back(big ? low : high);
  }
  static void Put32(std::vector<std::uint8_t>& out, std::uint32_t value, bool big) {
    const auto high = static_cast<std::uint16_t>(value >> 16);
    const auto low  = static_cast<std::uint16_t>(value);
    Put16(out, big ? high : low, big);
    Put16(out, big ? low : high, big);
  }

  static auto UnitSize(std::uint16_t type) -> int {
    switch (type) {
      case kAscii:
        return 1;
      case kShort:
        return 2;
      default:
        return 4;
    }
  }
  static auto UnitsPerValue(std::uint16_t type) -> std::size_t {
    return type == kSRational ? 2 : 1;
  }

  std::map<std::uint16_t, Field>         fields_;
  std::map<std::uint16_t, std::uint32_t> offset_overrides_;
  std::uint16_t                          magic_ = 0x4352;
};

/**
 * @brief Smallest DCP file that the reader accepts: the identity, `ColorMatrix1`, and
 * `CalibrationIlluminant1` = 21 (D65). @p matrix_scale changes the matrix, so that two files
 * with one identity have different content.
 */
inline auto MinimalDcpProfile(const std::string& unique_camera_model,
                              const std::string& profile_name, double matrix_scale = 1.0)
    -> DcpTestFileWriter {
  constexpr std::uint16_t kUniqueCameraModel      = 50708;
  constexpr std::uint16_t kColorMatrix1           = 50721;
  constexpr std::uint16_t kCalibrationIlluminant1 = 50778;
  constexpr std::uint16_t kProfileName            = 50936;
  std::vector<double>     matrix{0.6722, -0.0635, -0.0963, -0.4287, 1.246,
                             0.2028, -0.0908, 0.2162,  0.5668};
  for (double& value : matrix) value *= matrix_scale;
  DcpTestFileWriter writer;
  writer.Ascii(kUniqueCameraModel, unique_camera_model)
      .Ascii(kProfileName, profile_name)
      .SRationals(kColorMatrix1, matrix)
      .Shorts(kCalibrationIlluminant1, {21});
  return writer;
}

}  // namespace alcedo::test
