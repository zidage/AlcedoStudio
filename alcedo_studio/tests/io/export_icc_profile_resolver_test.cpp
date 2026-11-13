//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/export_icc_profile_resolver.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

#include "icc_tone_curve_test_support.hpp"

namespace alcedo {
namespace {

using ColorSpace = ColorUtils::ColorSpace;
using EOTF = ColorUtils::EOTF;

TEST(ExportIccProfileResolverTests, SupportedProfilesResolveToBytes) {
  const std::array<ExportColorProfileConfig, 9> configs = {{
      {ColorSpace::REC709, EOTF::BT1886, 100.0f},
      {ColorSpace::REC709, EOTF::GAMMA_2_2, 100.0f},
      {ColorSpace::P3_D65, EOTF::GAMMA_2_2, 100.0f},
      {ColorSpace::P3_D65, EOTF::ST2084, 600.0f},
      {ColorSpace::P3_D60, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::P3_DCI, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::XYZ, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::REC2020, EOTF::ST2084, 600.0f},
      {ColorSpace::REC2020, EOTF::HLG, 600.0f},
  }};

  for (const auto& config : configs) {
    const auto bytes = ExportIccProfileResolver::ResolveIccProfileBytes(config);
    EXPECT_FALSE(bytes.empty()) << ColorUtils::ColorSpaceToString(config.encoding_space) << "/"
                                << ColorUtils::EOTFToString(config.encoding_eotf);
  }
}

TEST(ExportIccProfileResolverTests, SupportedProfilesResolveToExistingFiles) {
  const std::array<ExportColorProfileConfig, 9> configs = {{
      {ColorSpace::REC709, EOTF::BT1886, 100.0f},
      {ColorSpace::REC709, EOTF::GAMMA_2_2, 100.0f},
      {ColorSpace::P3_D65, EOTF::GAMMA_2_2, 100.0f},
      {ColorSpace::P3_D65, EOTF::ST2084, 600.0f},
      {ColorSpace::P3_D60, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::P3_DCI, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::XYZ, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::REC2020, EOTF::ST2084, 600.0f},
      {ColorSpace::REC2020, EOTF::HLG, 600.0f},
  }};

  for (const auto& config : configs) {
    const auto path = ExportIccProfileResolver::ResolveConfigProfilePath(config);
    ASSERT_TRUE(path.has_value()) << ColorUtils::ColorSpaceToString(config.encoding_space) << "/"
                                  << ColorUtils::EOTFToString(config.encoding_eotf);
    EXPECT_TRUE(std::filesystem::exists(*path));
    EXPECT_TRUE(std::filesystem::is_regular_file(*path));
  }
}

// The embedded profile must decode the exported code values with the curve that encoded them; a
// profile with another curve shifts the colors a color-managed viewer shows (issue #391).
TEST(ExportIccProfileResolverTests, SdrProfilesDeclareTheEncodingToneCurve) {
  const std::array<ExportColorProfileConfig, 8> configs = {{
      {ColorSpace::REC709, EOTF::BT1886, 100.0f},
      {ColorSpace::REC709, EOTF::GAMMA_2_2, 100.0f},
      {ColorSpace::REC709, EOTF::SRGB_PIECEWISE, 100.0f},
      {ColorSpace::P3_D65, EOTF::GAMMA_2_2, 100.0f},
      {ColorSpace::P3_D65, EOTF::SRGB_PIECEWISE, 100.0f},
      {ColorSpace::P3_D60, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::P3_DCI, EOTF::GAMMA_2_6, 100.0f},
      {ColorSpace::XYZ, EOTF::GAMMA_2_6, 100.0f},
  }};

  for (const auto& config : configs) {
    const std::string label = ColorUtils::ColorSpaceToString(config.encoding_space) + "/" +
                              ColorUtils::EOTFToString(config.encoding_eotf);
    const auto bytes = ExportIccProfileResolver::ResolveIccProfileBytes(config);
    ASSERT_GE(bytes.size(), 20u) << label;
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes.data() + 16), 4), "RGB ")
        << label;
    for (const std::string_view tag : {"rTRC", "gTRC", "bTRC"}) {
      const auto curve = icc_tone_curve_test::ReadIccToneCurve(bytes, tag);
      ASSERT_TRUE(curve.has_value()) << label << " " << tag;
      for (const double code : {0.02, 0.05, 0.1, 0.18, 0.25, 0.5, 0.75, 1.0}) {
        const double expected =
            CeDecode(static_cast<int>(config.encoding_eotf), static_cast<float>(code));
        EXPECT_NEAR(curve->Evaluate(code), expected, 5e-4)
            << label << " " << tag << " code value " << code;
      }
    }
  }
}

}  // namespace
}  // namespace alcedo
