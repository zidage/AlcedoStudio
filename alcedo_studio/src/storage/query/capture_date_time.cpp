//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/query/capture_date_time.hpp"

#include <cctype>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>

namespace alcedo {
namespace {

auto ParseDigits(std::string_view text, size_t pos, size_t count) -> std::optional<int> {
  if (pos + count > text.size()) {
    return std::nullopt;
  }
  int value = 0;
  for (size_t i = pos; i < pos + count; ++i) {
    const auto ch = static_cast<unsigned char>(text[i]);
    if (std::isdigit(ch) == 0) {
      return std::nullopt;
    }
    value = value * 10 + (ch - '0');
  }
  return value;
}

auto IsLeapYear(int year) -> bool { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

auto DaysInMonth(int year, int month) -> int {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && IsLeapYear(year) ? 29 : kDays[month - 1];
}

auto IsDateSeparator(char ch) -> bool { return std::isdigit(static_cast<unsigned char>(ch)) == 0; }

}  // namespace

auto ParseCaptureDateTime(std::string_view text) -> std::optional<CaptureDateTime> {
  if (text.size() < 10 || !IsDateSeparator(text[4]) || !IsDateSeparator(text[7])) {
    return std::nullopt;
  }
  const auto year  = ParseDigits(text, 0, 4);
  const auto month = ParseDigits(text, 5, 2);
  const auto day   = ParseDigits(text, 8, 2);
  if (!year || !month || !day || *year < 1 || *month < 1 || *month > 12 || *day < 1 ||
      *day > DaysInMonth(*year, *month)) {
    return std::nullopt;
  }

  int hour = 0, minute = 0, second = 0;
  if (text.size() > 10) {
    // `YYYY-MM-DD HH:MM:SS`: the time must follow one separator and be complete.
    const auto h = ParseDigits(text, 11, 2);
    const auto m = ParseDigits(text, 14, 2);
    const auto s = ParseDigits(text, 17, 2);
    if (text.size() < 19 || !IsDateSeparator(text[10]) || text[13] != ':' || text[16] != ':' ||
        !h || !m || !s || *h > 23 || *m > 59 || *s > 59) {
      return std::nullopt;
    }
    hour   = *h;
    minute = *m;
    second = *s;
  }

  CaptureDateTime out;
  out.date      = std::format("{:04}-{:02}-{:02}", *year, *month, *day);
  out.date_time = std::format("{} {:02}:{:02}:{:02}", out.date, hour, minute, second);
  return out;
}

}  // namespace alcedo
