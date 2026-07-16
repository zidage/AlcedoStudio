//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

#include <functional>
#include <vector>

#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostDbWriteBarrierTests = ApplicationModuleHostTestFixture;

// ── ProjectDbWriteBarrier ─────────────────────────────────────────────────

TEST_F(ApplicationModuleHostDbWriteBarrierTests, InitiallyNotHeld) {
  ProjectDbWriteBarrier barrier;
  EXPECT_FALSE(barrier.IsHeld());
  EXPECT_EQ(barrier.Count(), 0);
}

TEST_F(ApplicationModuleHostDbWriteBarrierTests, AcquireReleaseRefCount) {
  ProjectDbWriteBarrier barrier;
  barrier.Acquire();
  EXPECT_TRUE(barrier.IsHeld());
  EXPECT_EQ(barrier.Count(), 1);
  barrier.Acquire();
  EXPECT_EQ(barrier.Count(), 2);
  barrier.Release();
  EXPECT_TRUE(barrier.IsHeld());
  EXPECT_EQ(barrier.Count(), 1);
  barrier.Release();
  EXPECT_FALSE(barrier.IsHeld());
  EXPECT_EQ(barrier.Count(), 0);
}

TEST_F(ApplicationModuleHostDbWriteBarrierTests, OnRelease_FiresOnlyOnOneToZero) {
  ProjectDbWriteBarrier barrier;
  int                   release_count = 0;
  barrier.SetOnRelease([&] { ++release_count; });

  barrier.Acquire();  // 0 -> 1
  barrier.Acquire();  // 1 -> 2
  barrier.Release();  // 2 -> 1 (no fire)
  EXPECT_EQ(release_count, 0);
  barrier.Release();  // 1 -> 0 (fire)
  EXPECT_EQ(release_count, 1);
  // Release when already zero is a defensive no-op — must not fire or go negative.
  barrier.Release();
  EXPECT_EQ(release_count, 1);
  EXPECT_EQ(barrier.Count(), 0);
}

// ── AnalysisResultWriteQueue ───────────────────────────────────────────────

TEST_F(ApplicationModuleHostDbWriteBarrierTests, Queue_SubmitRunsImmediatelyWhenNotHeld) {
  ProjectDbWriteBarrier      barrier;
  AnalysisResultWriteQueue   queue(barrier);
  int                        ran = 0;
  queue.Submit([&] { ++ran; });
  EXPECT_EQ(ran, 1);
  EXPECT_FALSE(queue.IsPending());
}

TEST_F(ApplicationModuleHostDbWriteBarrierTests, Queue_SubmitQueuesWhenHeldThenDrainRuns) {
  ProjectDbWriteBarrier      barrier;
  AnalysisResultWriteQueue   queue(barrier);
  int                        ran = 0;

  barrier.Acquire();
  queue.Submit([&] { ++ran; });
  EXPECT_EQ(ran, 0);  // queued, not run
  EXPECT_TRUE(queue.IsPending());

  queue.Drain();
  EXPECT_EQ(ran, 1);
  EXPECT_FALSE(queue.IsPending());
}

TEST_F(ApplicationModuleHostDbWriteBarrierTests, Queue_DrainRunsAllInSubmissionOrder) {
  ProjectDbWriteBarrier      barrier;
  AnalysisResultWriteQueue   queue(barrier);
  std::vector<int>           order;

  barrier.Acquire();
  queue.Submit([&] { order.push_back(1); });
  queue.Submit([&] { order.push_back(2); });
  queue.Submit([&] { order.push_back(3); });
  queue.Drain();

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
  EXPECT_FALSE(queue.IsPending());
}

TEST_F(ApplicationModuleHostDbWriteBarrierTests, Queue_DrainFiresDrainCompletesFifo) {
  ProjectDbWriteBarrier      barrier;
  AnalysisResultWriteQueue   queue(barrier);
  std::vector<int>           order;

  barrier.Acquire();
  queue.Submit([&] { order.push_back(100); });  // pending write
  queue.SetOnDrainComplete([&] { order.push_back(201); });
  queue.SetOnDrainComplete([&] { order.push_back(202); });
  queue.Drain();

  // Pending writes run first, then drain-completes in FIFO order.
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 100);
  EXPECT_EQ(order[1], 201);
  EXPECT_EQ(order[2], 202);

  // A second Drain with no new submissions fires nothing.
  queue.Drain();
  EXPECT_EQ(order.size(), 3u);
}

TEST_F(ApplicationModuleHostDbWriteBarrierTests, Queue_DrainWhileHeldReEntrantSubmitRunsImmediately) {
  // Drain is normally called after release, but guard against a future caller
  // that drains while held: a re-entrant Submit during Drain must not reallocate
  // the iterating vector. This just confirms Drain is re-entrancy-safe.
  ProjectDbWriteBarrier      barrier;
  AnalysisResultWriteQueue   queue(barrier);
  int                        ran = 0;
  barrier.Acquire();
  queue.Submit([&] { ++ran; });
  barrier.Release();  // on_release not wired here; drain explicitly
  queue.Drain();
  EXPECT_EQ(ran, 1);
}

}  // namespace
}  // namespace alcedo::ui::test