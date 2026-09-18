//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_interactive_pacing.hpp"

#include <gtest/gtest.h>

namespace alcedo {
namespace {

TEST(EditorInteractivePacingTest, FiveMillisecondCycleWaitsRemainingBudget) {
  EditorInteractivePacing pacing;
  pacing.NoteInteractiveStart(0);
  pacing.NoteInteractiveComplete(5'000'000);
  EXPECT_EQ(pacing.InteractiveWaitNs(5'000'000), 11'000'000);
  EXPECT_EQ(pacing.InteractiveWaitNs(16'000'000), 0);
}

TEST(EditorInteractivePacingTest, OverBudgetCycleIsEligibleAtCompletion) {
  EditorInteractivePacing pacing;
  pacing.NoteInteractiveStart(0);
  pacing.NoteInteractiveComplete(22'000'000);
  EXPECT_EQ(pacing.InteractiveWaitNs(22'000'000), 0);
  EXPECT_EQ(pacing.next_interactive_eligible_ns(), 22'000'000);
}

TEST(EditorInteractivePacingTest, ExactBudgetCycleHasNoRemainingWait) {
  EditorInteractivePacing pacing;
  pacing.NoteInteractiveStart(1'000'000);
  pacing.NoteInteractiveComplete(17'000'000);
  EXPECT_EQ(pacing.InteractiveWaitNs(17'000'000), 0);
}

TEST(EditorInteractivePacingTest, NonInteractiveResetAllowsImmediateStart) {
  EditorInteractivePacing pacing;
  pacing.NoteInteractiveStart(0);
  pacing.NoteInteractiveComplete(5'000'000);
  pacing.ResetAfterNonInteractive();
  EXPECT_EQ(pacing.InteractiveWaitNs(5'000'000), 0);
}

TEST(EditorInteractivePacingTest, IdleHasNoWaitAndDoesNotInventTicks) {
  EditorInteractivePacing pacing;
  EXPECT_EQ(pacing.InteractiveWaitNs(0), 0);
  EXPECT_EQ(pacing.InteractiveWaitNs(32'000'000), 0);
  EXPECT_FALSE(pacing.next_interactive_eligible_ns().has_value());
}

}  // namespace
}  // namespace alcedo
