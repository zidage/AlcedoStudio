//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/overlap_tile_jobs.hpp"

#include <stdexcept>
#include <string>

namespace alcedo::nn {
namespace {

auto RoundUp(int value, int alignment) -> int {
  return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

auto PlanOverlapAxis(int length, int tile, int overlap, int alignment)
    -> std::vector<OverlapAxisTile> {
  if (length <= 0 || alignment <= 0 || tile % alignment != 0 || overlap < 0 ||
      overlap % alignment != 0 || tile <= 2 * overlap) {
    throw std::invalid_argument(
        "overlap tiles: length " + std::to_string(length) + ", tile " + std::to_string(tile) +
        ", overlap " + std::to_string(overlap) + ", alignment " + std::to_string(alignment) +
        ": the tile and the overlap must be multiples of the alignment, and the tile larger "
        "than 2 x overlap");
  }
  if (length <= tile) {
    return {OverlapAxisTile{0, RoundUp(length, alignment), 0, length}};
  }
  const int                    kept  = tile - 2 * overlap;
  const int                    count = 1 + (length - tile + kept - 1) / kept;

  std::vector<OverlapAxisTile> tiles;
  tiles.reserve(count);
  tiles.push_back({0, tile, 0, tile - overlap});
  for (int index = 1; index < count - 1; ++index) {
    const int start = tile - overlap + (index - 1) * kept;
    tiles.push_back({start - overlap, tile, start, start + kept});
  }
  const int keep_start = tiles.back().keep_end;
  const int last_input = keep_start - overlap;
  tiles.push_back({last_input, RoundUp(length - last_input, alignment), keep_start, length});
  return tiles;
}

auto PlanOverlapTiles(int width, int height, int tile, int overlap, int alignment)
    -> std::vector<OverlapTile> {
  const auto               rows    = PlanOverlapAxis(height, tile, overlap, alignment);
  const auto               columns = PlanOverlapAxis(width, tile, overlap, alignment);
  std::vector<OverlapTile> tiles;
  tiles.reserve(rows.size() * columns.size());
  for (const auto& row : rows) {
    for (const auto& column : columns) {
      tiles.push_back({
          .input = {column.input_start, row.input_start, column.input_size, row.input_size},
          .kept  = {column.keep_start, row.keep_start, column.keep_end - column.keep_start,
                    row.keep_end - row.keep_start},
      });
    }
  }
  return tiles;
}

}  // namespace alcedo::nn
