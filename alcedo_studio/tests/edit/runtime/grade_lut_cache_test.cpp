//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/runtime/grade_lut.hpp"

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

auto PackedChannel(const PackedGradeLut& packed, std::size_t voxel, std::size_t channel) -> float {
  const auto* floats = reinterpret_cast<const float*>(packed.rgba.data());
  return floats[voxel * 4 + channel];
}

TEST(GradeLutCacheTest, EmptyLmtPathReturnsNull) {
  const auto grade = MakeGradeWithLut({});
  EXPECT_EQ(TryPackGradeLut(*grade), nullptr);
}

TEST(GradeLutCacheTest, UnchangedCubeFileSharesPackedInstance) {
  const auto path =
      std::filesystem::absolute("build/tmp/grade_lut_cache/unchanged/red.cube");
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  const auto grade = MakeGradeWithLut(path);

  const auto first  = TryPackGradeLut(*grade);
  const auto second = TryPackGradeLut(*grade);

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
  const auto red = TryPackGradeLut(*grade);
  ASSERT_NE(red, nullptr);

  // A different edge changes the file size, so the stamp check must re-parse
  // instead of serving the previous packed cube.
  WriteConstantCube(path, 3, 0.0f, 0.0f, 1.0f);
  const auto blue = TryPackGradeLut(*grade);
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
  const auto first = TryPackGradeLut(*grade);
  ASSERT_NE(first, nullptr);

  // Identical bytes with a newer write time still invalidates the entry.
  WriteConstantCube(path, 2, 1.0f, 0.0f, 0.0f);
  std::error_code ec;
  const auto      stamp = std::filesystem::last_write_time(path, ec);
  ASSERT_FALSE(ec);
  std::filesystem::last_write_time(path, stamp + std::chrono::hours(1), ec);
  ASSERT_FALSE(ec);

  const auto second = TryPackGradeLut(*grade);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second.get(), first.get());
  EXPECT_EQ(second->key, first->key);
}

TEST(GradeLutCacheTest, UnreadableCubeFileThrowsAndIsNotCached) {
  const auto path =
      std::filesystem::absolute("build/tmp/grade_lut_cache/unreadable/missing.cube");
  std::error_code ec;
  std::filesystem::remove(path, ec);
  const auto grade = MakeGradeWithLut(path);
  EXPECT_THROW((void)TryPackGradeLut(*grade), std::runtime_error);

  WriteConstantCube(path, 2, 0.0f, 1.0f, 0.0f);
  const auto packed = TryPackGradeLut(*grade);
  ASSERT_NE(packed, nullptr);
  EXPECT_EQ(packed->edge, 2U);
  EXPECT_NEAR(PackedChannel(*packed, 0, 1), 1.0f, 1.0e-6f);
}

}  // namespace
}  // namespace alcedo
