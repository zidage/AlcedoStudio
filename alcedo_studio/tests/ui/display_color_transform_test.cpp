//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file display_color_transform_test.cpp
/// @brief W1 of the Windows display ICC plan: the display table and the 8-bit image conversion
///        from a viewer encoding to a monitor profile.
///
/// The display profiles are LittleCMS matrix profiles that the test builds from the gamut catalog.
/// The expected values come from the catalog matrices and the Ce* curve functions, not from the
/// code under test.

#include "ui/edit_viewer/display_color_transform.hpp"

#include <gtest/gtest.h>
#include <lcms2.h>

#include <QImage>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "color/color_encoding_math.h"

namespace alcedo {
namespace {

using color::ColorGamutId;

auto SaveProfile(cmsHPROFILE profile) -> std::vector<std::byte> {
  cmsUInt32Number size = 0;
  EXPECT_TRUE(cmsSaveProfileToMem(profile, nullptr, &size));
  std::vector<std::byte> bytes(size);
  EXPECT_TRUE(cmsSaveProfileToMem(profile, bytes.data(), &size));
  cmsCloseProfile(profile);
  return bytes;
}

/// A matrix display profile with the primaries and the white of @p gamut and a pure gamma curve.
auto MatrixDisplayProfile(ColorGamutId gamut, double gamma) -> std::vector<std::byte> {
  const auto&           xy = color::GamutPrimariesXy(gamut);
  const cmsCIExyY       white{xy[6], xy[7], 1.0};
  const cmsCIExyYTRIPLE primaries{{xy[0], xy[1], 1.0}, {xy[2], xy[3], 1.0}, {xy[4], xy[5], 1.0}};
  cmsToneCurve*         curve     = cmsBuildGamma(nullptr, gamma);
  cmsToneCurve*         curves[3] = {curve, curve, curve};
  cmsHPROFILE           profile   = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  return SaveProfile(profile);
}

auto Encoding(ColorUtils::ColorSpace space, ColorUtils::EOTF eotf) -> ViewerDisplayConfig {
  ViewerDisplayConfig config;
  config.encoding_space = space;
  config.encoding_eotf  = eotf;
  return config;
}

auto Node(const DisplayTransformLut& lut, int r, int g, int b) -> std::array<float, 4> {
  const auto i = ((static_cast<std::size_t>(b) * lut.edge + g) * lut.edge + r) * 4;
  return {lut.rgba[i], lut.rgba[i + 1], lut.rgba[i + 2], lut.rgba[i + 3]};
}

auto Coordinate(const DisplayTransformLut& lut, int index) -> float {
  return static_cast<float>(index) / static_cast<float>(lut.edge - 1);
}

/// Calls @p check for each node of @p lut and stops after 8 failures.
template <class Check>
void ForEachNode(const DisplayTransformLut& lut, Check check) {
  int failures = 0;
  for (int b = 0; b < lut.edge && failures < 8; ++b) {
    for (int g = 0; g < lut.edge && failures < 8; ++g) {
      for (int r = 0; r < lut.edge && failures < 8; ++r) {
        if (!check(r, g, b, Node(lut, r, g, b))) {
          ++failures;
        }
      }
    }
  }
  EXPECT_EQ(failures, 0);
}

/// Linear-light tolerance of a channel on a pure power curve display. A stored ICC profile has
/// s15Fixed16 colorants: each one is rounded by up to half a step (7.6e-6). The XYZ to RGB matrix
/// of the display multiplies the rounding by its largest absolute row sum (about 5.3 for the
/// Rec.709 and P3 primaries), for each of the two other full channels. So a channel that the
/// analytic conversion gives as 0 can be up to 8.1e-5 in linear light. A pure power curve has an
/// infinite slope at 0 and encodes 8.1e-5 as 0.014.
constexpr double kColorantLinearTolerance = 2 * 7.63e-6 * 5.3;

/**
 * @brief Each channel of @p node equals @p expected within @p tolerance in code value, or, on a
 *        display with the pure power curve @p display_gamma (0: none), within
 *        kColorantLinearTolerance in linear light.
 */
auto ExpectNode(int r, int g, int b, const std::array<float, 4>& node,
                const std::array<double, 3>& expected, double tolerance,
                double display_gamma = 0.0) -> bool {
  bool ok = node[3] == 1.0f;
  for (int c = 0; c < 3; ++c) {
    const bool code_equal = std::abs(node[c] - expected[c]) <= tolerance;
    const bool linear_equal =
        display_gamma > 0.0 &&
        std::abs(std::pow(static_cast<double>(node[c]), display_gamma) -
                 std::pow(std::max(expected[c], 0.0), display_gamma)) <= kColorantLinearTolerance;
    ok = ok && (code_equal || linear_equal);
  }
  if (!ok) {
    ADD_FAILURE() << "node (" << r << ", " << g << ", " << b << ") = (" << node[0] << ", "
                  << node[1] << ", " << node[2] << ", " << node[3] << "), expected ("
                  << expected[0] << ", " << expected[1] << ", " << expected[2] << ", 1)";
  }
  return ok;
}

TEST(DisplayColorTransformTest, SameEncodingAndDisplayProfileGiveIdentityTable) {
  const auto display = MatrixDisplayProfile(ColorGamutId::Rec709, 2.2);
  const auto start   = std::chrono::steady_clock::now();
  const auto lut     = BuildDisplayTransformLut(
      Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2), display);
  const auto elapsed = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  // Exit criterion of W1: the build time of one 65-node table.
  std::cout << "65-node display table build time: " << elapsed << " ms\n";
  RecordProperty("table_build_ms", std::to_string(elapsed));

