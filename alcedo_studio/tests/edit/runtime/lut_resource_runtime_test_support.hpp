//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Backend-independent test bodies for LUT references, missing LUT resources, and LUT strength
// (lut_library_and_package_management_plan.md, phase L4). Each GPU backend supplies a Harness:
//
//   void UseLutResources(std::shared_ptr<const LutResourceResolver>);
//   auto Render(PipelineDocument&) -> RenderedGrades;   // full plan execute on one device
//
// Expected pixels are computed independently: the fixture cube encodes an affine map, which
// trilinear sampling at texel centers reproduces exactly for inputs in [0, 1].

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/lut_bake.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "multi_grade_runtime_test_support.hpp"

namespace alcedo::lut_resource_test {

using multi_grade_test::AcesccRgb;

/// Absolute channel tolerance of the independent pixel arithmetic (plan L4).
inline constexpr float kPixelTolerance          = 1.0e-5f;

/// Tolerance of one backend against the host sampling of a composite table: 2^-17, so any two
/// backends agree within 2^-16 (lut_color_encoding_plan.md, phase L3).
inline constexpr float kCompositeTableTolerance = 1.0f / 131072.0f;

/// Pixels of one execute: the Develop output and the output of the last Color Grade.
/// Grades encode on every execute; the display result after them is the cached result that a
/// LUT change must invalidate, so its execute/skip counts show whether the cache was reused.
struct RenderedGrades {
  std::vector<AcesccRgb> develop;
  std::vector<AcesccRgb> output;
  std::uint32_t          grade_execute   = 0;
  std::uint32_t          display_execute = 0;
  std::uint32_t          display_skip    = 0;
};

/**
 * @brief Test resolver whose official references map to files the test chooses and changes.
 *
 * File references resolve to their exact path like the production resolvers. The mapping and
 * the reported digest can change between renders, as a package installation or a returned
 * file changes them in the application.
 */
class SwitchableLutResolver final : public LutResourceResolver {
 public:
  void Map(const LutReference& reference, std::filesystem::path path, std::string sha256 = {}) {
    std::scoped_lock lock(mutex_);
    mapping_[DescribeLutReference(reference)] = {std::move(path), std::move(sha256)};
  }

  [[nodiscard]] auto Resolve(const LutReference& reference) const
      -> LutResourceResolution override {
    if (IsEmptyLutReference(reference)) {
      return {};
    }
    if (const auto* file = std::get_if<FileLutReference>(&reference)) {
      return ResolveLutFile(file->path);
    }
    std::scoped_lock lock(mutex_);
    const auto       it = mapping_.find(DescribeLutReference(reference));
    if (it == mapping_.end()) {
      LutResourceResolution missing;
      missing.status = LutResourceStatus::kMissing;
      return missing;
    }
    auto resolution           = ResolveLutFile(it->second.first);
    resolution.content_sha256 = it->second.second;
    return resolution;
  }

  void ReadResource(
      const LutReference&                                      reference,
      const std::function<void(const LutResourceResolution&)>& visitor) const override {
    visitor(Resolve(reference));
  }

 private:
  mutable std::mutex                                                   mutex_;
  std::map<std::string, std::pair<std::filesystem::path, std::string>> mapping_;
};

/// L(r, g, b) of the affine fixture cube, for inputs in [0, 1].
inline auto ApplyAffineLut(const AcesccRgb& c) -> AcesccRgb {
  const float r = std::clamp(c[0], 0.0f, 1.0f);
  const float g = std::clamp(c[1], 0.0f, 1.0f);
  const float b = std::clamp(c[2], 0.0f, 1.0f);
  return {0.2f + 0.6f * b, 0.9f - 0.5f * r, 0.3f + 0.4f * g};
}

/// c + a * (L(c) - c): the LUT strength blend at the LMT operation.
inline auto BlendAffineLut(const AcesccRgb& c, float strength) -> AcesccRgb {
  const AcesccRgb lut = ApplyAffineLut(c);
  return {c[0] + strength * (lut[0] - c[0]), c[1] + strength * (lut[1] - c[1]),
          c[2] + strength * (lut[2] - c[2])};
}

/// 2^3 cube of the affine map above; red varies fastest as in the CUBE format.
inline void WriteAffineCube(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::trunc);
  out << "LUT_3D_SIZE 2\n";
  for (int b = 0; b <= 1; ++b) {
    for (int g = 0; g <= 1; ++g) {
      for (int r = 0; r <= 1; ++r) {
        const auto value =
            ApplyAffineLut({static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)});
        out << value[0] << ' ' << value[1] << ' ' << value[2] << '\n';
      }
    }
  }
}

