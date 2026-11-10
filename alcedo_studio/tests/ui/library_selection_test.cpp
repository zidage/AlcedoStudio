//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LibrarySelection rules that SelectionState.qml had: no-op changes, replace, prune, and the
// target maps of the selected images. LibrarySelectionMarks: the summary of the marks of the
// selected photos for the marks control (#772).

#include "ui/alcedo_main/album_backend/library_selection.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

#include "ui/alcedo_main/album_backend/library_selection_marks.hpp"

namespace alcedo::ui::test {
namespace {

auto Row(sl_element_id_t file_id, int rating, ColorLabelMask color_labels, bool delete_mark)
    -> SearchResultRow {
  SearchResultRow row;
  row.file_id_      = file_id;
  row.image_id_     = file_id * 10;
  row.rating_       = rating;
  row.color_labels_ = color_labels;
  row.delete_mark_  = delete_mark;
  return row;
}

auto Counts(const LibrarySelectionMarks& marks) -> std::vector<int> {
  std::vector<int> counts;
  for (const QVariant& count : marks.ColorLabelCounts()) {
    counts.push_back(count.toInt());
  }
  return counts;
}

auto Item(uint element_id, uint image_id, const QString& file_name) -> QVariantMap {
  return QVariantMap{{QStringLiteral("elementId"), element_id},
                     {QStringLiteral("imageId"), image_id},
                     {QStringLiteral("fileName"), file_name},
                     {QStringLiteral("isHdr"), false}};
}

TEST(LibrarySelectionTest, SetSelectedIsNoOpWhenStateMatches) {
  LibrarySelection selection(nullptr);
  QSignalSpy       changed(&selection, &LibrarySelection::SelectionChanged);

  selection.SetImageSelected(5, 50, QStringLiteral("a.arw"), false, false);
  EXPECT_EQ(changed.count(), 0);
  selection.SetImageSelected(5, 50, QStringLiteral("a.arw"), false, true);
  EXPECT_EQ(changed.count(), 1);
  selection.SetImageSelected(5, 50, QStringLiteral("a.arw"), false, true);
  EXPECT_EQ(changed.count(), 1);
  selection.Replace(QVariantList{Item(5, 50, QStringLiteral("a.arw"))});
  EXPECT_EQ(changed.count(), 1);
  selection.PruneElements(QVariantList{7});
  EXPECT_EQ(changed.count(), 1);
  selection.Clear();
  EXPECT_EQ(changed.count(), 2);
  selection.Clear();
  EXPECT_EQ(changed.count(), 2);
}

TEST(LibrarySelectionTest, ReplaceAndPruneKeepTheSelectedImages) {
  LibrarySelection selection(nullptr);
  selection.Replace(QVariantList{Item(3, 30, QStringLiteral("c.arw")), Item(1, 10, QString()),
                                 Item(0, 99, QStringLiteral("ignored"))});
  ASSERT_EQ(selection.SelectedCount(), 2);
  EXPECT_TRUE(selection.IsSelected(3));
  EXPECT_TRUE(selection.IsSelected(1));
  EXPECT_FALSE(selection.IsSelected(0));
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1, 3}));
  const QVariantList before_prune = selection.SelectedItems();
  ASSERT_EQ(before_prune.size(), 2);
  EXPECT_EQ(before_prune.at(1).toMap().value(QStringLiteral("imageId")).toUInt(), 30u);
  // An empty file name gets the placeholder name, as in QML.
  EXPECT_FALSE(before_prune.at(0).toMap().value(QStringLiteral("fileName")).toString().isEmpty());

  selection.Prune({3});
  EXPECT_FALSE(selection.IsSelected(3));
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1}));
  const QVariantList items = selection.SelectedItems();
  ASSERT_EQ(items.size(), 1);
  EXPECT_EQ(items.front().toMap().value(QStringLiteral("fileId")).toUInt(), 1u);
}

TEST(LibrarySelectionTest, RevisionIncrementsOnEachChangeOnly) {
  LibrarySelection selection(nullptr);
  const int        start = selection.Revision();
  selection.SetImageSelected(5, 50, QStringLiteral("a.arw"), false, true);
  EXPECT_EQ(selection.Revision(), start + 1);
  selection.SetImageSelected(5, 50, QStringLiteral("a.arw"), false, true);
  EXPECT_EQ(selection.Revision(), start + 1);
  selection.AddItems(QVariantList{Item(5, 50, QStringLiteral("a.arw"))});
  EXPECT_EQ(selection.Revision(), start + 1);
  selection.AddItems(QVariantList{Item(6, 60, QStringLiteral("b.arw"))});
  EXPECT_EQ(selection.Revision(), start + 2);
}

