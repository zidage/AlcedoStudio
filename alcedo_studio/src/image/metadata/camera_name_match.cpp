//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "camera_name_match.hpp"

#include <algorithm>
#include <cctype>

namespace alcedo::metadata_detail {

auto NormalizeCameraName(const std::string& input) -> std::string {
  std::string normalized;
  normalized.reserve(input.size() + input.size() / 4);

  bool last_space = true;
  bool last_alpha = false;
  bool last_digit = false;
  for (char ch : input) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      const bool is_alpha = std::isalpha(uch) != 0;
      const bool is_digit = std::isdigit(uch) != 0;
      // Insert space at letter↔digit boundary so "Z5" → "z 5", "5D" → "5 d".
      if (!last_space && ((last_alpha && is_digit) || (last_digit && is_alpha))) {
        normalized.push_back(' ');
      }
      normalized.push_back(static_cast<char>(std::tolower(uch)));
      last_space = false;
      last_alpha = is_alpha;
      last_digit = is_digit;
    } else if (!last_space) {
      normalized.push_back(' ');
      last_space = true;
      last_alpha = false;
      last_digit = false;
    }
  }

  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }
  return normalized;
}

auto CompactNormalizedCameraName(std::string normalized) -> std::string {
  normalized.erase(std::remove(normalized.begin(), normalized.end(), ' '), normalized.end());
  return normalized;
}

auto CompactCameraName(const std::string& input) -> std::string {
  return CompactNormalizedCameraName(NormalizeCameraName(input));
}

auto TokenizeCameraNameLoose(const std::string& input) -> std::vector<std::string> {
  std::vector<std::string> tokens;
  std::string              token;
  token.reserve(input.size());

  for (char ch : input) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      token.push_back(static_cast<char>(std::tolower(uch)));
    } else if (!token.empty()) {
      tokens.push_back(std::move(token));
      token.clear();
    }
  }

  if (!token.empty()) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

namespace {
auto IsMeaningfulCameraMatchToken(const std::string& token) -> bool {
  if (token.size() >= 2) {
    return true;
  }
  return !token.empty() && std::isdigit(static_cast<unsigned char>(token.front())) != 0;
}
}  // namespace

auto UniqueMeaningfulCameraTokens(const std::vector<std::string>& tokens)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(tokens.size());
  for (const auto& token : tokens) {
    if (!IsMeaningfulCameraMatchToken(token)) {
      continue;
    }
    if (std::find(out.begin(), out.end(), token) == out.end()) {
      out.push_back(token);
    }
  }
  return out;
}

auto ContainsCameraToken(const std::vector<std::string>& tokens, const std::string& token) -> bool {
  return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

auto ContainsCameraTokenFragment(const std::vector<std::string>& tokens,
                                 const std::string&              fragment) -> bool {
  if (fragment.empty()) {
    return false;
  }
  return std::any_of(tokens.begin(), tokens.end(), [&](const std::string& token) {
    return token.find(fragment) != std::string::npos;
  });
}

auto HasMeaningfulCameraModelToken(const std::string& camera_model) -> bool {
  const auto normalized = NormalizeCameraName(camera_model);
  if (normalized.empty()) {
    return false;
  }

  const auto tokens = UniqueMeaningfulCameraTokens(TokenizeCameraNameLoose(normalized));
  if (!tokens.empty()) {
    return true;
  }

  return normalized.find(' ') != std::string::npos;
}

}  // namespace alcedo::metadata_detail
