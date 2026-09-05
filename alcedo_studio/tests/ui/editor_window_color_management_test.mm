//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#import <AppKit/AppKit.h>
#include <CoreGraphics/CGColorSpace.h>
#import <QuartzCore/CAMetalLayer.h>

#include <gtest/gtest.h>

#include "ui/edit_viewer/color_manager.hpp"

namespace alcedo {
namespace {

auto MakeMetalView() -> NSView* {
  NSView* view    = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 320.0, 180.0)];
  view.wantsLayer = YES;
  view.layer      = [CAMetalLayer layer];
  return view;
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
    EXPECT_TRUE(CFEqual(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceSRGB));
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
    EXPECT_TRUE(CFEqual(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceDisplayP3));
  }
}

}  // namespace
}  // namespace alcedo
