//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo::type_ids {

inline auto Exposure() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.exposure"};
  return id;
}
inline auto Contrast() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.contrast"};
  return id;
}
inline auto White() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.white"};
  return id;
}
inline auto Black() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.black"};
  return id;
}
inline auto Shadows() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.shadows"};
  return id;
}
inline auto Highlights() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.highlights"};
  return id;
}
inline auto Saturation() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.saturation"};
  return id;
}
inline auto Vibrance() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.vibrance"};
  return id;
}
inline auto Clarity() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.clarity"};
  return id;
}
inline auto Halation() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.halation"};
  return id;
}
inline auto FilmGrain() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.film_grain"};
  return id;
}
inline auto Tint() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.tint"};
  return id;
}
inline auto Cat02WhiteBalance() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.cat02_white_balance"};
  return id;
}
inline auto Curve() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.curve"};
  return id;
}
inline auto Hls() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.hls"};
  return id;
}
inline auto ColorWheel() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.color_wheel"};
  return id;
}
inline auto Lmt() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.lmt"};
  return id;
}
inline auto Sharpen() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.adjustment.sharpen"};
  return id;
}

inline auto DevelopNode() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.node.develop"};
  return id;
}
inline auto ColorGradeNode() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.node.color_grade"};
  return id;
}
inline auto DrtNode() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.node.drt"};
  return id;
}
/// Historical type text. Documents that still use this node type are rejected.
inline auto AnalyticMaskNode() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.node.analytic_mask"};
  return id;
}
/// Historical type text. Documents that still use this node type are rejected.
inline auto RasterMaskNode() -> const OperatorTypeId& {
  static const OperatorTypeId id{"alcedo.node.raster_mask"};
  return id;
}

}  // namespace alcedo::type_ids
