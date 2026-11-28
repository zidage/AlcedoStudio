//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <vector>

// The overlap tile planner of AI Denoise (integration plan, DN3): the tiles of a packed image
// for a network whose output has the size of its input (UtNet2), so that the tiled output is the
// one-pass output. It is the planner of the lab (alcedo-denoise-lab,
// scripts/export/measurement_common.py, `plan_axis` with `last_tile = "shrunk"`).

namespace alcedo::nn {

/// A rectangle in the packed image of a photo, in packed px. As a tile input, `x` and `y` are the
/// origin of the tile in the photo, and the rectangle can extend past the bottom and the right
/// border of the image: that part is filled by reflection.
struct PackedTileRect {
  int x      = 0;
  int y      = 0;
  int width  = 0;
  int height = 0;
};

/// One tile of one axis. `input_start` and `input_size` are the input range of the tile (the
/// size can extend past the end of the axis; that part is filled by reflection). The tile keeps
/// its output in [keep_start, keep_end).
struct OverlapAxisTile {
  int input_start = 0;
  int input_size  = 0;
  int keep_start  = 0;
  int keep_end    = 0;
};

/// One tile of the image: its input rectangle and the rectangle of the image that it keeps.
struct OverlapTile {
  PackedTileRect input;
  PackedTileRect kept;
};

/// The tiles of one axis of `length` packed px for the input size `tile` and the context
/// `overlap`. The tile inputs start at multiples of `alignment`, and their sizes are multiples of
/// `alignment`.
///
/// - `length <= tile`: one tile; its input is the whole axis, filled to a multiple of
///   `alignment`.
/// - Otherwise the first tile keeps [0, tile - overlap) and each next tile keeps
///   `tile - 2 overlap` px, with `overlap` px of input on each side. The last tile is shrunk:
///   its input starts `overlap` px before its kept range and ends at the end of the axis, filled
///   to a multiple of `alignment`. A fill to the full tile size is not used: it moves the grid of
///   the pooling layers (plan, DN1 addition of the last tile).
///
/// The kept ranges cover [0, length) exactly one time. Throws `std::invalid_argument` when
/// `length` is not positive, when `alignment` is not positive, when `tile` is not a multiple of
/// `alignment`, when `overlap` is negative or not a multiple of `alignment`, or when
/// `tile <= 2 overlap`.
[[nodiscard]] auto PlanOverlapAxis(int length, int tile, int overlap, int alignment)
    -> std::vector<OverlapAxisTile>;

/// The tiles of a packed image of `width` x `height` px in row-major order (all tiles of the
/// first row of tiles first). The arguments and the errors are those of `PlanOverlapAxis`.
[[nodiscard]] auto PlanOverlapTiles(int width, int height, int tile, int overlap, int alignment)
    -> std::vector<OverlapTile>;

}  // namespace alcedo::nn
