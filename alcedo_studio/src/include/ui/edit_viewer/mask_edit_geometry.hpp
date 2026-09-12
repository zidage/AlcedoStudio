//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include <QPointF>
#include <QVector2D>

#include "edit/geometry/render_request.hpp"
#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo {

/// Logical-pixel hit radius for Mask handles. Independent of zoom and DPR.
inline constexpr float kMaskHandleHitRadiusLogicalPx = 12.0f;
/// Normalized ReferenceSpace round-trip tolerance for NM7.5 mapping tests.
inline constexpr float kMaskNormalizedMappingEpsilon = 1.0e-5f;
/// Item-space round-trip tolerance in logical pixels.
inline constexpr float kMaskItemRoundTripLogicalPx = 0.25f;

/**
 * @brief Viewer inputs that compose item coordinates onto Mask ReferenceSpace.
 *
 * @p photograph is the letterboxed image used by @ref ViewportMapper, the same
 * space as @c EditorInteractionController::itemPointToImageUv. Crop overlay
 * rects are not applied here: crop/orientation live in @p geometry of the
 * displayed photograph. DetailPatch pixel size is never a ReferenceSpace.
 *
 * When @p presentation is @c FullFrame, @p displayed_roi is ignored and zoom/pan
 * map onto the full photograph. When it is @c RoiFrame, mapper UV is the displayed
 * patch and @p displayed_roi expands it onto full-frame photograph UV before
 * @c render_to_reference.
 */
struct MaskEditViewMapping {
  ViewportWidgetInfo       widget{};
  ViewportImageInfo        photograph{};
  float                     zoom = 1.0f;
  QVector2D                 pan{};
  FramePresentationMode     presentation  = FramePresentationMode::FullFrame;
  FrameRoiRect              displayed_roi{0.0f, 0.0f, 1.0f, 1.0f};
  ResolvedRenderGeometry    geometry{};
};

/**
 * @brief One mapped Mask location. Pointer samples have no extra half-pixel offset.
 *
 * @p reference_pixels are continuous ReferenceSpace pixels.
 * @p normalized is @p reference_pixels divided by @c full_reference_extent.
 * @p photograph_uv is full-frame photograph UV after RoiFrame expansion.
 */
struct MaskReferenceSample {
  Vector2 reference_pixels{};
  Vector2 normalized{};
  Vector2 photograph_uv{};
  bool    inside_photograph = false;
};

/**
 * @brief Snapshot of mapping inputs used to cancel an open Mask operation.
 *
 * Widget resize, DPR, zoom/pan, presentation, or a different full-reference
 * image cancel unfinished Mask input. Quality vs Interactive render extent is
 * compared through the composed item→reference mapping, not raw field equality.
 */
struct MaskEditMappingIdentity {
  int                   widget_width        = 0;
  int                   widget_height       = 0;
  float                 device_pixel_ratio   = 1.0f;
  int                   photograph_width    = 0;
  int                   photograph_height   = 0;
  float                 zoom                 = 1.0f;
  float                 pan_x               = 0.0f;
  float                 pan_y               = 0.0f;
  FramePresentationMode presentation         = FramePresentationMode::FullFrame;
  float                 roi_x               = 0.0f;
  float                 roi_y               = 0.0f;
  float                 roi_width           = 1.0f;
  float                 roi_height          = 1.0f;
  std::uint32_t         full_reference_width  = 0;
  std::uint32_t         full_reference_height = 0;
  std::uint32_t         render_width          = 0;
  std::uint32_t         render_height         = 0;
  float                 render_to_reference[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

  friend auto operator==(const MaskEditMappingIdentity&, const MaskEditMappingIdentity&)
      -> bool = default;
};

/**
 * @brief Shared item ↔ ReferenceSpace mapping for Mask input, controls, and pixels.
 *
 * GUI-thread geometry only. Does not lock the pipeline, read worker state, or
 * perform cache I/O. Crop tool overlays are not a second mapping.
 */
class MaskEditGeometry {
 public:
  /**
   * @brief Identity photograph geometry: render, edit, and reference extents match.
   *
   * Use when the displayed photograph is uncropped and unrotated. Matrices are
   * identity. Empty @p extent yields empty extents so @ref IsValid is false.
   */
  [[nodiscard]] static auto MakeIdentityPhotographGeometry(Extent2D extent)
      -> ResolvedRenderGeometry;

