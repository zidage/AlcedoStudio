//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/agent_control_owner.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <optional>
#include <vector>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"

namespace alcedo::ui {
namespace {

auto FindTask(const BackgroundTaskController& tasks, const QString& id) -> QVariantMap {
  for (const QVariant& value : tasks.Tasks()) {
    const QVariantMap task = value.toMap();
    if (task.value(QStringLiteral("id")).toString() == id) {
      return task;
    }
  }
  return {};
}

/// Runs the queued calls, for example the FinishTask that follows Stop.
void DrainEvents() {
  for (int i = 0; i < 4; ++i) {
    QCoreApplication::processEvents();
  }
}

TEST(AgentControlOwnerTest, AcquireWaitsForPendingInputRelease) {
  BackgroundTaskController tasks;
  AgentControlOwner        control(&tasks);
  bool                     unsealed = true;
  control.SetUnsealedInputReader([&unsealed]() { return unsealed; });

  EXPECT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::UnsealedEditorInput);
  EXPECT_FALSE(control.held());
  EXPECT_TRUE(tasks.Tasks().isEmpty());

  std::vector<AgentControlRefusal> outcomes;
  control.RequestAcquire(
      QStringLiteral("agent"), []() { return true; },
      [&outcomes](AgentControlRefusal refusal) { outcomes.push_back(refusal); });
  EXPECT_TRUE(outcomes.empty());
  EXPECT_EQ(control.pending_acquire_count(), 1u);

  // The user still drags the slider: the acquire keeps waiting.
  control.RecheckPendingAcquires();
  EXPECT_TRUE(outcomes.empty());
  EXPECT_FALSE(control.held());

  // The release boundary sealed the input: the acquire succeeds once.
  unsealed = false;
  control.RecheckPendingAcquires();
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes.front(), AgentControlRefusal::None);
  EXPECT_TRUE(control.held());
  EXPECT_EQ(control.pending_acquire_count(), 0u);
  control.RecheckPendingAcquires();
  EXPECT_EQ(outcomes.size(), 1u);
}

TEST(AgentControlOwnerTest, AcquireWaitIsDroppedWhenTheCallerStopsWaiting) {
  BackgroundTaskController tasks;
  AgentControlOwner        control(&tasks);
  bool                     unsealed = true;
  control.SetUnsealedInputReader([&unsealed]() { return unsealed; });

  bool waiting = true;
  int  done    = 0;
  control.RequestAcquire(
      QStringLiteral("agent"), [&waiting]() { return waiting; },
      [&done](AgentControlRefusal) { ++done; });
  waiting  = false;
  unsealed = false;
  control.RecheckPendingAcquires();
  EXPECT_EQ(done, 0);
  EXPECT_FALSE(control.held());
  EXPECT_EQ(control.pending_acquire_count(), 0u);
}

TEST(AgentControlOwnerTest, AcquireRegistersRunningAgentTaskWithUserStateChangeLock) {
  BackgroundTaskController    tasks;
  InteractionPolicyController policy(&tasks);
  AgentControlOwner           control(&tasks);
  QSignalSpy                  changed(&control, &AgentControlOwner::ControlChanged);
  ASSERT_TRUE(policy.CanChangeStateAsUser());

  ASSERT_EQ(control.TryAcquire(QStringLiteral("claude-code")), AgentControlRefusal::None);
  EXPECT_TRUE(control.held());
  EXPECT_EQ(control.holder_name(), QStringLiteral("claude-code"));
  EXPECT_EQ(changed.count(), 1);

  const QVariantMap task = FindTask(tasks, control.task_id());
  ASSERT_FALSE(task.isEmpty());
  EXPECT_EQ(task.value(QStringLiteral("kind")).toString(), QStringLiteral("agentControl"));
  EXPECT_EQ(task.value(QStringLiteral("state")).toString(), QStringLiteral("running"));
  EXPECT_EQ(task.value(QStringLiteral("title")).toString(), QStringLiteral("claude-code"));
  EXPECT_EQ(task.value(QStringLiteral("detail")).toString(), AgentControlOwner::WaitingDetail());
  EXPECT_EQ(task.value(QStringLiteral("progressPercent")).toInt(), -1);
  EXPECT_TRUE(task.value(QStringLiteral("cancelable")).toBool());
  const QVariantList locks = task.value(QStringLiteral("locks")).toList();
  ASSERT_EQ(locks.size(), 1);
  EXPECT_EQ(locks.front().toMap().value(QStringLiteral("capability")).toString(),
            QStringLiteral("userStateChange"));

  EXPECT_FALSE(policy.CanChangeStateAsUser());
  EXPECT_FALSE(policy.ChangeStateAsUserReason().isEmpty());
  EXPECT_EQ(UserStateChangeRefusal(&policy), policy.ChangeStateAsUserReason());

  // The same holder acquires again without a second task; another holder is refused.
  EXPECT_EQ(control.TryAcquire(QStringLiteral("claude-code")), AgentControlRefusal::None);
  EXPECT_EQ(tasks.Tasks().size(), 1);
  EXPECT_EQ(control.TryAcquire(QStringLiteral("other")), AgentControlRefusal::HeldByOtherAgent);
  EXPECT_EQ(control.holder_name(), QStringLiteral("claude-code"));
  EXPECT_EQ(changed.count(), 1);
}