TEST(LibrarySelectionTest, AddItemsKeepsTheSelection) {
  LibrarySelection selection(nullptr);
  selection.Replace(QVariantList{Item(1, 10, QStringLiteral("a.arw"))});
  selection.AddItems(
      QVariantList{Item(2, 20, QStringLiteral("b.arw")), Item(1, 10, QStringLiteral("a.arw"))});
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1, 2}));
}

TEST(LibrarySelectionTest, RangeItemsReplaceOnlyWhatTheOpenRangeAdded) {
  LibrarySelection selection(nullptr);
  selection.Replace(
      QVariantList{Item(1, 10, QStringLiteral("a.arw")), Item(3, 30, QStringLiteral("c.arw"))});

  // The band grows over 2, 3 and 4: 3 was selected before the range.
  selection.SetRangeItems(QVariantList{Item(2, 20, QStringLiteral("b.arw")),
                                       Item(3, 30, QStringLiteral("c.arw")),
                                       Item(4, 40, QStringLiteral("d.arw"))});
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1, 2, 3, 4}));

  // The band shrinks to 4 only: 2 leaves, 3 stays because the range did not add it.
  selection.SetRangeItems(QVariantList{Item(4, 40, QStringLiteral("d.arw"))});
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1, 3, 4}));

  // After EndRange, a new range keeps 4.
  selection.EndRange();
  selection.SetRangeItems(QVariantList{Item(5, 50, QStringLiteral("e.arw"))});
  selection.SetRangeItems(QVariantList{});
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1, 3, 4}));
}

TEST(LibrarySelectionMarksTest, SummaryCountsCodesRatingsAndTheDeleteMark) {
  LibrarySelectionMarks marks;
  QSignalSpy            changed(&marks, &LibrarySelectionMarks::Changed);
  EXPECT_EQ(marks.Count(), 0);
  EXPECT_EQ(marks.Rating(), -1);
  EXPECT_FALSE(marks.RatingMixed());

  const ColorLabelMask red   = ColorLabelBit(ColorLabel::kRed);
  const ColorLabelMask green = ColorLabelBit(ColorLabel::kGreen);
  marks.Replace({Row(1, 2, red | green, false), Row(2, 4, red, true), Row(3, 2, 0, false)});
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(marks.Count(), 3);
  EXPECT_EQ(marks.Rating(), -1);
  EXPECT_TRUE(marks.RatingMixed());
  EXPECT_EQ(Counts(marks), (std::vector<int>{2, 0, 0, 1, 0, 0, 0}));
  EXPECT_EQ(marks.DeleteMarkCount(), 1);

  // The same marks again change nothing.
  marks.Replace({Row(3, 2, 0, false), Row(2, 4, red, true), Row(1, 2, red | green, false)});
  EXPECT_EQ(changed.count(), 1);

  marks.Replace({Row(1, 5, 0, false), Row(2, 5, 0, false)});
  EXPECT_EQ(marks.Rating(), 5);
  EXPECT_FALSE(marks.RatingMixed());

  marks.Clear();
  EXPECT_EQ(marks.Count(), 0);
  EXPECT_EQ(marks.Rating(), -1);
  EXPECT_EQ(Counts(marks), std::vector<int>(kColorLabelCount, 0));
}

TEST(LibrarySelectionMarksTest, ApplyChangesOnlyTheHeldPhotosAndAdvancesTheRevision) {
  LibrarySelectionMarks marks;
  const ColorLabelMask  red = ColorLabelBit(ColorLabel::kRed);
  marks.Replace({Row(1, 2, red, false), Row(2, 4, 0, true)});
  const auto                         revision = marks.LocalChangeRevision();

  // Photo 9 is not selected: the summary ignores it.
  const std::vector<sl_element_id_t> ids{1, 2, 9};
  marks.ApplyColorLabel(ids, ColorLabel::kBlue, true);
  EXPECT_EQ(marks.ColorLabelCount(ColorLabel::kBlue), 2);
  EXPECT_EQ(marks.ColorLabelCount(ColorLabel::kRed), 1);
  EXPECT_GT(marks.LocalChangeRevision(), revision);

  marks.ApplyColorLabel(std::vector<sl_element_id_t>{1}, ColorLabel::kRed, false);
  EXPECT_EQ(marks.ColorLabelCount(ColorLabel::kRed), 0);
  EXPECT_EQ(marks.ColorLabelCount(ColorLabel::kBlue), 2);

  marks.ApplyRating(ids, 3);
  EXPECT_EQ(marks.Rating(), 3);
  EXPECT_FALSE(marks.RatingMixed());

  marks.ApplyDeleteMark(std::vector<sl_element_id_t>{1}, true);
  EXPECT_EQ(marks.DeleteMarkCount(), 2);
  marks.ApplyDeleteMark(ids, false);
  EXPECT_EQ(marks.DeleteMarkCount(), 0);
  EXPECT_EQ(marks.Count(), 2);
}

}  // namespace
}  // namespace alcedo::ui::test
