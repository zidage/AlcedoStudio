//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

class IOperatorModel;

/** @brief Backend-neutral Grade adjustment semantic. CUDA and Metal share this list. */
enum class AdjustmentBehavior : std::uint32_t {
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

struct alignas(16) GradeAdjustmentParams {
  std::uint32_t behavior = 0;
  std::uint32_t count    = 0;
  float         values[30]{};
};

struct GradeAdjustmentCommand {
  std::uint32_t parameter_offset = 0;
};

inline constexpr std::uint32_t kGradeRuntimeParamDirtyBit = 1;
inline constexpr std::uint32_t kGradeRuntimeParamBytes =
    static_cast<std::uint32_t>(sizeof(GradeAdjustmentParams));

/**
 * @brief Resolve a built-in Model type to its Grade runtime behavior.
 * @return nullopt when the type has no GPU grade implementation (legacy Tint, etc.).
 */
[[nodiscard]] auto TryResolveAdjustmentBehavior(const OperatorTypeId& type)
    -> std::optional<AdjustmentBehavior>;

/**
 * @brief Resolve a built-in Model type to its Grade runtime behavior.
 * @throws std::runtime_error when the type has no GPU grade implementation.
 */
[[nodiscard]] auto ResolveAdjustmentBehavior(const OperatorTypeId& type) -> AdjustmentBehavior;

[[nodiscard]] auto IsLocalToneBehavior(AdjustmentBehavior behavior) -> bool;
[[nodiscard]] auto IsNeighborhoodBehavior(AdjustmentBehavior behavior) -> bool;

/**
 * @brief Pack a Model DTO into the shared Grade GPU parameter slot.
 * @throws std::runtime_error when the payload layout is not supported.
 */
[[nodiscard]] auto MakeGradeRuntimeParams(const IOperatorModel& model, AdjustmentBehavior behavior)
    -> GradeAdjustmentParams;

}  // namespace alcedo
