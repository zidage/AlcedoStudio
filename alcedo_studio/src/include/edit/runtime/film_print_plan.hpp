//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/film/film_stock_bundle.hpp"

namespace alcedo {

class PipelineDocument;

/**
 * @brief The white of the RAW decode of @p document, for scene light As Shot.
 *
 * Develop mode Custom gives its Kelvin and tint; the other modes give the solved as-shot values.
 * The chromaticity is that of the Develop white balance (`WhiteBalanceTemperatureTintToXy`), so it
 * holds the tint (rule 'as_shot_tint'). A raster input (JPEG, PNG, TIFF, OpenEXR), a document
 * with no Develop, or a RAW whose colour transform does not resolve has no RAW white balance, so
 * the result is none, and As Shot reads as the reference. See `RawWhiteBalanceWhite`.
 */
[[nodiscard]] auto FilmSceneRawWhite(const PipelineDocument& document)
    -> std::optional<FilmRawWhite>;

/**
 * @brief Graph value for the canonical Halation result of the DRT node @p drt_id.
 *
 * Like the scatter image of `DiffusionScatterId`, the result covers the full reference frame.
 * Full-edit renders publish it; viewport ROI renders sample it.
 */
[[nodiscard]] inline auto FilmHalationResultId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"film.halation.0"}};
}

/// Graph value for the canonical Enlarger flare result. See @ref FilmHalationResultId.
[[nodiscard]] inline auto FilmFlareResultId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"film.flare.0"}};
}

/**
 * @brief Graph value for the canonical Loupe flare result of a colour reversal film.
 *
 * It holds the blurred linear display light of the light table, not the blurred transmittance of
 * @ref FilmFlareResultId, so it has its own key. See @ref FilmHalationResultId.
 */
[[nodiscard]] inline auto FilmLoupeFlareResultId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"film.loupe_flare.0"}};
}

/**
 * @brief Graph value for the canonical prescan of print material Negative Scan (issue 664).
 *
 * Rule 'frame_auto_colour' reads a prescan of the frame: the scan density T of the full frame,
 * reduced by area to a long edge of 96 pixels (`film_negative_scan::PrescanPlanOf`). A full-edit
 * render publishes it; a viewport ROI render samples it, so the auto colour of a zoom or a pan is
 * that of the full frame. It reads the negative and the scanner, not the rendering settings. See
 * @ref FilmHalationResultId.
 */
[[nodiscard]] inline auto FilmScanPrescanResultId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"film.scan_prescan.0"}};
}

/// True when @p id is a canonical film print result.
[[nodiscard]] inline auto IsFilmPrintResultPort(const GraphValueId& id) -> bool {
  const std::string_view port = id.output_port.Value();
  return port == "film.halation.0" || port == "film.flare.0" || port == "film.loupe_flare.0" ||
         port == "film.scan_prescan.0";
}

/// Largest tap distance of one blur pass. The sigma of a blur in pixels is its length on the film
/// over the pixel pitch, so the limit depends on the film format. At `35mm` it holds the spread of
/// the widest negative at slider 3 up to a long edge of about 40000 pixels; at `16mm`, with
/// 36 / 12.52 the pitch, up to about 13900 pixels.
inline constexpr std::uint32_t kFilmBlurMaxRadius          = 128;

/// A reference frame with a smaller edge has no image plane: lab rule
/// 'no_image_plane_steps_on_ramps'. Only the point stages run.
inline constexpr std::uint32_t kFilmImagePlaneSmallestEdge = 16;

/// Lab rule 'inhibitor_on_half_size': from this sigma in pixels, the inhibitor blur runs on a
/// half-size image.
inline constexpr float         kFilmHalfSizeSmallestSigma  = 4.0f;

/// Lab rule 'large_blur_on_reduced_image': from this sigma in pixels, a blur runs on a reduced
/// image, with a sigma of about @ref kFilmReducedBlurSigma on that image.
inline constexpr float         kFilmLargeSigma             = 12.0f;
inline constexpr float         kFilmReducedBlurSigma       = 6.0f;

/**
 * @brief One pass of a separable Gaussian, with its own kernel for each layer.
 *
 * `weights[layer][tap]` holds one half of the symmetric kernel. A layer with a shorter kernel holds
 * 0 past its radius. A layer with a sigma of 0 holds 1 at tap 0. The struct travels by value to a
 * kernel, so a new sigma uploads nothing.
 */
struct FilmBlurKernel {
  std::uint32_t radius = 0;
  float         weights[3][kFilmBlurMaxRadius + 1]{};
};

