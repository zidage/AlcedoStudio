//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/adjustment_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kMaxCurvePoints = 8;

}  // namespace

auto TryResolveAdjustmentBehavior(const OperatorTypeId& type) -> std::optional<AdjustmentBehavior> {
  using enum AdjustmentBehavior;
  if (type == type_ids::Cat02WhiteBalance()) return Cat02WhiteBalance;
  if (type == type_ids::Exposure()) return Exposure;
  if (type == type_ids::Contrast()) return Contrast;
  if (type == type_ids::White()) return White;
  if (type == type_ids::Black()) return Black;
  if (type == type_ids::Shadows()) return Shadows;
  if (type == type_ids::Highlights()) return Highlights;
  if (type == type_ids::Curve()) return Curve;
  if (type == type_ids::Hls()) return Hls;
  if (type == type_ids::Saturation()) return Saturation;
  if (type == type_ids::Vibrance()) return Vibrance;
  if (type == type_ids::ColorWheel()) return ColorWheel;
  if (type == type_ids::Lmt()) return Lmt;
  if (type == type_ids::Clarity()) return Clarity;
  if (type == type_ids::Sharpen()) return Sharpen;
  if (type == type_ids::Halation()) return Halation;
  if (type == type_ids::FilmGrain()) return FilmGrain;
  return std::nullopt;
}

auto ResolveAdjustmentBehavior(const OperatorTypeId& type) -> AdjustmentBehavior {
  if (auto behavior = TryResolveAdjustmentBehavior(type)) {
    return *behavior;
  }
  throw std::runtime_error("Primary grade: unregistered adjustment type '" +
                           std::string{type.Text()} + "'");
}

auto IsLocalToneBehavior(AdjustmentBehavior behavior) -> bool {
  return behavior == AdjustmentBehavior::Shadows || behavior == AdjustmentBehavior::Highlights;
}

auto IsNeighborhoodBehavior(AdjustmentBehavior behavior) -> bool {
  return behavior == AdjustmentBehavior::Clarity || behavior == AdjustmentBehavior::Sharpen ||
         behavior == AdjustmentBehavior::Halation || behavior == AdjustmentBehavior::FilmGrain;
}

auto MakeGradeRuntimeParams(const IOperatorModel& model, AdjustmentBehavior behavior)
    -> GradeAdjustmentParams {
  GradeAdjustmentParams result;
  result.behavior = static_cast<std::uint32_t>(behavior);
  const auto dto  = model.MakeFullDto();

  if (const auto* p = PayloadAs<ScalarFloatPayload>(dto.payload.get())) {
    result.values[0] = p->value;
    return result;
  }
  if (const auto* p = PayloadAs<Cat02WhiteBalancePayload>(dto.payload.get())) {
    result.values[0] = p->enabled ? 1.0f : 0.0f;
    result.values[1] = p->temperature_offset;
    result.values[2] = p->tint_offset;
    return result;
  }
  if (const auto* p = PayloadAs<CurvePayload>(dto.payload.get())) {
    result.count = static_cast<std::uint32_t>(std::min(p->points.size(), kMaxCurvePoints));
    for (std::uint32_t i = 0; i < result.count; ++i) {
      result.values[i * 2]     = p->points[i].x;
      result.values[i * 2 + 1] = p->points[i].y;
    }
    return result;
  }
  if (const auto* p = PayloadAs<HlsPayload>(dto.payload.get())) {
    for (int i = 0; i < kHlsHueBinCount; ++i) {
      result.values[i]      = p->hls_adj_table[static_cast<std::size_t>(i)].h;
      result.values[8 + i]  = p->hls_adj_table[static_cast<std::size_t>(i)].l;
      result.values[16 + i] = p->hls_adj_table[static_cast<std::size_t>(i)].s;
    }
    return result;
  }
  if (const auto* p = PayloadAs<ColorWheelPayload>(dto.payload.get())) {
    const ColorWheelControl controls[3] = {p->lift, p->gamma, p->gain};
    for (int i = 0; i < 3; ++i) {
      result.values[i * 4]     = controls[i].color_offset.x;
      result.values[i * 4 + 1] = controls[i].color_offset.y;
      result.values[i * 4 + 2] = controls[i].color_offset.z;
      result.values[i * 4 + 3] = controls[i].luminance_offset;
    }
    return result;
  }
  if (const auto* p = PayloadAs<SharpenPayload>(dto.payload.get())) {
    result.values[0] = p->amount;
    result.values[1] = p->radius;
    result.values[2] = p->threshold;
    return result;
  }
  if (const auto* p = PayloadAs<LmtPayload>(dto.payload.get())) {
    result.values[0] = p->cube_path.empty() ? 0.0f : 1.0f;
    return result;
  }
  throw std::runtime_error("Primary grade: Model DTO is not supported");
}

}  // namespace alcedo
