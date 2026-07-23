//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_task_port.hpp"

#include <gtest/gtest.h>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui {
namespace {

TEST(EditorSessionTaskPortTest, RegistersEditorSaveLocksAndFinishesTask) {
  BackgroundTaskController tasks;
  EditorSessionTaskPort    port(&tasks);

  const auto               task_id = port.BeginTask("editor_save", 42);
  ASSERT_NE(task_id, 0u);
  ASSERT_EQ(tasks.RunningCount(), 1);

  const auto locks = tasks.ActiveLocks();
  ASSERT_EQ(locks.size(), 5u);
  EXPECT_EQ(locks[0].lock_.capability_, InteractionCapability::SelectEditorImage);
  EXPECT_EQ(locks[1].lock_.capability_, InteractionCapability::SwitchWorkspace);
  EXPECT_EQ(locks[2].lock_.capability_, InteractionCapability::CheckoutVersion);
  EXPECT_EQ(locks[3].lock_.capability_, InteractionCapability::PasteAdjustments);
  EXPECT_EQ(locks[4].lock_.capability_, InteractionCapability::MergeAdjustments);

  port.EndTask(task_id, true, "checkpoint finished");
  EXPECT_EQ(tasks.RunningCount(), 0);
  EXPECT_TRUE(tasks.ActiveLocks().empty());
  ASSERT_EQ(tasks.Tasks().size(), 1);
  EXPECT_EQ(tasks.Tasks().first().toMap().value("state").toString(), QStringLiteral("succeeded"));
}

TEST(EditorSessionTaskPortTest, MissingControllerStillReturnsLocalTaskId) {
  EditorSessionTaskPort port;
  const auto            task_id = port.BeginTask("editor_save", 42);

  EXPECT_NE(task_id, 0u);
  EXPECT_NO_THROW(port.EndTask(task_id, false, "failed"));
}

}  // namespace
}  // namespace alcedo::ui
