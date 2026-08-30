// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dng_color_profile_import.hpp"

#include <limits>
#include <type_traits>

#include "image/dng_camera_matrix.hpp"

namespace alcedo {
namespace {
auto Find(const Exiv2::ExifData& exif, std::uint16_t tag) -> const Exiv2::Exifdatum* {
  for (const auto& datum : exif) {
    if (datum.groupName() == "Image" && datum.tag() == tag) return &datum;
  }
  return nullptr;
}
template <typename T, std::size_t N>
auto ReadArray(const Exiv2::ExifData& exif, std::uint16_t tag, std::array<T, N>& out) -> bool {
  const auto* datum = Find(exif, tag);
  if (!datum) return false;
  if (datum->count() != N)
    throw std::runtime_error("DNG profile: invalid array length for tag " + std::to_string(tag));
  for (std::size_t i = 0; i < N; ++i) {
    const double value = datum->toFloat(i);
    if (!std::isfinite(value)) throw std::runtime_error("DNG profile: non-finite array entry");
    if constexpr (std::is_integral_v<T>) {
      if (value < 0 || value > std::numeric_limits<T>::max() || value != std::floor(value)) {
        throw std::runtime_error("DNG profile: invalid integer array entry");
      }
    }
    out[i] = static_cast<T>(value);
  }
  return true;
}
auto ReadString(const Exiv2::ExifData& exif, std::uint16_t tag) -> std::string {
  const auto* datum = Find(exif, tag);
  return datum ? datum->toString() : std::string{};
}
auto ReadScalar(const Exiv2::ExifData& exif, std::uint16_t tag, double absent) -> double {
  const auto* datum = Find(exif, tag);
  if (!datum) return absent;
  if (datum->count() != 1)
    throw std::runtime_error("DNG profile: invalid scalar tag " + std::to_string(tag));
  return datum->toFloat();
}
auto ReadMap(const Exiv2::ExifData& exif, std::uint16_t dims_tag, std::uint16_t data_tag,
             std::uint16_t encoding_tag) -> DngHueSatMap {
  DngHueSatMap map;
  const auto*  datum = Find(exif, data_tag);
  if (!datum) return map;
  const auto* dimensions = Find(exif, dims_tag);
  if (dimensions && dimensions->count() == 2) {
    std::array<std::uint32_t, 2> hs;
    ReadArray(exif, dims_tag, hs);
    map.divisions = {hs[0], hs[1], 1};
  } else if (!ReadArray(exif, dims_tag, map.divisions)) {
    throw std::runtime_error("DNG profile: table dimensions are missing");
  }
  const double encoding = ReadScalar(exif, encoding_tag, 0);
  if (encoding != 0 && encoding != 1)
    throw std::runtime_error("DNG profile: unsupported table encoding");
  map.encoding         = static_cast<std::uint32_t>(encoding);
  const auto [h, s, v] = map.divisions;
  if (h == 0 || s < 2 || v == 0 || h > 360 || s > 256 || v > 256) {
    throw std::runtime_error("DNG profile: invalid table dimensions");
  }
  const std::uint64_t count = static_cast<std::uint64_t>(h) * s * v * 3;
  const bool          omit_zero_saturation =
      datum->count() == static_cast<std::uint64_t>(h) * (s - 1) * v * 3;
  if (count > kMaxDngTableFloats || (!omit_zero_saturation && count != datum->count())) {
    throw std::runtime_error("DNG profile: table dimensions disagree with data count");
  }
  map.entries.resize(static_cast<std::size_t>(count));
  std::size_t input_index = 0;
  for (std::size_t slice = 0; slice < static_cast<std::size_t>(h) * v; ++slice) {
    const auto offset = slice * s * 3;
    for (unsigned saturation = omit_zero_saturation ? 1 : 0; saturation < s; ++saturation) {
      for (unsigned channel = 0; channel < 3; ++channel) {
        map.entries[offset + saturation * 3 + channel] = datum->toFloat(input_index++);
      }
    }
    // DNG permits omitting the neutral row; extrapolate hue/saturation from the first row.
    if (omit_zero_saturation) {
      map.entries[offset]     = map.entries[offset + 3];
      map.entries[offset + 1] = map.entries[offset + 4];
      map.entries[offset + 2] = 1;
    } else if (map.entries[offset + 2] != 1) {
      throw std::runtime_error("DNG profile: neutral table entries must preserve value");
    }
  }
  map.Validate();
  return map;
}
}  // namespace

auto ReadDngColorProfile(const Exiv2::ExifData& exif) -> DngColorProfilePtr {
  DngColorProfile profile;
  if ((Find(exif, 50937) && !Find(exif, 50938)) || (Find(exif, 50981) && !Find(exif, 50982))) {
    throw std::runtime_error("DNG profile: table data is missing");
  }
  profile.name = ReadString(exif, 50936);
  ReadArray(exif, 50727, profile.analog_balance);
  if (CameraCalibrationSignaturesMatch(ReadString(exif, 50931), ReadString(exif, 50932))) {
    const bool first  = ReadArray(exif, 50723, profile.camera_calibration_1);
    const bool second = ReadArray(exif, 50724, profile.camera_calibration_2);
    if (first && !second) profile.camera_calibration_2 = profile.camera_calibration_1;
    if (second && !first) profile.camera_calibration_1 = profile.camera_calibration_2;
  }
  profile.hue_sat_map_1            = ReadMap(exif, 50937, 50938, 51107);
  profile.hue_sat_map_2            = ReadMap(exif, 50937, 50939, 51107);
  profile.look_table               = ReadMap(exif, 50981, 50982, 51108);
  profile.baseline_exposure        = ReadScalar(exif, 50730, 0);
  profile.baseline_exposure_offset = ReadScalar(exif, 51109, 0);
  return MakeDngColorProfile(std::move(profile));
}
}  // namespace alcedo
