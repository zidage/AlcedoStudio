//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Section projection of grouped library results (album_sort_group_and_import_time_plan.md,
// Phase 2): row geometry, collapse state, occurrence ranges, and bounded page retention.

#include "ui/alcedo_main/album_backend/album_section_model.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace alcedo::ui::test {
namespace {

using Row = AlbumSectionModel::Row;

/// Groups of 5, 1, and 3 photos (the last one is the unknown group).
auto ThreeGroups() -> std::vector<AlbumGroupDescriptor> {
  return {
      {.key_ = std::string("2026-06-07"), .photo_count_ = 5, .first_occurrence_ = 0},
      {.key_ = std::string("2026-06-06"), .photo_count_ = 1, .first_occurrence_ = 5},
      {.key_ = std::monostate{}, .photo_count_ = 3, .first_occurrence_ = 6},
  };
}

auto Range(const QVariantMap& map) -> std::pair<int64_t, int64_t> {
  return {map.value("begin").toLongLong(), map.value("end").toLongLong()};
}

auto Ranges(const QVariantList& list) -> std::vector<std::pair<int64_t, int64_t>> {
  std::vector<std::pair<int64_t, int64_t>> out;
  for (const auto& value : list) {
    out.push_back(Range(value.toMap()));
  }
  return out;
}

TEST(AlbumSectionModelTest, ExpandedGroupsHaveOneHeaderAndCeilingPhotoRows) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetColumnCount(2);
  // (1 + ceil(5/2)) + (1 + ceil(1/2)) + (1 + ceil(3/2)) = 4 + 2 + 3.
  ASSERT_EQ(model.rowCount(), 9);
  EXPECT_EQ(model.RowAt(0).type_, AlbumSectionModel::RowType::kHeader);
  EXPECT_EQ(model.RowAt(3).first_occurrence_, 4);
  EXPECT_EQ(model.RowAt(3).occurrence_count_, 1);
  EXPECT_EQ(model.RowAt(4).type_, AlbumSectionModel::RowType::kHeader);
  EXPECT_EQ(model.RowAt(4).group_index_, 1);
  EXPECT_EQ(model.RowAt(8).first_occurrence_, 8);
  EXPECT_EQ(model.data(model.index(6), AlbumSectionModel::GroupUnknown).toBool(), true);
  EXPECT_EQ(model.data(model.index(6), AlbumSectionModel::PhotoCount).toLongLong(), 3);

  // Column changes recompute rows from counts only; one column at least.
  model.SetColumnCount(0);
  EXPECT_EQ(model.ColumnCount(), 1);
  EXPECT_EQ(model.rowCount(), 3 + 9);
  model.SetColumnCount(10);
  EXPECT_EQ(model.rowCount(), 6);
}

TEST(AlbumSectionModelTest, SectionHeadersRemainUniqueAcrossPageBoundaries) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetColumnCount(2);
  // Pages of 4 occurrences split the first group; each group still has one header with its
  // full count, and every loaded cell resolves through its page.
  model.StorePage(0, {10, 11, 12, 13}, 8);
  model.StorePage(4, {14, 15, 16, 17}, 8);
  model.StorePage(8, {18}, 8);
  int headers = 0;
  for (int row = 0; row < model.rowCount(); ++row) {
    if (model.RowAt(row).type_ == AlbumSectionModel::RowType::kHeader) {
      ++headers;
    }
  }
  EXPECT_EQ(headers, 3);
  EXPECT_EQ(model.data(model.index(0), AlbumSectionModel::PhotoCount).toLongLong(), 5);
  const auto cells = model.data(model.index(3), AlbumSectionModel::FileIds).toList();
  ASSERT_EQ(cells.size(), 1);
  EXPECT_EQ(cells.front().toUInt(), 14u);
  EXPECT_EQ(model.FileIdAt(8), 18u);
}

TEST(AlbumSectionModelTest, SectionRowsSkipCollapsedPhotoRanges) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetColumnCount(2);
  model.SetGroupCollapsed(0, true);
  EXPECT_TRUE(model.IsGroupCollapsed(0));
  // The collapsed group keeps its header row only; counts do not change.
  EXPECT_EQ(model.rowCount(), 1 + 2 + 3);
  EXPECT_EQ(model.UniqueFileCount(), 9);
  EXPECT_EQ(model.OccurrenceTotal(), 9);
  EXPECT_EQ(model.RowForOccurrence(3), 0);  // inside the collapsed group -> its header
  EXPECT_EQ(model.RowForOccurrence(5), 2);
  // Shift selection from occurrence 2 to 7 skips the collapsed group's photos.
  EXPECT_EQ(Ranges(model.VisibleOccurrenceRanges(2, 7)),
            (std::vector<std::pair<int64_t, int64_t>>{{5, 6}, {6, 8}}));
  EXPECT_EQ(Range(model.OccurrenceRangeForRows(0, 5)), (std::pair<int64_t, int64_t>{5, 9}));

  model.ExpandAll();
  EXPECT_FALSE(model.IsGroupCollapsed(0));
  EXPECT_EQ(Ranges(model.VisibleOccurrenceRanges(7, 2)),
            (std::vector<std::pair<int64_t, int64_t>>{{2, 5}, {5, 6}, {6, 8}}));
  model.CollapseAll();
  EXPECT_EQ(model.rowCount(), 3);
}

