//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_adjustment_runtime.hpp"

#include <stdexcept>

#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

auto ResolveCudaAdjustmentBehavior(const OperatorTypeId& type) -> CudaAdjustmentBehavior {
  using enum CudaAdjustmentBehavior;
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
  throw std::runtime_error("CUDA primary grade: unregistered adjustment type");
}

auto IsCudaLocalToneBehavior(CudaAdjustmentBehavior behavior) -> bool {
  return behavior == CudaAdjustmentBehavior::Shadows ||
         behavior == CudaAdjustmentBehavior::Highlights ||
         behavior == CudaAdjustmentBehavior::Clarity;
}

}  // namespace alcedo
