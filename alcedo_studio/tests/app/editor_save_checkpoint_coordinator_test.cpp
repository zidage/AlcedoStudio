//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_save_checkpoint_coordinator.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <exception>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

namespace alcedo {
namespace {

/// Verify that the project-owned save lock admits exactly one concurrent owner.
TEST(EditorSaveCheckpointCoordinatorTest, TwoThreadsCannotOwnTheGlobalSaveLockAtTheSameTime) {
  EditorSaveCheckpointCoordinator coordinator;
  std::barrier                    start(3);
  std::binary_semaphore           acquired(0);
  std::binary_semaphore           release_owner(0);
  std::atomic<int>                active_owners{0};
  std::atomic<int>                maximum_owners{0};
  std::atomic<bool>               every_waiter_owned{true};

  const auto wait_for_lock = [&](sl_element_id_t element_id) {
    start.arrive_and_wait();
    auto lock = coordinator.AcquireBlocking(element_id);
    if (!lock.owns_lock()) {
      every_waiter_owned.store(false);
    }
    const auto owners  = active_owners.fetch_add(1) + 1;
    auto       maximum = maximum_owners.load();
    while (owners > maximum && !maximum_owners.compare_exchange_weak(maximum, owners)) {
    }
    acquired.release();
    release_owner.acquire();
    active_owners.fetch_sub(1);
  };

  std::thread first(wait_for_lock, 1);
  std::thread second(wait_for_lock, 2);
  start.arrive_and_wait();

  acquired.acquire();
  EXPECT_EQ(active_owners.load(), 1);
  release_owner.release();
  acquired.acquire();
  EXPECT_EQ(active_owners.load(), 1);
  release_owner.release();

  first.join();
  second.join();
  EXPECT_TRUE(every_waiter_owned.load());
  EXPECT_EQ(maximum_owners.load(), 1);
  EXPECT_FALSE(coordinator.is_saving());
  EXPECT_EQ(coordinator.active_element_id(), 0u);
}

/// Exercise every release path required by the move-only ownership model.
TEST(EditorSaveCheckpointCoordinatorTest,
     SaveCheckpointLockReleasesAfterSuccessFailureExceptionAndMove) {
  EditorSaveCheckpointCoordinator coordinator;

  // Normal destruction releases ownership.
  {
    auto lock = coordinator.TryAcquire(11);
    ASSERT_TRUE(lock.owns_lock());
    EXPECT_TRUE(coordinator.is_saving());
    EXPECT_EQ(coordinator.active_element_id(), 11u);
  }
  EXPECT_FALSE(coordinator.is_saving());
  EXPECT_EQ(coordinator.active_element_id(), 0u);

  // Explicit Release before scope exit.
  {
    auto lock = coordinator.TryAcquire(12);
    ASSERT_TRUE(lock.owns_lock());
    lock.Release();
    EXPECT_FALSE(lock.owns_lock());
    EXPECT_FALSE(coordinator.is_saving());
  }

  // Failure-style early return path (scope exit without explicit Release).
  {
    auto lock = coordinator.TryAcquire(13);
    ASSERT_TRUE(lock.owns_lock());
    if (!lock.owns_lock()) {
      FAIL() << "unexpected non-owning lock";
    }
    // Simulate failure return: leave scope without further work.
  }
  EXPECT_FALSE(coordinator.is_saving());

  // Exception path still releases via destructor.
  try {
    auto lock = coordinator.TryAcquire(14);
    ASSERT_TRUE(lock.owns_lock());
    throw std::runtime_error("forced failure");
  } catch (const std::runtime_error&) {
  }
  EXPECT_FALSE(coordinator.is_saving());

  // Move construction transfers ownership; moved-from owns nothing.
  {
    auto first = coordinator.TryAcquire(15);
    ASSERT_TRUE(first.owns_lock());
    auto second = std::move(first);
    EXPECT_FALSE(first.owns_lock());
    EXPECT_TRUE(second.owns_lock());
    EXPECT_TRUE(coordinator.is_saving());
    EXPECT_EQ(coordinator.active_element_id(), 15u);
  }
  EXPECT_FALSE(coordinator.is_saving());

  // Move assignment releases the previous owner and takes the new one.
  {
    auto first  = coordinator.TryAcquire(16);
    auto second = coordinator.TryAcquire(17);
    EXPECT_TRUE(first.owns_lock());
    EXPECT_FALSE(second.owns_lock());  // contention while first owns
    second = std::move(first);
    EXPECT_FALSE(first.owns_lock());
    EXPECT_TRUE(second.owns_lock());
    EXPECT_EQ(coordinator.active_element_id(), 16u);
  }
  EXPECT_FALSE(coordinator.is_saving());
}

/// A waiter blocked on AcquireBlocking must exit when the project shuts down.
TEST(EditorSaveCheckpointCoordinatorTest, WaitingSaveStopsCleanlyWhenProjectShutsDown) {
  EditorSaveCheckpointCoordinator coordinator;
  std::binary_semaphore           owner_ready(0);
  std::binary_semaphore           owner_release(0);
  std::binary_semaphore           waiter_entered(0);
  std::atomic<bool>               owner_acquired{false};
  std::atomic<bool>               waiter_owns{true};

  std::thread owner([&] {
    auto lock = coordinator.TryAcquire(21);
    owner_acquired.store(lock.owns_lock(), std::memory_order_release);
    owner_ready.release();
    // Hold ownership until the main thread releases the owner after Shutdown.
    owner_release.acquire();
  });

  owner_ready.acquire();
  ASSERT_TRUE(owner_acquired.load(std::memory_order_acquire));
  EXPECT_TRUE(coordinator.is_saving());

  std::thread waiter([&] {
    waiter_entered.release();
    auto lock = coordinator.AcquireBlocking(22);
    waiter_owns.store(lock.owns_lock(), std::memory_order_release);
  });

  waiter_entered.acquire();
  // No sleep: Shutdown wakes the condition_variable waiter immediately.
  coordinator.Shutdown();
  waiter.join();
  EXPECT_FALSE(waiter_owns.load(std::memory_order_acquire));
  EXPECT_TRUE(coordinator.is_shutdown());
  EXPECT_FALSE(waiter.joinable());

  // Owner still holds the lock until its scope ends; Shutdown does not steal it.
  owner_release.release();
  owner.join();
  EXPECT_FALSE(owner.joinable());
  EXPECT_FALSE(coordinator.is_saving());

  // New acquires fail after shutdown.
  auto post = coordinator.TryAcquire(23);
  EXPECT_FALSE(post.owns_lock());
  auto blocked = coordinator.AcquireBlocking(24);
  EXPECT_FALSE(blocked.owns_lock());
}

}  // namespace
}  // namespace alcedo
