//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/mask/brush_raster_encoding.hpp"

namespace alcedo {

/**
 * @brief Converts ordered reference-pixel pointer events into canonical Brush samples.
 *
 * Owns the open-stroke remainder so dab spacing is independent of event batching.
 * Samples are stored in local reference pixels (`reference - translation`). The first
 * press is recorded as-is; drag thresholds are not applied here.
 *
 * Thread: serial owner worker. Not thread-safe. Cancel discards the open stroke
 * without returning samples.
 */
class BrushCanonicalSampler {
 public:
  /**
   * @brief Open a stroke and emit the press dab.
   *
   * @param mode Paint or erase.
   * @param reference Press position in reference pixels.
   * @param translation Current Brush placement; subtracted from every event.
   * @param radius Positive dab radius in reference pixels.
   * @param strength Coverage strength in `[0, 1]`.
   * @param hardness Edge hardness in `[0, 1]`.
   * @throws std::runtime_error when a stroke is already open or a value is invalid.
   */
  void BeginStroke(BrushStrokeMode mode, Vector2 reference, Vector2 translation, float radius,
                   float strength, float hardness);

  /**
   * @brief Record a size/strength/hardness change at the current position.
   *
   * Emits a boundary sample so the change is not discarded across later events.
   * Subsequent spacing uses the new radius. No-op when values are unchanged.
   *
   * @throws std::runtime_error when no stroke is open or the new values are invalid.
   */
  void SetSampleParameters(float radius, float strength, float hardness);

  /**
   * @brief Walk from the last local point to @p reference and emit interior dabs.
   *
   * Does not emit an extra dab at the event end. Arc-length remainder is kept for
   * the next batch. Zero-length moves are ignored.
   *
   * @throws std::runtime_error when no stroke is open or @p reference is not finite.
   */
  void AppendReference(Vector2 reference);

  /**
   * @brief Seal the stroke, emitting the endpoint once if it is not already a dab.
   *
   * @return Ordered canonical samples. The sampler is then idle.
   * @throws std::runtime_error when no stroke is open.
   */
  [[nodiscard]] auto FinishStroke() -> std::vector<BrushCanonicalSample>;

  /**
   * @brief Drop an unfinished stroke. Idle when already closed.
   */
  void CancelStroke();

  /**
   * @brief Samples emitted so far on the open stroke, excluding a pending endpoint.
   *
   * Empty when idle. Does not copy the vector. Valid until the next mutating call.
   */
  [[nodiscard]] auto DraftSamples() const -> std::span<const BrushCanonicalSample>;

  [[nodiscard]] auto IsOpen() const -> bool { return open_; }
  [[nodiscard]] auto Mode() const -> BrushStrokeMode { return mode_; }

 private:
  void RequireOpen() const;
  void Emit(const BrushCanonicalSample& sample);
  void WalkTo(Vector2 local);

  bool            open_        = false;
  BrushStrokeMode mode_        = BrushStrokeMode::Paint;
  Vector2         translation_{};
  Vector2         last_local_{};
  float           remainder_   = 0.0f;
  float           radius_      = 1.0f;
  float           strength_    = 1.0f;
  float           hardness_    = 1.0f;
  std::vector<BrushCanonicalSample> samples_;
};

}  // namespace alcedo
