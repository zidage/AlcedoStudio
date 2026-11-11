//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_filter/filter_combo.hpp"

#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace alcedo {

auto ParseStatsBucketDay(std::string_view text) -> std::chrono::year_month_day {
  int                year = 0, month = 0, day = 0;
  char               separator_1 = 0, separator_2 = 0;
  std::istringstream stream{std::string(text)};
  stream >> year >> separator_1 >> month >> separator_2 >> day;
  const std::chrono::year_month_day date{std::chrono::year{year},
                                         std::chrono::month{static_cast<unsigned>(month)},
                                         std::chrono::day{static_cast<unsigned>(day)}};
  if (text.size() != 10 || stream.fail() || !stream.eof() || separator_1 != '-' ||
      separator_2 != '-' || !date.ok()) {
    throw std::invalid_argument("Stats bucket: the day is not a valid YYYY-MM-DD day");
  }
  return date;
}

auto MergeFilterNodes(const std::optional<FilterNode>& left, const std::optional<FilterNode>& right)
    -> std::optional<FilterNode> {
  if (!left.has_value()) {
    return right;
  }
  if (!right.has_value()) {
    return left;
  }
  return FilterNode{
      FilterNode::Type::Logical, FilterOp::AND, {*left, *right}, std::nullopt, std::nullopt};
}

}  // namespace alcedo
