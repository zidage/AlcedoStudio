//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QCursor>
#include <QPointF>
#include <Qt>
#include <cstdint>
#include <optional>

namespace alcedo {

/**
 * @brief Pointer cursor for a viewer overlay control (Crop or Mask).
 *
 * Move is the four-way arrow. Rotate is a custom bitmap. Resize kinds follow
 * the on-screen drag axis of the control.
 */
enum class OverlayCursor : std::uint8_t {
  None             = 0,
  Move             = 1,
  Rotate           = 2,
  ResizeHorizontal = 3,
  ResizeVertical   = 4,
  /// Top-left to bottom-right ("\").
  ResizeDiagonalDown = 5,
  /// Bottom-left to top-right ("/").
  ResizeDiagonalUp = 6,
};

/**
 * @brief Marker for @ref OverlayCursor::Rotate in @c Qt::CursorShape channels.
 *
 * Qt has no rotate cursor shape. Interaction results carry this marker;
 * @c EditorOverlayItem shows @ref OverlayRotateCursor for it, and the viewport
 * HoverHandler must not apply the marker itself.
 */
inline constexpr Qt::CursorShape kOverlayRotateCursorShape = Qt::BitmapCursor;

/**
 * @brief Resize cursor for a drag along @p axis in item space (y down).
 *
 * The axis is quantized to the nearest of horizontal, vertical, and the two
 * diagonals. A zero or non-finite axis yields @ref OverlayCursor::None.
 */
[[nodiscard]] auto OverlayResizeCursorForAxis(QPointF axis) -> OverlayCursor;

/**
 * @brief Qt shape for @p cursor. Rotate maps to @ref kOverlayRotateCursorShape.
 *
 * @return Empty for @ref OverlayCursor::None.
 */
[[nodiscard]] auto OverlayCursorShape(OverlayCursor cursor) -> std::optional<Qt::CursorShape>;

/**
 * @brief Rotate cursor bitmap: a 270-degree arc with an arrowhead at each end.
 *
 * Thread: GUI (builds a QPixmap).
 */
[[nodiscard]] auto OverlayRotateCursor() -> QCursor;

}  // namespace alcedo
