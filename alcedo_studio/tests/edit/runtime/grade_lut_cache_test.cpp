//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/lut_bake.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "utils/lut/cube_lut.hpp"

namespace alcedo {
namespace {

auto MakeGradeWithLut(const std::filesystem::path& cube_path)
    -> std::unique_ptr<ColorGradeNodeModel> {
  auto grade = ColorGradeNodeModel::MakeClean(NodeId{"grade.primary"});
  auto* lmt  = dynamic_cast<LmtModel*>(grade->FindAdjustmentByType(type_ids::Lmt()));
  if (lmt == nullptr) {
    throw std::runtime_error("grade_lut_cache_test: clean grade has no LMT adjustment");
  }
  if (!cube_path.empty()) {
    lmt->SetCubePath(cube_path.string());
  }
  return grade;
}

void WriteConstantCube(const std::filesystem::path& path, int edge, float r, float g, float b) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "LUT_3D_SIZE " << edge << '\n';
  for (int i = 0; i < edge * edge * edge; ++i) {
    out << r << ' ' << g << ' ' << b << '\n';
  }
}

auto                 Files() -> const LutResourceResolver& { return *DefaultLutResourceResolver(); }

/// Resolves every reference to one file and reports a digest the test chooses, like the library
/// does for official package files.
class DigestResolver final : public LutResourceResolver {
 public:
  explicit DigestResolver(std::filesystem::path path) : path_(std::move(path)) {}
  void               SetDigest(std::string digest) { digest_ = std::move(digest); }
  [[nodiscard]] auto Resolve(const LutReference&) const -> LutResourceResolution override {
    auto resolution           = ResolveLutFile(path_);
    resolution.content_sha256 = digest_;
    return resolution;
  }
  void ReadResource(
      const LutReference&                                      reference,
      const std::function<void(const LutResourceResolution&)>& visitor) const override {
    visitor(Resolve(reference));
  }

 private:
  std::filesystem::path path_;
  std::string           digest_;
};

auto PackedChannel(const PackedGradeLut& packed, std::size_t voxel, std::size_t channel) -> float {
  const auto* floats = reinterpret_cast<const float*>(packed.rgba.data());
  return floats[voxel * 4 + channel];
}

TEST(GradeLutCacheTest, EmptyLmtPathReturnsNull) {
  const auto grade = MakeGradeWithLut({});
  EXPECT_EQ(TryPackGradeLut(*grade, Files()), nullptr);
}

TEST(GradeLutCacheTest, UnchangedCubeFileSharesPackedInstance) {
  const auto path =
      std::filesystem::absolute("build/tmp/grade_lut_cache/unchanged/red.cube");
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto grade = MakeGradeWithLut(path);

  const auto first  = TryPackGradeLut(*grade, Files());
  const auto second = TryPackGradeLut(*grade, Files());

  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(first->key, second->key);
  EXPECT_EQ(first->edge, 2U);
  EXPECT_NEAR(PackedChannel(*first, 0, 0), 1.0f, 1.0e-6f);
}

TEST(GradeLutCacheTest, RewrittenCubeFileReparsesAndRekeys) {
  const auto path =
      std::filesystem::absolute("build/tmp/grade_lut_cache/rewritten/swapped.cube");
  const auto grade = MakeGradeWithLut(path);

  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto red = TryPackGradeLut(*grade, Files());
  ASSERT_NE(red, nullptr);

  // A different edge changes the file size, so the stamp check must re-parse
  // instead of serving the previous packed cube.
  WriteConstantCube(path, 3, 0.0f, 0.0f, 1.0f);
  const auto blue = TryPackGradeLut(*grade, Files());
  ASSERT_NE(blue, nullptr);
  EXPECT_NE(blue.get(), red.get());
  EXPECT_NE(blue->key, red->key);
  EXPECT_EQ(blue->edge, 3U);
  EXPECT_NEAR(PackedChannel(*blue, 0, 0), 0.0f, 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*blue, 0, 2), 1.0f, 1.0e-6f);
}

TEST(GradeLutCacheTest, WriteTimeBumpReparsesSameSizeCube) {
  const auto path =
      std::filesystem::absolute("build/tmp/grade_lut_cache/write_time/same_size.cube");
  const auto grade = MakeGradeWithLut(path);

  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto first = TryPackGradeLut(*grade, Files());
  ASSERT_NE(first, nullptr);

  // Identical bytes with a newer write time still invalidates the entry.
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  std::error_code ec;
  const auto      stamp = std::filesystem::last_write_time(path, ec);
  ASSERT_FALSE(ec);
  std::filesystem::last_write_time(path, stamp + std::chrono::hours(1), ec);
  ASSERT_FALSE(ec);

  const auto second = TryPackGradeLut(*grade, Files());
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second.get(), first.get());
  EXPECT_EQ(second->key, first->key);
}

