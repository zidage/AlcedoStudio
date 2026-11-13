//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Section projection of grouped library results (album_sort_group_and_import_time_plan.md,
// Phase 2): row geometry, collapse state, occurrence ranges, and bounded page retention.
// Issue #609: an import or a delete changes the rows and the retained pages in place.

#include "ui/alcedo_main/album_backend/album_section_model.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
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

// Collapsing or expanding inserts or removes only the photo rows of the group below its header
// and updates the header; it never resets the model, so a view keeps its scroll position.
TEST(AlbumSectionModelTest, CollapseChangesOnlyTheGroupRowsWithoutAModelReset) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetColumnCount(2);  // Rows: H P P P | H P | H P P
  QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
  QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
  QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
  QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

  model.SetGroupCollapsed(1, true);
  ASSERT_EQ(removed.count(), 1);
  EXPECT_EQ(removed.at(0).at(1).toInt(), 5);
  EXPECT_EQ(removed.at(0).at(2).toInt(), 5);
  ASSERT_EQ(changed.count(), 1);
  EXPECT_EQ(changed.at(0).at(0).value<QModelIndex>().row(), 4);
  EXPECT_TRUE(model.data(model.index(4), AlbumSectionModel::Collapsed).toBool());
  EXPECT_EQ(model.rowCount(), 8);
  EXPECT_EQ(model.RowAt(5).group_index_, 2);  // the next header moved up one row

  model.SetGroupCollapsed(1, false);
  ASSERT_EQ(inserted.count(), 1);
  EXPECT_EQ(inserted.at(0).at(1).toInt(), 5);
  EXPECT_EQ(inserted.at(0).at(2).toInt(), 5);
  EXPECT_EQ(model.rowCount(), 9);

  removed.clear();
  inserted.clear();
  EXPECT_EQ(model.GroupHeaderRow(2), 6);
  EXPECT_EQ(model.GroupHeaderRow(3), -1);
  model.CollapseAll();
  EXPECT_EQ(removed.count(), 3);
  EXPECT_EQ(model.GroupHeaderRow(2), 2);
  EXPECT_EQ(model.rowCount(), 3);
  model.ExpandAll();
  EXPECT_EQ(inserted.count(), 3);
  EXPECT_EQ(model.rowCount(), 9);
  EXPECT_EQ(resets.count(), 0);
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

TEST(AlbumSectionModelTest, RowInfoDescribesRowsTheViewHasNotCreated) {
  AlbumSectionModel model;
  model.ResetGroups(ThreeGroups(), 9, 9, false);
  model.SetColumnCount(2);
  const auto header = model.RowInfo(4);
  EXPECT_EQ(header.value("kind").toInt(), 0);
  EXPECT_EQ(header.value("groupIndex").toInt(), 1);
  EXPECT_EQ(header.value("firstOccurrence").toLongLong(), 5);
  const auto photos = model.RowInfo(7);
  EXPECT_EQ(photos.value("kind").toInt(), 1);
  EXPECT_EQ(photos.value("firstOccurrence").toLongLong(), 6);
  EXPECT_EQ(photos.value("occurrenceCount").toLongLong(), 2);
  EXPECT_EQ(model.RowInfo(9).value("kind").toInt(), -1);
  // Role data carries the separate header roles a view needs for accessible names.
  EXPECT_EQ(model.data(model.index(4), AlbumSectionModel::GroupTitle).toString(),
            QStringLiteral("2026-06-06"));
  EXPECT_EQ(model.data(model.index(4), AlbumSectionModel::RowKind).toInt(), 0);
  EXPECT_EQ(model.data(model.index(5), AlbumSectionModel::RowOccurrenceCount).toLongLong(), 1);
  EXPECT_TRUE(model.roleNames().contains(AlbumSectionModel::Collapsed));
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

// Groups A (10 photos, ids 101..110), B (10, 201..210), and C (5, 301..305) at 4 columns, with
// the pages [0, 8), [8, 16), and [16, 24) loaded.
auto IdsOf(const std::vector<std::vector<sl_element_id_t>>& groups)
    -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& group : groups) {
    ids.insert(ids.end(), group.begin(), group.end());
  }
  return ids;
}

auto GroupsOf(const std::vector<std::pair<std::string, int64_t>>& counts)
    -> std::vector<AlbumGroupDescriptor> {
  std::vector<AlbumGroupDescriptor> groups;
  int64_t                           next = 0;
  for (const auto& [key, count] : counts) {
    groups.push_back({.key_ = key, .photo_count_ = count, .first_occurrence_ = next});
    next += count;
  }
  return groups;
}