  ASSERT_EQ(lut.edge, kDisplayTransformLutEdge);
  ASSERT_EQ(lut.rgba.size(), static_cast<std::size_t>(65 * 65 * 65 * 4));
  EXPECT_EQ(lut.input_exponent, 1.0f);
  ForEachNode(lut, [&](int r, int g, int b, const std::array<float, 4>& node) {
    return ExpectNode(r, g, b, node, {Coordinate(lut, r), Coordinate(lut, g), Coordinate(lut, b)},
                      5e-4, 2.2);
  });
}

TEST(DisplayColorTransformTest, Rec709OnP3DisplayEqualsMatrixConversion) {
  const auto display = MatrixDisplayProfile(ColorGamutId::P3D65, 2.2);
  const auto lut     = BuildDisplayTransformLut(
      Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2), display, 17);
  const auto m       = color::GamutConversionMatrix(ColorGamutId::Rec709, ColorGamutId::P3D65);
  ForEachNode(lut, [&](int r, int g, int b, const std::array<float, 4>& node) {
    const std::array<double, 3> linear = {CeDecode(CE_TF_GAMMA_2_2, Coordinate(lut, r)),
                                          CeDecode(CE_TF_GAMMA_2_2, Coordinate(lut, g)),
                                          CeDecode(CE_TF_GAMMA_2_2, Coordinate(lut, b))};
    std::array<double, 3>       expected{};
    for (int row = 0; row < 3; ++row) {
      const double value = m[row * 3] * linear[0] + m[row * 3 + 1] * linear[1] +
                           m[row * 3 + 2] * linear[2];
      expected[row] = std::clamp(
          static_cast<double>(CeEncode(CE_TF_GAMMA_2_2, static_cast<float>(value))), 0.0, 1.0);
    }
    return ExpectNode(r, g, b, node, expected, 1e-3, 2.2);
  });
  // The table is not the identity: Rec.709 red is inside the P3 gamut.
  EXPECT_LT(Node(lut, 16, 0, 0)[0], 0.95f);
}

TEST(DisplayColorTransformTest, P3RedOutsideSrgbDisplayClipsPerChannel) {
  const auto lut = BuildDisplayTransformLut(
      Encoding(ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::SRGB_PIECEWISE),
      SrgbDisplayProfile(), 5);
  const auto red = Node(lut, 4, 0, 0);
  EXPECT_NEAR(red[0], 1.0f, 1e-4f);
  EXPECT_NEAR(red[1], 0.0f, 1e-4f);
  EXPECT_NEAR(red[2], 0.0f, 1e-4f);
  EXPECT_EQ(red[3], 1.0f);
}

