//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file ai_denoise_library_actions_test.cpp
/// @brief AI Denoise outside the editor (integration plan, DN8): the action of the image context
///        menu (Bayer check, the photo open in the editor, the missing model) and Turn Off AI
///        Denoise of an export queue item (the write and the queue state).

#include "ui/alcedo_main/album_backend/ai_denoise_library_actions.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "edit/input/ai_denoise_error.hpp"
#include "ui/alcedo_main/album_backend/export_queue.hpp"

namespace alcedo::ui {
namespace {

const std::string kVersion(64, 'a');

/// The fake collaborators of the actions. The sensor of a photo comes from the extension of its
/// file: `.raf` is X-Trans, `.jpg` is another layout, `.bad` cannot be opened, the rest is Bayer.
struct FakeLibrary {
  struct StoredWrite {
    sl_element_id_t                 element_id = 0;
    std::optional<DevelopAiDenoise> denoise;
  };

  auto Make() -> AiDenoiseLibraryActions::Services {
    AiDenoiseLibraryActions::Services services;
    services.platform_available      = [this] { return platform; };
    services.installed_model_version = [this]() -> std::string {
      if (!model_installed) {
        throw AiDenoiseError(AiDenoiseErrorKind::ModelNotInstalled, "no graph file");
      }
      return kVersion;
    };
    services.user_refusal = [this] { return refusal; };
    services.image_path   = [](image_id_t image_id) {
      static const char* const kNames[] = {"", "a.arw", "b.raf", "c.jpg", "d.bad", "e.arw"};
      return image_id < std::size(kNames) ? std::filesystem::path(kNames[image_id])
                                            : std::filesystem::path{};
    };
    services.read_sensor_layout = [](const std::filesystem::path& path) {
      const auto extension = path.extension().string();
      if (extension == ".bad") {
        throw std::runtime_error("LibRaw cannot open the file");
      }
      if (extension == ".raf") {
        return RawSensorLayout::XTrans;
      }
      return extension == ".jpg" ? RawSensorLayout::Other : RawSensorLayout::Bayer;
    };
    services.stored_history_writer = [this]() -> AiDenoiseLibraryActions::StoredHistoryWriter {
      return [this](sl_element_id_t element_id, const std::optional<DevelopAiDenoise>& denoise) {
        while (hold_writes.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (element_id == failing_element) {
          throw std::runtime_error("stored history changed");
        }
        std::lock_guard lock(mutex);
        stored_writes.push_back({element_id, denoise});
        return AiDenoiseHistoryWrite{true, 1234, "Default"};
      };
    };
    services.editor_element_id  = [this] { return editor_element; };
    services.write_editor_photo = [this](const std::optional<DevelopAiDenoise>& denoise) {
      editor_writes.push_back(denoise);
      return editor_accepts;
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

  bool                                         platform        = true;
  bool                                         model_installed = true;
  QString                                      refusal;
  sl_element_id_t                              editor_element  = 0;
  bool                                         editor_accepts  = true;
  sl_element_id_t                              failing_element = 0;
  std::atomic<bool>                            hold_writes{false};
  std::mutex                                   mutex;
  std::vector<StoredWrite>                     stored_writes;
  std::vector<std::optional<DevelopAiDenoise>> editor_writes;
  std::vector<sl_element_id_t>                 refreshed;
};

auto Target(uint element_id, uint image_id) -> QVariantMap {
  return QVariantMap{{QStringLiteral("elementId"), element_id},
                     {QStringLiteral("imageId"), image_id},
                     {QStringLiteral("fileName"), QStringLiteral("photo %1").arg(element_id)}};
}

auto ElementIds(const QVariantList& rows) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const QVariant& row : rows) {
    ids.push_back(
        static_cast<sl_element_id_t>(row.toMap().value(QStringLiteral("elementId")).toULongLong()));
  }
  return ids;
}

/// Starts nothing more and waits for the result of the started run.
auto WaitForResult(AiDenoiseLibraryActions& actions, QSignalSpy& finished) -> QVariantMap {
  if (finished.isEmpty() && !finished.wait(5000)) {
    ADD_FAILURE() << "the run did not finish";
    return {};
  }
  EXPECT_FALSE(actions.running());
  return finished.takeFirst().at(0).toMap();
}

TEST(AiDenoiseLibraryActionsTest, TurnOnWritesTheBayerTargetsAndReportsTheOthers) {
  FakeLibrary             library;
  AiDenoiseLibraryActions actions(library.Make(), nullptr, nullptr);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  const QVariantMap       started = actions.turnOnForTargets(
      QVariantList{Target(11, 1), Target(12, 2), Target(13, 3), Target(14, 4), Target(15, 5)});
  EXPECT_TRUE(started.value(QStringLiteral("started")).toBool());
  EXPECT_TRUE(actions.running());
  const QVariantMap result = WaitForResult(actions, finished);

  const auto        writes = library.StoredWrites();
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[0].element_id, 11u);
  EXPECT_EQ(writes[1].element_id, 15u);
  for (const auto& write : writes) {
    ASSERT_TRUE(write.denoise.has_value());
    EXPECT_EQ(write.denoise->model_version, kVersion) << "the installed version (D10)";
  }
  EXPECT_EQ(library.refreshed, (std::vector<sl_element_id_t>{11, 15}));
  EXPECT_EQ(result.value(QStringLiteral("action")).toString(), QStringLiteral("turn_on"));
  EXPECT_FALSE(result.value(QStringLiteral("success")).toBool());
  EXPECT_EQ(result.value(QStringLiteral("doneCount")).toInt(), 2);
  // X-Trans, another layout, and a file that LibRaw cannot open are not Bayer RAW photos (D14).
  EXPECT_EQ(ElementIds(result.value(QStringLiteral("notSupported")).toList()),
            (std::vector<sl_element_id_t>{12, 13, 14}));
  EXPECT_TRUE(result.value(QStringLiteral("failures")).toList().isEmpty());
  EXPECT_FALSE(result.value(QStringLiteral("message")).toString().isEmpty());
}

TEST(AiDenoiseLibraryActionsTest, FailedWriteIsReportedAndTheOtherTargetsAreWritten) {
  FakeLibrary library;
  library.failing_element = 11;
  AiDenoiseLibraryActions actions(library.Make(), nullptr, nullptr);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOnForTargets(QVariantList{Target(11, 1), Target(15, 5)})
                  .value(QStringLiteral("started"))
                  .toBool());
  const QVariantMap result = WaitForResult(actions, finished);

