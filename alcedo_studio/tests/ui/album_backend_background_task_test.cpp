//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostBackgroundTaskTests = ApplicationModuleHostTestFixture;

auto MakeSnapshot(BackgroundTaskKind kind = BackgroundTaskKind::ImageAnalysis)
    -> BackgroundTaskSnapshot {
  BackgroundTaskSnapshot s;
  s.kind_            = kind;
  s.state_           = BackgroundTaskState::Running;
  s.title_           = QStringLiteral("Test task");
  s.cancelable_      = true;
  s.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  return s;
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, RegisterTask_PopulatesReads) {
  BackgroundTaskController controller;
  QSignalSpy              spy(&controller, &BackgroundTaskController::TasksChanged);

  const QString id = controller.RegisterTask(MakeSnapshot());
  EXPECT_FALSE(id.isEmpty());
  EXPECT_EQ(controller.RunningCount(), 1);
  EXPECT_TRUE(controller.HasBlockingShutdownTasks());

  const QVariantList tasks = controller.Tasks();
  ASSERT_EQ(tasks.size(), 1);
  const QVariantMap task = tasks.first().toMap();
  EXPECT_EQ(task.value("id").toString(), id);
  EXPECT_EQ(task.value("state").toString(), QStringLiteral("running"));
  EXPECT_EQ(task.value("cancelable").toBool(), true);

  const QVariantMap primary = controller.PrimaryTask();
  EXPECT_EQ(primary.value("id").toString(), id);
  EXPECT_GE(spy.count(), 1);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, UpdateTask_ReflectsProgress) {
  BackgroundTaskController controller;
  const QString            id = controller.RegisterTask(MakeSnapshot());
  QSignalSpy               spy(&controller, &BackgroundTaskController::TasksChanged);

  controller.UpdateTask(id, QStringLiteral("Working"), QStringLiteral("d"), 42);
  EXPECT_EQ(spy.count(), 1);
  const QVariantMap task = controller.Tasks().first().toMap();
  EXPECT_EQ(task.value("title").toString(), QStringLiteral("Working"));
  EXPECT_EQ(task.value("detail").toString(), QStringLiteral("d"));
  EXPECT_EQ(task.value("progressPercent").toInt(), 42);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, UpdateTask_OnUnknownIdIsNoOp) {
  BackgroundTaskController controller;
  controller.RegisterTask(MakeSnapshot());
  QSignalSpy spy(&controller, &BackgroundTaskController::TasksChanged);
  controller.UpdateTask(QStringLiteral("nope"), QStringLiteral("x"), QStringLiteral("y"), 1);
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, FinishTask_SetsTerminalState) {
  BackgroundTaskController controller;
  const QString            id = controller.RegisterTask(MakeSnapshot());
  controller.FinishTask(id, BackgroundTaskState::Succeeded, QStringLiteral("done"));
  EXPECT_EQ(controller.RunningCount(), 0);
  EXPECT_FALSE(controller.HasBlockingShutdownTasks());
  const QVariantMap task = controller.Tasks().first().toMap();
  EXPECT_EQ(task.value("state").toString(), QStringLiteral("succeeded"));
  EXPECT_EQ(task.value("detail").toString(), QStringLiteral("done"));
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, FinishTask_FailedAndCanceledStates) {
  BackgroundTaskController controller;
  const QString            a = controller.RegisterTask(MakeSnapshot());
  controller.FinishTask(a, BackgroundTaskState::Failed);
  EXPECT_EQ(controller.Tasks().first().toMap().value("state").toString(),
            QStringLiteral("failed"));

  const QString b = controller.RegisterTask(MakeSnapshot());
  controller.FinishTask(b, BackgroundTaskState::Canceled);
  EXPECT_EQ(controller.Tasks().last().toMap().value("state").toString(),
            QStringLiteral("canceled"));
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, CancelTask_InvokesCallbackExactlyOnce) {
  BackgroundTaskController controller;
  int                       cancel_count = 0;
  const QString             id = controller.RegisterTask(MakeSnapshot(), [&] { ++cancel_count; });

  EXPECT_TRUE(controller.CancelTask(id));
  EXPECT_EQ(cancel_count, 1);
  // A second cancel on the still-non-terminal task must not re-invoke.
  EXPECT_TRUE(controller.CancelTask(id));
  EXPECT_EQ(cancel_count, 1);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, CancelTask_OnFinishedTaskIsNoOp) {
  BackgroundTaskController controller;
  int                       cancel_count = 0;
  const QString             id = controller.RegisterTask(MakeSnapshot(), [&] { ++cancel_count; });
  controller.FinishTask(id, BackgroundTaskState::Succeeded);
  EXPECT_FALSE(controller.CancelTask(id));
  EXPECT_EQ(cancel_count, 0);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, CancelTask_OnNonCancelableTaskIsNoOp) {
  BackgroundTaskController controller;
  int                       cancel_count = 0;
  auto                      s = MakeSnapshot();
  s.cancelable_ = false;
  const QString id = controller.RegisterTask(s, [&] { ++cancel_count; });
  EXPECT_FALSE(controller.CancelTask(id));
  EXPECT_EQ(cancel_count, 0);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, CancelAll_InvokesEveryActiveCancelableOnce) {
  BackgroundTaskController controller;
  int                       a = 0, b = 0;
  controller.RegisterTask(MakeSnapshot(), [&] { ++a; });
  // A non-cancelable WaitForFinish task must NOT be canceled.
  auto s = MakeSnapshot();
  s.cancelable_       = false;
  s.shutdown_policy_ = BackgroundTaskShutdownPolicy::WaitForFinish;
  controller.RegisterTask(s, [&] { ++b; });

  controller.CancelAll();
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 0);
  // CancelAll again: a already invoked, so no second call.
  controller.CancelAll();
  EXPECT_EQ(a, 1);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, HasBlockingShutdownTasks_OnlyForActiveCancelAndWait) {
  BackgroundTaskController controller;
  EXPECT_FALSE(controller.HasBlockingShutdownTasks());

  auto s = MakeSnapshot();
  s.shutdown_policy_ = BackgroundTaskShutdownPolicy::WaitForFinish;
  const QString id = controller.RegisterTask(s);
  EXPECT_FALSE(controller.HasBlockingShutdownTasks());

  controller.FinishTask(id, BackgroundTaskState::Succeeded);
  EXPECT_FALSE(controller.HasBlockingShutdownTasks());

  // A Running CancelAndWait task is blocking.
  controller.RegisterTask(MakeSnapshot());
  EXPECT_TRUE(controller.HasBlockingShutdownTasks());
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, PrimaryTask_PrefersActiveThenLastFinished) {
  BackgroundTaskController controller;
  const QString            finished = controller.RegisterTask(MakeSnapshot());
  controller.FinishTask(finished, BackgroundTaskState::Succeeded);
  // Only a finished task -> primary is that finished task.
  EXPECT_EQ(controller.PrimaryTask().value("id").toString(), finished);

  // Register an active task -> primary switches to the active one.
  const QString active = controller.RegisterTask(MakeSnapshot());
  EXPECT_EQ(controller.PrimaryTask().value("id").toString(), active);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, PruneFinished_KeepsAtMostCap) {
  BackgroundTaskController controller;
  // Register and finish more than the retention cap (8) tasks one at a time;
  // the oldest finished tasks are pruned so the recent list stays bounded.
  for (int i = 0; i < 12; ++i) {
    const QString id = controller.RegisterTask(MakeSnapshot());
    controller.FinishTask(id, BackgroundTaskState::Succeeded);
  }
  EXPECT_LE(controller.Tasks().size(), 8);
  EXPECT_EQ(controller.RunningCount(), 0);
}

TEST_F(ApplicationModuleHostBackgroundTaskTests, EmptyController_HasNoPrimary) {
  BackgroundTaskController controller;
  EXPECT_EQ(controller.RunningCount(), 0);
  EXPECT_TRUE(controller.PrimaryTask().isEmpty());
  EXPECT_TRUE(controller.Tasks().isEmpty());
  EXPECT_FALSE(controller.HasBlockingShutdownTasks());
  EXPECT_FALSE(controller.CancelTask(QStringLiteral("nonexistent")));
}

}  // namespace
}  // namespace alcedo::ui::test