/// Constant cube with fixed-width text, so two colors give files of equal size.
inline void WriteConstantCube(const std::filesystem::path& path, const AcesccRgb& color) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::trunc);
  out << "LUT_3D_SIZE 2\n";
  char line[64];
  std::snprintf(line, sizeof(line), "%.6f %.6f %.6f\n", color[0], color[1], color[2]);
  for (int i = 0; i < 8; ++i) {
    out << line;
  }
}

inline auto FixtureDirectory(const std::string& backend, const std::string& test)
    -> std::filesystem::path {
  const auto directory =
      std::filesystem::absolute("build/tmp/lut_resource_runtime") / backend / test;
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
  std::filesystem::create_directories(directory);
  return directory;
}

inline auto Lmt(PipelineDocument& document, const char* grade = "grade.primary") -> LmtModel& {
  return multi_grade_test::GradeAdjustment<LmtModel>(document, NodeId{grade}, type_ids::Lmt());
}

inline auto Exposure(PipelineDocument& document, const char* grade = "grade.primary")
    -> ExposureModel& {
  return multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{grade},
                                                          type_ids::Exposure());
}

/// Every output pixel equals @p expected(develop pixel) within the L4 tolerance.
template <class Expected>
auto PixelsMatch(const RenderedGrades& rendered, Expected expected,
                 float tolerance = kPixelTolerance) -> ::testing::AssertionResult {
  if (rendered.output.empty() || rendered.output.size() != rendered.develop.size()) {
    return ::testing::AssertionFailure() << "missing pixels";
  }
  for (std::size_t i = 0; i < rendered.output.size(); ++i) {
    const AcesccRgb want = expected(rendered.develop[i]);
    for (std::size_t c = 0; c < 3; ++c) {
      if (!(std::fabs(rendered.output[i][c] - want[c]) <= tolerance)) {
        return ::testing::AssertionFailure() << "pixel " << i << " channel " << c << ": actual "
                                             << rendered.output[i][c] << ", expected " << want[c];
      }
    }
  }
  return ::testing::AssertionSuccess();
}

inline auto SamePixels(const std::vector<AcesccRgb>& a, const std::vector<AcesccRgb>& b)
    -> ::testing::AssertionResult {
  if (a.empty() || a.size() != b.size()) {
    return ::testing::AssertionFailure()
           << "pixel counts differ: " << a.size() << " vs " << b.size();
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      if (!(std::fabs(a[i][c] - b[i][c]) <= kPixelTolerance)) {
        return ::testing::AssertionFailure()
               << "pixel " << i << " channel " << c << ": " << a[i][c] << " vs " << b[i][c];
      }
    }
  }
  return ::testing::AssertionSuccess();
}

inline auto ExposedBy(float exposure_ev) {
  return [exposure_ev](const AcesccRgb& c) {
    return multi_grade_test::ApplyExposureAcescc(c, exposure_ev);
  };
}

// ── Test bodies ─────────────────────────────────────────────────────────────

/// 0, 0.5, and 1 give the input, the half blend, and the complete LUT result.
template <class Harness>
void CheckStrengthZeroHalfAndOne(Harness& harness, const std::string& backend) {
  const auto cube = FixtureDirectory(backend, "strength") / "affine.cube";
  WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetCubePath(cube.string());
  for (const float strength : {0.0f, 0.5f, 1.0f}) {
    Lmt(document).SetStrength(strength);
    const auto rendered = harness.Render(document);
    EXPECT_TRUE(PixelsMatch(rendered,
                            [strength](const AcesccRgb& c) { return BlendAffineLut(c, strength); }))
        << "strength " << strength;
  }
}

/// Strength blends only the LMT result; exposure before it and the Grade mix after it keep
/// their own values.
template <class Harness>
void CheckStrengthDoesNotScaleOtherAdjustments(Harness& harness, const std::string& backend) {
  const auto cube = FixtureDirectory(backend, "independent") / "affine.cube";
  WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Exposure(document).SetValue(0.75f);
  Lmt(document).SetCubePath(cube.string());
  Lmt(document).SetStrength(0.5f);
  document.PrimaryGrade()->SetMix(0.6f);
  const auto rendered = harness.Render(document);
  EXPECT_TRUE(PixelsMatch(rendered, [](const AcesccRgb& c) {
    const AcesccRgb adjusted =
        BlendAffineLut(multi_grade_test::ApplyExposureAcescc(c, 0.75f), 0.5f);
    return multi_grade_test::MixToward(c, adjusted, 0.6f);
  }));
}

