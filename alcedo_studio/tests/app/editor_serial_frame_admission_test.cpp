//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_monotonic_clock.hpp"
#include "app/editor_serial_frame_admission.hpp"
#include "support/manual_monotonic_clock.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace alcedo {
namespace {

class ManualEditorClock final : public IEditorMonotonicClock {
 public:
  test::ManualMonotonicClock clock;

  [[nodiscard]] auto NowNs() const -> std::int64_t override { return clock.now_ns(); }
};

TEST(EditorSerialFrameAdmissionTest, InteractiveWaitDoesNotBeginUntilBudgetElapses) {
  EditorSerialFrameAdmission admission;
  auto                       clock = std::make_shared<ManualEditorClock>();
  clock->clock.set_ns(0);
  admission.SetClock(clock);
  std::int64_t last_delay = 0;
  admission.SetDeadlineHandler([&](std::int64_t delay_ns) { last_delay = delay_ns; });

  ASSERT_TRUE(admission.TryBeginCycle(true));
  admission.NoteScheduledRequest(4);
  clock->clock.set_ns(5'000'000);
  ASSERT_TRUE(admission.CompleteIfMatches(4));
  EXPECT_FALSE(admission.TryBeginCycle(true));
  EXPECT_EQ(last_delay, 11'000'000);
  EXPECT_FALSE(admission.HoldsOwnership());

  clock->clock.set_ns(16'000'000);
  EXPECT_TRUE(admission.TryBeginCycle(true));
  EXPECT_TRUE(admission.HoldsOwnership());
}

TEST(EditorSerialFrameAdmissionTest, CompleteIgnoresZeroInflightAndMismatchedIds) {
  EditorSerialFrameAdmission admission;
  ASSERT_TRUE(admission.TryBeginCycle(false));
  EXPECT_FALSE(admission.CompleteIfMatches(1));
  EXPECT_TRUE(admission.HoldsOwnership());
  admission.NoteScheduledRequest(9);
  EXPECT_FALSE(admission.CompleteIfMatches(8));
  EXPECT_TRUE(admission.HoldsOwnership());
  EXPECT_TRUE(admission.CompleteIfMatches(9));
  EXPECT_FALSE(admission.HoldsOwnership());
}

TEST(EditorSerialFrameAdmissionTest, AbortAllowsImmediateRetryWithoutBudget) {
  EditorSerialFrameAdmission admission;
  auto                       clock = std::make_shared<ManualEditorClock>();
  clock->clock.set_ns(0);
  admission.SetClock(clock);
  ASSERT_TRUE(admission.TryBeginCycle(true));
  admission.AbortCycle();
  EXPECT_FALSE(admission.HoldsOwnership());
  clock->clock.set_ns(1'000'000);
  EXPECT_TRUE(admission.TryBeginCycle(true));
}

TEST(EditorSerialFrameAdmissionTest, AbandonedCycleAllowsImmediateInteractiveRetry) {
  EditorSerialFrameAdmission admission;
  auto                       clock = std::make_shared<ManualEditorClock>();
  clock->clock.set_ns(0);
  admission.SetClock(clock);
  ASSERT_TRUE(admission.TryBeginCycle(true));
  admission.NoteScheduledRequest(3);
  clock->clock.set_ns(5'000'000);
  ASSERT_TRUE(admission.CompleteIfMatches(3, false));
  EXPECT_FALSE(admission.HoldsOwnership());
  EXPECT_TRUE(admission.TryBeginCycle(true));
}

TEST(EditorSerialFrameAdmissionTest, NonInteractiveCycleBypassesRemainingInteractiveWait) {
  EditorSerialFrameAdmission admission;
  auto                       clock = std::make_shared<ManualEditorClock>();
  clock->clock.set_ns(0);
  admission.SetClock(clock);
  ASSERT_TRUE(admission.TryBeginCycle(true));
  admission.NoteScheduledRequest(2);
  clock->clock.set_ns(5'000'000);
  ASSERT_TRUE(admission.CompleteIfMatches(2));
  EXPECT_TRUE(admission.TryBeginCycle(false));
}

}  // namespace
}  // namespace alcedo
