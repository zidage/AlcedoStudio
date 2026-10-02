//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QVariantMap>

#include "edit/geometry/types.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"

namespace alcedo::ui {

/**
 * @brief Where one rendered comparison image sits in the full source reference space.
 *
 * Copied from the ResolvedRenderGeometry of the executed render. The values are the only
 * geometry that the comparison canvas reads: it never guesses the reference extent from the
 * image size.
 */
struct ComparisonImagePlacement {
  /// Full source reference extent. Both images of a pair share it.
  Extent2D           reference_extent{};
  /// Pixel extent of the rendered image; equal to the QImage size.
  Extent2D           render_extent{};
  /// Affine map from continuous render pixel coordinates to reference coordinates.
  Matrix3x3          render_to_reference = Matrix3x3::Identity();

  /**
   * @brief Values for QML: `referenceWidth`, `referenceHeight`, `renderWidth`, `renderHeight`,
   *        and the affine terms `m11`, `m12`, `dx`, `m21`, `m22`, `dy` of render_to_reference.
   */
  [[nodiscard]] auto ToVariantMap() const -> QVariantMap;
};

/**
 * @brief One 8-bit SDR comparison image and its placement.
 *
 * The QImage owns its pixels (Format_RGBA8888). It shares no memory with the float render
 * output, so the caller can release the RenderedPipelineImage after the conversion.
 */
struct ComparisonPresentationImage {
  QImage                   image;
  ComparisonImagePlacement placement{};
};

/**
 * @brief Reads the placement of @p rendered from its executed render geometry.
 *
 * @throws std::invalid_argument when the reference or render extent is empty, or when
 *         render_to_reference is not a finite, invertible affine map.
 */
[[nodiscard]] auto ComparisonPlacementFromGeometry(const ResolvedRenderGeometry& geometry)
    -> ComparisonImagePlacement;

/**
 * @brief Converts one rendered comparison image to the approved 8-bit SDR presentation.
 *
 * Validation, in order: CPU pixels exist; the type is RGBA32F; the pixel size equals
 * `geometry.render_extent`; the placement is valid; the output encoding is not ST 2084 or
 * HLG; every channel value is finite. Then each channel is quantized once to
 * `round(clamp(v, 0, 1) * 255)`, keeping the RGBA channel order. No DRT, gamma, or gamut
 * operation is applied.
 *
 * Thread: any thread. Reads @p rendered only.
 *
 * @throws std::invalid_argument with the reason when a validation fails. Nothing is returned
 *         for a partly valid image.
 */
[[nodiscard]] auto ConvertRenderedImageForComparison(const RenderedPipelineImage& rendered)
    -> ComparisonPresentationImage;

}  // namespace alcedo::ui