TEST(DisplayColorTransformTest, LinearEncodingUsesInputExponent) {
  const auto display = MatrixDisplayProfile(ColorGamutId::Rec709, 2.2);
  const auto lut     = BuildDisplayTransformLut(
      Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::LINEAR), display, 33);
  EXPECT_FLOAT_EQ(lut.input_exponent, 1.0f / 2.2f);
  // Node coordinate c has the linear code c ^ 2.2, which the gamma 2.2 display encodes to c.
  ForEachNode(lut, [&](int r, int g, int b, const std::array<float, 4>& node) {
    return ExpectNode(r, g, b, node, {Coordinate(lut, r), Coordinate(lut, g), Coordinate(lut, b)},
                      5e-4, 2.2);
  });
}

TEST(DisplayColorTransformTest, SrgbCurveOnSrgbDisplayIsIdentity) {
  const auto lut = BuildDisplayTransformLut(
      Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::SRGB_PIECEWISE),
      SrgbDisplayProfile(), 33);
  ForEachNode(lut, [&](int r, int g, int b, const std::array<float, 4>& node) {
    return ExpectNode(r, g, b, node, {Coordinate(lut, r), Coordinate(lut, g), Coordinate(lut, b)},
                      5e-4);
  });
}

TEST(DisplayColorTransformTest, Gamma22OnSrgbDisplayEqualsCurveConversion) {
  const auto lut = BuildDisplayTransformLut(
      Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2), SrgbDisplayProfile(),
      kDisplayTransformLutEdge);
  int failures = 0;
  for (int i = 0; i < lut.edge && failures < 8; ++i) {
    const float  code     = Coordinate(lut, i);
    const double expected = CeSrgbEncode(std::pow(code, 2.2f));
    const auto   node     = Node(lut, i, i, i);
    if (!ExpectNode(i, i, i, node, {expected, expected, expected}, 1e-3)) {
      ++failures;
    }
  }
  EXPECT_EQ(failures, 0);
  // Plan decision D2: the shadows change. Code 0.094 is 0.0055 in linear light and 0.066 in sRGB.
  EXPECT_LT(Node(lut, 6, 6, 6)[0], Coordinate(lut, 6) - 0.02f);
}

TEST(DisplayColorTransformTest, HdrEncodingIsRejected) {
  const auto display = SrgbDisplayProfile();
  for (const auto eotf : {ColorUtils::EOTF::ST2084, ColorUtils::EOTF::HLG}) {
    const auto config = Encoding(ColorUtils::ColorSpace::REC2020, eotf);
    EXPECT_FALSE(HasDisplayTransform(config));
    EXPECT_THROW((void)BuildDisplayTransformLut(config, display), std::invalid_argument);
    QImage image(2, 2, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    EXPECT_THROW(ConvertImageToDisplay(image, config, display), std::invalid_argument);
  }
  EXPECT_TRUE(HasDisplayTransform(
      Encoding(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::BT1886)));
}

TEST(DisplayColorTransformTest, UnusableProfileReportsItsReason) {
  const auto config = Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2);
  const auto message_of = [&](const std::vector<std::byte>& bytes) -> std::string {
    try {
      (void)BuildDisplayTransformLut(config, bytes, 2);
    } catch (const std::runtime_error& error) {
      return error.what();
    }
    ADD_FAILURE() << "the profile was accepted";
    return {};
  };

  const std::string empty = message_of({});

  std::vector<std::byte> random(512);
  std::uint32_t          state = 0x2545F491u;
  for (auto& value : random) {
    state = state * 1664525u + 1013904223u;
    value = static_cast<std::byte>(state >> 24);
  }
  const std::string garbage = message_of(random);

  cmsToneCurve* curve     = cmsBuildGamma(nullptr, 1.0);
  cmsToneCurve* curves[4] = {curve, curve, curve, curve};
  cmsHPROFILE   cmyk      = cmsCreateLinearizationDeviceLink(cmsSigCmykData, curves);
  cmsFreeToneCurve(curve);
  const std::string not_rgb = message_of(SaveProfile(cmyk));

  EXPECT_NE(empty.find("empty"), std::string::npos) << empty;
  EXPECT_NE(garbage.find("cannot be parsed"), std::string::npos) << garbage;
  EXPECT_NE(not_rgb.find("not an RGB profile"), std::string::npos) << not_rgb;
  EXPECT_NE(empty, garbage);
  EXPECT_NE(garbage, not_rgb);
  EXPECT_NE(empty, not_rgb);
}

