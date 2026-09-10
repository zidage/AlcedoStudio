//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <span>

#include "edit/geometry/types.hpp"
#include "edit/mask/brush_canonical_sampler.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {

/**
 * @brief Open-stroke sampler plus the size/strength/hardness used for new dabs.
 *
 * Owns remainder and the draft sample body for one paint or erase sequence. The
 * creation controller commits that body through AddMask or AppendBrushStroke.
 * Movement does not use this type. Thread: document-owning thread. Not thread-safe.
 */
class BrushMaskInput {
 public:
  /**
   * @brief Store radius/strength/hardness for the next dab or open-stroke boundary.
   *
   * When a stroke is open, emits a parameter-boundary sample at the current point.
   *
   * @throws std::runtime_error when a value fails canonical sample rules.
   */
  void SetStrokeParameters(float radius, float strength, float hardness);

  /**
   * @brief Paint or erase for the next @ref Begin. Ignored while a stroke is open.
   */
  void SetStrokeMode(BrushStrokeMode mode);

  /**
   * @brief Open a stroke at the press, emitting the first dab.
   *
   * @param id Stable StrokeId for this sequence. Must be unique on the target Brush.
   * @param reference Press in world reference pixels.
   * @param translation Current Brush placement; subtracted from every event.
   * @throws std::runtime_error when a stroke is already open or an input is invalid.
   */
  void Begin(StrokeId id, Vector2 reference, Vector2 translation);

  /**
   * @brief Walk to @p reference and emit interior dabs. Does not emit an event-end dab.
   *
   * @throws std::runtime_error when no stroke is open or @p reference is not finite.
   */
  void Append(Vector2 reference);

  /**
   * @brief Seal the stroke, emitting the endpoint once when it is not already a dab.
   *
   * @return Stroke sharing one immutable sample body. The session is then idle.
   * @throws std::runtime_error when no stroke is open.
   */
  [[nodiscard]] auto Finish() -> BrushStroke;

  /**
   * @brief Drop an unfinished stroke. Idle when already closed.
   */
  void Cancel();

  /**
   * @brief Open-stroke samples as a BrushStroke for live Mix. Empty when idle.
   *
   * Copies the draft vector into a new shared body. Committed strokes are not
   * included. Valid independently of later sampler mutations.
   */
  [[nodiscard]] auto DraftStroke() const -> std::optional<BrushStroke>;

  [[nodiscard]] auto IsOpen() const -> bool { return sampler_.IsOpen(); }
  [[nodiscard]] auto stroke_id() const -> const StrokeId& { return stroke_id_; }
  [[nodiscard]] auto mode() const -> BrushStrokeMode { return mode_; }
  [[nodiscard]] auto radius() const -> float { return radius_; }
  [[nodiscard]] auto strength() const -> float { return strength_; }
  [[nodiscard]] auto hardness() const -> float { return hardness_; }
  [[nodiscard]] auto DraftSamples() const -> std::span<const BrushCanonicalSample> {
    return sampler_.DraftSamples();
  }

 private:
  BrushCanonicalSampler sampler_;
  StrokeId              stroke_id_;
  BrushStrokeMode       mode_      = BrushStrokeMode::Paint;
  float                 radius_    = 1.0f;
  float                 strength_  = 1.0f;
  float                 hardness_  = 1.0f;
};

}  // namespace alcedo
