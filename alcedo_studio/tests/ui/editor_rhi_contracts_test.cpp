//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/editor_rhi/harness_fixtures.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"

namespace alcedo::editor_rhi {
namespace {

TEST(EditorBackendParseTest, ParsesEqualsAndSeparateForms) {
  {
    char arg0[] = "EditorRhiHarness";
    char arg1[] = "--editor-backend=cuda";
    char* argv[] = {arg0, arg1};
    const auto result = ParseEditorBackendArgs(2, argv);
    ASSERT_TRUE(result.present);
    ASSERT_TRUE(result.backend.has_value());
    EXPECT_EQ(*result.backend, EditorBackend::Cuda);
    EXPECT_TRUE(result.error.empty());
  }
  {
    char arg0[] = "EditorRhiHarness";
    char arg1[] = "--editor-backend";
    char arg2[] = "opencl";
    char* argv[] = {arg0, arg1, arg2};
    const auto result = ParseEditorBackendArgs(3, argv);
    ASSERT_TRUE(result.backend.has_value());
    EXPECT_EQ(*result.backend, EditorBackend::OpenCl);
  }
  {
    char arg0[] = "EditorRhiHarness";
    char arg1[] = "--editor-backend=metal";
    char* argv[] = {arg0, arg1};
    const auto result = ParseEditorBackendArgs(2, argv);
    ASSERT_TRUE(result.backend.has_value());
    EXPECT_EQ(*result.backend, EditorBackend::Metal);
  }
}

TEST(EditorBackendParseTest, RejectsUnknownBackendToken) {
  char arg0[] = "EditorRhiHarness";
  char arg1[] = "--editor-backend=vulkan";
  char* argv[] = {arg0, arg1};
  const auto result = ParseEditorBackendArgs(2, argv);
  EXPECT_TRUE(result.present);
  EXPECT_FALSE(result.backend.has_value());
  EXPECT_FALSE(result.error.empty());
}

TEST(EditorBackendParseTest, GraphicsApiNamesMatchStartupContract) {
  EXPECT_STREQ(QtGraphicsApiName(EditorBackend::Cuda), "Direct3D11");
  EXPECT_STREQ(QtGraphicsApiName(EditorBackend::OpenCl), "OpenGL");
  EXPECT_STREQ(QtGraphicsApiName(EditorBackend::Metal), "Metal");
}

TEST(HarnessFixturesTest, GradientIsDeterministicAndNormalized) {
  const auto a = MakeFp32Gradient(4, 3);
  const auto b = MakeFp32Gradient(4, 3);
  ASSERT_EQ(a.pixels.size(), 12u);
  EXPECT_EQ(a.pixels.size(), b.pixels.size());
  EXPECT_EQ(std::memcmp(a.data(), b.data(), a.byte_size()), 0);
  EXPECT_FLOAT_EQ(a.pixels.front().r, 0.0f);
  EXPECT_FLOAT_EQ(a.pixels.front().g, 0.0f);
  EXPECT_FLOAT_EQ(a.pixels.back().a, 1.0f);
  EXPECT_NEAR(a.pixels.back().r, 1.0f, 1e-6f);
  EXPECT_NEAR(a.pixels.back().g, 1.0f, 1e-6f);
}

TEST(HarnessFixturesTest, CheckerboardAndOddSizedAndRoi) {
  const auto checker = MakeCheckerboard(16, 16, 8);
  EXPECT_EQ(checker.width, 16);
  EXPECT_FLOAT_EQ(checker.pixels.front().r, 0.9f);

  const auto odd = MakeOddSized(62, 46);
  EXPECT_EQ(odd.width % 2, 1);
  EXPECT_EQ(odd.height % 2, 1);

  const auto roi = MakeRoiPatch(32, 32, 8, 8, 4, 4, {1.0f, 0.0f, 0.0f, 1.0f});
  const auto& p = roi.pixels[static_cast<std::size_t>(8) * 32 + 8];
  EXPECT_FLOAT_EQ(p.r, 1.0f);
  EXPECT_FLOAT_EQ(p.g, 0.0f);
}

TEST(HarnessFixturesTest, MaxAbsPixelErrorWithinTolerance) {
  const auto expected = MakeFixture(HarnessFixtureKind::Fp32Gradient);
  std::vector<float> actual(expected.byte_size() / sizeof(float));
  std::memcpy(actual.data(), expected.data(), expected.byte_size());
  actual[0] += kHarnessPixelAbsTolerance * 0.5f;
  const float err =
      MaxAbsPixelError(expected, actual.data(), expected.width, expected.height,
                       expected.row_bytes());
  EXPECT_GE(err, 0.0f);
  EXPECT_LE(err, kHarnessPixelAbsTolerance);
}

TEST(FramePresentationLeaseTest, MetalLeaseContractIsDefinedWithoutImplementationClaim) {
  MetalSharedTextureLeasePayload metal{};
  metal.mtl_texture      = 0x1;
  metal.width            = 64;
  metal.height           = 48;
  metal.mtl_pixel_format = 0;

  WritableTargetLease lease;
  lease.backend       = EditorBackend::Metal;
  lease.handle_kind   = LeaseHandleKindForBackend(EditorBackend::Metal);
  lease.dimensions    = {metal.width, metal.height};
  lease.generation    = {1, 1, 1};
  lease.native_handle = metal.mtl_texture;
  lease.lifetime_token =
      std::shared_ptr<const void>(reinterpret_cast<const void*>(metal.mtl_texture),
                                  [](const void*) {});
  EXPECT_EQ(lease.handle_kind, LeaseNativeHandleKind::MetalTexture);
  EXPECT_TRUE(lease.valid());
  const std::string desc = DescribeLease(lease);
  EXPECT_NE(desc.find("metal"), std::string::npos);
}

TEST(NativeResourceCountersTest, LiveTotalTracksCreateDestroy) {
  auto& counters = NativeResourceCounters::Instance();
  counters.ResetForTest();
  EXPECT_EQ(counters.LiveTotal(), 0);
  counters.OnCreateSharedTexture();
  counters.OnCreateImportedQRhiTexture();
  EXPECT_EQ(counters.LiveTotal(), 2);
  counters.OnDestroySharedTexture();
  counters.OnDestroyImportedQRhiTexture();
  EXPECT_EQ(counters.LiveTotal(), 0);
}

TEST(HarnessFixturesTest, SmallRealRawFixturePathIsStable) {
  const auto path = SmallRealRawFixtureRelativePath();
  EXPECT_FALSE(path.empty());
  EXPECT_NE(path.find(".ARW"), std::string::npos);
}

}  // namespace
}  // namespace alcedo::editor_rhi