/// A missing LUT skips only its operation: both Grades render as if no LUT were referenced.
template <class Harness>
void CheckMissingLutKeepsOtherGradeAdjustments(Harness& with_missing, Harness& without_lut,
                                               const std::string& backend) {
  const auto missing = FixtureDirectory(backend, "missing") / "absent.cube";
  const auto build   = [](bool reference_lut, const std::filesystem::path& path) {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
    Exposure(document).SetValue(1.0f);
    multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                       type_ids::Contrast())
        .SetValue(40.0f);
    if (reference_lut) {
      Lmt(document).SetCubePath(path.string());
      Lmt(document).SetStrength(0.7f);
    }
    return document;
  };
  auto       missing_document    = build(true, missing);
  auto       plain_document      = build(false, missing);
  const auto json_before         = missing_document.ToJson().dump();
  const auto revision_before     = Lmt(missing_document).Revision();

  const auto with_missing_pixels = with_missing.Render(missing_document);
  const auto plain_pixels        = without_lut.Render(plain_document);
  EXPECT_EQ(with_missing_pixels.grade_execute, 2U);
  EXPECT_TRUE(SamePixels(with_missing_pixels.output, plain_pixels.output));
  // Rendering a missing association changes no document state (no history edit possible).
  EXPECT_EQ(missing_document.ToJson().dump(), json_before);
  EXPECT_EQ(Lmt(missing_document).Revision(), revision_before);
  EXPECT_FLOAT_EQ(Lmt(missing_document).Strength(), 0.7f);
  EXPECT_EQ(Lmt(missing_document).CubePath(), missing.string());
}

/// Corrupted CUBE content is an explicit render failure, not an identity substitute.
template <class Harness>
void CheckInvalidCubeRemainsAnError(Harness& harness, const std::string& backend) {
  const auto cube = FixtureDirectory(backend, "invalid") / "broken.cube";
  {
    std::ofstream out(cube);
    out << "LUT_3D_SIZE 2\n0.1 0.2\nnot numbers\n";
  }
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetCubePath(cube.string());
  EXPECT_ANY_THROW((void)harness.Render(document));
}

/// The same official ID renders the newly installed bytes although the Model is unchanged and
/// the previous frame is still cached: a new content directory, then bytes replaced in place
/// with an identical size and write time but a new verified digest.
template <class Harness>
void CheckOfficialUpdateChangesPixelsWithWarmResultCache(Harness&           harness,
                                                         const std::string& backend) {
  const auto directory = FixtureDirectory(backend, "official_update");
  const auto first     = directory / "content-a" / "look.cube";
  const auto second    = directory / "content-b" / "look.cube";
  WriteConstantCube(first, {0.1f, 0.2f, 0.3f});
  WriteConstantCube(second, {0.7f, 0.6f, 0.5f});
  const LutReference official = OfficialLutReference{"spectral_film_lut", "kodak-5207"};
  auto               resolver = std::make_shared<SwitchableLutResolver>();
  resolver->Map(official, first, std::string(64, 'a'));
  harness.UseLutResources(resolver);

  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetReference(official, "Vision3 250D");
  const auto revision = Lmt(document).Revision();
  const auto constant = [](AcesccRgb color) { return [color](const AcesccRgb&) { return color; }; };

  EXPECT_TRUE(PixelsMatch(harness.Render(document), constant({0.1f, 0.2f, 0.3f})));
  const auto warm = harness.Render(document);
  EXPECT_EQ(warm.display_skip, 1U) << "an unchanged resource must reuse the cached display";
  EXPECT_EQ(warm.display_execute, 0U);

  resolver->Map(official, second, std::string(64, 'b'));
  const auto moved = harness.Render(document);
  EXPECT_EQ(moved.display_execute, 1U) << "new content must invalidate the cached display";
  EXPECT_TRUE(PixelsMatch(moved, constant({0.7f, 0.6f, 0.5f})));

  // Replace the bytes in place: same path, size, and write time; only the digest differs.
  const auto stamp = std::filesystem::last_write_time(second);
  WriteConstantCube(second, {0.4f, 0.3f, 0.2f});
  std::filesystem::last_write_time(second, stamp);
  resolver->Map(official, second, std::string(64, 'c'));
  const auto replaced = harness.Render(document);
  EXPECT_EQ(replaced.display_execute, 1U);
  EXPECT_TRUE(PixelsMatch(replaced, constant({0.4f, 0.3f, 0.2f})));
  EXPECT_EQ(Lmt(document).Revision(), revision);
}

