//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <string_view>

// Historical compatibility fixture. These reader functions and version bounds come from
// project_package_backend.cpp and pipeline_history_format.hpp at e59529c66d2d.
// Keep this implementation fixed: it executes the 0.11 reader's version check independently
// of the current reader. A current-reader test cannot prove that an older reader rejects a
// newly written format. No production code includes this fixture.
namespace alcedo::test::version_011 {
inline constexpr std::string_view kMinSupportedProjectFileVersion = "0.10.0";
inline constexpr std::string_view kMaxSupportedProjectFileVersion = "0.11.0";

inline auto ParseSemVer(std::string_view version, std::array<int, 3>* out) -> bool {
  std::array<int, 3> parts{};
  size_t             begin = 0;
  for (size_t index = 0; index < parts.size(); ++index) {
    const size_t end = version.find('.', begin);
    const auto   token =
        version.substr(begin, end == std::string_view::npos ? version.size() - begin : end - begin);
    if (token.empty()) {
      return false;
    }
    int value = 0;
    for (const char ch : token) {
      if (ch < '0' || ch > '9') {
        return false;
      }
      value = value * 10 + (ch - '0');
    }
    parts[index] = value;
    if (index + 1 < parts.size()) {
      if (end == std::string_view::npos) {
        return false;
      }
      begin = end + 1;
    } else if (end != std::string_view::npos) {
      return false;
    }
  }
  *out = parts;
  return true;
}

inline auto ProjectVersionIsSupported(std::string_view version) -> bool {
  std::array<int, 3> parsed{};
  std::array<int, 3> min_supported{};
  std::array<int, 3> max_supported{};
  return ParseSemVer(version, &parsed) &&
         ParseSemVer(kMinSupportedProjectFileVersion, &min_supported) &&
         ParseSemVer(kMaxSupportedProjectFileVersion, &max_supported) && parsed >= min_supported &&
         parsed <= max_supported;
}

}  // namespace alcedo::test::version_011
