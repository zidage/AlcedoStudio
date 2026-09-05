//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class ColorManager {
 public:
  /// Apply @p config to the CAMetalLayer owned by an AppKit window or view.
  /// @param native_view_or_window NSWindow or NSView pointer. Null, Qt dummy
  ///        `winId()` values, and other non-AppKit objects return false and
  ///        leave display state unchanged.
  /// @param config Viewer output color space and transfer. SDR values clear HDR
  ///        EDR metadata on the layer.
  /// @return true when a CAMetalLayer received the color space.
  /// @thread The AppKit/GUI thread that owns the native object.
  static auto ApplyWindowColorSpace(void* native_view_or_window,
                                    const ViewerDisplayConfig& config) -> bool;
};

}  // namespace alcedo
