//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
//  The matrix construction is a port of OpenColorIO 2.5.1
//  (src/OpenColorIO/transforms/builtins/ColorMatrixHelpers.cpp and
//  src/OpenColorIO/ops/matrix/MatrixOpData.cpp):
//  Copyright Contributors to the OpenColorIO Project. SPDX-License-Identifier: BSD-3-Clause.
//  See THIRD_PARTY_NOTICE.txt.

#include "color/color_encoding_catalog.hpp"

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace alcedo::color {
namespace {

constexpr float                      kD65X = 0.3127f, kD65Y = 0.3290f;
constexpr float                      kAcesX = 0.32168f, kAcesY = 0.33767f;

// Sources: ITU-R BT.709-6, BT.2020-2, BT.601-7; SMPTE RP 431-2 and EG 432-1 (P3); Academy
// S-2014-004 (AP0, AP1); ISO 22028-2 (ROMM / ProPhoto); Adobe RGB (1998) 2005-05; ARRI
// "ALEXA Log C Curve - Usage in VFX" and "ARRI Wide Gamut 4 Specification"; Sony
// "S-Gamut3.Cine/S-Log3 Technical Summary"; Panasonic "V-Log/V-Gamut Reference Manual"; Canon
// "Cinema Gamut" (Academy CSC.Canon); RED "REDWideGamutRGB and Log3G10" white paper;
// Blackmagic "Generation 5 Color Science"; Blackmagic "DaVinci Wide Gamut Intermediate"; DJI
// "D-Log and D-Gamut" white paper.
constexpr std::array<ColorGamut, 22> kGamuts = {{
    {ColorGamutId::Ap0,
     "ACES AP0",
     {0.7347f, 0.2653f, 0.0f, 1.0f, 0.0001f, -0.0770f, kAcesX, kAcesY},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::Ap1,
     "ACES AP1",
     {0.713f, 0.293f, 0.165f, 0.830f, 0.128f, 0.044f, kAcesX, kAcesY},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::Rec709,
     "Rec.709",
     {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::Rec2020,
     "Rec.2020",
     {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::P3D65,
     "P3-D65",
     {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::P3D60,
     "P3-D60",
     {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, kAcesX, kAcesY},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::P3Dci,
     "P3-DCI",
     {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.314f, 0.351f},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::CieXyz,
     "CIE XYZ",
     {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f / 3.0f, 1.0f / 3.0f},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::ProPhoto,
     "ProPhoto RGB",
     {0.7347f, 0.2653f, 0.1596f, 0.8404f, 0.0366f, 0.0001f, 0.3457f, 0.3585f},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::AdobeRgb,
     "Adobe RGB (1998)",
     {0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::Bt601_625,
     "BT.601 625",
     {0.64f, 0.33f, 0.29f, 0.60f, 0.15f, 0.06f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::Bt601_525,
     "BT.601 525",
     {0.630f, 0.340f, 0.310f, 0.595f, 0.155f, 0.070f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::ArriWideGamut3,
     "ARRI Wide Gamut 3",
     {0.684f, 0.313f, 0.221f, 0.848f, 0.0861f, -0.102f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::ArriWideGamut4,
     "ARRI Wide Gamut 4",
     {0.7347f, 0.2653f, 0.1424f, 0.8576f, 0.0991f, -0.0308f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::SGamut3,
     "S-Gamut3",
     {0.730f, 0.280f, 0.140f, 0.855f, 0.100f, -0.050f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::SGamut3Cine,
     "S-Gamut3.Cine",
     {0.766f, 0.275f, 0.225f, 0.800f, 0.089f, -0.087f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::VGamut,
     "V-Gamut",
     {0.730f, 0.280f, 0.165f, 0.840f, 0.100f, -0.030f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::CinemaGamut,
     "Cinema Gamut",
     {0.74f, 0.27f, 0.17f, 1.14f, 0.08f, -0.10f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::RedWideGamutRgb,
     "REDWideGamutRGB",
     {0.780308f, 0.304253f, 0.121595f, 1.493994f, 0.095612f, -0.084589f, kD65X, kD65Y},
     ChromaticAdaptation::Bradford},
    {ColorGamutId::BlackmagicWideGamutGen5,
     "Blackmagic Wide Gamut Gen 5",
     {0.7177215f, 0.3171181f, 0.2280410f, 0.8615690f, 0.1005841f, -0.0820452f, 0.3127170f,
      0.3290312f},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::DaVinciWideGamut,
     "DaVinci Wide Gamut",
     {0.8000f, 0.3130f, 0.1682f, 0.9877f, 0.0790f, -0.1155f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
    {ColorGamutId::DGamut,
     "D-Gamut",
     {0.71f, 0.31f, 0.21f, 0.88f, 0.09f, -0.08f, kD65X, kD65Y},
     ChromaticAdaptation::Cat02},
}};

constexpr bool                       GamutsAreInIdOrder() {
  for (std::size_t i = 0; i < kGamuts.size(); ++i) {
    if (static_cast<std::size_t>(kGamuts[i].id_) != i) {
      return false;
    }
  }
  return true;
}
static_assert(GamutsAreInIdOrder(), "kGamuts must be indexed by ColorGamutId");

using G                                            = ColorGamutId;
using T                                            = TransferFunctionId;
using CR                                           = ColorReferral;

// F-Gamut (Fujifilm F-Log/F-Log2 data sheets, section 3) and the N-Log gamut (Nikon N-Log
// specification, section 3) have the ITU-R BT.2020 primaries and white.
constexpr std::array<ColorEncoding, 26> kEncodings = {{
    {"acescc", G::Ap1, T::Acescc, CR::SceneReferred, 0.0f, "ACEScc (AP1)"},
    {"acescct", G::Ap1, T::Acescct, CR::SceneReferred, 0.0f, "ACEScct (AP1)"},
    {"arri_logc3_awg3", G::ArriWideGamut3, T::ArriLogC3Ei800, CR::SceneReferred, 0.0f,
     "ARRI LogC3 (EI 800) / ARRI Wide Gamut 3"},
    {"arri_logc4_awg4", G::ArriWideGamut4, T::ArriLogC4, CR::SceneReferred, 0.0f,
     "ARRI LogC4 / ARRI Wide Gamut 4"},
    {"sony_slog3_sgamut3cine", G::SGamut3Cine, T::SonySLog3, CR::SceneReferred, 0.0f,
     "Sony S-Log3 / S-Gamut3.Cine"},
    {"sony_slog3_sgamut3", G::SGamut3, T::SonySLog3, CR::SceneReferred, 0.0f,
     "Sony S-Log3 / S-Gamut3"},
    {"fujifilm_flog_fgamut", G::Rec2020, T::FujifilmFLog, CR::SceneReferred, 0.0f,
     "Fujifilm F-Log / F-Gamut"},
    {"fujifilm_flog2_fgamut", G::Rec2020, T::FujifilmFLog2, CR::SceneReferred, 0.0f,
     "Fujifilm F-Log2 / F-Gamut"},
    {"panasonic_vlog_vgamut", G::VGamut, T::PanasonicVLog, CR::SceneReferred, 0.0f,
     "Panasonic V-Log / V-Gamut"},
    {"canon_clog2_cinemagamut", G::CinemaGamut, T::CanonCLog2, CR::SceneReferred, 0.0f,
     "Canon Log 2 / Cinema Gamut"},
    {"canon_clog3_cinemagamut", G::CinemaGamut, T::CanonCLog3, CR::SceneReferred, 0.0f,
     "Canon Log 3 / Cinema Gamut"},
    {"red_log3g10_rwg", G::RedWideGamutRgb, T::RedLog3G10, CR::SceneReferred, 0.0f,
     "RED Log3G10 / REDWideGamutRGB"},
    {"blackmagic_film_gen5_bmdwg", G::BlackmagicWideGamutGen5, T::BlackmagicFilmGen5,
     CR::SceneReferred, 0.0f, "Blackmagic Film Gen 5 / Blackmagic Wide Gamut Gen 5"},
    {"davinci_intermediate_dwg", G::DaVinciWideGamut, T::DaVinciIntermediate, CR::SceneReferred,
     0.0f, "DaVinci Intermediate / DaVinci Wide Gamut"},
    {"apple_log_rec2020", G::Rec2020, T::AppleLog, CR::SceneReferred, 0.0f, "Apple Log / Rec.2020"},
    {"nikon_nlog_rec2020", G::Rec2020, T::NikonNLog, CR::SceneReferred, 0.0f,
     "Nikon N-Log / Rec.2020"},
    {"dji_dlog_dgamut", G::DGamut, T::DjiDLog, CR::SceneReferred, 0.0f, "DJI D-Log / D-Gamut"},
    {"rec709_bt1886", G::Rec709, T::Bt1886, CR::DisplayReferred, 100.0f, "Rec.709 BT.1886"},
    {"rec709_srgb", G::Rec709, T::Srgb, CR::DisplayReferred, 100.0f, "Rec.709 sRGB"},
    {"rec709_gamma22", G::Rec709, T::Gamma22, CR::DisplayReferred, 100.0f, "Rec.709 Gamma 2.2"},
    {"displayp3_srgb", G::P3D65, T::Srgb, CR::DisplayReferred, 100.0f, "Display P3"},
    {"p3d65_gamma26", G::P3D65, T::Gamma26, CR::DisplayReferred, 48.0f, "P3-D65 Gamma 2.6"},
    {"p3dci_gamma26", G::P3Dci, T::Gamma26, CR::DisplayReferred, 48.0f, "P3-DCI Gamma 2.6"},
    {"rec2020_bt1886", G::Rec2020, T::Bt1886, CR::DisplayReferred, 100.0f, "Rec.2020 BT.1886"},
    {"rec2100_pq1000", G::Rec2020, T::St2084, CR::DisplayReferred, 1000.0f,
     "Rec.2100 PQ (1000 nits)"},
    {"rec2100_hlg1000", G::Rec2020, T::Hlg, CR::DisplayReferred, 1000.0f,
     "Rec.2100 HLG (1000 nits)"},
}};

auto WhiteXy(const PrimariesXy& primaries) -> std::array<float, 2> {
  return {primaries[6], primaries[7]};
}

constexpr Matrix33d kIdentity = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

}  // namespace

auto ColorGamuts() -> std::span<const ColorGamut> { return kGamuts; }

auto FindColorGamut(ColorGamutId id) -> const ColorGamut& {
  const auto index = static_cast<std::size_t>(id);
  if (index >= kGamuts.size()) {
    throw std::invalid_argument("FindColorGamut: unknown gamut id");
  }
  return kGamuts[index];
}

auto GamutPrimariesXy(ColorGamutId id) -> const PrimariesXy& {
  return FindColorGamut(id).primaries_xy_;
}

auto ColorEncodings() -> std::span<const ColorEncoding> { return kEncodings; }

auto FindColorEncoding(std::string_view id) -> const ColorEncoding* {
  for (const auto& encoding : kEncodings) {
    if (encoding.id_ == id) {
      return &encoding;
    }
  }
  return nullptr;
}

auto MultiplyMatrices(const Matrix33d& a, const Matrix33d& b) -> Matrix33d {
  Matrix33d out{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    }
  }
  return out;
}

auto InvertMatrix(const Matrix33d& m) -> Matrix33d {
  Matrix33d t = m;
  Matrix33d s = kIdentity;
  for (int i = 0; i < 3; ++i) {
    int    pivot     = i;
    double pivotsize = std::abs(t[i * 3 + i]);
    for (int j = i + 1; j < 3; ++j) {
      const double tmp = std::abs(t[j * 3 + i]);
      if (tmp > pivotsize) {
        pivot     = j;
        pivotsize = tmp;
      }
    }
    if (pivotsize == 0.0) {
      throw std::runtime_error("InvertMatrix: singular matrix");
    }
    if (pivot != i) {
      for (int j = 0; j < 3; ++j) {
        std::swap(t[i * 3 + j], t[pivot * 3 + j]);
        std::swap(s[i * 3 + j], s[pivot * 3 + j]);
      }
    }
    for (int j = i + 1; j < 3; ++j) {
      const double f = t[j * 3 + i] / t[i * 3 + i];
      for (int k = 0; k < 3; ++k) {
        t[j * 3 + k] -= f * t[i * 3 + k];
        s[j * 3 + k] -= f * s[i * 3 + k];
      }
    }
  }
  for (int i = 2; i >= 0; --i) {
    const double f = t[i * 3 + i];
    if (f == 0.0) {
      throw std::runtime_error("InvertMatrix: singular matrix");
    }
    for (int j = 0; j < 3; ++j) {
      t[i * 3 + j] /= f;
      s[i * 3 + j] /= f;
    }
    for (int j = 0; j < i; ++j) {
      const double g = t[j * 3 + i];
      for (int k = 0; k < 3; ++k) {
        t[j * 3 + k] -= g * t[i * 3 + k];
        s[j * 3 + k] -= g * s[i * 3 + k];
      }
    }
  }
  return s;
}

auto RgbToXyzMatrix(const PrimariesXy& p) -> Matrix33d {
  const double    rx = p[0], ry = p[1], gx = p[2], gy = p[3], bx = p[4], by = p[5];
  const double    wx = p[6], wy = p[7];
  const Matrix33d xyz      = {rx, gx, bx, ry, gy, by, 1.0 - rx - ry, 1.0 - gx - gy, 1.0 - bx - by};
  const Matrix33d inv      = InvertMatrix(xyz);
  const double    white[3] = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
  Matrix33d       out{};
  for (int i = 0; i < 3; ++i) {
    const double gain =
        white[0] * inv[i * 3] + white[1] * inv[i * 3 + 1] + white[2] * inv[i * 3 + 2];
    for (int j = 0; j < 3; ++j) {
      out[j * 3 + i] = gain * xyz[j * 3 + i];
    }
  }
  return out;
}

auto RgbToXyzMatrix(ColorGamutId id) -> Matrix33d {
  if (id == ColorGamutId::CieXyz) {
    return kIdentity;
  }
  return RgbToXyzMatrix(GamutPrimariesXy(id));
}

auto ChromaticAdaptationMatrix(const std::array<float, 2>& source_white_xy,
                               const std::array<float, 2>& target_white_xy,
                               ChromaticAdaptation         method) -> Matrix33d {
  constexpr Matrix33d kCat02    = {0.7328, 0.4296, -0.1624, -0.7036, 1.6975,
                                   0.0061, 0.0030, 0.0136,  0.9834};
  constexpr Matrix33d kBradford = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135,
                                   0.0367, 0.0389, -0.0685, 1.0296};
  const Matrix33d&    cone      = method == ChromaticAdaptation::Bradford ? kBradford : kCat02;
  const double        sx = source_white_xy[0], sy = source_white_xy[1];
  const double        tx = target_white_xy[0], ty = target_white_xy[1];
  const double        src[3] = {sx / sy, 1.0, (1.0 - sx - sy) / sy};
  const double        dst[3] = {tx / ty, 1.0, (1.0 - tx - ty) / ty};
  double              src_lms[3]{}, dst_lms[3]{};
  for (int r = 0; r < 3; ++r) {
    src_lms[r] = cone[r * 3] * src[0] + cone[r * 3 + 1] * src[1] + cone[r * 3 + 2] * src[2];
    dst_lms[r] = cone[r * 3] * dst[0] + cone[r * 3 + 1] * dst[1] + cone[r * 3 + 2] * dst[2];
  }
  const Matrix33d scale = {dst_lms[0] / src_lms[0], 0, 0, 0, dst_lms[1] / src_lms[1], 0, 0, 0,
                           dst_lms[2] / src_lms[2]};
  return MultiplyMatrices(InvertMatrix(cone), MultiplyMatrices(scale, cone));
}

auto RgbToRgbMatrix(const PrimariesXy& source, const PrimariesXy& target,
                    ChromaticAdaptation method) -> Matrix33d {
  const Matrix33d to_xyz   = RgbToXyzMatrix(source);
  const Matrix33d from_xyz = InvertMatrix(RgbToXyzMatrix(target));
  if (source[6] == target[6] && source[7] == target[7]) {
    return MultiplyMatrices(from_xyz, to_xyz);
  }
  return MultiplyMatrices(
      from_xyz, MultiplyMatrices(
                    ChromaticAdaptationMatrix(WhiteXy(source), WhiteXy(target), method), to_xyz));
}

auto GamutConversionMatrix(ColorGamutId from, ColorGamutId to) -> Matrix33d {
  const ColorGamut& source   = FindColorGamut(from);
  const ColorGamut& target   = FindColorGamut(to);
  const auto        method   = source.aces_adaptation_ == ChromaticAdaptation::Bradford ||
                              target.aces_adaptation_ == ChromaticAdaptation::Bradford
                                   ? ChromaticAdaptation::Bradford
                                   : ChromaticAdaptation::Cat02;
  const Matrix33d   to_xyz   = RgbToXyzMatrix(from);
  const Matrix33d   from_xyz = InvertMatrix(RgbToXyzMatrix(to));
  if (from == ColorGamutId::CieXyz || to == ColorGamutId::CieXyz ||
      (source.primaries_xy_[6] == target.primaries_xy_[6] &&
       source.primaries_xy_[7] == target.primaries_xy_[7])) {
    return MultiplyMatrices(from_xyz, to_xyz);
  }
  return MultiplyMatrices(
      from_xyz, MultiplyMatrices(ChromaticAdaptationMatrix(WhiteXy(source.primaries_xy_),
                                                           WhiteXy(target.primaries_xy_), method),
                                 to_xyz));
}

}  // namespace alcedo::color