  ASSERT_EQ(library.StoredWrites().size(), 1u);
  EXPECT_EQ(library.StoredWrites()[0].element_id, 15u);
  EXPECT_EQ(library.refreshed, (std::vector<sl_element_id_t>{15}));
  const QVariantList failures = result.value(QStringLiteral("failures")).toList();
  ASSERT_EQ(ElementIds(failures), (std::vector<sl_element_id_t>{11}));
  EXPECT_EQ(failures[0].toMap().value(QStringLiteral("message")).toString(),
            QStringLiteral("stored history changed"));
}

TEST(AiDenoiseLibraryActionsTest, TurnOnWithoutAModelRequestsTheAiModelsPageAndWritesNothing) {
  FakeLibrary library;
  library.model_installed = false;
  AiDenoiseLibraryActions actions(library.Make(), nullptr, nullptr);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  const QVariantMap       result = actions.turnOnForTargets(QVariantList{Target(11, 1)});

  EXPECT_FALSE(result.value(QStringLiteral("started")).toBool());
  EXPECT_EQ(result.value(QStringLiteral("result")).toString(),
            QStringLiteral("model_not_installed"));
  EXPECT_FALSE(actions.running());
  EXPECT_FALSE(finished.wait(200));
  EXPECT_TRUE(library.StoredWrites().empty());
}

TEST(AiDenoiseLibraryActionsTest, TurnOnIsRefusedWithoutAPlatformOrWhileAnAgentHoldsControl) {
  FakeLibrary library;
  library.platform = false;
  AiDenoiseLibraryActions without_platform(library.Make(), nullptr, nullptr);
  EXPECT_FALSE(without_platform.available());
  EXPECT_EQ(without_platform.turnOnForTargets(QVariantList{Target(11, 1)})
                .value(QStringLiteral("result"))
                .toString(),
            QStringLiteral("refused"));

  library.platform = true;
  library.refusal  = QStringLiteral("An agent controls the application.");
  AiDenoiseLibraryActions held(library.Make(), nullptr, nullptr);
  const QVariantMap       result = held.turnOnForTargets(QVariantList{Target(11, 1)});
  EXPECT_EQ(result.value(QStringLiteral("result")).toString(), QStringLiteral("refused"));
  EXPECT_EQ(result.value(QStringLiteral("message")).toString(), library.refusal);
  EXPECT_EQ(held.turnOffForExportItem(11).value(QStringLiteral("result")).toString(),
            QStringLiteral("refused"));
  EXPECT_TRUE(library.StoredWrites().empty());
}

TEST(AiDenoiseLibraryActionsTest, TurnOnRoutesTheEditorPhotoThroughTheEditor) {
  FakeLibrary library;
  library.editor_element = 15;
  AiDenoiseLibraryActions actions(library.Make(), nullptr, nullptr);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOnForTargets(QVariantList{Target(11, 1), Target(15, 5)})
                  .value(QStringLiteral("started"))
                  .toBool());
  const QVariantMap result = WaitForResult(actions, finished);

  ASSERT_EQ(library.StoredWrites().size(), 1u);
  EXPECT_EQ(library.StoredWrites()[0].element_id, 11u) << "the session owns the open history";
  ASSERT_EQ(library.editor_writes.size(), 1u);
  ASSERT_TRUE(library.editor_writes[0].has_value());
  EXPECT_EQ(library.editor_writes[0]->model_version, kVersion);
  EXPECT_EQ(library.refreshed, (std::vector<sl_element_id_t>{11}));
  EXPECT_TRUE(result.value(QStringLiteral("success")).toBool());
  EXPECT_EQ(result.value(QStringLiteral("doneCount")).toInt(), 2);
}

