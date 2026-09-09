//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <QColor>
#include <QPointF>
#include <QRectF>

namespace alcedo {

/// Draw radius of a Mask handle disc, in logical pixels. Constant at any zoom/DPR.
inline constexpr float kMaskOverlayHandleRadiusLogicalPx = 5.0f;
/// Two-layer handle outline width, in logical pixels.
inline constexpr float kMaskOverlayHandleOutlineWidthLogicalPx = 1.2f;
/// Connector and cursor stroke width, in logical pixels.
inline constexpr float kMaskOverlayStrokeWidthLogicalPx = 1.5f;
/// Premultiplied edge-alpha fringe width, in logical pixels.
inline constexpr float kMaskOverlayAntialiasWidthLogicalPx = 1.0f;
/// Pointer hit radius for Mask handles, in logical pixels. Matches @ref kMaskHandleHitRadiusLogicalPx.
inline constexpr float kMaskOverlayHandleHitRadiusLogicalPx = 12.0f;
/// Rotation/direction handle offset from the mapped origin, in logical pixels.
inline constexpr float kMaskOverlayRotateHandleOffsetLogicalPx = 24.0f;
/// Maximum chord deviation when tessellating a Radial creation outline, in logical pixels.
inline constexpr float kMaskOverlayMaxChordDeviationLogicalPx = 0.25f;
/// Upper bound on Radial outline vertices after adaptive subdivision.
inline constexpr int kMaskOverlayMaxEllipseVertices = 256;

/**
 * @brief Published Mask overlay mode. Creating may emit temporary path/outline guides.
 *
 * Existing editing never emits coverage fill, heatmap, or settled Brush path geometry.
 */
enum class MaskOverlayMode : std::uint8_t {
  Hidden   = 0,
  Existing = 1,
  Creating = 2,
};

/** @brief Source kind whose controls are shown. None when the overlay is hidden. */
enum class MaskOverlaySourceKind : std::uint8_t {
  None            = 0,
  Brush           = 1,
  Radial          = 2,
  LinearGradient  = 3,
};

/** @brief Finite selected-source handle identity. Not a per-dab proxy. */
enum class MaskOverlayHandleId : std::uint8_t {
  None                 = 0,
  BrushMove            = 1,
  RadialCenter         = 2,
  RadialMajor          = 3,
  RadialMinor          = 4,
  RadialRotate         = 5,
  RadialInnerFeather   = 6,
  RadialOuterFeather   = 7,
  LinearOrigin         = 8,
  LinearDirection      = 9,
  LinearStartBoundary  = 10,
  LinearEndBoundary    = 11,
};

/** @brief One handle disc in item/logical coordinates. */
struct MaskOverlayHandle {
  MaskOverlayHandleId id = MaskOverlayHandleId::None;
  QPointF             item{};
};

/**
 * @brief Theme-resolved Mask overlay metrics and colors.
 *
 * @p control_fill / @p control_outline are the two-layer handle stroke.
 * @p inactive tints temporary creation guides. There is no coverage-area color.
 */
struct MaskOverlayStyle {
  QColor control_fill    = QColor(0xE0, 0xE0, 0xE0);
  QColor control_outline = QColor(0x0C, 0x0C, 0x0C);
  QColor inactive        = QColor(0x7A, 0x7A, 0x7A);
  float  handle_radius_logical_px          = kMaskOverlayHandleRadiusLogicalPx;
  float  handle_outline_width_logical_px   = kMaskOverlayHandleOutlineWidthLogicalPx;
  float  stroke_width_logical_px           = kMaskOverlayStrokeWidthLogicalPx;
  float  antialias_width_logical_px        = kMaskOverlayAntialiasWidthLogicalPx;
  float  hit_radius_logical_px             = kMaskOverlayHandleHitRadiusLogicalPx;
  float  rotate_handle_offset_logical_px   = kMaskOverlayRotateHandleOffsetLogicalPx;
};

/**
 * @brief GUI-thread Mask overlay publication. Item coordinates only.
 *
 * Built from current input and read-only mapped control positions. The scene
 * graph copies this derived geometry; it must not lock the pipeline, read worker
 * state, or perform cache I/O.
 *
 * An empty @p clip_rect disables clipping. Degenerate mapped controls yield empty
 * triangle lists rather than NaN vertices.
 */
struct MaskOverlayDisplay {
  MaskOverlayMode       mode         = MaskOverlayMode::Hidden;
  MaskOverlaySourceKind source_kind  = MaskOverlaySourceKind::None;
  std::vector<MaskOverlayHandle> handles;
  std::vector<std::pair<QPointF, QPointF>> connectors;
  bool    cursor_visible = false;
  QPointF cursor_center{};
  float   cursor_radius_logical_px = 0.0f;
  std::vector<QPointF> creation_path;
  std::vector<QPointF> creation_outline;
  std::vector<std::pair<QPointF, QPointF>> creation_guides;
  QRectF clip_rect{};
};

/**
 * @brief Premultiplied vertex for QSGVertexColorMaterial.
 *
 * RGB channels are already multiplied by alpha in 0..255. Coordinates are item/
 * logical pixels.
 */
struct MaskOverlayVertex {
  float        x = 0.0f;
  float        y = 0.0f;
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 0;
};

/**
 * @brief Triangle lists for retained Mask overlay nodes.
 *
 * @p coverage_fill_vertex_count and @p settled_stroke_path_vertex_count stay
 * zero: coverage feedback is the photograph, not QSG fill.
 */
struct MaskOverlaySceneGeometry {
  std::vector<MaskOverlayVertex> handle_fill;
  std::vector<MaskOverlayVertex> handle_outline;
  std::vector<MaskOverlayVertex> connectors;
  std::vector<MaskOverlayVertex> cursor;
  std::vector<MaskOverlayVertex> creation_guides;
  int handle_count                       = 0;
  int coverage_fill_vertex_count         = 0;
  int settled_stroke_path_vertex_count   = 0;
};

/**
 * @brief Alcedo-theme defaults matching @c AppTheme index 0 Mask overlay tokens.
 *
 * Tests may call this without constructing @c AppTheme. Production QML binds
 * live theme colors onto @ref EditorOverlayItem.
 */
[[nodiscard]] auto DefaultMaskOverlayStyle() -> MaskOverlayStyle;

/**
 * @brief Build triangle lists from a published Mask overlay display.
 *
 * @param display Item-space controls and optional creation guides.
 * @param style Logical sizes and theme colors. Handle/stroke widths stay in
 *        logical pixels; they are not scaled by zoom or DPR.
 * @return Triangle lists. Hidden or fully clipped displays are empty.
 *
 * Thread: GUI. Pure; no scene-graph, pipeline, or I/O access.
 */
[[nodiscard]] auto BuildMaskOverlaySceneGeometry(const MaskOverlayDisplay& display,
                                                 const MaskOverlayStyle& style)
    -> MaskOverlaySceneGeometry;

}  // namespace alcedo
