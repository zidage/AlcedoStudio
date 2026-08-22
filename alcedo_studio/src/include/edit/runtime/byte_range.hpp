//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace alcedo {

/// Half-open byte interval in a ParameterArena device buffer.
struct ByteRange {
  std::uint32_t offset = 0;
  std::uint32_t size   = 0;

  [[nodiscard]] auto End() const -> std::uint32_t { return offset + size; }
};

/**
 * @brief Sort ranges by offset and merge overlapping or adjacent intervals.
 * @param ranges Input ranges; may be empty or unsorted.
 * @return Non-overlapping ranges in offset order.
 */
inline auto MergeAdjacentRanges(std::vector<ByteRange> ranges) -> std::vector<ByteRange> {
  if (ranges.empty()) {
    return ranges;
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const ByteRange& lhs, const ByteRange& rhs) { return lhs.offset < rhs.offset; });
  std::vector<ByteRange> merged;
  merged.push_back(ranges.front());
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    ByteRange& last = merged.back();
    if (ranges[i].offset <= last.End()) {
      const auto end = (std::max)(last.End(), ranges[i].End());
      last.size      = end - last.offset;
    } else {
      merged.push_back(ranges[i]);
    }
  }
  return merged;
}

}  // namespace alcedo
