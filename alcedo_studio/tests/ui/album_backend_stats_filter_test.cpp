//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_seeded_project_fixture.hpp"

namespace alcedo::ui::test {
namespace {

using StatsFilterTests = ApplicationModuleHostTestFixture;

}  // namespace

TEST_F(StatsFilterTests, CameraBucketFilterRestrictsThumbnailGridAndStatsTogether) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);

  // Matching bucket: grid and stats panel both keep the single synthetic file.
  backend.stats()->ToggleStatsFilter("camera", "Synthetic Album Camera");
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);

  // Non-matching bucket: both surfaces restrict to zero.
  backend.stats()->ToggleStatsFilter("camera", "Other Camera");
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 0);

  backend.stats()->ClearStatsFilter();
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);
}

TEST_F(StatsFilterTests, DateBucketFilterRestrictsThumbnailGrid) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  // The seeded synthetic image carries 2026-05-25 as its capture date.
  backend.stats()->ToggleStatsFilter("date", "2026-05-25");
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);

  // The unknown-date bucket must not match a dated file.
  backend.stats()->ToggleStatsFilter("date", "(unknown)");
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 0);
}

TEST_F(StatsFilterTests, CombinedSearchAndStatsBarFilterRestrictGridAndStatsTogether) {
  const auto seeded = CreateSeededPackedProject(temp_dir_);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  ASSERT_FALSE(backend.library()->Thumbnails().isEmpty());

  // Active fuzzy search matches the seeded file name.
  backend.search()->ApplyFuzzySearch("album-delete");
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);

  // A stats-bar bucket that excludes the file must shrink grid and stats together.
  backend.stats()->ToggleStatsFilter("camera", "Other Camera");
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 0);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 0);

  // Clearing the stats filter restores the search-only result.
  backend.stats()->ClearStatsFilter();
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);

  // Clearing the search restores the full folder.
  backend.search()->ClearFuzzySearch();
  ProcessEvents(500);
  EXPECT_EQ(backend.library()->ShownCount(), 1);
  EXPECT_EQ(backend.stats()->TotalPhotoCount(), 1);
}

}  // namespace alcedo::ui::test