TEST(AgentControlOwnerTest, CommandSummaryUpdatesAgentTaskDetail) {
  BackgroundTaskController tasks;
  AgentControlOwner        control(&tasks);
  EXPECT_EQ(control.BeginCommand(QStringLiteral("editor.set exposure=0.5"), QString()), 0u);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);

  const quint64 first = control.BeginCommand(QStringLiteral("editor.set exposure=0.5"), QString());
  ASSERT_NE(first, 0u);
  EXPECT_EQ(FindTask(tasks, control.task_id()).value(QStringLiteral("detail")).toString(),
            QStringLiteral("editor.set exposure=0.5"));

  const quint64 second = control.BeginCommand(QStringLiteral("editor.undo"), QString());
  EXPECT_EQ(FindTask(tasks, control.task_id()).value(QStringLiteral("detail")).toString(),
            QStringLiteral("editor.undo"));

  // The newest command that still runs shows; after the last one, the waiting text.
  control.EndCommand(second, true);
  EXPECT_EQ(FindTask(tasks, control.task_id()).value(QStringLiteral("detail")).toString(),
            QStringLiteral("editor.set exposure=0.5"));
  control.EndCommand(first, true);
  EXPECT_EQ(FindTask(tasks, control.task_id()).value(QStringLiteral("detail")).toString(),
            AgentControlOwner::WaitingDetail());
}

TEST(AgentControlOwnerTest, CommandsAppearInAgentTaskActivityWithNoteAndOutcome) {
  BackgroundTaskController tasks;
  AgentControlOwner        control(&tasks);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);

  const quint64 set  = control.BeginCommand(QStringLiteral("editor.set exposure 0.5"),
                                            QStringLiteral("agent: lift exposure"));
  const quint64 undo = control.BeginCommand(QStringLiteral("editor.undo"), QString());
  QVariantList  activity =
      FindTask(tasks, control.task_id()).value(QStringLiteral("activity")).toList();
  // The acquire entry, then one entry per command in start order.
  ASSERT_EQ(activity.size(), 3);
  EXPECT_EQ(activity[1].toMap().value(QStringLiteral("text")).toString(),
            QStringLiteral("editor.set exposure 0.5"));
  EXPECT_EQ(activity[1].toMap().value(QStringLiteral("note")).toString(),
            QStringLiteral("agent: lift exposure"));
  EXPECT_EQ(activity[1].toMap().value(QStringLiteral("state")).toString(),
            QStringLiteral("running"));

  control.EndCommand(set, true);
  control.EndCommand(undo, false);
  activity = FindTask(tasks, control.task_id()).value(QStringLiteral("activity")).toList();
  EXPECT_EQ(activity[1].toMap().value(QStringLiteral("state")).toString(),
            QStringLiteral("succeeded"));
  EXPECT_EQ(activity[2].toMap().value(QStringLiteral("state")).toString(),
            QStringLiteral("failed"));
}