TEST(AiDenoiseLibraryActionsTest, EditorPhotoThatIsNotBayerIsNotWritten) {
  FakeLibrary library;
  library.editor_element = 12;
  AiDenoiseLibraryActions actions(library.Make(), nullptr, nullptr);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOnForTargets(QVariantList{Target(12, 2)})
                  .value(QStringLiteral("started"))
                  .toBool());
  const QVariantMap result = WaitForResult(actions, finished);

  EXPECT_TRUE(library.editor_writes.empty());
  EXPECT_EQ(ElementIds(result.value(QStringLiteral("notSupported")).toList()),
            (std::vector<sl_element_id_t>{12}));
}

/// An export queue whose entry of element 21 failed with an AI Denoise error in run 1.
auto QueueWithAiDenoiseError(ExportQueue& queue) {
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

TEST(AiDenoiseLibraryActionsTest, TurnOffOfAQueueItemWritesTheSwitchOffAndReturnsItToQueued) {
  FakeLibrary library;
  ExportQueue queue(nullptr, nullptr);
  QueueWithAiDenoiseError(queue);
  ASSERT_EQ(StatusKeyOfEntry(queue), QStringLiteral("21:1"));
  AiDenoiseLibraryActions actions(library.Make(), nullptr, &queue);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  EXPECT_EQ(StatusKeyOfEntry(queue), QStringLiteral("21:1")) << "the item waits for the write";
  const QVariantMap result = WaitForResult(actions, finished);

  ASSERT_EQ(library.StoredWrites().size(), 1u);
  EXPECT_EQ(library.StoredWrites()[0].element_id, 21u);
  EXPECT_FALSE(library.StoredWrites()[0].denoise.has_value()) << "the switch off";
  EXPECT_EQ(library.refreshed, (std::vector<sl_element_id_t>{21}));
  ASSERT_EQ(queue.Count(), 1);
  EXPECT_TRUE(StatusKeyOfEntry(queue).isEmpty()) << "Queued for the next export";
  EXPECT_EQ(queue.Entries()[0].run_id, 0u);
  EXPECT_TRUE(result.value(QStringLiteral("success")).toBool());
  EXPECT_EQ(result.value(QStringLiteral("action")).toString(), QStringLiteral("turn_off"));
  EXPECT_TRUE(result.value(QStringLiteral("message")).toString().isEmpty());
}

TEST(AiDenoiseLibraryActionsTest, FailedTurnOffKeepsTheItemInItsRun) {
  FakeLibrary library;
  library.failing_element = 21;
  ExportQueue queue(nullptr, nullptr);
  QueueWithAiDenoiseError(queue);
  AiDenoiseLibraryActions actions(library.Make(), nullptr, &queue);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  const QVariantMap result = WaitForResult(actions, finished);

  EXPECT_EQ(StatusKeyOfEntry(queue), QStringLiteral("21:1"));
  EXPECT_FALSE(result.value(QStringLiteral("success")).toBool());
  EXPECT_FALSE(result.value(QStringLiteral("message")).toString().isEmpty());
}

TEST(AiDenoiseLibraryActionsTest, TurnOffOfTheEditorPhotoGoesThroughTheEditor) {
  FakeLibrary library;
  library.editor_element = 21;
  ExportQueue queue(nullptr, nullptr);
  QueueWithAiDenoiseError(queue);
  AiDenoiseLibraryActions actions(library.Make(), nullptr, &queue);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOffForExportItem(21).value(QStringLiteral("started")).toBool());
  (void)WaitForResult(actions, finished);

  EXPECT_TRUE(library.StoredWrites().empty());
  ASSERT_EQ(library.editor_writes.size(), 1u);
  EXPECT_FALSE(library.editor_writes[0].has_value());
  EXPECT_TRUE(StatusKeyOfEntry(queue).isEmpty());
}

TEST(AiDenoiseLibraryActionsTest, SecondRunIsRefusedWhileOneRuns) {
  FakeLibrary library;
  library.hold_writes = true;
  AiDenoiseLibraryActions actions(library.Make(), nullptr, nullptr);
  QSignalSpy              running(&actions, &AiDenoiseLibraryActions::RunningChanged);
  QSignalSpy              finished(&actions, &AiDenoiseLibraryActions::RunFinished);

  ASSERT_TRUE(actions.turnOnForTargets(QVariantList{Target(11, 1)})
                  .value(QStringLiteral("started"))
                  .toBool());
  EXPECT_EQ(running.count(), 1);
  EXPECT_EQ(actions.turnOnForTargets(QVariantList{Target(15, 5)})
                .value(QStringLiteral("result"))
                .toString(),
            QStringLiteral("refused"));
  EXPECT_EQ(actions.turnOffForExportItem(15).value(QStringLiteral("result")).toString(),
            QStringLiteral("refused"));

  library.hold_writes = false;
  (void)WaitForResult(actions, finished);
  EXPECT_EQ(running.count(), 2);
  ASSERT_EQ(library.StoredWrites().size(), 1u);
  EXPECT_EQ(library.StoredWrites()[0].element_id, 11u);
}

}  // namespace
}  // namespace alcedo::ui
