//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file export_ai_denoise_control_test.cpp
/// @brief Turn Off AI Denoise of an export queue item (integration plan, DN8): the write of the
///        switch, the photo open in the editor, and the queue state after the write.

#include "ui/alcedo_main/album_backend/export_ai_denoise_control.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ui/alcedo_main/album_backend/export_queue.hpp"

namespace alcedo::ui {
namespace {

/// The fake collaborators of the control.
struct FakeLibrary {
  struct StoredWrite {
    sl_element_id_t                 element_id = 0;
    std::optional<DevelopAiDenoise> denoise;
  };

  auto Make() -> ExportAiDenoiseControl::Services {
    ExportAiDenoiseControl::Services services;
    services.user_refusal          = [this] { return refusal; };
    services.stored_history_writer = [this]() -> ExportAiDenoiseControl::StoredHistoryWriter {
      return [this](sl_element_id_t element_id, const std::optional<DevelopAiDenoise>& denoise) {
        while (hold_writes.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (fail_write) {
          throw std::runtime_error("stored history changed");
        }
        std::lock_guard lock(mutex);
        stored_writes.push_back({element_id, denoise});
        return AiDenoiseHistoryWrite{true, 1234, "Default"};
      };
    };
    services.editor_element_id     = [this] { return editor_element; };
    services.turn_off_editor_photo = [this] {
      ++editor_turn_offs;
      return true;
    };
    services.refresh_written_photo = [this](sl_element_id_t              element_id,
                                            const AiDenoiseHistoryWrite& write) {
      refreshed.push_back(element_id);
      EXPECT_EQ(write.head_created_at_ns, 1234);
    };
    return services;
  }

  [[nodiscard]] auto StoredWrites() -> std::vector<StoredWrite> {
    std::lock_guard lock(mutex);
    return stored_writes;
  }

  QString                      refusal;
  sl_element_id_t              editor_element = 0;
  bool                         fail_write     = false;
  std::atomic<bool>            hold_writes{false};
  std::mutex                   mutex;
  std::vector<StoredWrite>     stored_writes;
  int                          editor_turn_offs = 0;
  std::vector<sl_element_id_t> refreshed;
};

/// An export queue whose entry of element 21 failed with an AI Denoise error in run 1.
void QueueWithAiDenoiseError(ExportQueue& queue) {
  queue.AddTargets(
      QVariantList{QVariantMap{{QStringLiteral("elementId"), 21u},
                               {QStringLiteral("imageId"), 1u},
                               {QStringLiteral("fileName"), QStringLiteral("a.arw")}}});
  queue.ApplyExportState(1, QVariantMap{{QStringLiteral("21:1"), QStringLiteral("queued")}});
  queue.ApplyExportState(1, QVariantMap{{QStringLiteral("21:1"), QStringLiteral("ai_denoise")}});
}

auto StatusKeyOfEntry(const ExportQueue& queue) -> QString {
  return queue.PreviewRows().front().toMap().value(QStringLiteral("statusKey")).toString();
}

/// Waits for the result of the started write.
auto WaitForResult(ExportAiDenoiseControl& control, QSignalSpy& finished) -> QVariantMap {
  if (finished.isEmpty() && !finished.wait(5000)) {
    ADD_FAILURE() << "the write did not finish";
    return {};
  }
  EXPECT_FALSE(control.running());
  return finished.takeFirst().at(0).toMap();
}

TEST(ExportAiDenoiseControlTest, TurnOffOfAQueueItemWritesTheSwitchOffAndReturnsItToQueued) {
  FakeLibrary library;
  ExportQueue queue(nullptr, nullptr);
  QueueWithAiDenoiseError(queue);
  ASSERT_EQ(StatusKeyOfEntry(queue), QStringLiteral("21:1"));
  ExportAiDenoiseControl control(library.Make(), nullptr, &queue);
  QSignalSpy             finished(&control, &ExportAiDenoiseControl::TurnOffFinished);

  ASSERT_TRUE(control.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  EXPECT_TRUE(control.running());
  EXPECT_EQ(StatusKeyOfEntry(queue), QStringLiteral("21:1")) << "the item waits for the write";
  const QVariantMap result = WaitForResult(control, finished);

  ASSERT_EQ(library.StoredWrites().size(), 1u);
  EXPECT_EQ(library.StoredWrites()[0].element_id, 21u);
  EXPECT_FALSE(library.StoredWrites()[0].denoise.has_value()) << "the switch off";
  EXPECT_EQ(library.refreshed, (std::vector<sl_element_id_t>{21}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_TRUE(StatusKeyOfEntry(queue).isEmpty()) << "Queued for the next export";
  EXPECT_EQ(queue.Entries()[0].run_id, 0u);
  EXPECT_TRUE(result.value(QStringLiteral("success")).toBool());
  EXPECT_TRUE(result.value(QStringLiteral("message")).toString().isEmpty());
}

TEST(ExportAiDenoiseControlTest, FailedTurnOffKeepsTheItemInItsRun) {
  FakeLibrary library;
  library.fail_write = true;
  ExportQueue queue(nullptr, nullptr);
  QueueWithAiDenoiseError(queue);
  ExportAiDenoiseControl control(library.Make(), nullptr, &queue);
  QSignalSpy             finished(&control, &ExportAiDenoiseControl::TurnOffFinished);

  ASSERT_TRUE(control.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  const QVariantMap result = WaitForResult(control, finished);

  EXPECT_EQ(StatusKeyOfEntry(queue), QStringLiteral("21:1"));
  EXPECT_TRUE(library.refreshed.empty());
  EXPECT_FALSE(result.value(QStringLiteral("success")).toBool());
  EXPECT_TRUE(result.value(QStringLiteral("message"))
                  .toString()
                  .contains(QStringLiteral("stored history changed")));
}

TEST(ExportAiDenoiseControlTest, TurnOffOfTheEditorPhotoGoesThroughTheEditor) {
  FakeLibrary library;
  library.editor_element = 21;
  ExportQueue queue(nullptr, nullptr);
  QueueWithAiDenoiseError(queue);
  ExportAiDenoiseControl control(library.Make(), nullptr, &queue);
  QSignalSpy             finished(&control, &ExportAiDenoiseControl::TurnOffFinished);

  ASSERT_TRUE(control.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  const QVariantMap result = WaitForResult(control, finished);

  EXPECT_TRUE(library.StoredWrites().empty()) << "the session owns the open history";
  EXPECT_EQ(library.editor_turn_offs, 1);
  EXPECT_TRUE(StatusKeyOfEntry(queue).isEmpty());
  EXPECT_TRUE(result.value(QStringLiteral("success")).toBool());
}

TEST(ExportAiDenoiseControlTest, TurnOffIsRefusedWhileAnAgentHoldsControlOrAWriteRuns) {
  FakeLibrary library;
  library.refusal = QStringLiteral("An agent controls the application.");
  ExportAiDenoiseControl held(library.Make(), nullptr, nullptr);
  const QVariantMap      refused = held.turnOffForExportItem(21);
  EXPECT_FALSE(refused.value(QStringLiteral("started")).toBool());
  EXPECT_EQ(refused.value(QStringLiteral("message")).toString(), library.refusal);

  library.refusal.clear();
  library.hold_writes = true;
  ExportAiDenoiseControl control(library.Make(), nullptr, nullptr);
  QSignalSpy             finished(&control, &ExportAiDenoiseControl::TurnOffFinished);
  ASSERT_TRUE(control.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  EXPECT_FALSE(control.turnOffForExportItem(22).value(QStringLiteral("started")).toBool());

  library.hold_writes = false;
  (void)WaitForResult(control, finished);
  ASSERT_EQ(library.StoredWrites().size(), 1u);
  EXPECT_EQ(library.StoredWrites()[0].element_id, 21u);
}

}  // namespace
}  // namespace alcedo::ui
