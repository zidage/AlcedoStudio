//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file application_module_host_shutdown_test.cpp
/// @brief Verifies host shutdown waits for every registered task and drains
/// analysis writes released by an export barrier.

#include "ui/album_backend_test_fixture.hpp"

#include <QTimer>

#include <array>
#include <memory>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostShutdownTests = ApplicationModuleHostTestFixture;

auto RunningTask(BackgroundTaskKind kind, bool cancelable,
                 BackgroundTaskShutdownPolicy policy) -> BackgroundTaskSnapshot {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_            = kind;
  snapshot.state_           = BackgroundTaskState::Running;
  snapshot.title_           = QStringLiteral("shutdown test task");
  snapshot.cancelable_      = cancelable;
  snapshot.shutdown_policy_ = policy;
  return snapshot;
}

TEST_F(ApplicationModuleHostShutdownTests,
       ShutdownCancelsAndWaitsForImageAnalysisSemanticAndModelTasks) {
  ApplicationModuleHost host;
  auto*                 tasks = host.background_tasks();
  ASSERT_NE(tasks, nullptr);

  const std::array kinds = {BackgroundTaskKind::ImageAnalysis,
                            BackgroundTaskKind::SemanticGeneration,
                            BackgroundTaskKind::ModelDownload};
  for (const auto kind : kinds) {
    const auto id = std::make_shared<QString>();
    *id = tasks->RegisterTask(
        RunningTask(kind, true, BackgroundTaskShutdownPolicy::CancelAndWait),
        [tasks, id] { tasks->FinishTask(*id, BackgroundTaskState::Canceled); });
    ASSERT_FALSE(id->isEmpty());
  }

  EXPECT_EQ(tasks->RunningCount(), 3);
  host.Shutdown();
  EXPECT_EQ(tasks->RunningCount(), 0);
  host.Shutdown();
  EXPECT_EQ(tasks->RunningCount(), 0);
}

TEST_F(ApplicationModuleHostShutdownTests,
       ShutdownWaitsForExportBarrierAndDrainsAnalysisCompletion) {
  ApplicationModuleHost host;
  auto*                 tasks = host.background_tasks();
  auto*                 sink  = host.image_analysis_sink();
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(sink, nullptr);

  host.db_write_barrier().Acquire();
  ImageAnalysisItemResult result;
  result.item.element_id = 1;
  ASSERT_TRUE(sink->PersistUnderstanding(result));
  ASSERT_TRUE(sink->HasPendingWrites());

  const QString export_id = tasks->RegisterTask(
      RunningTask(BackgroundTaskKind::Export, false,
                  BackgroundTaskShutdownPolicy::WaitForFinish));
  ASSERT_FALSE(export_id.isEmpty());

  QTimer::singleShot(0, [&host, tasks, export_id] {
    host.db_write_barrier().Release();
    tasks->FinishTask(export_id, BackgroundTaskState::Succeeded);
  });

  host.Shutdown();
  EXPECT_EQ(tasks->RunningCount(), 0);
  EXPECT_FALSE(host.db_write_barrier().IsHeld());
  EXPECT_FALSE(sink->HasPendingWrites());
}

}  // namespace
}  // namespace alcedo::ui::test
