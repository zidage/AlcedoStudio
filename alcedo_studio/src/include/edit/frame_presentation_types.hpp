//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// Shared frame-role and viewport geometry types used by the app-layer render
/// coordinator and by UI presentation sinks. Kept outside `ui/` so application
/// services do not include UI headers (Phase 5A-Fix).

#include <string_view>

namespace alcedo {

/// Leading text of the render error a presenter raises when its sink refused the
/// write because the viewport is suspended (window minimized or hidden). The
/// render completion carries only a message, so the coordinator reads this
/// prefix to report the frame as PresentationSuspended instead of Failed.
inline constexpr std::string_view kPresentationSuspendedMessage = "Presentation suspended: ";

[[nodiscard]] inline auto IsPresentationSuspendedMessage(std::string_view message) -> bool {
  return message.starts_with(kPresentationSuspendedMessage);
}

/// Sub-rectangle of the full frame that matches the current viewport mapping.
struct ViewportRenderRegion {
  int   x_               = 0;
  int   y_               = 0;
  float scale_x_         = 1.0f;
  float scale_y_         = 1.0f;
  int   reference_width_ = 0;
  int   reference_height_ = 0;
  int   target_width_    = 0;
  int   target_height_   = 0;
};

/// Which presentation layer a rendered frame belongs to.
enum class FrameRole {
  InteractivePrimary,
  QualityBase,
  DetailPatch,
};

}  // namespace alcedo
