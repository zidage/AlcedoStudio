//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Volume identity of a mounted folder (mounted_folder_plan.md, MF2).

#include "utils/file/volume_identity.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../support/test_temp_directory.hpp"
#include "utils/file/mount_directory_listing.hpp"

namespace alcedo {
namespace {

TEST(VolumeIdentityTest, TwoDirectoriesOfOneVolumeHaveOneIdentity) {
  test::TestTempDirectory dir;
  std::filesystem::create_directories(dir / "a" / "deep");
  std::filesystem::create_directories(dir / "b");

  const auto first  = ReadVolumeIdentity(dir / "a" / "deep");
  const auto second = ReadVolumeIdentity(dir / "b");
  EXPECT_EQ(first, second);
#if defined(_WIN32)
  // The temporary directory is on a local NTFS volume.
  EXPECT_TRUE(first.starts_with("\\\\?\\Volume{")) << first;
  EXPECT_TRUE(first.ends_with("}\\")) << first;
#elif defined(__APPLE__)
  // The temporary directory is on the APFS data volume: a 36-character UUID.
  EXPECT_EQ(first.size(), 36u) << first;
#endif
}

TEST(VolumeIdentityTest, MountPointListHoldsTheVolumeOfADirectory) {
  test::TestTempDirectory dir;
  const auto              identity = ReadVolumeIdentity(dir.Path());
  if (identity.empty()) {
    GTEST_SKIP() << "the volume of the temporary directory has no identity";
  }

  // The volume of the temporary directory is listed with a mount point that holds the directory
  // (Mounted Folder Plan, MF8: the volume search of a mount that is offline).
  const auto canonical = std::filesystem::canonical(dir.Path());
  bool       found     = false;
  for (const auto& volume : ListVolumeMountPoints()) {
    if (volume.identity == identity && IsPathAtOrBelow(canonical, volume.mount_point)) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << identity;
}

TEST(VolumeIdentityTest, MountTableHoldsAMountPointOfADirectory) {
  // The directory watcher of the mounts compares two reads of the mount table for the volume
  // events (#1020).
  test::TestTempDirectory dir;
  const auto              canonical = std::filesystem::canonical(dir.Path());
  bool                    found     = false;
  for (const auto& mount_point : ListMountPointPaths()) {
    if (IsPathAtOrBelow(canonical, mount_point)) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << canonical.string();
}

TEST(VolumeIdentityTest, MissingDirectoryHasEmptyIdentity) {
  test::TestTempDirectory dir;
  EXPECT_TRUE(ReadVolumeIdentity(dir / "absent").empty());
  EXPECT_TRUE(ReadVolumeIdentity(std::filesystem::path{}).empty());
}

}  // namespace
}  // namespace alcedo
