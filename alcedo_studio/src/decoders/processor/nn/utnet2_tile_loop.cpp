//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "decoders/processor/nn/utnet2_tile_loop.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/denoise_errors.hpp"

namespace alcedo::nn {
namespace {

auto MebibyteText(std::uint64_t bytes) -> std::string {
  return std::to_string((bytes + (std::uint64_t{1} << 19)) >> 20) + " MiB";
}

auto TooSlowText(double projected_seconds, double limit_seconds, int tiles_run, int tile_count)
    -> std::string {
  std::ostringstream text;
  text.precision(3);
  text << "AI Denoise: the device is too slow: the projected time of the photo is "
       << projected_seconds << " s, the limit is " << limit_seconds << " s (stopped after "
       << tiles_run << " of " << tile_count << " tiles)";
  return text.str();
}

/// Copies the input of `tile` from `mosaic` into `input` (4, tile.height, tile.width). The rows
/// and columns past the bottom and the right border are the reflection of the image without
/// the border pixel (numpy and PyTorch "reflect"), as in the lab planner.
void CopyTileInput(const PackedMosaicView& mosaic, const PackedTileRect& tile,
                   std::vector<float>& input) {
  const auto plane_size = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  const auto tile_plane = static_cast<std::size_t>(tile.width) * tile.height;
  input.resize(UtNet2Spec::kInputChannels * tile_plane);
  const int inside_width = std::min(tile.width, mosaic.width - tile.x);
  for (int channel = 0; channel < UtNet2Spec::kInputChannels; ++channel) {
    const float* source_plane = mosaic.data.data() + channel * plane_size;
    float*       target_plane = input.data() + channel * tile_plane;
    for (int row = 0; row < tile.height; ++row) {
      int source_row = tile.y + row;
      if (source_row >= mosaic.height) {
        source_row = 2 * (mosaic.height - 1) - source_row;
      }
      const float* source = source_plane + static_cast<std::size_t>(source_row) * mosaic.width;
      float*       target = target_plane + static_cast<std::size_t>(row) * tile.width;
      std::copy_n(source + tile.x, inside_width, target);
      for (int column = inside_width; column < tile.width; ++column) {
        target[column] = source[2 * (mosaic.width - 1) - (tile.x + column)];
      }
    }
  }
}

}  // namespace

auto AvailableTileMemory(const DeviceMemoryState& memory) -> std::uint64_t {
  // The memory that the session holds is in both uses; it counts as available because the
  // session reuses it.
  const auto free_of = [&](std::uint64_t total, std::uint64_t used) {
    const auto others = used - std::min(memory.held_bytes, used);
    return total - std::min(others, total);
  };
  auto available = free_of(memory.budget_bytes, memory.usage_bytes);
  if (memory.adapter_local_bytes != 0) {
    available =
        std::min(available, free_of(memory.adapter_local_bytes, memory.adapter_local_usage_bytes));
  }
  return available;
}

DenoiseDeviceTooSlowError::DenoiseDeviceTooSlowError(double projected_seconds, double limit_seconds,
                                                     int tiles_run, int tile_count)
    : std::runtime_error(TooSlowText(projected_seconds, limit_seconds, tiles_run, tile_count)),
      projected_seconds_(projected_seconds),
      limit_seconds_(limit_seconds),
      tiles_run_(tiles_run),
      tile_count_(tile_count) {}

auto SelectUtNet2TileSize(std::span<const UtNet2TileMemory> tile_memory,
                          std::uint64_t                     available_bytes) -> int {
  const UtNet2TileMemory* selected = nullptr;
  const UtNet2TileMemory* smallest = nullptr;
  for (const auto& entry : tile_memory) {
    if (entry.device_bytes <= available_bytes &&
        (selected == nullptr || entry.tile_size > selected->tile_size)) {
      selected = &entry;
    }
    if (smallest == nullptr || entry.tile_size < smallest->tile_size) {
      smallest = &entry;
    }
  }
  if (smallest == nullptr) {
    throw std::invalid_argument("AI Denoise: the tile memory table is empty");
  }
  if (selected == nullptr) {
    throw std::runtime_error("AI Denoise: not enough device memory: tiles of " +
                             std::to_string(smallest->tile_size) + " packed px need " +
                             MebibyteText(smallest->device_bytes) + ", " +
                             MebibyteText(available_bytes) + " are free");
  }
  return selected->tile_size;
}

auto DenoiseBayerMosaic(UtNet2TileSession& session, const PackedMosaicView& mosaic,
                        const RgbPlanesView& output, const UtNet2TileCallback& on_tile,
                        const UtNet2CancelCheck& cancel_requested, const UtNet2SteadyClock& clock,
                        const UtNet2TileLoopOptions& options) -> UtNet2TileLoopRecord {
  constexpr int kAlignment = UtNet2Spec::kSizeAlignment;
  constexpr int kUpscale   = UtNet2Spec::kOutputUpscale;
  // Reflection fills at most kAlignment - 1 px, so each axis needs kAlignment px.
  if (mosaic.width < kAlignment || mosaic.height < kAlignment) {
    throw std::invalid_argument("AI Denoise: the packed mosaic of " + std::to_string(mosaic.width) +
                                " x " + std::to_string(mosaic.height) + " px is smaller than " +
                                std::to_string(kAlignment) + " px on an axis");
  }
  const auto plane_size = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  if (mosaic.data.size() != UtNet2Spec::kInputChannels * plane_size ||
      output.width != kUpscale * mosaic.width || output.height != kUpscale * mosaic.height ||
      output.data.size() !=
          UtNet2Spec::kOutputChannels * static_cast<std::size_t>(output.width) * output.height) {
    throw std::invalid_argument(
        "AI Denoise: the mosaic or the output view does not match the "
        "packed size " +
        std::to_string(mosaic.width) + " x " + std::to_string(mosaic.height));
  }

  const auto           started = clock();
  UtNet2TileLoopRecord record;
  if (options.tile_size != 0) {
    record.tile_size = options.tile_size;
  } else {
    const auto table = options.tile_memory.empty() ? session.TileMemory() : options.tile_memory;
    record.tile_size = SelectUtNet2TileSize(table, AvailableTileMemory(session.DeviceMemory()));
  }
  const auto tiles =
      PlanOverlapTiles(mosaic.width, mosaic.height, record.tile_size, options.overlap, kAlignment);
  record.tile_count          = static_cast<int>(tiles.size());

  const double output_pixels = static_cast<double>(output.width) * output.height;
  const double limit_seconds =
      options.slow_device_limit_seconds_per_megapixel * output_pixels / 1e6;
  // The fastest time for each kept output px of the measured tiles. Another process that shares
  // the GPU or pages its memory slows single tiles of a fast device (issue #1043); a device that
  // is too slow is slow in each tile.
  double     fastest_seconds_per_pixel = 0.0;
  const auto check_slow_device         = [&](bool last) {
    if (record.measured_tiles == 0 ||
        (!last && record.measured_tiles < options.slow_device_measured_tiles)) {
      return;
    }
    const double projected = fastest_seconds_per_pixel * output_pixels;
    if (projected > limit_seconds) {
      throw DenoiseDeviceTooSlowError(projected, limit_seconds, record.tiles_run,
                                      record.tile_count);
    }
  };

  std::vector<float> input;
  for (const auto& tile : tiles) {
    if (cancel_requested && cancel_requested()) {
      record.cancelled = true;
      record.loop_time = clock() - started;
      return record;
    }
    check_slow_device(false);

    CopyTileInput(mosaic, tile.input, input);
    const TileOutputCrop crop{
        .source_x      = kUpscale * (tile.kept.x - tile.input.x),
        .source_y      = kUpscale * (tile.kept.y - tile.input.y),
        .width         = kUpscale * tile.kept.width,
        .height        = kUpscale * tile.kept.height,
        .destination   = output,
        .destination_x = kUpscale * tile.kept.x,
        .destination_y = kUpscale * tile.kept.y,
    };
    const bool measured  = session.IsReadyFor(tile.input.width, tile.input.height);
    const auto run_start = clock();
    session.Run(input, tile.input, crop);
    const auto run_time = clock() - run_start;

    record.run_time += run_time;
    record.computed_packed_pixels += std::int64_t{tile.input.width} * tile.input.height;
    ++record.tiles_run;
    if (measured) {
      const double seconds_per_pixel = std::chrono::duration<double>(run_time).count() /
                                       (static_cast<double>(crop.width) * crop.height);
      fastest_seconds_per_pixel = record.measured_tiles == 0
                                      ? seconds_per_pixel
                                      : std::min(fastest_seconds_per_pixel, seconds_per_pixel);
      ++record.measured_tiles;
    }
    if (on_tile) {
      on_tile(UtNet2TileResult{.index      = record.tiles_run - 1,
                               .tile_count = record.tile_count,
                               .input      = tile.input,
                               .kept       = tile.kept});
    }
  }
  check_slow_device(true);
  record.loop_time = clock() - started;
  return record;
}

}  // namespace alcedo::nn
