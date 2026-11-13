//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/version_name_rules.hpp"

namespace alcedo {
namespace {

/// The white space and line terminators that the JavaScript String.prototype.trim removes.
auto IsTrimmedCodePoint(char32_t code_point) -> bool {
  switch (code_point) {
    case 0x0009:
    case 0x000A:
    case 0x000B:
    case 0x000C:
    case 0x000D:
    case 0x0020:
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
      return true;
    default:
      return code_point >= 0x2000 && code_point <= 0x200A;
  }
}

/// Length of the UTF-8 sequence that starts with @p lead; 0 for a byte that cannot start one.
auto SequenceLength(unsigned char lead) -> std::size_t {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;
}

/// Decodes the sequence of @p length bytes at @p text; the caller checked the length.
auto Decode(std::string_view text, std::size_t length) -> char32_t {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  switch (length) {
    case 1:
      return byte(0);
    case 2:
      return (static_cast<char32_t>(byte(0) & 0x1F) << 6) | (byte(1) & 0x3F);
    case 3:
      return (static_cast<char32_t>(byte(0) & 0x0F) << 12) |
             (static_cast<char32_t>(byte(1) & 0x3F) << 6) | (byte(2) & 0x3F);
    default:
      return (static_cast<char32_t>(byte(0) & 0x07) << 18) |
             (static_cast<char32_t>(byte(1) & 0x3F) << 12) |
             (static_cast<char32_t>(byte(2) & 0x3F) << 6) | (byte(3) & 0x3F);
  }
}

/// Removes trimmed code points from both ends of @p text. Bytes that are no valid UTF-8 stop
/// the trim, so they stay in the name.
auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty()) {
    const std::size_t length = SequenceLength(static_cast<unsigned char>(text.front()));
    if (length == 0 || length > text.size() || !IsTrimmedCodePoint(Decode(text, length))) {
      break;
    }
    text.remove_prefix(length);
  }
  while (!text.empty()) {
    // Step back over continuation bytes to the lead byte of the last sequence.
    std::size_t start = text.size() - 1;
    while (start > 0 && text.size() - start < 4 &&
           (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
      --start;
    }
    const std::string_view last   = text.substr(start);
    const std::size_t      length = SequenceLength(static_cast<unsigned char>(last.front()));
    if (length != last.size() || !IsTrimmedCodePoint(Decode(last, length))) {
      break;
    }
    text.remove_suffix(length);
  }
  return text;
}

}  // namespace

auto NormalizeVersionName(std::string_view requested, std::string* error)
    -> std::optional<std::string> {
  const std::string_view name = Trim(requested);
  if (name.empty()) {
    if (error != nullptr) {
      *error = kEmptyVersionNameMessage;
    }
    return std::nullopt;
  }
  return std::string(name);
}

auto DefaultVersionName(std::size_t version_count, std::string_view pattern) -> std::string {
  const std::string number = std::to_string(version_count + 1);
  std::string       name(pattern);
  const auto        marker = name.find("%1");
  if (marker == std::string::npos) {
    return name + " " + number;
  }
  name.replace(marker, 2, number);
  return name;
}

auto IsUnchangedVersionName(std::string_view current, std::string_view requested) -> bool {
  const std::string_view name = Trim(requested);
  return !name.empty() && name == Trim(current);
}

}  // namespace alcedo
