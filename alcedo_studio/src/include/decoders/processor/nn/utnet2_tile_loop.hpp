//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>

#include "decoders/processor/nn/overlap_tile_jobs.hpp"
#include "decoders/processor/nn/utnet2_specs.hpp"

// The tile loop of AI Denoise (integration plan, DN3): it denoises one packed Bayer mosaic in
// overlap tiles with a platform session of the FP16 UtNet2 graph (UtNet2DirectMlSession on
// Windows, UtNet2CoreMlSession on macOS, DN10), and stops a run on a device that is too slow
// (plan 2.4).

namespace alcedo::nn {

/// Camera RGB in float32 planes (3, height, width), row-major: the output image of a photo, at
/// the mosaic size (2 x the packed size).
struct RgbPlanesView {
  std::span<float> data;
  int              width  = 0;
  int              height = 0;
};

/// The part of a tile output that a run writes: the rectangle at (`source_x`, `source_y`) of
/// `width` x `height` output px of the tile output goes to (`destination_x`, `destination_y`)
/// of `destination`.
struct TileOutputCrop {
  int           source_x = 0;
  int           source_y = 0;
  int           width    = 0;
  int           height   = 0;
  RgbPlanesView destination;
  int           destination_x = 0;
  int           destination_y = 0;
};

/// The device memory of a session, for the tile size selection: the local video memory of the
/// adapter on Windows (DXGI), the physical memory on macOS (the Neural Engine uses the unified
/// memory).
struct DeviceMemoryState {
  /// The budget of the process (DXGI `Budget`; macOS: the physical memory).
  std::uint64_t budget_bytes = 0;
  /// The current use of the process, the session included (DXGI `CurrentUsage`; macOS: the
  /// physical memory that macOS does not count as available, of all processes).
  std::uint64_t usage_bytes  = 0;
  /// The part of `usage_bytes` that the session holds and reuses for its next tiles (its memory
  /// arena), 0 when the session is released.
  std::uint64_t held_bytes   = 0;
  /// The local video memory of the adapter (DXGI `DedicatedVideoMemory`). 0 for an adapter
  /// that has no memory of its own (UMA: an integrated GPU uses the system memory), and for a
  /// session that cannot read it; the selection then uses the budget of the process only.
  std::uint64_t adapter_local_bytes       = 0;
  /// The local video memory that all processes use on the adapter, the session included (the
  /// Windows performance counter `GPU Adapter Memory\Dedicated Usage`). The budget of a process
  /// does not count the memory of the other processes (issue #1043).
  std::uint64_t adapter_local_usage_bytes = 0;
};

/// The device memory that the tile loop can use: the budget of the process minus its current
/// use, and on an adapter with local memory at most the local memory that no process uses;
/// then plus the memory that the session holds (it reuses it).
[[nodiscard]] auto AvailableTileMemory(const DeviceMemoryState& memory) -> std::uint64_t;

/// The interface of a platform session for the tile loop. One call at a time.
class UtNet2TileSession {
 public:
  virtual ~UtNet2TileSession()                                               = default;

  /// Runs one tile. `input` holds the packed tile in float32 planes (4, tile.height, tile.width);
  /// the session writes the rectangle of `crop` of the camera RGB output (multiplied by the
  /// `output_scale` of the graph) into `crop.destination`. Throws `std::runtime_error` on a
  /// failure of the run and on an output value that is not finite in the written rectangle.
  virtual void               Run(std::span<const float> input, const PackedTileRect& tile,
                                 const TileOutputCrop& crop)                 = 0;

  /// True when a run of a tile of `width` x `height` packed px creates no session and compiles
  /// no graph, so that its time is the time of the device (the slow-device check measures it).
  [[nodiscard]] virtual auto IsReadyFor(int width, int height) const -> bool = 0;

  /// The device memory of the session (see DeviceMemoryState).
  [[nodiscard]] virtual auto DeviceMemory() const -> DeviceMemoryState       = 0;

