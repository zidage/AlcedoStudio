// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <json.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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
  std::uint64_t         fingerprint = 0;  // FNV-1a of the content; computed by MakeDngColorProfile.
};
using DngColorProfilePtr = std::shared_ptr<const DngColorProfile>;

inline void to_json(nlohmann::json& j, const DngHueSatMap& map) {
  j = {{"divisions", map.divisions}, {"encoding", map.encoding}, {"entries", map.entries}};
}
/// Complete profile content as JSON. It is the fingerprint input; project data never stores it.
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

/// Validate before publishing immutable data read from a source file.
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

/// Profiles are equal when their content fingerprints are equal. The fingerprint is the FNV-1a
/// hash of the complete profile content, so no table data is compared.
inline auto DngColorProfilesEqual(const DngColorProfilePtr& a, const DngColorProfilePtr& b)
    -> bool {
  return a == b || (a && b && a->fingerprint == b->fingerprint);
}

/// Persisted text form of a profile fingerprint: 16 lowercase hexadecimal digits.
inline auto DngColorProfileFingerprintToText(std::uint64_t fingerprint) -> std::string {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string           text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[static_cast<std::size_t>(i)] = kDigits[fingerprint & 0xF];
    fingerprint >>= 4;
  }
  return text;
}

/// Parse the text written by @ref DngColorProfileFingerprintToText.
/// @throws std::runtime_error when @p text is not 16 hexadecimal digits.
inline auto DngColorProfileFingerprintFromText(const std::string& text) -> std::uint64_t {
  if (text.size() != 16) throw std::runtime_error("DNG profile: invalid fingerprint text");
  std::uint64_t value = 0;
  for (const char c : text) {
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a' + 10);
    } else {
      throw std::runtime_error("DNG profile: invalid fingerprint text");
    }
    value = (value << 4) | digit;
  }
  return value;
}

/**
 * @brief Reference from persisted image or Develop data to an import-bound DNG profile.
 *
 * Project data stores only the fingerprint. The profile tables are runtime data: the owner of a
 * loaded document binds the profile from the source file (see DngColorProfileCache) before the
 * document renders. A reference is in one of three states:
 * - empty: the image has no DNG profile;
 * - referenced and bound: fingerprint and profile are both set, and the fingerprint is the
 *   profile's own fingerprint;
 * - referenced and unbound: only the fingerprint is set (read from project data). Rendering code
 *   must call @ref RequireBound and fail; it must never render such a reference as "no profile".
 *
 * Equality compares fingerprints only, so a bound and an unbound reference to the same profile
 * are equal.
 */
class DngColorProfileRef {
 public:
  DngColorProfileRef() = default;
  /// Bound reference to @p profile, or an empty reference when @p profile is null.
  DngColorProfileRef(DngColorProfilePtr profile)  // NOLINT(google-explicit-constructor)
      : profile_(std::move(profile)) {
    if (profile_) fingerprint_ = profile_->fingerprint;
  }

  /// Unbound reference read from project data.
  static auto Unbound(std::uint64_t fingerprint) -> DngColorProfileRef {
    DngColorProfileRef ref;
    ref.fingerprint_ = fingerprint;
    return ref;
  }

  [[nodiscard]] auto IsReferenced() const -> bool { return fingerprint_.has_value(); }
  [[nodiscard]] auto IsBound() const -> bool { return profile_ != nullptr; }
  [[nodiscard]] auto Fingerprint() const -> const std::optional<std::uint64_t>& {
    return fingerprint_;
  }
  /// Bound profile, or null for an empty or unbound reference.
  [[nodiscard]] auto Profile() const -> const DngColorProfilePtr& { return profile_; }

  /// Profile for rendering: null for an empty reference.
  /// @throws std::runtime_error for a referenced but unbound reference.
  [[nodiscard]] auto RequireBound() const -> const DngColorProfile* {
    if (fingerprint_.has_value() && !profile_) {
      throw std::runtime_error("DNG profile " + DngColorProfileFingerprintToText(*fingerprint_) +
                               " is referenced but was not loaded from the source file");
    }
    return profile_.get();
  }

  explicit    operator bool() const { return IsBound(); }
  auto        operator->() const -> const DngColorProfile* { return profile_.get(); }
  auto        operator*() const -> const DngColorProfile& { return *profile_; }

  friend auto operator==(const DngColorProfileRef& a, const DngColorProfileRef& b) -> bool {
    return a.fingerprint_ == b.fingerprint_;
  }

 private:
  std::optional<std::uint64_t> fingerprint_;
  DngColorProfilePtr           profile_;
};

/// Persisted form of a reference: the fingerprint text, or null for an empty reference.
inline auto DngColorProfileRefToJson(const DngColorProfileRef& ref) -> nlohmann::json {
  if (!ref.IsReferenced()) return nullptr;
  return DngColorProfileFingerprintToText(*ref.Fingerprint());
}

/// Read a persisted reference. The result is unbound; @p value null gives an empty reference.
inline auto DngColorProfileRefFromJson(const nlohmann::json& value) -> DngColorProfileRef {
  if (value.is_null()) return {};
  if (!value.is_string()) throw std::runtime_error("DNG profile: fingerprint must be a string");
  return DngColorProfileRef::Unbound(DngColorProfileFingerprintFromText(value.get<std::string>()));
}
}  // namespace alcedo