TEST(AgentControlOwnerTest, CancelingAgentTaskStopsAgent) {
  BackgroundTaskController    tasks;
  InteractionPolicyController policy(&tasks);
  AgentControlOwner           control(&tasks);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
  const QString task_id = control.task_id();
  const quint64 running = control.BeginCommand(QStringLiteral("editor.set"), QString());

  // The task dialog cancels the task: Stop.
  ASSERT_TRUE(tasks.CancelTask(task_id));
  EXPECT_FALSE(control.held());
  EXPECT_TRUE(control.stopped_by_user());
  EXPECT_TRUE(control.holder_name().isEmpty());

  DrainEvents();
  const QVariantMap task = FindTask(tasks, task_id);
  EXPECT_EQ(task.value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
  EXPECT_TRUE(policy.CanChangeStateAsUser());
  EXPECT_EQ(control.task_id(), task_id);
  // A command that the agent sends after the stop does not start.
  EXPECT_EQ(control.BeginCommand(QStringLiteral("editor.undo"), QString()), 0u);

  // The command that ran during the stop still reports its outcome in the activity.
  control.EndCommand(running, true);
  const QVariantList activity = FindTask(tasks, task_id).value(QStringLiteral("activity")).toList();
  bool               found    = false;
  for (const QVariant& entry : activity) {
    if (entry.toMap().value(QStringLiteral("text")).toString() == QStringLiteral("editor.set")) {
      found = true;
      EXPECT_EQ(entry.toMap().value(QStringLiteral("state")).toString(),
                QStringLiteral("succeeded"));
    }
  }
  EXPECT_TRUE(found);
}

TEST(AgentControlOwnerTest, AcquireAfterStopResumesStoppedTaskOfSameHolder) {
  BackgroundTaskController    tasks;
  InteractionPolicyController policy(&tasks);
  AgentControlOwner           control(&tasks);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
  const QString task_id = control.task_id();
  control.Stop();
  DrainEvents();
  ASSERT_TRUE(control.stopped_by_user());

  // The user told the agent to continue; nothing is selected in the application.
  QSignalSpy changed(&control, &AgentControlOwner::ControlChanged);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
  EXPECT_TRUE(control.held());
  EXPECT_FALSE(control.stopped_by_user());
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(control.task_id(), task_id);
  EXPECT_EQ(tasks.Tasks().size(), 1);
  EXPECT_EQ(FindTask(tasks, task_id).value(QStringLiteral("state")).toString(),
            QStringLiteral("running"));
  EXPECT_FALSE(policy.CanChangeStateAsUser());

  // A stop of the resumed task works like the first one.
  control.Stop();
  DrainEvents();
  EXPECT_TRUE(control.stopped_by_user());
  EXPECT_EQ(FindTask(tasks, task_id).value(QStringLiteral("state")).toString(),
            QStringLiteral("canceled"));
}

TEST(AgentControlOwnerTest, AcquireAfterStopStartsNewTaskForOtherHolderOrDismissedTask) {
  BackgroundTaskController tasks;
  AgentControlOwner        control(&tasks);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
  const QString first_task = control.task_id();
  control.Stop();
  DrainEvents();

  ASSERT_EQ(control.TryAcquire(QStringLiteral("other")), AgentControlRefusal::None);
  EXPECT_NE(control.task_id(), first_task);
  EXPECT_EQ(tasks.Tasks().size(), 2);
  EXPECT_EQ(FindTask(tasks, first_task).value(QStringLiteral("state")).toString(),
            QStringLiteral("canceled"));

  const QString second_task = control.task_id();
  control.Stop();
  DrainEvents();
  ASSERT_TRUE(tasks.DismissTask(second_task));
  ASSERT_EQ(control.TryAcquire(QStringLiteral("other")), AgentControlRefusal::None);
  EXPECT_NE(control.task_id(), second_task);
  EXPECT_EQ(FindTask(tasks, control.task_id()).value(QStringLiteral("state")).toString(),
            QStringLiteral("running"));
}

TEST(AgentControlOwnerTest, AgentTaskIsPinnedAndOutlivesManyShortTasks) {
  BackgroundTaskController tasks;
  AgentControlOwner        control(&tasks);
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
  const QString agent_task = control.task_id();
  EXPECT_TRUE(FindTask(tasks, agent_task).value(QStringLiteral("pinned")).toBool());
  control.Stop();
  DrainEvents();

  // Many editor saves finish after the stop.
  for (int i = 0; i < 20; ++i) {
    BackgroundTaskSnapshot save;
    save.kind_  = BackgroundTaskKind::EditorSave;
    save.state_ = BackgroundTaskState::Running;
    tasks.FinishTask(tasks.RegisterTask(save), BackgroundTaskState::Succeeded);
  }
  EXPECT_FALSE(FindTask(tasks, agent_task).isEmpty());
  EXPECT_EQ(tasks.PrimaryTask().value(QStringLiteral("id")).toString(), agent_task);

  // The user dismisses it: the island shows the last short task again.
  ASSERT_TRUE(tasks.DismissTask(agent_task));
  EXPECT_TRUE(FindTask(tasks, agent_task).isEmpty());
  EXPECT_EQ(tasks.PrimaryTask().value(QStringLiteral("kind")).toString(),
            QStringLiteral("editorSave"));
}

TEST(AgentControlOwnerTest, ReleaseFinishesAgentTaskAsSucceeded) {
  BackgroundTaskController    tasks;
  InteractionPolicyController policy(&tasks);
  AgentControlOwner           control(&tasks);
  EXPECT_FALSE(control.Release());
  ASSERT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
  const QString task_id = control.task_id();

  EXPECT_TRUE(control.Release());
  EXPECT_FALSE(control.held());
  EXPECT_FALSE(control.stopped_by_user());
  const QVariantMap task = FindTask(tasks, task_id);
  EXPECT_EQ(task.value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
  EXPECT_TRUE(policy.CanChangeStateAsUser());
  EXPECT_EQ(tasks.ActiveCountExcept(BackgroundTaskKind::SemanticGeneration), 0);

  // A released agent can acquire again at once.
  EXPECT_EQ(control.TryAcquire(QStringLiteral("agent")), AgentControlRefusal::None);
}

}  // namespace
}  // namespace alcedo::ui