/// Largest radius of the narrow Gaussian of FP2 that the vertical pass of the wide Gaussian can
/// read in one dispatch. A wider one has too many taps in two dimensions, so it has its own two
/// passes.
inline constexpr std::uint32_t kFilmNarrowSpreadMaxRadius = 2;

/**
 * @brief The narrow Gaussian of FP2 in the form of one dispatch.
 *
 * `x[layer][tap]` and `y[layer][tap]` hold one half of each symmetric kernel, as in
 * @ref FilmBlurKernel. The struct is small because one dispatch also holds a @ref FilmBlurKernel,
 * and a kernel launch has a limit of 4 KB for its arguments.
 */
struct FilmNarrowKernel {
  std::uint32_t radius = 0;
  float         x[3][kFilmNarrowSpreadMaxRadius + 1]{};
  float         y[3][kFilmNarrowSpreadMaxRadius + 1]{};
};

/**
 * @brief Host plan of the image-plane steps of the film print for one render.
 *
 * FP2 and the FP4 blur run on the pixels of the render, with a size in pixels from the frame and
 * @ref RenderAxisScale. Halation and Enlarger flare are canonical results: the sizes come from the
 * full reference frame only, so a zoom does not change them.
 */
struct FilmPrintImagePlan {
  bool                          image_plane   = false;

  /// FP2, rule 'spread_per_layer_pass'. 0 passes: the spread is off.
  std::uint32_t                 spread_passes = 0;
  std::array<FilmBlurKernel, 2> spread_x{};
  std::array<FilmBlurKernel, 2> spread_y{};
  std::array<float, 2>          spread_weight{};
  /// True when @ref spread_narrow holds pass 0, so that FP2 needs only 2 dispatches.
  bool                          spread_narrow_fused = false;
  FilmNarrowKernel              spread_narrow{};

  /// FP4. @ref inhibitor_half_size selects the half-size image of @ref inhibitor_extent.
  bool                          inhibitor_half_size = false;
  ImageExtent                   inhibitor_extent{};
  FilmBlurKernel                inhibitor_x{};
  FilmBlurKernel                inhibitor_y{};

  /// The canvas of `DiffusionCanvasExtent`, and how it maps onto the render.
  ImageExtent                   canvas_extent{};
  Matrix3x3                     render_to_canvas{};
  Matrix3x3                     canvas_to_render{};
  std::uint32_t                 canvas_samples  = 1;  ///< Render samples per axis for one texel

  /// FP3. @ref halation_active is false when all strengths are 0.
  bool                          halation_active = false;
  std::array<float, 3>          halation_strength{};
  FilmBlurKernel                halation_x{};
  FilmBlurKernel                halation_y{};

  /// FP6. The published image is the canvas, reduced by rule 'large_blur_on_reduced_image'.
  bool                          flare_active = false;
  float                         flare        = 0.0f;
  ImageExtent                   flare_extent{};
  Matrix3x3                     render_to_flare{};
  FilmBlurKernel                flare_x{};
  FilmBlurKernel                flare_y{};
};

/**
 * @brief Make the plan of the image-plane steps. Backend-neutral; computed on the CPU per render.
 *
 * The pixel pitch on the film is the long side of the frame of @p format over the long side of the
 * full reference frame (plan 'film_grain_format_and_stock_plan.md', phase G7). Light spread,
 * Halation, and the inhibitor have a length on the film, so their sigma in pixels scales with the
 * format. Enlarger flare is a share of the frame, so the format does not change it. At `35mm` the
 * frame is 36 mm, the frame of the lab values.
 *
 * @param layer_spread_um The spread lengths of the negative.
 * @param has_inhibitor_blur False for a selection with no image part of its inhibition and no
 *        negative grade, which reads the same blur. The plan then has no inhibitor.
 * @param format The film format of the DRT node.
 * @throws std::runtime_error when a blur needs more than @ref kFilmBlurMaxRadius taps. The message
 *         states the sigma. There is no substitute blur.
 * @throws std::invalid_argument that names the field `film_format` for a format outside the enum.
 */
[[nodiscard]] auto MakeFilmPrintImagePlan(const ResolvedRenderGeometry&  geometry,
                                          const FilmImagePlaneConstants& constants,
                                          std::span<const double>        layer_spread_um,
                                          bool has_inhibitor_blur, const FilmPrintParams& film,
                                          FilmFormat format) -> FilmPrintImagePlan;

}  // namespace alcedo