auto IdRange(sl_element_id_t first, sl_element_id_t last) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids(last - first + 1);
  std::iota(ids.begin(), ids.end(), first);
  return ids;
}

void LoadThreeGroupsWithThreePages(AlbumSectionModel& model) {
  model.ResetGroups(GroupsOf({{"A", 10}, {"B", 10}, {"C", 5}}), 25, 25, false);
  model.SetColumnCount(4);  // Rows: H P P P | H P P P | H P P
  const auto ids = IdsOf({IdRange(101, 110), IdRange(201, 210), IdRange(301, 305)});
  for (int64_t start = 0; start < 24; start += 8) {
    model.StorePage(start,
                    std::vector<sl_element_id_t>(ids.begin() + start, ids.begin() + start + 8), 3);
  }
}

auto RowFileIds(const AlbumSectionModel& model, int row) -> std::vector<uint> {
  std::vector<uint> out;
  for (const auto& id : model.data(model.index(row), AlbumSectionModel::FileIds).toList()) {
    out.push_back(id.toUInt());
  }
  return out;
}

// A delete of 102 and of 109 (the first photo of the second page) removes the last photo row of
// group A; 110 moves up into the second photo row, and every retained page moves to the new
// occurrences of its photos without a page read.
TEST(AlbumSectionModelTest,
     InPlaceDeleteRemovesRowsAndMovesRetainedPagesAcrossPageAndRowBoundaries) {
  AlbumSectionModel model;
  LoadThreeGroupsWithThreePages(model);
  ASSERT_EQ(model.rowCount(), 11);
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  QSignalSpy               resets(&model, &QAbstractItemModel::modelReset);
  QSignalSpy               removed(&model, &QAbstractItemModel::rowsRemoved);
  QSignalSpy               inserted(&model, &QAbstractItemModel::rowsInserted);
  QSignalSpy               changed(&model, &QAbstractItemModel::dataChanged);

  auto                     a_ids = IdRange(101, 110);
  std::erase(a_ids, 102u);
  std::erase(a_ids, 109u);
  const auto ids    = IdsOf({a_ids, IdRange(201, 210), IdRange(301, 305)});
  const auto groups = GroupsOf({{"A", 8}, {"B", 10}, {"C", 5}});
  auto       pages  = model.RetainedPagesInResult(groups, ids);
  ASSERT_EQ(pages.size(), 3u);
  model.ApplyResultChange(groups, 23, 23, std::move(pages));

  EXPECT_EQ(resets.count(), 0);
  EXPECT_EQ(inserted.count(), 0);
  ASSERT_EQ(removed.count(), 1);
  EXPECT_EQ(removed.at(0).at(1).toInt(), 3);
  EXPECT_EQ(removed.at(0).at(2).toInt(), 3);
  EXPECT_GE(changed.count(), 1);
  EXPECT_EQ(model.rowCount(), 10);
  EXPECT_EQ(model.OccurrenceTotal(), 23);
  // [0, 8) lost 102 and [8, 16) lost 109: the pages start at 0, 7, and 14.
  EXPECT_EQ(model.LoadedPageStarts(), (std::vector<int64_t>{0, 7, 14}));
  EXPECT_EQ(model.LoadedOccurrenceCount(), 22u);
  EXPECT_EQ(model.FileIdAt(1), 103u);
  EXPECT_EQ(model.FileIdAt(7), 110u);
  EXPECT_EQ(model.FileIdAt(8), 201u);
  EXPECT_EQ(model.FileIdAt(18), 301u);
  EXPECT_EQ(model.FileIdAt(22), 0u);  // 305 was never loaded
  EXPECT_EQ(RowFileIds(model, 2), (std::vector<uint>{106, 107, 108, 110}));
  EXPECT_EQ(model.GroupHeaderRow(1), 3);
  EXPECT_EQ(model.RowForOccurrence(8), 4);
  EXPECT_EQ(RowFileIds(model, 4), (std::vector<uint>{201, 202, 203, 204}));
  EXPECT_EQ(model.FirstUnloadedOccurrence(0, 23), 22);
}

