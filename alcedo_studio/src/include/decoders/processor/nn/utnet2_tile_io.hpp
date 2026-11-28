//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "decoders/processor/nn/overlap_tile_jobs.hpp"
#include "decoders/processor/nn/utnet2_tile_loop.hpp"

// The tile input and output of the platform sessions of the FP16 UtNet2 graph (integration plan,
// DN2 and DN10): UtNet2DirectMlSession (Windows) and UtNet2CoreMlSession (macOS) check a tile
// run, convert the float32 tile input to the float16 input of the graph, and write the float16
// output rectangle of a run into the float32 output image with the same code.

namespace alcedo::nn {

/// "UtNet2 tile at (x, y) of w x h packed px", the start of each run error text.
[[nodiscard]] auto UtNet2TileText(const PackedTileRect& tile) -> std::string;

/// Checks a run of `tile` before it starts: the height and the width are positive multiples of
/// `UtNet2Spec::kSizeAlignment`, `input` holds (4, tile.height, tile.width) values, and the
/// rectangle of `crop` is inside the tile output and inside its destination, whose data has
/// the size of its planes. Throws `std::runtime_error` that names the tile.
void               CheckUtNet2TileRun(std::span<const float> input, const PackedTileRect& tile,
                                      const TileOutputCrop& crop);

/// Converts the float32 tile input (4, tile.height, tile.width) to float16 into
/// `float16_destination` (the same layout, 2 bytes for each value), the rows in parallel on the
/// threads of `cv::parallel_for_`.
void               ConvertUtNet2TileInput(std::span<const float> input, const PackedTileRect& tile,
                                          void* float16_destination);

/// The float16 output rows of a run that hold the rectangle of a crop: row `r` of channel `c`
/// (r from 0 to crop.height - 1, output row crop.source_y + r) starts at
/// `first_row + c * plane_bytes + r * row_bytes`, and each row holds the 2 x tile.width values
/// of the tile output.
struct UtNet2Float16OutputRows {
  const std::uint8_t* first_row   = nullptr;
  std::size_t         row_bytes   = 0;
  std::size_t         plane_bytes = 0;
};

/// Writes the rectangle of `crop` from `source` into `crop.destination` as float32 times
/// `output_scale`, the rows of the three channels in parallel, and checks the written values.
/// Throws `std::runtime_error` that names the tile, the channel, and the output row and column of
/// the first value that is not finite (FP16 overflow); the destination rectangle is then
/// unspecified.
void WriteUtNet2OutputCrop(const UtNet2Float16OutputRows& source, const PackedTileRect& tile,
                           const TileOutputCrop& crop, float output_scale);

}  // namespace alcedo::nn
