//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The one catalog of color gamuts, transfer functions and color encodings
// (docs/roadmap/alcedo_studio/edit/lut_color_encoding_plan.md, section 5). The curves live in
// color/color_encoding_math.h; this header holds the gamut primaries, the encoding table and the
// double-precision matrix construction.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "color/color_encoding_math.h"

namespace alcedo::color {

/// Row-major 3x3 matrix applied to column vectors: out = m * v.
using Matrix33d   = std::array<double, 9>;

/// CIE xy chromaticities Rx Ry Gx Gy Bx By Wx Wy, with the native (not adapted) white.
using PrimariesXy = std::array<float, 8>;

enum class ColorGamutId : uint8_t {
  Ap0,
  Ap1,
  Rec709,
  Rec2020,
  P3D65,
  P3D60,
  P3Dci,
  /// CIE XYZ as RGB: unit primaries and the equal-energy white, so RGB to XYZ is the identity.
  CieXyz,
  ProPhoto,
  AdobeRgb,
  /// BT.601 625-line (CICP colour primaries 5).
  Bt601_625,
  /// BT.601 525-line / SMPTE 170M (CICP colour primaries 6).
  Bt601_525,
  ArriWideGamut3,
  ArriWideGamut4,
  SGamut3,
  SGamut3Cine,
  VGamut,
  CinemaGamut,
  RedWideGamutRgb,
  BlackmagicWideGamutGen5,
  DaVinciWideGamut,
  DGamut,
  FGamutC,
  AppleWideGamut,
  KinefinityWideGamut,
};

/// Chromatic adaptation between two white points.
enum class ChromaticAdaptation : uint8_t { Cat02, Bradford };

/// One gamut of the catalog.
struct ColorGamut {
  ColorGamutId        id_;
  std::string_view    name_;
  PrimariesXy         primaries_xy_;
  /// Adaptation of the gamut's published conversion to ACES (the OCIO 2.5.1 studio config).
  /// Bradford for the video and print gamuts (Rec.709, Rec.2020, P3, Adobe RGB, ProPhoto,
  /// BT.601), V-Gamut, REDWideGamutRGB and Apple Wide Gamut; CAT02 for the other camera gamuts.
  ChromaticAdaptation aces_adaptation_;
};

/// Transfer function ids, numerically equal to the CE_TF_* ids of color_encoding_math.h.
enum class TransferFunctionId : int {
  Linear              = CE_TF_LINEAR,
  St2084              = CE_TF_ST2084,
  Hlg                 = CE_TF_HLG,
  Gamma26             = CE_TF_GAMMA_2_6,
  Bt1886              = CE_TF_BT1886,
  Gamma22             = CE_TF_GAMMA_2_2,
  Gamma18             = CE_TF_GAMMA_1_8,
  Srgb                = CE_TF_SRGB,
  Acescc              = CE_TF_ACESCC,
  Acescct             = CE_TF_ACESCCT,
  ArriLogC3Ei800      = CE_TF_ARRI_LOGC3_EI800,
  ArriLogC4           = CE_TF_ARRI_LOGC4,
  SonySLog3           = CE_TF_SONY_SLOG3,
  FujifilmFLog        = CE_TF_FUJIFILM_FLOG,
  FujifilmFLog2       = CE_TF_FUJIFILM_FLOG2,
  PanasonicVLog       = CE_TF_PANASONIC_VLOG,
  CanonCLog2          = CE_TF_CANON_CLOG2,
  CanonCLog3          = CE_TF_CANON_CLOG3,
  RedLog3G10          = CE_TF_RED_LOG3G10,
  BlackmagicFilmGen5  = CE_TF_BLACKMAGIC_FILM_GEN5,
  DaVinciIntermediate = CE_TF_DAVINCI_INTERMEDIATE,
  AppleLog            = CE_TF_APPLE_LOG,
  NikonNLog           = CE_TF_NIKON_NLOG,
  DjiDLog             = CE_TF_DJI_DLOG,
  CanonCLog           = CE_TF_CANON_CLOG,
  Cineon              = CE_TF_CINEON,
  ArriLogC3Ei160      = CE_TF_ARRI_LOGC3_EI160,
  ArriLogC3Ei200      = CE_TF_ARRI_LOGC3_EI200,
  ArriLogC3Ei250      = CE_TF_ARRI_LOGC3_EI250,
  ArriLogC3Ei320      = CE_TF_ARRI_LOGC3_EI320,
  ArriLogC3Ei400      = CE_TF_ARRI_LOGC3_EI400,
  ArriLogC3Ei500      = CE_TF_ARRI_LOGC3_EI500,
  ArriLogC3Ei640      = CE_TF_ARRI_LOGC3_EI640,
  ArriLogC3Ei1000     = CE_TF_ARRI_LOGC3_EI1000,
  ArriLogC3Ei1280     = CE_TF_ARRI_LOGC3_EI1280,
  ArriLogC3Ei1600     = CE_TF_ARRI_LOGC3_EI1600,
  LeicaLLog           = CE_TF_LEICA_LLOG,
  KinefinityKineLog3  = CE_TF_KINEFINITY_KINELOG3,
  GoproGpLog2         = CE_TF_GOPRO_GPLOG2,
};

/// Whether code values encode scene light or light after a display rendering.
enum class ColorReferral : uint8_t { SceneReferred, DisplayReferred };

/// One color encoding: a gamut plus a transfer function.
struct ColorEncoding {
  /// Stable lower-case id used in JSON. Never translated or renamed.
  std::string_view   id_;
  ColorGamutId       gamut_;
  TransferFunctionId transfer_;
  ColorReferral      referral_;
  /// Peak luminance of a display encoding; 0 for scene-referred encodings.
  float              peak_luminance_nits_;
  /// English display name; the UI translates it.
  std::string_view   display_name_;
};

/// CAM16 cone primaries of the ACES 2.0 output transform. Not a color gamut: the ACES 2.0
/// model builds its CAM16 matrix from them with RgbToXyzMatrix.
inline constexpr PrimariesXy      kAces2Cam16Primaries    = {0.8336f, 0.1735f, 2.3854f, -1.4659f,
                                                             0.087f,  -0.125f, 0.333f,  0.333f};

/// Id of the default encoding (ACEScc, AP1).
inline constexpr std::string_view kDefaultColorEncodingId = "acescc";

/// All gamuts, in ColorGamutId order.
[[nodiscard]] auto                ColorGamuts() -> std::span<const ColorGamut>;

/// The catalog entry of @p id.
[[nodiscard]] auto                FindColorGamut(ColorGamutId id) -> const ColorGamut&;

/// Primaries and white of @p id.
[[nodiscard]] auto                GamutPrimariesXy(ColorGamutId id) -> const PrimariesXy&;

/// All encodings offered for LUTs (section 5.2), scene-referred first.
[[nodiscard]] auto                ColorEncodings() -> std::span<const ColorEncoding>;

/// The encoding with JSON id @p id, or nullptr when the catalog has none.
[[nodiscard]] auto                FindColorEncoding(std::string_view id) -> const ColorEncoding*;

/**
 * @brief RGB to CIE XYZ with Y(white) = 1 for @p primaries (OCIO rgb2xyz_from_xy).
 *
 * Computed in double precision from the single-precision chromaticities, so that matrices
 * converted to float match OpenColorIO's bit for bit.
 * @throws std::runtime_error when the primaries are degenerate.
 */
[[nodiscard]] auto                RgbToXyzMatrix(const PrimariesXy& primaries) -> Matrix33d;

/// RGB to CIE XYZ for catalog gamut @p id. The identity for CieXyz.
[[nodiscard]] auto                RgbToXyzMatrix(ColorGamutId id) -> Matrix33d;

/**
 * @brief Inverse of @p m by Gauss-Jordan elimination with partial pivoting, in the operation
 * order of OpenColorIO's MatrixOpData::MatrixArray::inverse.
 * @throws std::runtime_error when @p m is singular.
 */
[[nodiscard]] auto                InvertMatrix(const Matrix33d& m) -> Matrix33d;

/// Matrix product a * b.
[[nodiscard]] auto MultiplyMatrices(const Matrix33d& a, const Matrix33d& b) -> Matrix33d;

/// Adaptation of XYZ from white @p source_white_xy to white @p target_white_xy.
[[nodiscard]] auto ChromaticAdaptationMatrix(const std::array<float, 2>& source_white_xy,
                                             const std::array<float, 2>& target_white_xy,
                                             ChromaticAdaptation         method) -> Matrix33d;

/**
 * @brief RGB in @p source to RGB in @p target through XYZ, adapting white with @p method when
 * the two whites differ.
 */
[[nodiscard]] auto RgbToRgbMatrix(const PrimariesXy& source, const PrimariesXy& target,
                                  ChromaticAdaptation method) -> Matrix33d;

/**
 * @brief RGB in catalog gamut @p from to RGB in catalog gamut @p to.
 *
 * The adaptation is Bradford when either gamut's aces_adaptation_ is Bradford, otherwise CAT02,
 * so that conversions to AP0/AP1 match the published matrices. CieXyz is not adapted.
 */
[[nodiscard]] auto GamutConversionMatrix(ColorGamutId from, ColorGamutId to) -> Matrix33d;

}  // namespace alcedo::color
