//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/operators/models/operator_type_id.hpp"

namespace alcedo {

class IOperatorModel;
struct ResolvedRenderGeometry;

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

inline constexpr std::uint32_t kGradeNeighborMaxTapCount = 64;

/**
 * @brief Per-dispatch parameters for a separable neighborhood adjustment.
 *
 * This layout is shared verbatim with GPU neighborhood kernels. The horizontal pass writes the
 * filtered neighborhood to a scratch image. The vertical pass reads that scratch image while
 * retaining the unfiltered source image for the final operator formula.
 */
struct GradeNeighborParams {
  std::uint32_t behavior  = 0;
  std::uint32_t radius    = 0;
  std::uint32_t tap_count = 0;
  float         amount    = 0.0f;
  float         threshold = 0.0f;
  float         weights[kGradeNeighborMaxTapCount]{};
  std::uint32_t enabled = 0;
  std::uint32_t seed_lo = 0;
  std::uint32_t seed_hi = 0;
  float         sigma_x = 0.0f;
  float         sigma_y = 0.0f;
  float         redshift[3]{};
  float         render_to_reference[6]    = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  std::uint32_t use_reference_coordinates = 0;
  std::int32_t  reference_width           = 0;
  std::int32_t  reference_height          = 0;
};

static_assert(sizeof(GradeNeighborParams) == 344);

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
 * @brief Pack owner Model fields into the shared Grade GPU parameter slot.
 *
 * Reads only the fields the GPU layout needs under the Model lock. Does not copy a
 * full DTO. Throws when the Model type does not match @p behavior or the layout is
 * unsupported.
 */
[[nodiscard]] auto MakeGradeRuntimeParams(const IOperatorModel& model, AdjustmentBehavior behavior)
    -> GradeAdjustmentParams;

/**
 * @brief Build the original separable-neighborhood parameters for one compiled adjustment.
 *
 * Slider normalization, hidden effect defaults, Gaussian weights, and render-space scaling are
 * resolved on the CPU once per dispatch rather than recomputed for every GPU pixel. Spatial
 * radii and sigmas are specified in full-reference pixels and scaled by the current
 * render-to-reference mapping so preview, max-edge, and DecodeRes downscales keep the same
 * image-space neighborhood as a full-resolution render (the same reference-space rule LLF and
 * mask sampling use).
 */
[[nodiscard]] auto MakeGradeNeighborParams(const IOperatorModel& model, AdjustmentBehavior behavior,
                                           const ResolvedRenderGeometry& geometry)
    -> GradeNeighborParams;

}  // namespace alcedo
