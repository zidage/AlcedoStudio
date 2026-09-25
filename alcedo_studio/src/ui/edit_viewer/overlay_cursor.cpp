//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/overlay_cursor.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <cmath>

namespace alcedo {
namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] auto BuildRotateCursor() -> QCursor {
  // White over a dark outline, like the platform resize cursors.
  constexpr int    kLogicalSize = 24;
  constexpr qreal  kDpr         = 2.0;
  constexpr double kCenter      = kLogicalSize * 0.5;
  constexpr double kRadius      = 6.5;
  constexpr double kArrow       = 3.5;
  constexpr double kStart       = 135.0 * kPi / 180.0;
  constexpr double kSweep       = 270.0 * kPi / 180.0;
  constexpr int    kSegments    = 48;

  const auto on_arc = [&](double theta) {
    return QPointF(kCenter + kRadius * std::cos(theta), kCenter + kRadius * std::sin(theta));
  };
  QPainterPath arc(on_arc(kStart));
  for (int i = 1; i <= kSegments; ++i) {
    arc.lineTo(on_arc(kStart + kSweep * i / kSegments));
  }
  // Arrowheads point along the arc tangent, outward at both ends.
  const auto arrowhead = [&](double theta, double direction) {
    const QPointF base = on_arc(theta);
    const QPointF tangent(-std::sin(theta) * direction, std::cos(theta) * direction);
    const QPointF radial(std::cos(theta), std::sin(theta));
    QPainterPath head(base + tangent * kArrow);
    head.lineTo(base + radial * kArrow);
    head.lineTo(base - radial * kArrow);
    head.closeSubpath();
    return head;
  };
  const QPainterPath head_start = arrowhead(kStart, -1.0);
  const QPainterPath head_end   = arrowhead(kStart + kSweep, 1.0);

  QPixmap pixmap(static_cast<int>(kLogicalSize * kDpr), static_cast<int>(kLogicalSize * kDpr));
  pixmap.setDevicePixelRatio(kDpr);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(Qt::black, 4.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(arc);
  painter.setBrush(Qt::black);
  painter.setPen(QPen(Qt::black, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(head_start);
  painter.drawPath(head_end);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(arc);
  painter.setPen(Qt::NoPen);
  painter.setBrush(Qt::white);
  painter.drawPath(head_start);
  painter.drawPath(head_end);
  painter.end();
  // Negative hotspot = pixmap center.
  return QCursor(pixmap, -1, -1);
}

}  // namespace

auto OverlayResizeCursorForAxis(QPointF axis) -> OverlayCursor {
  if (!std::isfinite(axis.x()) || !std::isfinite(axis.y()) ||
      std::hypot(axis.x(), axis.y()) < 1.0e-5) {
    return OverlayCursor::None;
  }
  // Fold to [0, 180) degrees in item space (y down), then bucket by 45.
  double degrees = std::atan2(axis.y(), axis.x()) * 180.0 / kPi;
  if (degrees < 0.0) {
    degrees += 180.0;
  }
  if (degrees < 22.5 || degrees >= 157.5) {
    return OverlayCursor::ResizeHorizontal;
  }
  if (degrees < 67.5) {
    return OverlayCursor::ResizeDiagonalDown;
  }
  if (degrees < 112.5) {
    return OverlayCursor::ResizeVertical;
  }
  return OverlayCursor::ResizeDiagonalUp;
}

auto OverlayCursorShape(OverlayCursor cursor) -> std::optional<Qt::CursorShape> {
  switch (cursor) {
    case OverlayCursor::None:
      return std::nullopt;
    case OverlayCursor::Move:
      return Qt::SizeAllCursor;
    case OverlayCursor::Rotate:
      return kOverlayRotateCursorShape;
    case OverlayCursor::ResizeHorizontal:
      return Qt::SizeHorCursor;
    case OverlayCursor::ResizeVertical:
      return Qt::SizeVerCursor;
    case OverlayCursor::ResizeDiagonalDown:
      return Qt::SizeFDiagCursor;
    case OverlayCursor::ResizeDiagonalUp:
      return Qt::SizeBDiagCursor;
  }
  return std::nullopt;
}

auto OverlayRotateCursor() -> QCursor {
  static const QCursor cursor = BuildRotateCursor();
  return cursor;
}

}  // namespace alcedo