// An import creates group Z before A and adds 150 at the start of A; group C, now empty, is
// removed. The new group and the removed group give row inserts and removes, the collapse state
// of B stays, and the photos that the pages do not hold (Z and 150) are left to the next page
// read.
TEST(AlbumSectionModelTest, InPlaceImportInsertsAndRemovesGroupRowsAndKeepsRetainedPages) {
  AlbumSectionModel model;
  LoadThreeGroupsWithThreePages(model);
  model.SetGroupCollapsed(1, true);  // Rows: H P P P | H | H P P
  ASSERT_EQ(model.rowCount(), 8);
  QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
  QSignalSpy               resets(&model, &QAbstractItemModel::modelReset);
  QSignalSpy               removed(&model, &QAbstractItemModel::rowsRemoved);
  QSignalSpy               inserted(&model, &QAbstractItemModel::rowsInserted);

  auto                     a_ids = IdRange(101, 110);
  a_ids.insert(a_ids.begin(), 150u);
  const auto ids    = IdsOf({{901, 902}, a_ids, IdRange(201, 210)});
  const auto groups = GroupsOf({{"Z", 2}, {"A", 11}, {"B", 10}});
  auto       pages  = model.RetainedPagesInResult(groups, ids);
  model.ApplyResultChange(groups, 23, 23, std::move(pages));

  EXPECT_EQ(resets.count(), 0);
  ASSERT_EQ(inserted.count(), 1);  // Z: its header and one photo row at the top
  EXPECT_EQ(inserted.at(0).at(1).toInt(), 0);
  EXPECT_EQ(inserted.at(0).at(2).toInt(), 1);
  ASSERT_EQ(removed.count(), 1);  // C: its header and two photo rows after the collapsed B
  EXPECT_EQ(removed.at(0).at(1).toInt(), 7);
  EXPECT_EQ(removed.at(0).at(2).toInt(), 9);
  EXPECT_EQ(model.GroupCount(), 3);
  EXPECT_TRUE(model.IsGroupCollapsed(2));
  EXPECT_EQ(model.rowCount(), 2 + 4 + 1);
  // [0, 8) moved by 3 (Z and 150 before it); the photos of C left the last page.
  EXPECT_EQ(model.LoadedPageStarts(), (std::vector<int64_t>{3, 11, 19}));
  EXPECT_EQ(model.FileIdAt(3), 101u);
  EXPECT_EQ(model.FileIdAt(12), 110u);
  EXPECT_EQ(model.FileIdAt(13), 201u);
  EXPECT_EQ(model.FileIdAt(22), 210u);
  EXPECT_EQ(model.LoadedEnd(), 23);
  EXPECT_EQ(model.FirstUnloadedOccurrence(0, 23), 0);
  EXPECT_EQ(model.NextLoadedPageStart(0), 3);
  EXPECT_EQ(model.FirstUnloadedOccurrence(3, 23), -1);
  // A page read of the gap joins no other page, and the retention limit holds.
  model.StorePage(0, {901, 902, 150}, 3);
  EXPECT_EQ(model.FileIdAt(2), 150u);
  EXPECT_EQ(model.LoadedPageCount(), 3u);
  EXPECT_EQ(model.FirstUnloadedOccurrence(0, 19), -1);
}

// A file in two label groups keeps the occurrence of each group: the page finds the photo in
// the group of the same key, not at its first occurrence.
TEST(AlbumSectionModelTest, RetainedPagesFindEachLabelOccurrenceInItsOwnGroup) {
  AlbumSectionModel model;
  model.ResetGroups(GroupsOf({{"landscape", 2}, {"portrait", 2}}), 3, 4, false);
  model.StorePage(0, {7, 8, 9, 7}, 3);
  const auto                         groups = GroupsOf({{"landscape", 3}, {"portrait", 2}});
  const std::vector<sl_element_id_t> ids{5, 7, 8, 9, 7};
  auto                               pages = model.RetainedPagesInResult(groups, ids);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(pages.begin()->first, 1);
  model.ApplyResultChange(groups, 4, 5, std::move(pages));
  EXPECT_EQ(model.LoadedPageStarts(), (std::vector<int64_t>{1}));
  EXPECT_EQ(model.LoadedOccurrenceOf(7, 0), 1);
  EXPECT_EQ(model.LoadedOccurrenceOf(7, 1), 4);
  EXPECT_EQ(model.GroupIndexForTitle(QStringLiteral("portrait"), false), 1);
  EXPECT_EQ(model.GroupIndexForTitle(QStringLiteral("portrait"), true), -1);
}

}  // namespace
}  // namespace alcedo::ui::test
