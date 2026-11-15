//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_model_base.hpp"

namespace alcedo {

struct CurvePoint {
  float x = 0.0f;
  float y = 0.0f;
};

inline auto operator==(const CurvePoint& lhs, const CurvePoint& rhs) -> bool {
  return lhs.x == rhs.x && lhs.y == rhs.y;
}

/**
 * @brief The four curves of the Curve adjustment, in the order that the render applies them.
 *
 * The RGB curve applies to the three channels. Then the red, green, and blue curves apply to
 * their channel.
 */
enum class CurveChannel : std::uint8_t {
  Rgb,
  Red,
  Green,
  Blue,
};

inline constexpr std::size_t kCurveChannelCount = 4;

inline constexpr std::array<CurveChannel, kCurveChannelCount> kCurveChannels{
    CurveChannel::Rgb, CurveChannel::Red, CurveChannel::Green, CurveChannel::Blue};

/// Key of the curve in the Model JSON and in a parameter write: `points` for the RGB curve (the
/// key from before the channel curves), `red`, `green`, and `blue`.
[[nodiscard]] auto CurveChannelKey(CurveChannel channel) -> std::string_view;

/// The identity curve (0, 0) - (1, 1).
[[nodiscard]] auto IdentityCurvePoints() -> std::vector<CurvePoint>;

/// True when @p points are exactly the two points of @ref IdentityCurvePoints.
[[nodiscard]] auto IsIdentityCurve(const std::vector<CurvePoint>& points) -> bool;

struct CurvePayload {
  /// The RGB curve.
  std::vector<CurvePoint> points{{0.0f, 0.0f}, {1.0f, 1.0f}};
  std::vector<CurvePoint> red{{0.0f, 0.0f}, {1.0f, 1.0f}};
  std::vector<CurvePoint> green{{0.0f, 0.0f}, {1.0f, 1.0f}};
  std::vector<CurvePoint> blue{{0.0f, 0.0f}, {1.0f, 1.0f}};
};

/// The points of @p channel in @p payload.
[[nodiscard]] auto CurveChannelPoints(const CurvePayload& payload, CurveChannel channel)
    -> const std::vector<CurvePoint>&;

/**
 * @brief Replacement points for some of the four curves of one Curve Model.
 *
 * A curve without a value stays as it is. The update is one Model change: one revision.
 */
struct CurveUpdate {
  /// The RGB curve.
  std::optional<std::vector<CurvePoint>> points;
  std::optional<std::vector<CurvePoint>> red;
  std::optional<std::vector<CurvePoint>> green;
  std::optional<std::vector<CurvePoint>> blue;

  [[nodiscard]] auto Channel(CurveChannel channel) -> std::optional<std::vector<CurvePoint>>&;
  [[nodiscard]] auto Channel(CurveChannel channel) const
      -> const std::optional<std::vector<CurvePoint>>&;
};

enum class CurveDirty : std::uint32_t {
  None   = 0,
  Points = 1U << 0,
  Red    = 1U << 1,
  Green  = 1U << 2,
  Blue   = 1U << 3,
  All    = Points | Red | Green | Blue,
};

/**
 * @brief Tone curve control points in the unit square: the RGB curve and the red, green, and
 * blue curves. Each curve is the identity (0,0)-(1,1) by default.
 *
 * The Model JSON is `{"points": [...]}` for the RGB curve, as before the channel curves, and
 * `"red"`, `"green"`, and `"blue"` only for a channel curve that is not the identity. A document
 * from before the channel curves encodes to the same bytes.
 */
class CurveModel final : public OperatorModelBase<CurveModel, CurvePayload, CurveDirty> {
 public:
  static auto                       TypeId() -> const OperatorTypeId& { return type_ids::Curve(); }
  static constexpr std::string_view kInstanceSuffix = "curve";

  /// True only when the four curves are the identity.
  [[nodiscard]] auto                IsDefault() const -> bool override;

  /// Set the RGB curve. Fewer than two points set the identity.
  void                              SetPoints(std::vector<CurvePoint> points);
  /// The RGB curve.
  [[nodiscard]] auto                Points() const -> std::vector<CurvePoint>;

  /**
   * @brief Set the curves that @p update has, in one Model change.
   *
   * A curve with fewer than two points becomes the identity. The other curves do not change.
   */
  void                              ApplyUpdate(CurveUpdate update);
  /// The points of @p channel.
  [[nodiscard]] auto ChannelPoints(CurveChannel channel) const -> std::vector<CurvePoint>;

  [[nodiscard]] auto                ToJson() const -> nlohmann::json override;
  /// Load the complete state: a channel key that @p json does not have is the identity.
  void                              LoadJson(const nlohmann::json& json) override;
};

}  // namespace alcedo
