//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Color encoding catalog (lut_color_encoding_plan.md, Phase L1): curves against OpenColorIO
// 2.5.1 builtins or the vendors' documents, gamut matrices against OpenColorIO, round trips and
// the encoding table. Only test targets link OpenColorIO.

#include "color/color_encoding_catalog.hpp"

#include <OpenColorIO/OpenColorIO.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "color/color_encoding_math.h"
#include "color_encoding_test_support.hpp"

namespace alcedo::color {
namespace {

namespace OCIO = OCIO_NAMESPACE;
using color_encoding_test::CodeValues;

constexpr const char* kStudioConfig = "studio-config-v4.0.0_aces-v2.0_ocio-v2.5";

auto                  StudioConfig() -> OCIO::ConstConfigRcPtr {
  static const auto config = OCIO::Config::CreateFromBuiltinConfig(kStudioConfig);
  return config;
}

auto BuiltinProcessor(const char* style) -> OCIO::ConstCPUProcessorRcPtr {
  auto transform = OCIO::BuiltinTransform::Create();
  transform->setStyle(style);
  return OCIO::Config::CreateRaw()->getProcessor(transform)->getDefaultCPUProcessor();
}

auto NamedTransformProcessor(const char* name) -> OCIO::ConstCPUProcessorRcPtr {
  return StudioConfig()->getProcessor(name, OCIO::TRANSFORM_DIR_FORWARD)->getDefaultCPUProcessor();
}

auto ApplyGrey(const OCIO::ConstCPUProcessorRcPtr& processor, float value) -> float {
  float rgb[3] = {value, value, value};
  processor->applyRGB(rgb);
  return rgb[1];
}

/// One curve and the OCIO 2.5.1 transform from its code values to linear light.
struct OcioCurveCase {
  const char* name_;
  int         transfer_;
  /// Builtin style, or a named transform of the studio config when builtin_ is false.
  const char* ocio_;
  bool        builtin_;
  /// OCIO linear value per catalog linear value (100 for ST 2084: OCIO uses 1.0 = 100 nits).
  float       ocio_scale_;
};

const std::array<OcioCurveCase, 14> kOcioCurveCases = {{
    {"ACEScc", CE_TF_ACESCC, "ACEScc_to_ACES2065-1", true, 1.0f},
    {"ACEScct", CE_TF_ACESCCT, "CURVE - ACEScct-LOG_to_LINEAR", true, 1.0f},
    {"ARRI LogC3 EI 800", CE_TF_ARRI_LOGC3_EI800, "ARRI LogC3 - Curve (EI800)", false, 1.0f},
    {"ARRI LogC4", CE_TF_ARRI_LOGC4, "ARRI LogC4 - Curve", false, 1.0f},
    {"Sony S-Log3", CE_TF_SONY_SLOG3, "S-Log3 - Curve", false, 1.0f},
    {"Panasonic V-Log", CE_TF_PANASONIC_VLOG, "V-Log - Curve", false, 1.0f},
    {"Canon Log 2", CE_TF_CANON_CLOG2, "CURVE - CANON_CLOG2_to_LINEAR", true, 1.0f},
    {"Canon Log 3", CE_TF_CANON_CLOG3, "CURVE - CANON_CLOG3_to_LINEAR", true, 1.0f},
    {"RED Log3G10", CE_TF_RED_LOG3G10, "Log3G10 - Curve", false, 1.0f},
    {"Blackmagic Film Gen 5", CE_TF_BLACKMAGIC_FILM_GEN5, "BMDFilm Gen5 Log - Curve", false, 1.0f},
    {"DaVinci Intermediate", CE_TF_DAVINCI_INTERMEDIATE, "DaVinci Intermediate Log - Curve", false,
     1.0f},
    {"Apple Log", CE_TF_APPLE_LOG, "CURVE - APPLE_LOG_to_LINEAR", true, 1.0f},
    {"DJI D-Log", CE_TF_DJI_DLOG, "D-Log - Curve", false, 1.0f},
    {"ST 2084", CE_TF_ST2084, "CURVE - ST-2084_to_LINEAR", true, 100.0f},
}};

TEST(ColorEncodingCatalog, CurvesMatchOcioBuiltinsOn4096CodeValues) {
  for (const auto& c : kOcioCurveCases) {
    SCOPED_TRACE(c.name_);
    const auto processor =
        c.builtin_ ? BuiltinProcessor(c.ocio_) : NamedTransformProcessor(c.ocio_);
    float worst    = 0.0f;
    float worst_at = 0.0f;
    for (const float code : CodeValues()) {
      const float ours  = CeDecode(c.transfer_, code) * c.ocio_scale_;
      const float ocio  = ApplyGrey(processor, code);
      // 1e-5 relative. Below a linear value of 1e-2 the bound is the absolute 1e-7: where a
      // curve crosses 0 (LogC4, Log3G10) single precision cancels about 7e-8.
      const float error = std::abs(ours - ocio) / std::fmax(std::abs(ocio), 1e-2f);
      if (error > worst) {
        worst    = error;
        worst_at = code;
      }
    }
    EXPECT_LE(worst, 1e-5f) << "worst at code value " << worst_at;
  }
}

/// Vendor document code values (10-bit) at 0%, 18% and 90% reflectance, and the document's
/// formula evaluated in double precision.
struct VendorCase {
  const char*        name_;
  int                transfer_;
  std::array<int, 3> code_10bit_;
  double (*formula_)(double reflectance);
};

auto FLogFormula(double x) -> double {
  return x >= 0.00089 ? 0.344676 * std::log10(0.555556 * x + 0.009468) + 0.790453
                      : 8.735631 * x + 0.092864;
}
auto FLog2Formula(double x) -> double {
  return x >= 0.000889 ? 0.245281 * std::log10(5.555556 * x + 0.064829) + 0.384316
                       : 8.799461 * x + 0.092864;
}
auto NLogFormula(double y) -> double {
  return (y < 0.328 ? 650.0 * std::cbrt(y + 0.0075) : 150.0 * std::log(y) + 619.0) / 1023.0;
}
auto DLogFormula(double x) -> double {
  return x <= 0.0078 ? 6.025 * x + 0.0929 : 0.256663 * std::log10(0.9892 * x + 0.0108) + 0.584555;
}
auto LLogFormula(double x) -> double {
  return x <= 0.006 ? 8.0 * x + 0.09 : 0.27 * std::log10(1.3 * x + 0.0115) + 0.6;
}

// Fujifilm "F-Log Data Sheet" Ver.1.2 and "F-Log2 Data Sheet" Ver.1.1, section 2-2; Nikon
// "N-Log Specification Document" 1.0.0 gives only the formula, and Nikon's N-Log technical guide
// places 18% grey at 10-bit code 372 (0% and 90% are the formula's values); DJI "White Paper on
// D-Log and D-Gamut" (2017), 18% at 408 and 90% at 586; Leica "L-Log Reference Manual" V1.9,
// Table 1.
const std::array<VendorCase, 5> kVendorCases = {{
    {"Fujifilm F-Log", CE_TF_FUJIFILM_FLOG, {95, 470, 705}, &FLogFormula},
    {"Fujifilm F-Log2", CE_TF_FUJIFILM_FLOG2, {95, 400, 570}, &FLog2Formula},
    {"Nikon N-Log", CE_TF_NIKON_NLOG, {127, 372, 603}, &NLogFormula},
    {"DJI D-Log", CE_TF_DJI_DLOG, {95, 408, 586}, &DLogFormula},
    {"Leica L-Log", CE_TF_LEICA_LLOG, {92, 445, 634}, &LLogFormula},
}};

TEST(ColorEncodingCatalog, CurvesWithoutOcioBuiltinMatchVendorCodeValues) {
  constexpr std::array<double, 3> kReflectance = {0.0, 0.18, 0.90};
  for (const auto& c : kVendorCases) {
    SCOPED_TRACE(c.name_);
    for (std::size_t i = 0; i < kReflectance.size(); ++i) {
      const float code = CeEncode(c.transfer_, static_cast<float>(kReflectance[i]));
      // The document's formula, within 1/4096 of a code value.
      EXPECT_NEAR(code, c.formula_(kReflectance[i]), 1.0 / 4096.0) << kReflectance[i];
      // The document's table rounds to whole 10-bit code values.
      EXPECT_EQ(static_cast<int>(std::lround(code * 1023.0f)), c.code_10bit_[i]) << kReflectance[i];
    }
  }
}

TEST(ColorEncodingCatalog, CanonLogMatchesThorpeFormulaAnd8BitCodeValues) {
  // "Canon-Log Transfer Characteristic" (Canon U.S.A. 2012), figure 9: the formula in legal-range
  // code values with x relative to 90% white, and 8-bit codes 32, 88 and 153 at 0%, 18% and 90%.
  constexpr std::array<double, 3> kReflectance = {0.0, 0.18, 0.90};
  constexpr std::array<int, 3>    kCode8Bit    = {32, 88, 153};
  for (std::size_t i = 0; i < kReflectance.size(); ++i) {
    const double ire  = 0.529136 * std::log10(10.1596 * kReflectance[i] / 0.9 + 1.0) + 0.0730597;
    const float  code = CeEncode(CE_TF_CANON_CLOG, static_cast<float>(kReflectance[i]));
    EXPECT_NEAR(code, (64.0 + 876.0 * ire) / 1023.0, 1.0 / 4096.0) << kReflectance[i];
    EXPECT_EQ(static_cast<int>(std::lround(code * 1023.0f / 4.0f)), kCode8Bit[i])
        << kReflectance[i];
  }
}

TEST(ColorEncodingCatalog, CineonMatchesOcioRedLogFilmInputTransformOnNeutralCodeValues) {
  // RED_REDLOGFILM-RWG_to_ACES2065-1 is the Cineon curve then REDWideGamutRGB to AP0. A neutral
  // stays neutral through the matrix, so the curve is the output over the matrix row sum.
  const auto   processor = BuiltinProcessor("RED_REDLOGFILM-RWG_to_ACES2065-1");
  const auto   matrix    = GamutConversionMatrix(ColorGamutId::RedWideGamutRgb, ColorGamutId::Ap0);
  const double row_sum   = matrix[3] + matrix[4] + matrix[5];
  float        worst = 0.0f, worst_at = 0.0f;
  for (const float code : CodeValues()) {
    const float ours  = static_cast<float>(CeDecode(CE_TF_CINEON, code) * row_sum);
    const float ocio  = ApplyGrey(processor, code);
    const float error = std::abs(ours - ocio) / std::fmax(std::abs(ocio), 1e-2f);
    if (error > worst) {
      worst    = error;
      worst_at = code;
    }
  }
  EXPECT_LE(worst, 1e-5f) << "worst at code value " << worst_at;
  // Black at code 95 and linear 1.0 at code 685.
  EXPECT_NEAR(CeEncode(CE_TF_CINEON, 0.0f) * 1023.0f, 95.0f, 1e-3f);
  EXPECT_NEAR(CeEncode(CE_TF_CINEON, 1.0f) * 1023.0f, 685.0f, 1e-3f);
}

TEST(ColorEncodingCatalog, ArriLogC3MatchesTheDocumentParametersAtEveryExposureIndex) {
  // "ALEXA Log C Curve - Usage in VFX" (ARRI 2017), SUP 3.x "Linear Scene Exposure Factor"
  // table: b, c, d, e, f per EI; a is 5.555556 and cut is where a * x + b = 1 / 9.
  struct ArriEi {
    int    transfer_;
    double b_, c_, d_, e_, f_;
  };
  constexpr std::array<ArriEi, 11> kTable = {{
      {CE_TF_ARRI_LOGC3_EI160, 0.080216, 0.269036, 0.381991, 5.842037, 0.092778},
      {CE_TF_ARRI_LOGC3_EI200, 0.076621, 0.266007, 0.382478, 5.776265, 0.092782},
      {CE_TF_ARRI_LOGC3_EI250, 0.072941, 0.262978, 0.382966, 5.710494, 0.092786},
      {CE_TF_ARRI_LOGC3_EI320, 0.068768, 0.259627, 0.383508, 5.637732, 0.092791},
      {CE_TF_ARRI_LOGC3_EI400, 0.064901, 0.256598, 0.383999, 5.571960, 0.092795},
      {CE_TF_ARRI_LOGC3_EI500, 0.060939, 0.253569, 0.384493, 5.506188, 0.092800},
      {CE_TF_ARRI_LOGC3_EI640, 0.056443, 0.250219, 0.385040, 5.433426, 0.092805},
      {CE_TF_ARRI_LOGC3_EI800, 0.052272, 0.247190, 0.385537, 5.367655, 0.092809},
      {CE_TF_ARRI_LOGC3_EI1000, 0.047996, 0.244161, 0.386036, 5.301883, 0.092814},
      {CE_TF_ARRI_LOGC3_EI1280, 0.043137, 0.240810, 0.386590, 5.229121, 0.092819},
      {CE_TF_ARRI_LOGC3_EI1600, 0.038625, 0.237781, 0.387093, 5.163350, 0.092824},
  }};
  for (const auto& ei : kTable) {
    SCOPED_TRACE(ei.transfer_);
    const double a     = 5.555556;
    const double cut   = (1.0 / 9.0 - ei.b_) / a;
    float        worst = 0.0f;
    for (const float x : color_encoding_test::LinearValues()) {
      const double expected =
          x > cut ? ei.c_ * std::log10(a * x + ei.b_) + ei.d_ : ei.e_ * x + ei.f_;
      worst = std::fmax(worst, static_cast<float>(std::abs(CeEncode(ei.transfer_, x) - expected)));
    }
    // The table rounds to 6 decimals; its e is the tangent at cut within 1.1e-5.
    EXPECT_LE(worst, 2e-6f);
  }
}

TEST(ColorEncodingCatalog, KineLog3MatchesKinefinityFormulaAndReferenceIre) {
  // "KineLOG3 Technical Specifications" (Kinefinity 2025): 18% grey at IRE 39.2, 90% at 57.2.
  const auto formula = [](double x) {
    return x >= -0.008239 ? std::log10(66.64 * x + 1.0) * 0.296 * 0.907136 + 0.092864
                          : (x + 0.008239) / 0.017178;
  };
  for (const double x : {-0.02, -0.005, 0.0, 0.01, 0.18, 0.9, 10.0}) {
    EXPECT_NEAR(CeEncode(CE_TF_KINEFINITY_KINELOG3, static_cast<float>(x)), formula(x), 1e-6) << x;
  }
  EXPECT_NEAR(CeEncode(CE_TF_KINEFINITY_KINELOG3, 0.18f) * 100.0f, 39.2f, 0.05f);
  EXPECT_NEAR(CeEncode(CE_TF_KINEFINITY_KINELOG3, 0.90f) * 100.0f, 57.2f, 0.05f);
}

TEST(ColorEncodingCatalog, GpLog2MatchesGoProFormulaAndMirrorsBelowZero) {
  // GoPro Labs "GP-Log2": log(599 * L + 1) / log(600) with 18% grey at L = 0.0517, about code
  // value 554 of 1023, mirrored about 0 for negative values.
  for (const double x : {0.0, 0.01, 0.18, 1.0, 3.0}) {
    const double l = x * 0.0517 / 0.18;
    EXPECT_NEAR(CeEncode(CE_TF_GOPRO_GPLOG2, static_cast<float>(x)),
                std::log(599.0 * l + 1.0) / std::log(600.0), 1e-6)
        << x;
    EXPECT_EQ(CeEncode(CE_TF_GOPRO_GPLOG2, static_cast<float>(-x)),
              -CeEncode(CE_TF_GOPRO_GPLOG2, static_cast<float>(x)))
        << x;
  }
  EXPECT_EQ(std::lround(CeEncode(CE_TF_GOPRO_GPLOG2, 0.18f) * 1023.0f), 554);
}

TEST(ColorEncodingCatalog, HlgMatchesBt2100InverseOetfOn4096CodeValues) {
  // ITU-R BT.2100 Table 5, evaluated in double precision.
  const double a = 0.17883277, b = 1.0 - 4.0 * a, c = 0.5 - a * std::log(4.0 * a);
  for (const float code : CodeValues()) {
    const double v        = code;
    const double expected = v <= 0.5 ? v * v / 3.0 : (std::exp((v - c) / a) + b) / 12.0;
    EXPECT_NEAR(CeHlgDecode(code), expected, 1e-6 * std::fmax(1.0, expected)) << code;
  }
}

TEST(ColorEncodingCatalog, EncodeAfterDecodeReturnsCodeValueWithin1e6ForEveryCurve) {
  // N-Log's document switches the decode at code 452 and the encode at reflectance 0.328, which
  // is code 451.69 on the cube-root segment: code values between them have no encode preimage.
  const float nlog_band_lo = 650.0f * std::cbrt(0.328f + 0.0075f) / 1023.0f;
  const float nlog_band_hi = 452.0f / 1023.0f;
  for (int tf = 0; tf < CE_TF_COUNT; ++tf) {
    SCOPED_TRACE(tf);
    float worst = 0.0f;
    for (const float code : CodeValues()) {
      if (tf == CE_TF_NIKON_NLOG && code > nlog_band_lo && code < nlog_band_hi) continue;
      // Below code 0.01 N-Log decodes to -0.0075 + (code * 1023 / 650)^3: single precision holds
      // that reflectance to about 5e-10, too coarse to return the code value within 1e-6. These
      // code values lie far below N-Log black (reflectance 0 is code 0.124).
      if (tf == CE_TF_NIKON_NLOG && code < 0.01f) continue;
      worst = std::fmax(worst, std::abs(CeEncode(tf, CeDecode(tf, code)) - code));
    }
    EXPECT_LE(worst, 1e-6f);
  }
}

TEST(ColorEncodingCatalog, AcesccMiddleGreyConstantEqualsEncodedMiddleGrey) {
  EXPECT_EQ(CE_ACESCC_MIDDLE_GREY, CeAcesccEncode(0.18f));
}

/// Matrix of an OCIO processor from linear RGB, read from the images of the unit vectors.
auto ProcessorMatrix(const OCIO::ConstCPUProcessorRcPtr& processor) -> Matrix33d {
  Matrix33d m{};
  for (int column = 0; column < 3; ++column) {
    float rgb[3] = {column == 0 ? 1.0f : 0.0f, column == 1 ? 1.0f : 0.0f,
                    column == 2 ? 1.0f : 0.0f};
    processor->applyRGB(rgb);
    for (int row = 0; row < 3; ++row) {
      m[static_cast<std::size_t>(row * 3 + column)] = rgb[row];
    }
  }
  return m;
}

struct GamutCase {
  ColorGamutId id_;
  const char*  ocio_colorspace_;
};

// Studio config colorspaces: the Academy / vendor camera CSC matrices and the utility spaces.
const std::array<GamutCase, 15> kGamutCases = {{
    {ColorGamutId::Ap0, "ACES2065-1"},
    {ColorGamutId::Rec709, "Linear Rec.709 (sRGB)"},
    {ColorGamutId::Rec2020, "Linear Rec.2020"},
    {ColorGamutId::P3D65, "Linear P3-D65"},
    {ColorGamutId::AdobeRgb, "Linear AdobeRGB"},
    {ColorGamutId::ArriWideGamut3, "Linear ARRI Wide Gamut 3"},
    {ColorGamutId::ArriWideGamut4, "Linear ARRI Wide Gamut 4"},
    {ColorGamutId::SGamut3, "Linear S-Gamut3"},
    {ColorGamutId::SGamut3Cine, "Linear S-Gamut3.Cine"},
    {ColorGamutId::VGamut, "Linear V-Gamut"},
    {ColorGamutId::CinemaGamut, "Linear CinemaGamut D55"},
    {ColorGamutId::RedWideGamutRgb, "Linear REDWideGamutRGB"},
    {ColorGamutId::BlackmagicWideGamutGen5, "Linear BMD WideGamut Gen5"},
    {ColorGamutId::DaVinciWideGamut, "Linear DaVinci WideGamut"},
    {ColorGamutId::DGamut, "Linear D-Gamut"},
}};

TEST(ColorEncodingCatalog, GamutToAp1MatricesMatchOcioWithin1e6) {
  for (const auto& c : kGamutCases) {
    SCOPED_TRACE(c.ocio_colorspace_);
    const auto expected = ProcessorMatrix(
        StudioConfig()->getProcessor(c.ocio_colorspace_, "ACEScg")->getDefaultCPUProcessor());
    const auto ours = GamutConversionMatrix(c.id_, ColorGamutId::Ap1);
    for (std::size_t i = 0; i < 9; ++i) {
      EXPECT_NEAR(ours[i], expected[i], 1e-6) << i;
    }
  }
}

TEST(ColorEncodingCatalog, Rec2020ToAp0MatrixMatchesOcioAppleLogInputTransform) {
  // APPLE_LOG_to_ACES2065-1 is the Apple Log curve then Rec.2020 to AP0 (Bradford). Code values
  // that decode to 0 and 1 isolate the matrix columns; the bound includes the curve's decode.
  const auto  processor = BuiltinProcessor("APPLE_LOG_to_ACES2065-1");
  const float zero      = CeAppleLogEncode(0.0f);
  const float one       = CeAppleLogEncode(1.0f);
  const auto  ours      = GamutConversionMatrix(ColorGamutId::Rec2020, ColorGamutId::Ap0);
  for (int column = 0; column < 3; ++column) {
    float rgb[3] = {column == 0 ? one : zero, column == 1 ? one : zero, column == 2 ? one : zero};
    processor->applyRGB(rgb);
    for (int row = 0; row < 3; ++row) {
      EXPECT_NEAR(ours[static_cast<std::size_t>(row * 3 + column)], rgb[row], 1e-5)
          << row << ',' << column;
    }
  }
}

TEST(ColorEncodingCatalog, FGamutCToAp0MatrixMatchesFujifilmInputTransform) {
  // FUJIFILM_IDT_F-Log2C_Ver.1.10.ctl (Fujifilm 2026): F-Gamut C to AP0 with CAT02.
  constexpr Matrix33d kFujifilm = {0.84089501562825,  0.02752756409115,  0.13157742028060,
                                   0.00092026357010,  1.00739011387088,  -0.00831037744099,
                                   -0.00055982840886, -0.00077644453594, 1.00133627294480};
  const auto          ours      = GamutConversionMatrix(ColorGamutId::FGamutC, ColorGamutId::Ap0);
  for (std::size_t i = 0; i < 9; ++i) {
    EXPECT_NEAR(ours[i], kFujifilm[i], 1e-6) << i;
  }
}

TEST(ColorEncodingCatalog, KinefinityWideGamutToXyzMatchesKinefinityMatrix) {
  // "KineLOG3 Technical Specifications" (Kinefinity 2025), to 4 decimals.
  constexpr Matrix33d kKinefinity = {0.6879,  0.1979, 0.0646,  0.2073, 1.0622,
                                     -0.2696, 0.0134, -0.3349, 1.4106};
  const auto          ours        = RgbToXyzMatrix(ColorGamutId::KinefinityWideGamut);
  for (std::size_t i = 0; i < 9; ++i) {
    EXPECT_NEAR(ours[i], kKinefinity[i], 5e-5) << i;
  }
}

TEST(ColorEncodingCatalog, CieXyzRgbToXyzIsTheIdentity) {
  const auto m = RgbToXyzMatrix(ColorGamutId::CieXyz);
  for (std::size_t i = 0; i < 9; ++i) {
    EXPECT_EQ(m[i], i % 4 == 0 ? 1.0 : 0.0) << i;
  }
}

TEST(ColorEncodingCatalog, GamutsAreIndexedByIdAndPrimariesAreReadById) {
  for (std::size_t i = 0; i < ColorGamuts().size(); ++i) {
    const auto& gamut = ColorGamuts()[i];
    EXPECT_EQ(static_cast<std::size_t>(gamut.id_), i);
    EXPECT_EQ(&FindColorGamut(gamut.id_), &gamut);
    EXPECT_EQ(GamutPrimariesXy(gamut.id_), gamut.primaries_xy_);
  }
}

TEST(ColorEncodingCatalog, EncodingIdsAreUniqueLowerCaseAndFoundById) {
  std::set<std::string_view> ids;
  for (const auto& encoding : ColorEncodings()) {
    SCOPED_TRACE(std::string(encoding.id_));
    EXPECT_TRUE(ids.insert(encoding.id_).second);
    for (const char ch : encoding.id_) {
      EXPECT_TRUE((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_');
    }
    EXPECT_EQ(FindColorEncoding(encoding.id_), &encoding);
    if (encoding.referral_ == ColorReferral::DisplayReferred) {
      EXPECT_GT(encoding.peak_luminance_nits_, 0.0f);
    } else {
      EXPECT_EQ(encoding.peak_luminance_nits_, 0.0f);
    }
  }
  EXPECT_EQ(ids.size(), 50u);
  ASSERT_NE(FindColorEncoding(kDefaultColorEncodingId), nullptr);
  EXPECT_EQ(FindColorEncoding(kDefaultColorEncodingId)->transfer_, TransferFunctionId::Acescc);
  EXPECT_EQ(FindColorEncoding(kDefaultColorEncodingId)->gamut_, ColorGamutId::Ap1);
}

TEST(ColorEncodingCatalog, UnknownEncodingIdIsNotFound) {
  EXPECT_EQ(FindColorEncoding("srgb"), nullptr);
  EXPECT_EQ(FindColorEncoding("ACEScc"), nullptr);
  EXPECT_EQ(FindColorEncoding(""), nullptr);
}

TEST(ColorEncodingCatalog, HlgDisplayEncodingAppliesTheInverseOotfOfTheReferenceDisplay) {
  // A neutral at display light d: inverse OOTF gives scene light d^(1/1.2), then the OETF.
  const float d    = 0.25f;
  const float gain = CeHlgDisplayGain(d, d, d);
  EXPECT_NEAR(CeDisplayEncodeChannel(CE_TF_HLG, d, gain), CeHlgEncode(std::pow(d, 1.0f / 1.2f)),
              1e-6f);
  // ST 2084 display values are in cd/m^2.
  EXPECT_NEAR(CeDisplayEncodeChannel(CE_TF_ST2084, 100.0f, 1.0f), CePqEncode(0.01f), 1e-7f);
  EXPECT_EQ(CeDisplayEncodeChannel(CE_TF_GAMMA_2_2, -0.5f, 1.0f), 0.0f);
}

}  // namespace
}  // namespace alcedo::color
