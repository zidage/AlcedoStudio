//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ExportQueue rules: one entry for each element, the preview rows, the removal of deleted
// elements, and the export runs of the entries.

#include "ui/alcedo_main/album_backend/export_queue.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <initializer_list>
#include <string>
#include <utility>

#include "edit/input/ai_denoise_error.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"

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

auto Statuses(std::initializer_list<std::pair<const char*, const char*>> pairs) -> QVariantMap {
  QVariantMap statuses;
  for (const auto& [key, status] : pairs) {
    statuses.insert(QString::fromLatin1(key), QString::fromLatin1(status));
  }
  return statuses;
}

auto StatusKeyOfLabel(const ExportQueue& queue, const QString& label) -> QString {
  for (const QVariant& row : queue.PreviewRows()) {
    const QVariantMap map = row.toMap();
    if (map.value(QStringLiteral("label")).toString() == label) {
      return map.value(QStringLiteral("statusKey")).toString();
    }
  }
  ADD_FAILURE() << "no row " << label.toStdString();
  return {};
}

TEST(ExportQueueTest, PreviewRowsAreSortedByFileNameWithStatusKeys) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(QVariantList{Item(1, 10, QStringLiteral("c.arw")),
                                Item(2, 20, QStringLiteral("a.arw")),
                                Item(3, 30, QStringLiteral("b.arw"))});
  const QVariantList rows = queue.PreviewRows();
  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows[0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("a.arw"));
  EXPECT_EQ(rows[2].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("c.arw"));
  EXPECT_FALSE(rows[1].toMap().value(QStringLiteral("summaryRow")).toBool());
  // Entries outside an export run have no status key.
  EXPECT_TRUE(rows[0].toMap().value(QStringLiteral("statusKey")).toString().isEmpty());

  queue.ApplyExportState(1, Statuses({{"2:20", "queued"}, {"3:30", "running"}}));
  EXPECT_EQ(StatusKeyOfLabel(queue, QStringLiteral("a.arw")), QStringLiteral("2:20"));
  EXPECT_EQ(StatusKeyOfLabel(queue, QStringLiteral("b.arw")), QStringLiteral("3:30"));
  EXPECT_TRUE(StatusKeyOfLabel(queue, QStringLiteral("c.arw")).isEmpty());
}

TEST(ExportQueueTest, RunRemovesItsFinishedEntries) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(QVariantList{Item(1, 10, QStringLiteral("a.arw")),
                                Item(2, 20, QStringLiteral("b.arw")),
                                Item(3, 30, QStringLiteral("c.arw"))});
  QSignalSpy changed(&queue, &ExportQueue::QueueChanged);

  queue.ApplyExportState(1, Statuses({{"1:10", "queued"}, {"2:20", "queued"}}));
  EXPECT_EQ(changed.count(), 1);
  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}, {"2:20", "running"}}));
  EXPECT_EQ(changed.count(), 2);
  ASSERT_EQ(queue.Count(), 2);
  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}, {"2:20", "failed"}}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_EQ(queue.Entries()[0].element_id, 3u);
  // A state change that removes nothing publishes nothing.
  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}, {"2:20", "failed"}}));
  EXPECT_EQ(changed.count(), 3);
}

// #1018: a photo that its run reports as missing stays in the queue with that status, and the
// next run takes it again.
TEST(ExportQueueTest, MissingEntryStaysQueuedWithItsStatus) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(
      QVariantList{Item(1, 10, QStringLiteral("a.arw")), Item(2, 20, QStringLiteral("b.arw"))});
  queue.ApplyExportState(1, Statuses({{"1:10", "queued"}, {"2:20", "queued"}}));
  queue.ApplyExportState(1, Statuses({{"1:10", "missing"}, {"2:20", "succeeded"}}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_EQ(queue.Entries()[0].element_id, 1u);
  EXPECT_EQ(StatusKeyOfLabel(queue, QStringLiteral("a.arw")), QStringLiteral("1:10"));

  queue.ApplyExportState(2, Statuses({{"1:10", "queued"}}));
  EXPECT_EQ(queue.Entries()[0].run_id, 2u);
  queue.ApplyExportState(2, Statuses({{"1:10", "succeeded"}}));
  EXPECT_EQ(queue.Count(), 0);
}

// #955: a photo added again after its export is queued for the next export; the result of the
// earlier run neither shows on its row nor removes it.
TEST(ExportQueueTest, PhotoAddedAfterItsExportWaitsForTheNextRun) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(QVariantList{Item(1, 10, QStringLiteral("a.arw"))});
  queue.ApplyExportState(1, Statuses({{"1:10", "queued"}}));
  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}}));
  ASSERT_EQ(queue.Count(), 0);

  queue.AddTargets(QVariantList{Item(1, 10, QStringLiteral("a.arw"))});
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_TRUE(StatusKeyOfLabel(queue, QStringLiteral("a.arw")).isEmpty());
  // Another state change of the finished run (for example a reset of the panel) keeps it.
  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_EQ(queue.Targets().size(), 1);

  // The next run takes it and removes it when it is done.
  queue.ApplyExportState(2, Statuses({{"1:10", "queued"}}));
  EXPECT_EQ(StatusKeyOfLabel(queue, QStringLiteral("a.arw")), QStringLiteral("1:10"));
  queue.ApplyExportState(2, Statuses({{"1:10", "succeeded"}}));
  EXPECT_EQ(queue.Count(), 0);
}

