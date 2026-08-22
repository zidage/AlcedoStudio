//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

enum class CudaAdjustmentBehavior : std::uint32_t {
  Cat02WhiteBalance,
  Exposure,
  Contrast,
  White,
  Black,
  Shadows,
  Highlights,
  Curve,
  Hls,
  Saturation,
  Vibrance,
  ColorWheel,
  Lmt,
  Clarity,
  Sharpen,
  Halation,
  FilmGrain,
};

/**
 * @brief Resolve a built-in Model type to its CUDA runtime behavior.
 * @return nullopt when the type has no CUDA grade implementation (legacy Tint, etc.).
 */
[[nodiscard]] auto TryResolveCudaAdjustmentBehavior(const OperatorTypeId& type)
    -> std::optional<CudaAdjustmentBehavior>;

/**
 * @brief Resolve a built-in Model type to its CUDA runtime behavior.
 * @throws std::runtime_error when G5 has no CUDA implementation for the type.
 */
[[nodiscard]] auto ResolveCudaAdjustmentBehavior(const OperatorTypeId& type)
    -> CudaAdjustmentBehavior;

[[nodiscard]] auto IsCudaLocalToneBehavior(CudaAdjustmentBehavior behavior) -> bool;

}  // namespace alcedo
