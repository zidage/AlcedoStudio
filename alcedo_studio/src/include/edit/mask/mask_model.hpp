//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_id.hpp"
#include "json.hpp"

namespace alcedo {

/** @brief Discriminator for @ref MaskSource. */
enum class MaskSourceKind : std::uint8_t {
  Brush           = 0,
  Radial          = 1,
  LinearGradient  = 2,
};

/**
 * @brief Settled Brush coverage. A missing asset key is valid and yields zero coverage.
 *
 * @p descriptor must satisfy the raster-axis rule when @p asset_key is present.
 */
struct BrushMaskSource {
  std::optional<MaskAssetKey> asset_key;
  MaskAssetDescriptor         descriptor{};
  float                       feather_radius = 0.0f;

  friend auto operator==(const BrushMaskSource&, const BrushMaskSource&) -> bool = default;
};

/** @brief Radial ellipse in normalized reference space. */
struct RadialMaskSource {
  float center_x      = 0.5f;
  float center_y      = 0.5f;
  float major_radius  = 0.5f;
  float minor_radius  = 0.5f;
  float rotation      = 0.0f;
  float inner_feather = 0.0f;
  float outer_feather = 0.0f;

  friend auto operator==(const RadialMaskSource&, const RadialMaskSource&) -> bool = default;
};

/** @brief Linear Gradient coverage in normalized reference space. */
struct LinearGradientMaskSource {
  float origin_x            = 0.5f;
  float origin_y            = 0.5f;
  float normal_x            = 0.0f;
  float normal_y            = 1.0f;
  float transition_distance = 0.2f;
  float start_value         = 1.0f;
  float end_value           = 0.0f;

  friend auto operator==(const LinearGradientMaskSource&, const LinearGradientMaskSource&)
      -> bool = default;
};

using MaskSource = std::variant<BrushMaskSource, RadialMaskSource, LinearGradientMaskSource>;

/**
 * @brief Direct Color Range field. Only the disabled placeholder is supported.
 *
 * @p enabled must be false. Extra serialized fields are rejected.
 */
struct ColorRangeModel {
  bool enabled = false;

  friend auto operator==(const ColorRangeModel&, const ColorRangeModel&) -> bool = default;
};

/**
 * @brief Direct Luminance Range field. Only the disabled placeholder is supported.
 *
 * @p enabled must be false. Extra serialized fields are rejected.
 */
struct LuminanceRangeModel {
  bool enabled = false;

  friend auto operator==(const LuminanceRangeModel&, const LuminanceRangeModel&) -> bool = default;
};

/**
 * @brief One Color Grade Mask: identity, display metadata, source, and range fields.
 *
 * GPU-free. Invert and opacity belong to the Mask, not to the source variant.
 */
struct MaskModel {
  MaskId                             id;
  std::string                        display_name;
  bool                               enabled = true;
  float                              opacity = 1.0f;
  bool                               invert  = false;
  MaskSource                         source{RadialMaskSource{}};
  std::optional<ColorRangeModel>     color_range;
  std::optional<LuminanceRangeModel> luminance_range;

  friend auto operator==(const MaskModel&, const MaskModel&) -> bool = default;
};

/**
 * @brief Source discriminator for @p source.
 */
[[nodiscard]] auto GetMaskSourceKind(const MaskSource& source) -> MaskSourceKind;

/**
 * @brief JSON kind text for @p kind (`brush`, `radial`, `linear_gradient`).
 */
[[nodiscard]] auto MaskSourceKindText(MaskSourceKind kind) -> std::string_view;

/**
 * @brief Validate one Mask value. Does not inspect other Masks in a Grade.
 *
 * @param mask Candidate Mask.
 * @throws std::runtime_error when identity, numeric, source, descriptor, or range
 *         rules fail. Does not mutate @p mask.
 */
void ValidateMaskModel(const MaskModel& mask);

/**
 * @brief Serialize one Mask, including null range fields and an explicit source kind.
 */
[[nodiscard]] auto MaskModelToJson(const MaskModel& mask) -> nlohmann::json;

/**
 * @brief Read one Mask object. Requires an explicit source kind and both range keys.
 *
 * @throws std::runtime_error on missing fields, unknown kinds, extra range keys,
 *         enabled ranges, or invalid values.
 */
[[nodiscard]] auto MaskModelFromJson(const nlohmann::json& json) -> MaskModel;

/**
 * @brief True when @p masks contains a duplicate or empty @ref MaskId.
 */
[[nodiscard]] auto HasDuplicateOrEmptyMaskId(const std::vector<MaskModel>& masks) -> bool;

/**
 * @brief First enabled Mask in display order, or null when none are enabled.
 */
[[nodiscard]] auto FirstEnabledMask(std::span<const MaskModel> masks) -> const MaskModel*;

/**
 * @brief Packed Radial parameters for native analytic evaluators.
 *
 * Invert and opacity are copied from the Mask. Evaluators apply invert, then opacity,
 * then clamp to `[0, 1]` before R8 quantization.
 */
struct RadialMaskParams {
  float center_x      = 0.5f;
  float center_y      = 0.5f;
  float major_radius  = 0.5f;
  float minor_radius  = 0.5f;
  float rotation      = 0.0f;
  float inner_feather = 0.0f;
  float outer_feather = 0.0f;
  bool  invert        = false;
  float opacity       = 1.0f;
};

/**
 * @brief Packed Linear Gradient parameters for native analytic evaluators.
 *
 * Invert and opacity follow the same Mask-level order as Radial.
 */
struct LinearGradientMaskParams {
  float origin_x            = 0.5f;
  float origin_y            = 0.5f;
  float normal_x            = 0.0f;
  float normal_y            = 1.0f;
  float transition_distance = 0.2f;
  float start_value         = 1.0f;
  float end_value           = 0.0f;
  bool  invert              = false;
  float opacity             = 1.0f;
};

enum class AnalyticMaskKind : std::uint8_t {
  Radial         = 0,
  LinearGradient = 1,
};

[[nodiscard]] auto AnalyticKindFromMask(const MaskModel& mask) -> AnalyticMaskKind;
[[nodiscard]] auto RadialParamsFromMask(const MaskModel& mask) -> RadialMaskParams;
[[nodiscard]] auto LinearGradientParamsFromMask(const MaskModel& mask) -> LinearGradientMaskParams;

}  // namespace alcedo