// A photo added during a run that already exported it waits for the next run; adding a photo
// that the run has not finished keeps it in the run.
TEST(ExportQueueTest, PhotoAddedDuringARunKeepsItsOwnRun) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(
      QVariantList{Item(1, 10, QStringLiteral("a.arw")), Item(2, 20, QStringLiteral("b.arw"))});
  queue.ApplyExportState(1, Statuses({{"1:10", "queued"}, {"2:20", "queued"}}));
  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}, {"2:20", "running"}}));
  ASSERT_EQ(queue.Count(), 1);

  queue.AddTargets(
      QVariantList{Item(1, 10, QStringLiteral("a.arw")), Item(2, 20, QStringLiteral("b2.arw"))});
  ASSERT_EQ(queue.Count(), 2);
  EXPECT_TRUE(StatusKeyOfLabel(queue, QStringLiteral("a.arw")).isEmpty());
  EXPECT_EQ(StatusKeyOfLabel(queue, QStringLiteral("b2.arw")), QStringLiteral("2:20"));

  queue.ApplyExportState(1, Statuses({{"1:10", "succeeded"}, {"2:20", "succeeded"}}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_EQ(queue.Entries()[0].element_id, 1u);
}

// AI Denoise integration plan, DN8: the status of a task whose render failed with a model error.
TEST(ExportQueueTest, ItemStatusOfAnAiDenoiseErrorIsAiDenoise) {
  const std::string model_error =
      AiDenoiseError(AiDenoiseErrorKind::DeviceTooSlow, "projected 9.1 s").what();
  EXPECT_EQ(ExportItemStatusOf(true, false, {}), QStringLiteral("succeeded"));
  EXPECT_EQ(ExportItemStatusOf(false, true, model_error), QStringLiteral("missing"));
  EXPECT_EQ(ExportItemStatusOf(false, false, "ExportService: encode failed"),
            QStringLiteral("failed"));
  EXPECT_EQ(ExportItemStatusOf(false, false, model_error), QStringLiteral("ai_denoise"));
  // The render path can put text before the error.
  EXPECT_EQ(ExportItemStatusOf(false, false, "Pipeline render failed: " + model_error),
            QStringLiteral("ai_denoise"));
  // A succeeded task has no message to read.
  EXPECT_EQ(ExportItemStatusOf(true, false, model_error), QStringLiteral("succeeded"));
}

// DN8: a photo whose model run failed stays in the queue; Turn Off AI Denoise returns it to
// Queued, and the next run takes it.
TEST(ExportQueueTest, AiDenoiseEntriesStayInTheQueueAndReturnToQueued) {
  ExportQueue queue(nullptr, nullptr);
  queue.AddTargets(
      QVariantList{Item(1, 10, QStringLiteral("a.arw")), Item(2, 20, QStringLiteral("b.arw"))});
  queue.ApplyExportState(1, Statuses({{"1:10", "queued"}, {"2:20", "queued"}}));
  queue.ApplyExportState(1, Statuses({{"1:10", "ai_denoise"}, {"2:20", "succeeded"}}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_EQ(StatusKeyOfLabel(queue, QStringLiteral("a.arw")), QStringLiteral("1:10"));

  QSignalSpy changed(&queue, &ExportQueue::QueueChanged);
  EXPECT_FALSE(queue.ReturnToQueued(9)) << "no entry of the element";
  EXPECT_TRUE(queue.ReturnToQueued(1));
  EXPECT_EQ(changed.count(), 1);
  EXPECT_TRUE(StatusKeyOfLabel(queue, QStringLiteral("a.arw")).isEmpty()) << "Queued";
  EXPECT_EQ(queue.Entries()[0].run_id, 0u);
  EXPECT_TRUE(queue.ReturnToQueued(1));
  EXPECT_EQ(changed.count(), 1) << "an entry that is already queued publishes nothing";

  // Another state change of the finished run keeps the entry and does not show its old status.
  queue.ApplyExportState(1, Statuses({{"1:10", "ai_denoise"}, {"2:20", "succeeded"}}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_TRUE(StatusKeyOfLabel(queue, QStringLiteral("a.arw")).isEmpty());

  queue.ApplyExportState(2, Statuses({{"1:10", "queued"}}));
  EXPECT_EQ(queue.Entries()[0].run_id, 2u);
  queue.ApplyExportState(2, Statuses({{"1:10", "succeeded"}}));
  EXPECT_EQ(queue.Count(), 0);
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