TEST(GradeLutCacheTest, MissingCubeFileIsSkippedAndReturnedFileIsPacked) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/missing/returns.cube");
  std::error_code ec;
  std::filesystem::remove(path, ec);
  const auto grade = MakeGradeWithLut(path);
  // A missing file skips only the LUT operation; it is not an error (plan 6.1).
  EXPECT_EQ(TryPackGradeLut(*grade, Files()), nullptr);

  WriteConstantCube(path, 2, 0.0f, 1.0f, 0.0f);
  const auto packed = TryPackGradeLut(*grade, Files());
  ASSERT_NE(packed, nullptr);
  EXPECT_EQ(packed->edge, 2U);
  EXPECT_NEAR(PackedChannel(*packed, 0, 1), 1.0f, 1.0e-6f);
}

TEST(GradeLutCacheTest, InvalidCubeRemainsAnError) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/invalid/broken.cube");
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream out(path);
    out << "LUT_3D_SIZE 2\n0.5 0.5\nnot a row\n";
  }
  const auto grade = MakeGradeWithLut(path);
  EXPECT_THROW((void)TryPackGradeLut(*grade, Files()), std::runtime_error);
}

TEST(GradeLutCacheTest, ZeroStrengthSkipsCubeLoading) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/zero/red.cube");
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto grade = MakeGradeWithLut(path);
  auto*      lmt   = dynamic_cast<LmtModel*>(grade->FindAdjustmentByType(type_ids::Lmt()));
  ASSERT_NE(lmt, nullptr);
  lmt->SetStrength(0.0f);
  EXPECT_EQ(TryPackGradeLut(*grade, Files()), nullptr);
  EXPECT_EQ(lmt->CubePath(), path.string());
}

TEST(GradeLutCacheTest, ChangedDigestReparsesCubeWithUnchangedStamp) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/digest/official.cube");
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto     grade = MakeGradeWithLut(path);
  DigestResolver resolver(path);
  resolver.SetDigest(std::string(64, 'a'));
  const auto first = TryPackGradeLut(*grade, resolver);
  ASSERT_NE(first, nullptr);

  // Same size and write time, new bytes: only the verified digest identifies the change.
  std::error_code ec;
  const auto      stamp = std::filesystem::last_write_time(path, ec);
  ASSERT_FALSE(ec);
  WriteConstantCube(path, 2, 0.0f, 0.0f, 1.0f);
  std::filesystem::last_write_time(path, stamp, ec);
  ASSERT_FALSE(ec);
  resolver.SetDigest(std::string(64, 'b'));
  const auto second = TryPackGradeLut(*grade, resolver);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second->key, first->key);
  EXPECT_NEAR(PackedChannel(*second, 0, 2), 1.0f, 1.0e-6f);
  EXPECT_NE(GradeLutResourceIdentity(*grade, resolver), 0U);
}

auto LmtOf(ColorGradeNodeModel& grade) -> LmtModel& {
  return *dynamic_cast<LmtModel*>(grade.FindAdjustmentByType(type_ids::Lmt()));
}

/// Identity cube over a domain; red varies fastest.
void WriteDomainIdentityCube(const std::filesystem::path& path, const char* domain_min,
                             const char* domain_max) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "DOMAIN_MIN " << domain_min << "\nDOMAIN_MAX " << domain_max << "\nLUT_3D_SIZE 2\n";
  for (int b = 0; b <= 1; ++b) {
    for (int g = 0; g <= 1; ++g) {
      for (int r = 0; r <= 1; ++r) {
        out << r << ' ' << g << ' ' << b << '\n';
      }
    }
  }
}

TEST(GradeLutCacheTest, DefaultEncodingUnitDomainLutKeepsSourcePackedBytesAndKey) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/default_encoding/a.cube");
  WriteConstantCube(path, 3, 0.25f, 0.5f, 0.75f);
  const auto grade  = MakeGradeWithLut(path);
  const auto bakes  = LmtCompositeBakeCount();
  const auto packed = TryPackGradeLut(*grade, Files());
  ASSERT_NE(packed, nullptr);

  // The bytes and key that the pack produced before encodings existed: PackCubeLutRgba of the
  // parsed cube, keyed by its bytes and edge.
  CubeLut cube;
  ASSERT_TRUE(ParseCubeFile(path, cube));
  const auto  expected_bytes = PackCubeLutRgba(cube);
  ContentHash hash;
  hash.MixBytes(expected_bytes);
  hash.MixU32(3);
  EXPECT_EQ(packed->rgba, expected_bytes);
  EXPECT_EQ(packed->edge, 3U);
  EXPECT_EQ(packed->key, hash.Key());
  EXPECT_EQ(LmtCompositeBakeCount(), bakes);
}

