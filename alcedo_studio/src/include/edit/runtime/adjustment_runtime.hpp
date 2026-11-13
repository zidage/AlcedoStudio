//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
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
  float         values[48]{};
  std::uint32_t reserved[2]{};
};

static_assert(sizeof(GradeAdjustmentParams) == 208);

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
  float         sigma_x = 0.0f;
  float         sigma_y = 0.0f;
  float         redshift[3]{};
};

static_assert(sizeof(GradeNeighborParams) == 300);

inline constexpr std::uint32_t kGradeRuntimeParamDirtyBit = 1;
inline constexpr std::uint32_t kGradeRuntimeParamBytes =
    static_cast<std::uint32_t>(sizeof(GradeAdjustmentParams));

/// Number of consecutive GradeAdjustmentParams records in a Curve slot: the RGB, red, green, and
/// blue curves, in the order of CurveChannel.
inline constexpr std::uint32_t kCurveGradeRecordCount = 4;

/**
 * @brief The GPU layout of a Curve grade slot: one record for each curve.
 *
 * The grade kernels read the record at the slot offset and the three records after it. Each
 * record has count control points with x in values[0..15], y in values[16..31], and the slope in
 * values[32..47]. count 0 (an identity red, green, or blue curve) leaves the channel unchanged.
 */
using CurveGradeRuntimeParams = std::array<GradeAdjustmentParams, kCurveGradeRecordCount>;

static_assert(sizeof(CurveGradeRuntimeParams) == kCurveGradeRecordCount * kGradeRuntimeParamBytes,
              "The Curve records are consecutive with no padding");

/// Byte size of the Grade slot of @p behavior: four records for Curve, one record otherwise.
[[nodiscard]] constexpr auto GradeRuntimeParamBytes(AdjustmentBehavior behavior) -> std::uint32_t {
  return behavior == AdjustmentBehavior::Curve
             ? static_cast<std::uint32_t>(sizeof(CurveGradeRuntimeParams))
             : kGradeRuntimeParamBytes;
}

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
 * unsupported. Curve has four records: use @ref MakeCurveGradeRuntimeParams.
 */
[[nodiscard]] auto MakeGradeRuntimeParams(const IOperatorModel& model, AdjustmentBehavior behavior)
    -> GradeAdjustmentParams;

/**
 * @brief Pack the four curves of a CurveModel into the records of its Grade slot.
 *
 * Solves the spline of each curve under the Model lock (tone_curve::SolveToneCurveSpline). The
 * RGB curve always has its points. A red, green, or blue curve that is the identity has count 0.
 *
 * @throws std::runtime_error when @p model is not a CurveModel.
 * @throws std::invalid_argument with the text of tone_curve::FindToneCurvePointsError when a
 *         curve has points that the spline does not accept.
 */
[[nodiscard]] auto MakeCurveGradeRuntimeParams(const IOperatorModel& model)
    -> CurveGradeRuntimeParams;

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

/**
 * @brief Vertical shared-memory radius for a separable neighborhood pass.
 *
 * Halation uses a 3-sigma window of @p params.sigma_y. Other operators use @p params.radius.
 */
[[nodiscard]] auto NeighborhoodVerticalRadius(const GradeNeighborParams& params) -> std::uint32_t;

}  // namespace alcedo
