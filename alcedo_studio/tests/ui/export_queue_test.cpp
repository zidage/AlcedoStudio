//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ExportQueue rules that ExportQueueState.qml had: one entry for each element, the preview
// rows, and the removal of deleted elements.

#include "ui/alcedo_main/album_backend/export_queue.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>

namespace alcedo::ui::test {
namespace {

auto Item(uint element_id, uint image_id, const QString& file_name, bool is_hdr = false)
    -> QVariantMap {
  return QVariantMap{{QStringLiteral("elementId"), element_id},
                     {QStringLiteral("imageId"), image_id},
                     {QStringLiteral("fileName"), file_name},
                     {QStringLiteral("isHdr"), is_hdr}};
}

TEST(ExportQueueTest, AddTargetsKeepsLastEntryPerElement) {
  ExportQueue queue(nullptr, nullptr);
  QSignalSpy  changed(&queue, &ExportQueue::QueueChanged);

  queue.AddTargets(QVariantList{
      Item(7, 70, QStringLiteral("b.arw")), Item(3, 30, QStringLiteral("a.arw")),
      Item(7, 71, QStringLiteral("b2.arw"), true), Item(0, 99, QStringLiteral("ignored.arw"))});
  EXPECT_EQ(changed.count(), 1);
  ASSERT_EQ(queue.Count(), 2);
  // The repeated element keeps its position and takes the values of the last entry.
  EXPECT_EQ(queue.Entries()[0].element_id, 7u);
  EXPECT_EQ(queue.Entries()[0].image_id, 71u);
  EXPECT_EQ(queue.Entries()[0].file_name, QStringLiteral("b2.arw"));
  EXPECT_TRUE(queue.HasHdrItems());

  queue.AddTargets(QVariantList{Item(3, 31, QString())});
  ASSERT_EQ(queue.Count(), 2);
  EXPECT_EQ(queue.Entries()[1].image_id, 31u);
  EXPECT_FALSE(queue.Entries()[1].file_name.isEmpty());

  const QVariantList targets = queue.Targets();
  ASSERT_EQ(targets.size(), 2);
  EXPECT_EQ(targets[0].toMap().value(QStringLiteral("elementId")).toUInt(), 7u);
  EXPECT_EQ(targets[0].toMap().value(QStringLiteral("imageId")).toUInt(), 71u);
  EXPECT_TRUE(queue.EntriesById().contains(QStringLiteral("3")));
}

TEST(ExportQueueTest, PreviewRowsAreSortedByFileNameWithStatusKeys) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(QVariantList{Item(1, 10, QStringLiteral("c.arw")),
                                Item(2, 20, QStringLiteral("a.arw")),
                                Item(3, 30, QStringLiteral("b.arw"))});
  const QVariantList rows = queue.PreviewRows();
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows[0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("a.arw"));
  EXPECT_EQ(rows[0].toMap().value(QStringLiteral("statusKey")).toString(), QStringLiteral("2:20"));
  EXPECT_EQ(rows[2].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("c.arw"));
  EXPECT_FALSE(rows[1].toMap().value(QStringLiteral("summaryRow")).toBool());
}

TEST(ExportQueueTest, RemoveAndClearPublishOnlyWhenEntriesChange) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(
      QVariantList{Item(1, 10, QStringLiteral("a.arw")), Item(2, 20, QStringLiteral("b.arw"))});
  QSignalSpy changed(&queue, &ExportQueue::QueueChanged);

  queue.RemoveElements(QVariantList{9});
  EXPECT_EQ(changed.count(), 0);
  queue.RemoveElements(QVariantList{1});
  EXPECT_EQ(changed.count(), 1);
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_EQ(queue.Entries()[0].element_id, 2u);
  queue.Clear();
  EXPECT_EQ(changed.count(), 2);
  EXPECT_EQ(queue.Count(), 0);
  queue.Clear();
  EXPECT_EQ(changed.count(), 2);
}

}  // namespace
}  // namespace alcedo::ui::test