TEST(GradeLutCacheTest, EncodingChangeRepacksCompositeAndDefaultRestoresSourceTable) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/encoding/a.cube");
  WriteConstantCube(path, 2, 0.4f, 0.5f, 0.6f);
  const auto grade  = MakeGradeWithLut(path);
  const auto source = TryPackGradeLut(*grade, Files());
  ASSERT_NE(source, nullptr);

  LmtOf(*grade).SetEncodings("sony_slog3_sgamut3cine", "rec709_bt1886");
  const auto bakes     = LmtCompositeBakeCount();
  const auto composite = TryPackGradeLut(*grade, Files());
  ASSERT_NE(composite, nullptr);
  EXPECT_EQ(composite->edge, kLmtCompositeEdge);
  EXPECT_NE(composite->key, source->key);
  EXPECT_EQ(LmtCompositeBakeCount(), bakes + 1);
  // A constant LUT bakes to the constant brought back from Rec.709 BT.1886.
  const LmtEncodingConversion conversion(*color::FindColorEncoding("sony_slog3_sgamut3cine"),
                                         *color::FindColorEncoding("rec709_bt1886"));
  const auto                  expected = conversion.LutOutputToAcescc({0.4f, 0.5f, 0.6f});
  EXPECT_NEAR(PackedChannel(*composite, 0, 0), expected[0], 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*composite, 0, 2), expected[2], 1.0e-6f);

  // Unchanged content and encodings reuse the composite table without a bake.
  EXPECT_EQ(TryPackGradeLut(*grade, Files()).get(), composite.get());
  EXPECT_EQ(LmtCompositeBakeCount(), bakes + 1);

  LmtOf(*grade).SetEncodings("acescc", "acescc");
  const auto restored = TryPackGradeLut(*grade, Files());
  EXPECT_EQ(restored.get(), source.get());
}

TEST(GradeLutCacheTest, NonUnitDomainLutIsBakedWithItsDomainApplied) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/domain/wide.cube");
  WriteDomainIdentityCube(path, "-0.5 0 0.25", "1.5 2 0.75");
  const auto grade  = MakeGradeWithLut(path);
  const auto packed = TryPackGradeLut(*grade, Files());
  ASSERT_NE(packed, nullptr);
  // Default encodings, but the domain must be applied, so the table is a composite table.
  ASSERT_EQ(packed->edge, kLmtCompositeEdge);
  EXPECT_TRUE(HasUnitDomain(*packed));
  const auto node = [](std::size_t r, std::size_t g, std::size_t b) {
    return (b * kLmtCompositeEdge + g) * kLmtCompositeEdge + r;
  };
  // Code c samples the identity at u = (c - min) / (max - min), clamped to [0, 1].
  EXPECT_NEAR(PackedChannel(*packed, node(0, 0, 0), 0), 0.25f, 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*packed, node(64, 0, 0), 0), 0.75f, 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*packed, node(0, 32, 0), 1), 0.25f, 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*packed, node(0, 0, 0), 2), 0.0f, 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*packed, node(0, 0, 32), 2), 0.5f, 1.0e-6f);
  EXPECT_NEAR(PackedChannel(*packed, node(0, 0, 64), 2), 1.0f, 1.0e-6f);
}

TEST(GradeLutCacheTest, ShaperCubeIsRejectedWithUnsupportedError) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/shaper/shaper.cube");
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream out(path);
    out << "LUT_1D_SIZE 2\nLUT_3D_SIZE 2\n0 0 0\n1 1 1\n";
    for (int i = 0; i < 8; ++i) out << "0.5 0.5 0.5\n";
  }
  const auto grade = MakeGradeWithLut(path);
  try {
    (void)TryPackGradeLut(*grade, Files());
    ADD_FAILURE() << "a shaper LUT must not render without its shaper";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("1D shaper LUTs are not supported"), std::string::npos)
        << error.what();
  }
}

TEST(GradeLutCacheTest, ResourceIdentityFollowsAvailabilityAndClearedReference) {
  const auto path = std::filesystem::absolute("build/tmp/grade_lut_cache/identity/look.cube");
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto grade     = MakeGradeWithLut(path);
  const auto available = GradeLutResourceIdentity(*grade, Files());
  EXPECT_NE(available, 0U);
  const auto parked = path.string() + ".parked";
  std::filesystem::rename(path, parked);
  const auto missing = GradeLutResourceIdentity(*grade, Files());
  EXPECT_NE(missing, 0U);
  EXPECT_NE(missing, available);
  std::filesystem::rename(parked, path);
  EXPECT_EQ(GradeLutResourceIdentity(*grade, Files()), available);
  dynamic_cast<LmtModel*>(grade->FindAdjustmentByType(type_ids::Lmt()))
      ->SetReference(std::monostate{});
  EXPECT_EQ(GradeLutResourceIdentity(*grade, Files()), 0U);
}

}  // namespace
}  // namespace alcedo