TEST(AlbumSectionModelTest, CollapseStateFollowsTheGroupKeyAcrossResultReplacement) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetGroupCollapsed(2, true);
  // A new result of the same group field keeps the unknown group collapsed at its new index.
  model.ResetGroups({{.key_ = std::monostate{}, .photo_count_ = 2, .first_occurrence_ = 0}}, 2, 2,
                    true);
  EXPECT_TRUE(model.IsGroupCollapsed(0));
  // Another group field starts expanded.
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  EXPECT_FALSE(model.IsGroupCollapsed(2));
}

TEST(AlbumSectionModelTest, RowOffsetsUseDocumentedHeaderAndPhotoRowHeights) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetColumnCount(2);
  constexpr double kHeader = 40.0;
  constexpr double kPhoto  = 100.0;
  // Rows: H P P P | H P | H P P
  EXPECT_DOUBLE_EQ(model.RowOffset(0, kHeader, kPhoto), 0.0);
  EXPECT_DOUBLE_EQ(model.RowOffset(1, kHeader, kPhoto), 40.0);
  EXPECT_DOUBLE_EQ(model.RowOffset(4, kHeader, kPhoto), 340.0);
  EXPECT_DOUBLE_EQ(model.RowOffset(6, kHeader, kPhoto), 480.0);
  EXPECT_DOUBLE_EQ(model.ContentHeight(kHeader, kPhoto), 720.0);
  EXPECT_EQ(model.RowAtOffset(0.0, kHeader, kPhoto), 0);
  EXPECT_EQ(model.RowAtOffset(39.0, kHeader, kPhoto), 0);
  EXPECT_EQ(model.RowAtOffset(140.0, kHeader, kPhoto), 2);
  EXPECT_EQ(model.RowAtOffset(345.0, kHeader, kPhoto), 4);
  EXPECT_EQ(model.RowAtOffset(700.0, kHeader, kPhoto), 8);
  for (int row = 0; row < model.rowCount(); ++row) {
    EXPECT_EQ(model.RowAtOffset(model.RowOffset(row, kHeader, kPhoto), kHeader, kPhoto), row);
  }
}

TEST(AlbumSectionModelTest, DistantPageStoresKeepRetainedPagesBounded) {
  AlbumSectionModel model;
  // One group of 100,000 photos read in pages of 1000.
  model.ResetGroups(
      {{.key_ = std::string("Canon R5"), .photo_count_ = 100000, .first_occurrence_ = 0}}, 100000,
      100000, false);
  model.SetColumnCount(8);
  std::vector<sl_element_id_t> ids(1000);
  for (int64_t page = 0; page < 100; page += 9) {
    std::iota(ids.begin(), ids.end(), static_cast<sl_element_id_t>(page * 1000 + 1));
    model.StorePage(page * 1000, ids, 3);
    EXPECT_LE(model.LoadedPageCount(), 3u);
    EXPECT_LE(model.LoadedOccurrenceCount(), 3000u);
    EXPECT_EQ(model.FileIdAt(page * 1000), static_cast<uint>(page * 1000 + 1));
  }
  // The pages farthest from the last stored page left first.
  EXPECT_EQ(model.FileIdAt(0), 0u);
  EXPECT_EQ(model.LoadedPageStarts(), (std::vector<int64_t>{81000, 90000, 99000}));
}

TEST(AlbumSectionModelTest, DuplicateOccurrencesOfOneFileListItOnceInUniqueOrder) {
  AlbumSectionModel model;
  model.ResetGroups({{.key_ = std::string("landscape"), .photo_count_ = 2, .first_occurrence_ = 0},
                     {.key_ = std::string("portrait"), .photo_count_ = 2, .first_occurrence_ = 2}},
                    3, 4, false);
  model.StorePage(0, {7, 8, 9, 7}, 3);
  EXPECT_EQ(model.LoadedUniqueFileIds(), (std::vector<sl_element_id_t>{7, 8, 9}));
  EXPECT_EQ(model.FileIdAt(3), 7u);
}

}  // namespace
}  // namespace alcedo::ui::test
