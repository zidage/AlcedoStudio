//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Reads the tone curve tags of an RGB ICC profile (ICC.1:2022), so tests can compare the curve an
// export profile declares with the transfer function that encoded the exported pixels.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace alcedo::icc_tone_curve_test {

/// One `curv` or `para` tone curve tag (ICC.1:2022 sections 10.6 and 10.18).
struct IccToneCurve {
  /// Entries of a `curv` table with more than one entry, scaled to [0, 1].
  std::vector<double>   table_;
  /// `para` function type 0 to 4. A `curv` tag with one entry is stored as type 0, and one with
  /// zero entries as type 0 with gamma 1.
  int                   function_type_ = 0;
  /// `para` parameters g, a, b, c, d, e, f; unused parameters stay 0.
  std::array<double, 7> params_        = {};

  /// Linear value of code value @p x in [0, 1].
  [[nodiscard]] auto    Evaluate(double x) const -> double {
    if (!table_.empty()) {
      const double position = std::clamp(x, 0.0, 1.0) * static_cast<double>(table_.size() - 1);
      const auto   index    = std::min(static_cast<std::size_t>(position), table_.size() - 2);
      const double t        = position - static_cast<double>(index);
      return table_[index] + (table_[index + 1] - table_[index]) * t;
    }
    const auto [g, a, b, c, d, e, f] = params_;
    switch (function_type_) {
      case 0:
        return std::pow(x, g);
      case 1:
        return x >= -b / a ? std::pow(a * x + b, g) : 0.0;
      case 2:
        return x >= -b / a ? std::pow(a * x + b, g) + c : c;
      case 3:
        return x >= d ? std::pow(a * x + b, g) : c * x;
      case 4:
        return x >= d ? std::pow(a * x + b, g) + e : c * x + f;
      default:
        return NAN;
    }
  }
};

inline auto ReadUint16(const std::vector<uint8_t>& bytes, std::size_t offset) -> uint32_t {
  return (static_cast<uint32_t>(bytes[offset]) << 8) | bytes[offset + 1];
}

inline auto ReadUint32(const std::vector<uint8_t>& bytes, std::size_t offset) -> uint32_t {
  return (ReadUint16(bytes, offset) << 16) | ReadUint16(bytes, offset + 2);
}

inline auto ReadS15Fixed16(const std::vector<uint8_t>& bytes, std::size_t offset) -> double {
  return static_cast<double>(static_cast<int32_t>(ReadUint32(bytes, offset))) / 65536.0;
}

/// Tone curve of tag @p signature (for example "rTRC"), or nullopt when the profile has no such
/// tag or the tag is not a valid `curv` or `para` element.
inline auto ReadIccToneCurve(const std::vector<uint8_t>& profile, std::string_view signature)
    -> std::optional<IccToneCurve> {
  constexpr std::size_t kHeaderSize   = 128;
  constexpr std::size_t kTagEntrySize = 12;
  if (profile.size() < kHeaderSize + 4 || signature.size() != 4) {
    return std::nullopt;
  }

  const uint32_t tag_count = ReadUint32(profile, kHeaderSize);
  for (uint32_t i = 0; i < tag_count; ++i) {
    const std::size_t entry = kHeaderSize + 4 + i * kTagEntrySize;
    if (entry + kTagEntrySize > profile.size()) {
      return std::nullopt;
    }
    if (std::string_view(reinterpret_cast<const char*>(profile.data() + entry), 4) != signature) {
      continue;
    }

    const std::size_t offset = ReadUint32(profile, entry + 4);
    const std::size_t size   = ReadUint32(profile, entry + 8);
    if (size < 12 || offset + size > profile.size()) {
      return std::nullopt;
    }
    const std::string_view type(reinterpret_cast<const char*>(profile.data() + offset), 4);

    IccToneCurve           curve;
    if (type == "curv") {
      const uint32_t count = ReadUint32(profile, offset + 8);
      if (size < 12 + 2 * static_cast<std::size_t>(count)) {
        return std::nullopt;
      }
      if (count == 0) {
        curve.params_[0] = 1.0;
      } else if (count == 1) {
        curve.params_[0] = static_cast<double>(ReadUint16(profile, offset + 12)) / 256.0;
      } else {
        curve.table_.resize(count);
        for (uint32_t k = 0; k < count; ++k) {
          curve.table_[k] = static_cast<double>(ReadUint16(profile, offset + 12 + 2 * k)) / 65535.0;
        }
      }
      return curve;
    }

    if (type == "para") {
      constexpr std::array<std::size_t, 5> kParamCounts = {1, 3, 4, 5, 7};
      curve.function_type_ = static_cast<int>(ReadUint16(profile, offset + 8));
      if (curve.function_type_ < 0 || curve.function_type_ > 4) {
        return std::nullopt;
      }
      const std::size_t param_count = kParamCounts[static_cast<std::size_t>(curve.function_type_)];
      if (size < 12 + 4 * param_count) {
        return std::nullopt;
      }
      for (std::size_t k = 0; k < param_count; ++k) {
        curve.params_[k] = ReadS15Fixed16(profile, offset + 12 + 4 * k);
      }
      return curve;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace alcedo::icc_tone_curve_test
