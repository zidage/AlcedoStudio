//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/mapper/utc_timestamp_text.hpp"

#include <chrono>
#include <ctime>
#include <format>
#include <sstream>
#include <string>

namespace alcedo {

auto FormatUtcTimestampText(std::time_t value) -> std::string {
  using namespace std::chrono;
  const sys_seconds    time_point{seconds{value}};
  const auto           day = floor<days>(time_point);
  const year_month_day date{day};
  const hh_mm_ss       time{time_point - day};
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(date.year()),
                     static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                     time.hours().count(), time.minutes().count(), time.seconds().count());
}

auto ParseUtcTimestampText(const std::string& text) -> std::time_t {
  int                year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  char               separator_1 = 0, separator_2 = 0, separator_3 = 0, separator_4 = 0;
  std::istringstream stream(text);
  stream >> year >> separator_1 >> month >> separator_2 >> day >> hour >> separator_3 >> minute >>
      separator_4 >> second;
  if (stream.fail() || separator_1 != '-' || separator_2 != '-' || separator_3 != ':' ||
      separator_4 != ':') {
    return 0;
  }
  using namespace std::chrono;
  const year_month_day date{std::chrono::year{year},
                            std::chrono::month{static_cast<unsigned>(month)},
                            std::chrono::day{static_cast<unsigned>(day)}};
  if (!date.ok()) {
    return 0;
  }
  const auto time_point = sys_days{date} + hours{hour} + minutes{minute} + seconds{second};
  return static_cast<std::time_t>(time_point.time_since_epoch().count());
}

}  // namespace alcedo