  /**
   * @brief Resolved photograph geometry from document crop, rotation, and expand-to-fit.
   *
   * @p full_reference is uncropped ReferenceSpace. Identity crop and zero rotation
   * return @ref MakeIdentityPhotographGeometry. Empty @p full_reference yields
   * empty extents so @ref IsValid is false.
   *
   * Thread: GUI. Pure; does not lock the pipeline.
   */
  [[nodiscard]] static auto MakeDocumentPhotographGeometry(Extent2D full_reference,
                                                            const ImageGeometryParams& image)
      -> ResolvedRenderGeometry;

  /**
   * @brief True when widget, photograph, ROI, and @p geometry can map Mask input.
   *
   * Rejects zero extents, non-finite view values, a noninvertible
   * @c render_to_reference, and a degenerate RoiFrame ROI.
   */
  [[nodiscard]] static auto IsValid(const MaskEditViewMapping& mapping) -> bool;

  /**
   * @brief Identity of @p mapping for resize/DPR/geometry interruption.
   *
   * Invalid mappings still produce an identity so a later valid mapping is a change.
   */
  [[nodiscard]] static auto Identity(const MaskEditViewMapping& mapping) -> MaskEditMappingIdentity;

  /**
   * @brief True when unfinished Mask input must be cancelled before using @p after.
   *
   * Compares the composed item→ReferenceSpace mapping, not raw identity fields.
   * Quality and Interactive frames of the same view (different render extent with
   * a compensating @c render_to_reference) keep the stroke open. Widget resize,
   * DPR, zoom/pan, presentation mode, or a different full-reference image cancel.
   */
  [[nodiscard]] static auto MappingChanged(const MaskEditMappingIdentity& before,
                                           const MaskEditMappingIdentity& after) -> bool;

  /**
   * @brief Map an item/logical point to ReferenceSpace.
   *
   * @param mapping Current viewer and displayed-photograph geometry.
   * @param item Item coordinates (same space as QQuickItem pointer events).
   * @param allow_outside When false, UV outside the photograph rejects the press.
   *        When true, off-image coordinates are returned for an open drag.
   * @return Empty when @p mapping is invalid or a press is outside the photograph.
   *
   * Does not clamp off-image points onto the photograph edge.
   */
  [[nodiscard]] static auto MapItemToReference(const MaskEditViewMapping& mapping,
                                                const QPointF& item, bool allow_outside)
      -> std::optional<MaskReferenceSample>;

  /**
   * @brief Inverse of @ref MapItemToReference for control placement.
   *
   * Off-image reference pixels are not clamped onto the photograph. Empty when
   * @p mapping is invalid or @p reference_pixels is not finite.
   */
  [[nodiscard]] static auto MapReferenceToItem(const MaskEditViewMapping& mapping,
                                                Vector2 reference_pixels) -> std::optional<QPointF>;

  /**
   * @brief Normalized ReferenceSpace from continuous reference pixels.
   *
   * @throws std::runtime_error when @p full_reference is empty.
   */
  [[nodiscard]] static auto NormalizedFromReferencePixels(Vector2 reference_pixels,
                                                          Extent2D full_reference) -> Vector2;

  /**
   * @brief Continuous reference pixels from normalized ReferenceSpace.
   *
   * @throws std::runtime_error when @p full_reference is empty.
   */
  [[nodiscard]] static auto ReferencePixelsFromNormalized(Vector2 normalized,
                                                          Extent2D full_reference) -> Vector2;

  /**
   * @brief Hit-test a handle disc in logical pixels.
   *
   * @p radius_logical_px is constant at any zoom or DPR. Empty or non-finite
   * radius misses.
   */
  [[nodiscard]] static auto HitsHandle(const QPointF& item, const QPointF& handle_item,
                                        float radius_logical_px = kMaskHandleHitRadiusLogicalPx)
      -> bool;
};

}  // namespace alcedo
