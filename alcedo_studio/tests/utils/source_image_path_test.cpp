// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "utils/file/source_image_path.hpp"

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <filesystem>

namespace alcedo {
TEST(SourceImagePathTest, EquivalentSeparatorsAndDotSegmentsPreserveTheFullPath) {
  const auto root = std::filesystem::current_path();
  EXPECT_EQ(NormalizeSourceImagePath(root / "photos" / "other" / ".." / "one.jpg"),
            NormalizeSourceImagePath(root / "photos" / "one.jpg"));
  EXPECT_NE(NormalizeSourceImagePath(root / "photos" / "one.jpg"),
            NormalizeSourceImagePath(root / "other" / "one.jpg"));
}
TEST(SourceImagePathTest, RelativePathsAreRejected) {
  EXPECT_THROW(NormalizeSourceImagePath("photos/one.jpg"), std::invalid_argument);
}
TEST(SourceImagePathTest, PathCaseFollowsThePlatformRule) {
  const auto root = std::filesystem::current_path();
#ifdef _WIN32
  EXPECT_EQ(NormalizeSourceImagePath(root / "PHOTOS" / "One.JPG"),
            NormalizeSourceImagePath(root / "photos" / "one.jpg"));
  EXPECT_EQ(NormalizeSourceImagePath(L"C:\\Photos\\one.jpg"),
            NormalizeSourceImagePath(L"c:/photos/one.jpg"));
#else
  EXPECT_NE(NormalizeSourceImagePath(root / "PHOTOS" / "One.JPG"),
            NormalizeSourceImagePath(root / "photos" / "one.jpg"));
#endif
}
TEST(SourceImagePathTest, DifferentUserVisibleSymlinkPathsRemainSeparate) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const auto root = std::filesystem::path(directory.path().toStdWString());
  std::filesystem::create_directory(root / "source");
  std::error_code error;
  std::filesystem::create_directory_symlink(root / "source", root / "alias", error);
  if (error) GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
  EXPECT_NE(NormalizeSourceImagePath(root / "source/image.jpg"),
            NormalizeSourceImagePath(root / "alias/image.jpg"));
}
}  // namespace alcedo
