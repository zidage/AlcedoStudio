//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LibrarySelection rules that SelectionState.qml had: no-op changes, replace, prune, and the
// target maps of the selected images.

#include "ui/alcedo_main/album_backend/library_selection.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

namespace alcedo::ui::test {
namespace {

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

TEST(LibrarySelectionTest, ReplaceAndPruneKeepTheSelectedImagesById) {
  LibrarySelection selection(nullptr);
  selection.Replace(QVariantList{Item(3, 30, QStringLiteral("c.arw")), Item(1, 10, QString()),
                                 Item(0, 99, QStringLiteral("ignored"))});
  ASSERT_EQ(selection.SelectedCount(), 2);
  const QVariantMap by_id = selection.SelectedImagesById();
  EXPECT_EQ(by_id.value(QStringLiteral("3")).toMap().value(QStringLiteral("imageId")).toUInt(),
            30u);
  // An empty file name gets the placeholder name, as in QML.
  EXPECT_FALSE(by_id.value(QStringLiteral("1"))
                   .toMap()
                   .value(QStringLiteral("fileName"))
                   .toString()
                   .isEmpty());
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1, 3}));

  selection.Prune({3});
  EXPECT_EQ(selection.SelectedElementIds(), (std::vector<sl_element_id_t>{1}));
  const QVariantList items = selection.SelectedItems();
  ASSERT_EQ(items.size(), 1);
  EXPECT_EQ(items.front().toMap().value(QStringLiteral("fileId")).toUInt(), 1u);
}

}  // namespace
}  // namespace alcedo::ui::test
