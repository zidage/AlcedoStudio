//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "metadata_values.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <locale>

namespace alcedo::metadata_detail {

auto RationalToFloat(const Exiv2::Rational& value) -> float {
  if (value.second == 0) {
    return 0.0f;
  }
  return static_cast<float>(value.first) / static_cast<float>(value.second);
}

auto IsFinitePositive(float value) -> bool { return std::isfinite(value) && value > 0.0f; }

auto TrimTrailingZeroPadded(const char* s, size_t max_len) -> std::string {
  if (!s) return {};
  size_t len = std::min(std::strlen(s), max_len);
  while (len > 0 && (s[len - 1] == '\0' || std::isspace(s[len - 1], std::locale::classic()))) {
    --len;
  }
  return {s, len};
}

auto TrimAscii(const std::string& value) -> std::string {
  std::string out = value;
  while (!out.empty() && (out.back() == '\0' || std::isspace(out.back(), std::locale::classic()))) {
    out.pop_back();
  }
  size_t begin = 0;
  while (begin < out.size() &&
         (out[begin] == '\0' || std::isspace(out[begin], std::locale::classic()))) {
    ++begin;
  }
  if (begin > 0) {
    out.erase(0, begin);
  }
  return out;
}

auto ContainsCaseInsensitive(const std::string& text, const std::string& pattern) -> bool {
  if (text.empty() || pattern.empty()) return false;
  auto        to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
  std::string lower_text(text.size(), '\0');
  std::string lower_pattern(pattern.size(), '\0');
  std::transform(text.begin(), text.end(), lower_text.begin(), to_lower);
  std::transform(pattern.begin(), pattern.end(), lower_pattern.begin(), to_lower);
  return lower_text.find(lower_pattern) != std::string::npos;
}

auto IsDngExtension(const std::filesystem::path& path) -> bool {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".dng";
}

auto ResolveCropFactorHint(float focal_mm, float focal_35mm_mm) -> float {
  if (!IsFinitePositive(focal_mm) || !IsFinitePositive(focal_35mm_mm)) return 0.0f;
  return focal_35mm_mm / focal_mm;
}

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto u8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

auto ToLowerAscii(std::string value) -> std::string {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

auto ContainsAny(const std::string& text, const std::initializer_list<std::string_view>& patterns)
    -> bool {
  for (const auto pattern : patterns) {
    if (text.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace alcedo::metadata_detail