/// Removing and restoring the file skips and then restores the effect at the configured
/// strength, with no Model change and no enable action.
template <class Harness>
void CheckReturnedLutRestoresConfiguredStrength(Harness& harness, const std::string& backend) {
  const auto directory = FixtureDirectory(backend, "returned");
  const auto cube      = directory / "affine.cube";
  const auto parked    = directory / "affine.cube.parked";
  WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetCubePath(cube.string());
  Lmt(document).SetStrength(0.5f);
  const auto json     = document.ToJson().dump();
  const auto revision = Lmt(document).Revision();
  const auto blended  = [](const AcesccRgb& c) { return BlendAffineLut(c, 0.5f); };
  const auto input    = [](const AcesccRgb& c) { return c; };

  EXPECT_TRUE(PixelsMatch(harness.Render(document), blended));
  std::filesystem::rename(cube, parked);
  const auto missing = harness.Render(document);
  EXPECT_EQ(missing.display_execute, 1U) << "a missing file must invalidate the cached display";
  EXPECT_TRUE(PixelsMatch(missing, input));
  std::filesystem::rename(parked, cube);
  const auto restored = harness.Render(document);
  EXPECT_EQ(restored.display_execute, 1U);
  EXPECT_TRUE(PixelsMatch(restored, blended));
  EXPECT_EQ(document.ToJson().dump(), json);
  EXPECT_EQ(Lmt(document).Revision(), revision);
  EXPECT_FLOAT_EQ(Lmt(document).Strength(), 0.5f);
}

/// A user who cleared the association while the file was missing keeps it cleared.
template <class Harness>
void CheckReturnedFileDoesNotRestoreClearedLut(Harness& harness, const std::string& backend) {
  const auto directory = FixtureDirectory(backend, "cleared");
  const auto cube      = directory / "affine.cube";
  const auto parked    = directory / "affine.cube.parked";
  WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetCubePath(cube.string());
  EXPECT_TRUE(PixelsMatch(harness.Render(document),
                          [](const AcesccRgb& c) { return BlendAffineLut(c, 1.0f); }));
  std::filesystem::rename(cube, parked);
  (void)harness.Render(document);
  Lmt(document).SetReference(std::monostate{});
  std::filesystem::rename(parked, cube);
  EXPECT_TRUE(PixelsMatch(harness.Render(document), [](const AcesccRgb& c) { return c; }));
  EXPECT_TRUE(IsEmptyLutReference(Lmt(document).Reference()));
}

/// Availability returning does not change a zero strength; the LUT stays visually inactive.
template <class Harness>
void CheckReturnedLutAtZeroStrengthRemainsInactive(Harness& harness, const std::string& backend) {
  const auto directory = FixtureDirectory(backend, "zero_strength");
  const auto cube      = directory / "affine.cube";
  const auto parked    = directory / "affine.cube.parked";
  WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetCubePath(cube.string());
  Lmt(document).SetStrength(0.0f);
  const auto input = [](const AcesccRgb& c) { return c; };
  EXPECT_TRUE(PixelsMatch(harness.Render(document), input));
  std::filesystem::rename(cube, parked);
  EXPECT_TRUE(PixelsMatch(harness.Render(document), input));
  std::filesystem::rename(parked, cube);
  EXPECT_TRUE(PixelsMatch(harness.Render(document), input));
  EXPECT_FLOAT_EQ(Lmt(document).Strength(), 0.0f);
  EXPECT_EQ(Lmt(document).CubePath(), cube.string());
}

/// A non-default encoding pair renders the host-baked composite table: every pixel equals the
/// host trilinear sampling of that table within 2^-17, and the encoding change invalidates the
/// cached display like a LUT change. All backends sample the same table, so they agree within
/// 2^-16 (lut_color_encoding_plan.md, phase L3).
template <class Harness>
void CheckNonDefaultEncodingSamplesHostCompositeTable(Harness&           harness,
                                                      const std::string& backend) {
  const auto cube = FixtureDirectory(backend, "encoding") / "affine.cube";
  WriteAffineCube(cube);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  Lmt(document).SetCubePath(cube.string());
  EXPECT_TRUE(PixelsMatch(harness.Render(document),
                          [](const AcesccRgb& c) { return BlendAffineLut(c, 1.0f); }));
  (void)harness.Render(document);

  Lmt(document).SetEncodings("sony_slog3_sgamut3cine", "rec709_bt1886");
  const auto composite = TryPackGradeLut(*document.PrimaryGrade(), *DefaultLutResourceResolver());
  ASSERT_NE(composite, nullptr);
  ASSERT_EQ(composite->edge, kLmtCompositeEdge);
  const auto rendered = harness.Render(document);
  EXPECT_EQ(rendered.display_execute, 1U) << "an encoding change must invalidate the display";
  EXPECT_TRUE(PixelsMatch(
      rendered,
      [&composite](const AcesccRgb& c) {
        const LutRgb sampled = SampleLutTable(*composite, {c[0], c[1], c[2]});
        return AcesccRgb{sampled[0], sampled[1], sampled[2]};
      },
      kCompositeTableTolerance));
  // The composite table is not the source LUT: the pixels changed.
  EXPECT_FALSE(PixelsMatch(rendered, [](const AcesccRgb& c) { return BlendAffineLut(c, 1.0f); }));
}

}  // namespace alcedo::lut_resource_test
