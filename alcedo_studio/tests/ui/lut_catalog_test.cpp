//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_support/modules/lut_catalog.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace alcedo::ui::lut_catalog {
namespace {

class ScopedTempDir final {
 public:
  ScopedTempDir() {
    path_ = std::filesystem::temp_directory_path() /
            std::filesystem::path(
                "alcedo_lut_catalog_test_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(counter_++));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  auto path() const -> const std::filesystem::path& { return path_; }

 private:
  inline static int     counter_ = 0;
  std::filesystem::path path_{};
};

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  output << content;
}

auto ValidCubeContent() -> std::string {
  return "TITLE \"Valid\"\n"
         "LUT_3D_SIZE 2\n"
         "DOMAIN_MIN 0.0 0.0 0.0\n"
         "DOMAIN_MAX 1.0 1.0 1.0\n"
         "0 0 0\n"
         "1 0 0\n"
         "0 1 0\n"
         "1 1 0\n"
         "0 0 1\n"
         "1 0 1\n"
         "0 1 1\n"
         "1 1 1\n";
}

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

}  // namespace

TEST(LutCatalogTests, EmptyDirectoryStillProvidesNoneEntry) {
  ScopedTempDir    temp_dir;

  const LutCatalog catalog = BuildCatalogForDirectory(temp_dir.path(), std::string{});

  ASSERT_EQ(catalog.entries_.size(), 1u);
  EXPECT_TRUE(catalog.directory_exists_);
  EXPECT_EQ(catalog.entries_.front().kind_, LutCatalogEntryKind::None);
  EXPECT_TRUE(catalog.entries_.front().selectable_);
}

TEST(LutCatalogTests, EntriesAreSortedAndMetadataIsParsed) {
  ScopedTempDir temp_dir;
  WriteTextFile(temp_dir.path() / "b.cube", ValidCubeContent());
  WriteTextFile(temp_dir.path() / "a.cube", ValidCubeContent());

  const LutCatalog catalog = BuildCatalogForDirectory(temp_dir.path(), std::string{});

  ASSERT_EQ(catalog.entries_.size(), 3u);
  EXPECT_EQ(catalog.entries_[1].display_name_, "a.cube");
  EXPECT_EQ(catalog.entries_[2].display_name_, "b.cube");
  EXPECT_TRUE(catalog.entries_[1].valid_);
  EXPECT_EQ(catalog.entries_[1].edge3d_, 2);
  EXPECT_EQ(catalog.entries_[1].size1d_, 0);
  EXPECT_FALSE(catalog.entries_[1].secondary_text_.isEmpty());
}

TEST(LutCatalogTests, InvalidCubeFilesRemainVisibleWithErrorState) {
  ScopedTempDir temp_dir;
  WriteTextFile(temp_dir.path() / "broken.cube", "LUT_3D_SIZE nope\n0 0 0\n");

  const LutCatalog catalog = BuildCatalogForDirectory(temp_dir.path(), std::string{});

  ASSERT_EQ(catalog.entries_.size(), 2u);
  EXPECT_FALSE(catalog.entries_[1].valid_);
  EXPECT_FALSE(catalog.entries_[1].selectable_);
  EXPECT_EQ(catalog.entries_[1].status_text_, "Invalid");
  EXPECT_FALSE(catalog.entries_[1].secondary_text_.isEmpty());
}

TEST(LutCatalogTests, MissingCurrentPathProducesPlaceholderEntry) {
  ScopedTempDir     temp_dir;
  const std::string current_path = (temp_dir.path() / "outside" / "missing.cube").generic_string();

  const LutCatalog  catalog      = BuildCatalogForDirectory(temp_dir.path(), current_path);

  ASSERT_EQ(catalog.entries_.size(), 2u);
  EXPECT_EQ(catalog.entries_[1].kind_, LutCatalogEntryKind::MissingCurrent);
  EXPECT_EQ(catalog.entries_[1].path_, current_path);
  EXPECT_FALSE(catalog.entries_[1].selectable_);
}

TEST(LutCatalogTests, FindEntryIndexFallsBackToFilenameMatch) {
  ScopedTempDir temp_dir;
  WriteTextFile(temp_dir.path() / "5207.cube", ValidCubeContent());

  const LutCatalog catalog = BuildCatalogForDirectory(temp_dir.path(), "D:/custom/LUTs/5207.cube");

  EXPECT_EQ(FindEntryIndexForPath(catalog, "D:/custom/LUTs/5207.cube"), 1);
  EXPECT_EQ(DefaultLutPath(catalog), PathToUtf8(temp_dir.path() / "5207.cube"));
}

TEST(LutCatalogTests, UnicodePathsRoundTripAsUtf8) {
  ScopedTempDir temp_dir;
  const auto    unicode_dir = temp_dir.path() / L"项目";
  std::filesystem::create_directories(unicode_dir);
  const auto lut_path = unicode_dir / L"5207.cube";
  WriteTextFile(lut_path, ValidCubeContent());

  const LutCatalog catalog = BuildCatalogForDirectory(unicode_dir, std::string{});

  ASSERT_EQ(catalog.entries_.size(), 2u);
  EXPECT_EQ(catalog.entries_[1].path_, PathToUtf8(lut_path));
  EXPECT_EQ(catalog.entries_[1].display_name_, QStringLiteral("5207.cube"));
  EXPECT_EQ(FindEntryIndexForPath(catalog, PathToUtf8(lut_path)), 1);
}

}  // namespace alcedo::ui::lut_catalog
