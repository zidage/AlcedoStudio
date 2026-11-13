//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#import <AppKit/AppKit.h>
#include <CoreGraphics/CGColorSpace.h>
#import <QuartzCore/CAMetalLayer.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "io/image/export_color_profile_config.hpp"
#include "io/image/export_icc_profile_resolver.hpp"
#include "ui/edit_viewer/color_manager.hpp"

namespace alcedo {
namespace {

auto MakeMetalView() -> NSView* {
  NSView* view    = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 320.0, 180.0)];
  view.wantsLayer = YES;
  view.layer      = [CAMetalLayer layer];
  return view;
}

auto CopyIccBytes(CGColorSpaceRef color_space) -> std::vector<uint8_t> {
  CFDataRef icc_data = CGColorSpaceCopyICCData(color_space);
  if (!icc_data) {
    return {};
  }
  const UInt8*         begin = CFDataGetBytePtr(icc_data);
  std::vector<uint8_t> bytes(begin, begin + CFDataGetLength(icc_data));
  CFRelease(icc_data);
  return bytes;
}

auto ExportIccBytes(ColorUtils::ColorSpace encoding_space, ColorUtils::EOTF encoding_eotf)
    -> std::vector<uint8_t> {
  return ExportIccProfileResolver::ResolveIccProfileBytes(
      ExportColorProfileConfig{encoding_space, encoding_eotf, 100.0f});
}

TEST(EditorWindowColorManagementTest, PqAndHlgApplyEdrMetadataThenSdrClearsIt) {
  @autoreleasepool {
    NSView*             view  = MakeMetalView();
    CAMetalLayer*       layer = (CAMetalLayer*)view.layer;

    ViewerDisplayConfig pq;
    pq.encoding_space = ColorUtils::ColorSpace::REC2020;
    pq.encoding_eotf  = ColorUtils::EOTF::ST2084;
    pq.peak_luminance = 1600.0f;
    ASSERT_TRUE(ColorManager::ApplyWindowColorSpace((__bridge void*)view, pq));
    EXPECT_TRUE(layer.wantsExtendedDynamicRangeContent);
    EXPECT_NE(layer.EDRMetadata, nil);
    ASSERT_NE(layer.colorspace, nullptr);
    EXPECT_TRUE(CFEqual(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceITUR_2100_PQ));

    ViewerDisplayConfig hlg;
    hlg.encoding_space = ColorUtils::ColorSpace::P3_D65;
    hlg.encoding_eotf  = ColorUtils::EOTF::HLG;
    hlg.peak_luminance = 1000.0f;
    ASSERT_TRUE(ColorManager::ApplyWindowColorSpace((__bridge void*)view, hlg));
    EXPECT_TRUE(layer.wantsExtendedDynamicRangeContent);
    EXPECT_NE(layer.EDRMetadata, nil);
    ASSERT_NE(layer.colorspace, nullptr);
    EXPECT_TRUE(CFEqual(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceDisplayP3_HLG));

    ASSERT_TRUE(ColorManager::ApplyWindowColorSpace((__bridge void*)view, ViewerDisplayConfig{}));
    EXPECT_FALSE(layer.wantsExtendedDynamicRangeContent);
    EXPECT_EQ(layer.EDRMetadata, nil);
    ASSERT_NE(layer.colorspace, nullptr);
    const auto expected_icc =
        ExportIccBytes(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2);
    ASSERT_FALSE(expected_icc.empty());
    EXPECT_EQ(CopyIccBytes(layer.colorspace), expected_icc);
  }
}

// The preview decodes the code values with the curve that encoded them, the same curve as the
// exported file's profile: gamma 2.2 or 2.4 on Rec.709 primaries, not the sRGB curve of
// kCGColorSpaceSRGB (issue #391). ColorSync limits the slope of a pure power curve near black to
// 1/16, which replaces x^2.4 below code value 0.14 and x^2.2 below 0.10 for every gamma profile,
// the exported file's included, so the decoded values are compared from code value 0.18 up. There
// the sRGB and BT.709 camera curves differ from gamma 2.2 by more than 0.003.
TEST(EditorWindowColorManagementTest, Rec709PowerCurvesTagTheLayerWithTheEncodingCurve) {
  @autoreleasepool {
    NSView*         view   = MakeMetalView();
    CAMetalLayer*   layer  = (CAMetalLayer*)view.layer;
    CGColorSpaceRef linear = CGColorSpaceCreateWithName(kCGColorSpaceLinearSRGB);
    ASSERT_NE(linear, nullptr);

    for (const auto eotf : {ColorUtils::EOTF::GAMMA_2_2, ColorUtils::EOTF::BT1886}) {
      ViewerDisplayConfig config;
      config.encoding_space = ColorUtils::ColorSpace::REC709;
      config.encoding_eotf  = eotf;
      ASSERT_TRUE(ColorManager::ApplyWindowColorSpace((__bridge void*)view, config));
      ASSERT_NE(layer.colorspace, nullptr);
      EXPECT_EQ(CopyIccBytes(layer.colorspace), ExportIccBytes(config.encoding_space, eotf));

      for (const CGFloat code : {0.18, 0.25, 0.5, 0.75, 1.0}) {
        const CGFloat components[4] = {code, code, code, 1.0};
        CGColorRef    encoded       = CGColorCreate(layer.colorspace, components);
        ASSERT_NE(encoded, nullptr);
        CGColorRef decoded = CGColorCreateCopyByMatchingToColorSpace(
            linear, kCGRenderingIntentRelativeColorimetric, encoded, nullptr);
        CGColorRelease(encoded);
        ASSERT_NE(decoded, nullptr);
        const CGFloat* linear_components = CGColorGetComponents(decoded);
        const double   expected = CeDecode(static_cast<int>(eotf), static_cast<float>(code));
        for (int channel = 0; channel < 3; ++channel) {
          EXPECT_NEAR(linear_components[channel], expected, 1e-3)
              << ColorUtils::EOTFToString(eotf) << " code value " << code << " channel " << channel;
        }
        CGColorRelease(decoded);
      }
    }
    CGColorSpaceRelease(linear);
  }
}

TEST(EditorWindowColorManagementTest, DummyPlatformWinIdReturnsFalseWithoutCrashing) {
  EXPECT_FALSE(ColorManager::ApplyWindowColorSpace(nullptr, ViewerDisplayConfig{}));
  EXPECT_FALSE(
      ColorManager::ApplyWindowColorSpace(reinterpret_cast<void*>(1), ViewerDisplayConfig{}));
}

TEST(EditorWindowColorManagementTest, FindsMetalLayerBelowTheUnifiedContentView) {
  @autoreleasepool {
    NSView* root        = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 320.0, 180.0)];
    NSView* render_view = MakeMetalView();
    [root addSubview:render_view];

    ViewerDisplayConfig config;
    config.encoding_space = ColorUtils::ColorSpace::P3_D65;
    config.encoding_eotf  = ColorUtils::EOTF::GAMMA_2_2;
    ASSERT_TRUE(ColorManager::ApplyWindowColorSpace((__bridge void*)root, config));

    CAMetalLayer* layer = (CAMetalLayer*)render_view.layer;
    EXPECT_FALSE(layer.wantsExtendedDynamicRangeContent);
    EXPECT_EQ(layer.EDRMetadata, nil);
    ASSERT_NE(layer.colorspace, nullptr);
    const auto expected_icc =
        ExportIccBytes(ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::GAMMA_2_2);
    ASSERT_FALSE(expected_icc.empty());
    EXPECT_EQ(CopyIccBytes(layer.colorspace), expected_icc);
  }
}

}  // namespace
}  // namespace alcedo
