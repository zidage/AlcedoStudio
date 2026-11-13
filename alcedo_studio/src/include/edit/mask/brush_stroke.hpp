//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/mask/brush_raster_encoding.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Stable identity for one Brush stroke during its lifetime.
 *
 * Distinct from @ref MaskId. Empty values are invalid. Order of strokes is the
 * evaluation order; StrokeId is never used to reorder samples.
 */
class StrokeId {
 public:
  StrokeId() = default;
  explicit StrokeId(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] auto Value() const -> std::string_view { return value_; }
  [[nodiscard]] auto Empty() const -> bool { return value_.empty(); }

  friend auto operator==(const StrokeId& lhs, const StrokeId& rhs) -> bool {
    return lhs.value_ == rhs.value_;
  }
  friend auto operator!=(const StrokeId& lhs, const StrokeId& rhs) -> bool {
    return !(lhs == rhs);
  }
  friend auto operator<(const StrokeId& lhs, const StrokeId& rhs) -> bool {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::string value_;
};

/**
 * @brief One ordered Brush stroke: identity, paint/erase, and an immutable sample body.
 *
 * @p samples is shared, not copied per read or per translation. The vector is
 * immutable after @ref MakeBrushStroke. A null or empty body is invalid.
 */
struct BrushStroke {
  StrokeId                                                 id;
  BrushStrokeMode                                          mode = BrushStrokeMode::Paint;
  std::shared_ptr<const std::vector<BrushCanonicalSample>> samples;
};

/**
 * @brief Value equality for strokes. Compares sample contents, not pointer identity.
 */
[[nodiscard]] auto operator==(const BrushStroke& lhs, const BrushStroke& rhs) -> bool;
[[nodiscard]] inline auto operator!=(const BrushStroke& lhs, const BrushStroke& rhs) -> bool {
  return !(lhs == rhs);
}

/**
 * @brief True when @p lhs and @p rhs share the same immutable sample body pointer.
 *
 * Translation must keep this true for existing strokes. Distinct allocations with
 * equal sample values return false.
 */
[[nodiscard]] inline auto BrushStrokesShareSampleBody(const BrushStroke& lhs,
                                                      const BrushStroke& rhs) -> bool {
  return lhs.samples == rhs.samples;
}

/**
 * @brief Read-only view of @p stroke samples. Empty when the body pointer is null.
 */
[[nodiscard]] inline auto BrushStrokeSamples(const BrushStroke& stroke)
    -> std::span<const BrushCanonicalSample> {
  if (stroke.samples == nullptr) {
    return {};
  }
  return *stroke.samples;
}

/**
 * @brief Validate StrokeId, mode, and every canonical sample.
 *
 * @param stroke Candidate stroke. Not mutated.
 * @throws std::runtime_error when identity, mode, or sample encoding rules fail.
 */
void ValidateBrushStroke(const BrushStroke& stroke);

/**
 * @brief Validate ordered strokes, including unique non-empty StrokeIds.
 *
 * @throws std::runtime_error when any stroke is invalid or a StrokeId repeats.
 */
void ValidateBrushStrokeList(std::span<const BrushStroke> strokes);

/**
 * @brief Construct a stroke that owns one immutable sample body.
 *
 * @param id Non-empty StrokeId.
 * @param mode Paint or erase.
 * @param samples Non-empty canonical samples. Moved into the shared body.
 * @return Stroke sharing that body.
 * @throws std::runtime_error when validation fails. @p samples may be left
 *         moved-from; no owner is mutated by this factory.
 */
[[nodiscard]] auto MakeBrushStroke(StrokeId id, BrushStrokeMode mode,
                                   std::vector<BrushCanonicalSample> samples) -> BrushStroke;

/**
 * @brief Index of @p stroke_id in @p strokes, or @p strokes.size() when absent.
 */
[[nodiscard]] auto FindBrushStrokeIndex(std::span<const BrushStroke> strokes,
                                        const StrokeId& stroke_id) -> std::size_t;

/**
 * @brief Serialize one stroke as canonical JSON.
 *
 * Keys: `id`, `mode` (0 paint / 1 erase), `samples` (objects with local_x,
 * local_y, radius, strength, hardness).
 */
[[nodiscard]] auto BrushStrokeToJson(const BrushStroke& stroke) -> nlohmann::json;

/**
 * @brief Read one stroke object. Requires id, mode, and a non-empty samples array.
 *
 * @throws std::runtime_error on missing fields or invalid values.
 */
[[nodiscard]] auto BrushStrokeFromJson(const nlohmann::json& json) -> BrushStroke;

/**
 * @brief Serialize ordered strokes as a JSON array.
 */
[[nodiscard]] auto BrushStrokeListToJson(std::span<const BrushStroke> strokes) -> nlohmann::json;

/**
 * @brief Read an ordered stroke array. Duplicate StrokeIds are rejected.
 *
 * @throws std::runtime_error when the value is not an array or a stroke is invalid.
 */
[[nodiscard]] auto BrushStrokeListFromJson(const nlohmann::json& json) -> std::vector<BrushStroke>;

}  // namespace alcedo
