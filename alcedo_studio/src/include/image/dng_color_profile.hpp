// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <json.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace alcedo {

// Bound imported allocations and keep packed GPU offsets exactly representable as floats.
inline constexpr std::size_t kMaxDngTableFloats = 4 * 1024 * 1024;

/// DNG table order is value, hue, saturation; saturation varies fastest.
struct DngHueSatMap {
  std::array<std::uint32_t, 3> divisions{};
  std::uint32_t                encoding = 0;  // DNG: 0 linear, 1 sRGB value coordinate.
  std::vector<float>           entries;  // Hue shift in degrees, saturation scale, value scale.
  auto                         operator==(const DngHueSatMap&) const -> bool = default;

  void                         Validate() const {
    if (entries.empty() && divisions == std::array<std::uint32_t, 3>{}) return;
    const auto [h, s, v] = divisions;
    if (h == 0 || s < 2 || v == 0 || h > 360 || s > 256 || v > 256 || encoding > 1 ||
        entries.size() > kMaxDngTableFloats ||
        static_cast<std::uint64_t>(h) * s * v * 3 != entries.size()) {
      throw std::runtime_error(
          "DNG profile: invalid HueSatMap dimensions, encoding or entry count");
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (!std::isfinite(entries[i]) || (i % 3 != 0 && entries[i] < 0.0f)) {
        throw std::runtime_error("DNG profile: invalid HueSatMap correction");
      }
    }
    for (std::size_t i = 2; i < entries.size(); i += static_cast<std::size_t>(s) * 3) {
      if (entries[i] != 1.0f) {
        throw std::runtime_error("DNG profile: neutral table entries must preserve value");
      }
    }
  }
};

inline constexpr std::array<double, 9> kDngIdentityMatrix{1, 0, 0, 0, 1, 0, 0, 0, 1};

/** Import-resolved DNG calibration and profile tables. Shared immutably by image and graph. */
struct DngColorProfile {
  std::string           name;
  std::array<double, 3> analog_balance{1, 1, 1};
  std::array<double, 9> camera_calibration_1 = kDngIdentityMatrix;
  std::array<double, 9> camera_calibration_2 = kDngIdentityMatrix;
  DngHueSatMap          hue_sat_map_1;
  DngHueSatMap          hue_sat_map_2;
  DngHueSatMap          look_table;
  double                baseline_exposure        = 0.0;
  double                baseline_exposure_offset = 0.0;
  std::uint64_t fingerprint = 0;  // Computed on construction; never trusted from persisted JSON.
};
using DngColorProfilePtr = std::shared_ptr<const DngColorProfile>;

inline void to_json(nlohmann::json& j, const DngHueSatMap& map) {
  j = {{"divisions", map.divisions}, {"encoding", map.encoding}, {"entries", map.entries}};
}
inline void from_json(const nlohmann::json& j, DngHueSatMap& map) {
  j.at("divisions").get_to(map.divisions);
  j.at("encoding").get_to(map.encoding);
  j.at("entries").get_to(map.entries);
  map.Validate();
}
inline auto DngColorProfileToJson(const DngColorProfilePtr& profile) -> nlohmann::json {
  if (!profile) return nullptr;
  return {{"version", 1},
          {"name", profile->name},
          {"analog_balance", profile->analog_balance},
          {"camera_calibration_1", profile->camera_calibration_1},
          {"camera_calibration_2", profile->camera_calibration_2},
          {"hue_sat_map_1", profile->hue_sat_map_1},
          {"hue_sat_map_2", profile->hue_sat_map_2},
          {"look_table", profile->look_table},
          {"baseline_exposure", profile->baseline_exposure},
          {"baseline_exposure_offset", profile->baseline_exposure_offset}};
}

/// Validate before publishing immutable data, including data read from project files.
inline auto MakeDngColorProfile(DngColorProfile profile) -> DngColorProfilePtr {
  profile.hue_sat_map_1.Validate();
  profile.hue_sat_map_2.Validate();
  profile.look_table.Validate();
  if (!profile.hue_sat_map_2.entries.empty() &&
      (profile.hue_sat_map_1.divisions != profile.hue_sat_map_2.divisions ||
       profile.hue_sat_map_1.encoding != profile.hue_sat_map_2.encoding)) {
    throw std::runtime_error("DNG profile: illuminant tables have incompatible dimensions");
  }
  for (double value : profile.analog_balance) {
    if (!std::isfinite(value) || value <= 0)
      throw std::runtime_error("DNG profile: invalid AnalogBalance");
  }
  for (const auto& matrix : {profile.camera_calibration_1, profile.camera_calibration_2}) {
    for (double value : matrix) {
      if (!std::isfinite(value))
        throw std::runtime_error("DNG profile: non-finite CameraCalibration");
    }
  }
  const double exposure = profile.baseline_exposure + profile.baseline_exposure_offset;
  if (!std::isfinite(exposure) || std::abs(exposure) > 32) {
    throw std::runtime_error("DNG profile: invalid baseline exposure");
  }
  auto          result = std::make_shared<DngColorProfile>(std::move(profile));
  const auto    text   = DngColorProfileToJson(result).dump();
  std::uint64_t hash   = 14695981039346656037ULL;
  for (unsigned char byte : text) hash = (hash ^ byte) * 1099511628211ULL;
  result->fingerprint = hash;
  return result;
}

inline auto DngColorProfileFromJson(const nlohmann::json& j) -> DngColorProfilePtr {
  if (j.is_null()) return {};
  if (j.at("version").get<int>() != 1)
    throw std::runtime_error("Unsupported DNG profile data version");
  DngColorProfile result;
  j.at("name").get_to(result.name);
  j.at("analog_balance").get_to(result.analog_balance);
  j.at("camera_calibration_1").get_to(result.camera_calibration_1);
  j.at("camera_calibration_2").get_to(result.camera_calibration_2);
  j.at("hue_sat_map_1").get_to(result.hue_sat_map_1);
  j.at("hue_sat_map_2").get_to(result.hue_sat_map_2);
  j.at("look_table").get_to(result.look_table);
  j.at("baseline_exposure").get_to(result.baseline_exposure);
  j.at("baseline_exposure_offset").get_to(result.baseline_exposure_offset);
  return MakeDngColorProfile(std::move(result));
}

inline auto DngColorProfilesEqual(const DngColorProfilePtr& a, const DngColorProfilePtr& b)
    -> bool {
  return a == b || (a && b && a->fingerprint == b->fingerprint &&
                    DngColorProfileToJson(a) == DngColorProfileToJson(b));
}
}  // namespace alcedo