  /// The device memory that a run needs for each tile size on this platform, largest first
  /// (`UtNet2Spec::kDirectMlTileMemory`, `UtNet2Spec::kCoreMlTileMemory`).
  [[nodiscard]] virtual auto TileMemory() const -> std::span<const UtNet2TileMemory> = 0;
};

/// The packed RGGB mosaic of a photo in float32 planes (4, height, width), row-major, input
/// preparation of `UtNet2Spec::kInputPreparationVersion`.
struct PackedMosaicView {
  std::span<const float> data;
  int                    width  = 0;
  int                    height = 0;
};

/// One tile after its run, given to the tile callback. The output of the tile is in the output
/// image at 2 x `kept`.
struct UtNet2TileResult {
  int            index      = 0;
  int            tile_count = 0;
  PackedTileRect input;
  PackedTileRect kept;
};

struct UtNet2TileLoopOptions {
  /// The tile input size in packed px. 0 selects it from the device memory
  /// (`SelectUtNet2TileSize` with `tile_memory`); another value must be a multiple of 16 larger
  /// than 2 x `overlap` (the tests use small tiles).
  int                               tile_size    = 0;
  int                               overlap      = UtNet2Spec::kTileOverlap;
  /// The table of the tile size selection; empty (the product) selects the table of the session
  /// (`UtNet2TileSession::TileMemory`).
  std::span<const UtNet2TileMemory> tile_memory  = {};
  /// `slow_device_limit` (plan 2.4, section 9) in seconds for each megapixel of the mosaic.
  double slow_device_limit_seconds_per_megapixel = UtNet2Spec::kSlowDeviceLimitSecondsPerMegapixel;
  /// The measured tiles before the first slow-device check
  /// (`UtNet2Spec::kSlowDeviceMeasuredTiles`).
  int    slow_device_measured_tiles              = UtNet2Spec::kSlowDeviceMeasuredTiles;
};

/// The record of one run of the tile loop.
struct UtNet2TileLoopRecord {
  /// True when the cancel check stopped the run; the output image is then incomplete.
  bool                     cancelled              = false;
  int                      tile_size              = 0;
  int                      tile_count             = 0;
  int                      tiles_run              = 0;
  /// The tiles whose time the slow-device check measured.
  int                      measured_tiles         = 0;
  /// The sum of the input areas of the tiles that ran, in packed px.
  std::int64_t             computed_packed_pixels = 0;
  /// From the start of the loop to its end, on the clock of the loop.
  std::chrono::nanoseconds loop_time{};
  /// The sum of the times of the `Run` calls, on the clock of the loop.
  std::chrono::nanoseconds run_time{};
};

using UtNet2TileCallback = std::function<void(const UtNet2TileResult&)>;
/// True when the render no longer needs the result; the loop then stops between tiles.
using UtNet2CancelCheck  = std::function<bool()>;
/// A steady clock: `std::chrono::steady_clock::now` in the product, a test clock in the tests.
using UtNet2SteadyClock  = std::function<std::chrono::steady_clock::time_point()>;

/// The largest tile size of `tile_memory` whose device memory is at most `available_bytes`.
/// Throws `std::runtime_error` (the memory error of plan 2.4) when the smallest does not fit.
[[nodiscard]] auto SelectUtNet2TileSize(std::span<const UtNet2TileMemory> tile_memory,
                                        std::uint64_t                     available_bytes) -> int;

/// Denoises `mosaic` into `output` (3, 2 mosaic.height, 2 mosaic.width) with `session`.
///
/// The loop selects the tile size (options), plans the overlap tiles (`PlanOverlapTiles`), and
/// for each tile copies the tile input from the mosaic (the part past the bottom and the right
/// border filled by reflection), runs it, and lets the session write the kept rectangle into
/// `output`; then it calls `on_tile` (it can be empty).
///
/// Between tiles, `cancel_requested` runs first: when it returns true the loop returns a record
/// with `cancelled` set. Then the slow-device check: a tile whose run was measured (the session
/// was ready for its size before the run, `UtNet2TileSession::IsReadyFor`) gives its time for
/// each kept output px; the projected time of the photo is the fastest of these times times the
/// output px of the photo. The check starts when `options.slow_device_measured_tiles` tiles were
/// measured, and it also runs after the last tile. When the projected time is above the limit
/// of `options` times the megapixels of the mosaic, the loop throws `DenoiseDeviceTooSlowError`
/// and runs no more tiles. A run without a measured tile has no check.
///
/// Throws `std::invalid_argument` for a view whose size does not match, a mosaic smaller than
/// 16 packed px on an axis, or tile options that the planner rejects; the errors of the session
/// and of `SelectUtNet2TileSize` pass through. The session stays with the caller, which keeps it
/// between runs and releases it.
auto               DenoiseBayerMosaic(UtNet2TileSession& session, const PackedMosaicView& mosaic,
                                      const RgbPlanesView& output, const UtNet2TileCallback& on_tile,
                                      const UtNet2CancelCheck& cancel_requested, const UtNet2SteadyClock& clock,
                                      const UtNet2TileLoopOptions& options = {}) -> UtNet2TileLoopRecord;

}  // namespace alcedo::nn