TEST(DisplayColorTransformTest, ConvertedImageEqualsTableAtNodeColors) {
  const auto display = MatrixDisplayProfile(ColorGamutId::P3D65, 2.2);
  const auto config  = Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2);
  // 18 nodes: every node coordinate is a whole 8-bit code value (a multiple of 15).
  const auto lut     = BuildDisplayTransformLut(config, display, 18);

  const int edge = lut.edge;
  QImage    image(edge * edge, edge, QImage::Format_RGBA8888);
  for (int b = 0; b < edge; ++b) {
    uchar* row = image.scanLine(b);
    for (int g = 0; g < edge; ++g) {
      for (int r = 0; r < edge; ++r) {
        uchar* pixel = row + static_cast<std::size_t>(g * edge + r) * 4;
        pixel[0]     = static_cast<uchar>(r * 15);
        pixel[1]     = static_cast<uchar>(g * 15);
        pixel[2]     = static_cast<uchar>(b * 15);
        pixel[3]     = static_cast<uchar>((r + g + b) * 5);
      }
    }
  }
  ConvertImageToDisplay(image, config, display);
  ASSERT_EQ(image.format(), QImage::Format_RGBA8888);

  int failures = 0;
  for (int b = 0; b < edge && failures < 8; ++b) {
    const uchar* row = image.constScanLine(b);
    for (int g = 0; g < edge && failures < 8; ++g) {
      for (int r = 0; r < edge && failures < 8; ++r) {
        const uchar* pixel = row + static_cast<std::size_t>(g * edge + r) * 4;
        const auto   node  = Node(lut, r, g, b);
        bool         ok    = pixel[3] == static_cast<uchar>((r + g + b) * 5);
        for (int c = 0; c < 3; ++c) {
          ok = ok && std::abs(pixel[c] / 255.0 - node[c]) <= 1.0 / 255.0;
        }
        if (!ok) {
          ADD_FAILURE() << "node (" << r << ", " << g << ", " << b << ") converted to ("
                        << int(pixel[0]) << ", " << int(pixel[1]) << ", " << int(pixel[2]) << ", "
                        << int(pixel[3]) << "), table (" << node[0] * 255 << ", " << node[1] * 255
                        << ", " << node[2] * 255 << ")";
          ++failures;
        }
      }
    }
  }
  EXPECT_EQ(failures, 0);

  QImage other(2, 2, QImage::Format_ARGB32);
  EXPECT_THROW(ConvertImageToDisplay(other, config, display), std::invalid_argument);
}

// Windows display ICC plan, W6: an image store converts many thumbnails with one prepared
// conversion, also from several image provider threads at once.
TEST(DisplayColorTransformTest, PreparedTransformGivesTheOneShotPixelsOnEveryThread) {
  const auto display = MatrixDisplayProfile(ColorGamutId::P3D65, 2.2);
  const auto config  = Encoding(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::SRGB_PIECEWISE);
  QImage     source(96, 64, QImage::Format_RGBA8888);
  for (int y = 0; y < source.height(); ++y) {
    uchar* row = source.scanLine(y);
    for (int x = 0; x < source.width(); ++x) {
      row[x * 4 + 0] = static_cast<uchar>(x * 255 / (source.width() - 1));
      row[x * 4 + 1] = static_cast<uchar>(y * 255 / (source.height() - 1));
      row[x * 4 + 2] = static_cast<uchar>((x * 7 + y * 13) % 256);
      row[x * 4 + 3] = static_cast<uchar>((x + y) % 256);
    }
  }
  QImage expected = source.copy();
  ConvertImageToDisplay(expected, config, display);
  ASSERT_NE(expected, source);

  const DisplayImageTransform prepared(config, display);
  constexpr int               kThreads = 4;
  std::vector<QImage>         results(kThreads * 3);
  std::vector<std::thread>    threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 3; ++i) {
        QImage image = source.copy();
        prepared.Apply(image);
        results[static_cast<std::size_t>(t * 3 + i)] = image;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (const auto& image : results) {
    EXPECT_EQ(image, expected);
  }

  QImage other(2, 2, QImage::Format_ARGB32);
  EXPECT_THROW(prepared.Apply(other), std::invalid_argument);
  EXPECT_THROW(DisplayImageTransform(
                   Encoding(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084), display),
               std::invalid_argument);
}

}  // namespace
}  // namespace alcedo
