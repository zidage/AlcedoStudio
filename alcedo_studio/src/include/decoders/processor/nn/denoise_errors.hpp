//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>

namespace alcedo::nn {

/// The tile loop of AI Denoise stopped because the device is too slow (integration plan, 2.4):
/// after a measured tile, the projected time of the photo was above `slow_device_limit` times the
/// megapixels of the photo. A separate type, so that the owner of the session (DN6) keeps the
/// result for the device and the editor (DN7) shows the slow-device text.
class DenoiseDeviceTooSlowError : public std::runtime_error {
 public:
  /// `projected_seconds`: the projected time of the whole photo; `limit_seconds`: the allowed
  /// time of the photo; `tiles_run`: the tiles that ran before the stop.
  DenoiseDeviceTooSlowError(double projected_seconds, double limit_seconds, int tiles_run,
                            int tile_count);

  [[nodiscard]] auto ProjectedSeconds() const -> double { return projected_seconds_; }
  [[nodiscard]] auto LimitSeconds() const -> double { return limit_seconds_; }
  [[nodiscard]] auto TilesRun() const -> int { return tiles_run_; }
  [[nodiscard]] auto TileCount() const -> int { return tile_count_; }

 private:
  double projected_seconds_;
  double limit_seconds_;
  int    tiles_run_;
  int    tile_count_;
};

}  // namespace alcedo::nn
