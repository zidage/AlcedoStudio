//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SearchRequestWorker (library_search_and_project_size_plan.md, Phase S5 step 2): one worker
// thread, latest-wins coalescing for each request kind, and the generation that marks a
// running request stale.

#include "ui/alcedo_main/album_backend/search_request_worker.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alcedo::ui {
namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);

/// Records the jobs that ran and lets the test hold the worker inside one job.
class JobLog {
 public:
  /// A job that records @p name and returns.
  auto Record(std::string name) -> SearchRequestWorker::Job {
    return [this, name = std::move(name)](std::uint64_t generation) {
      std::lock_guard lock(mutex_);
      ran_.push_back(name);
      generations_.push_back(generation);
      changed_.notify_all();
    };
  }

  /// A job that records @p name, reports that it started, and waits for Release.
  auto RecordAndHold(std::string name) -> SearchRequestWorker::Job {
    return [this, name = std::move(name)](std::uint64_t generation) {
      std::unique_lock lock(mutex_);
      ran_.push_back(name);
      generations_.push_back(generation);
      held_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this]() { return released_; });
    };
  }

  auto WaitUntilHeld() -> bool {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWaitTimeout, [this]() { return held_; });
  }

  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

  auto WaitForRunCount(std::size_t count) -> bool {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWaitTimeout, [this, count]() { return ran_.size() >= count; });
  }

  auto Ran() -> std::vector<std::string> {
    std::lock_guard lock(mutex_);
    return ran_;
  }

  auto Generations() -> std::vector<std::uint64_t> {
    std::lock_guard lock(mutex_);
    return generations_;
  }

 private:
  std::mutex                 mutex_;
  std::condition_variable    changed_;
  std::vector<std::string>   ran_;
  std::vector<std::uint64_t> generations_;
  bool                       held_     = false;
  bool                       released_ = false;
};

TEST(SearchRequestWorkerTest, PendingRequestIsReplacedByTheNewerRequestOfItsKind) {
  JobLog              log;
  SearchRequestWorker worker;
  worker.Submit(SearchRequestKind::kPreview, log.RecordAndHold("a"));
  ASSERT_TRUE(log.WaitUntilHeld());

  // Nineteen more keystrokes while "a" runs: only the newest one runs afterwards.
  std::uint64_t last_generation = 0;
  for (int i = 0; i < 19; ++i) {
    last_generation =
        worker.Submit(SearchRequestKind::kPreview, log.Record("key" + std::to_string(i)));
  }
  log.Release();
  ASSERT_TRUE(log.WaitForRunCount(2));
  // Give a wrongly kept request time to run before the final check.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(log.Ran(), (std::vector<std::string>{"a", "key18"}));
  EXPECT_EQ(log.Generations().back(), last_generation);
  EXPECT_TRUE(worker.IsCurrent(SearchRequestKind::kPreview, last_generation));
}

TEST(SearchRequestWorkerTest, RequestOfAnotherKindKeepsItsPendingRequest) {
  JobLog              log;
  SearchRequestWorker worker;
  worker.Submit(SearchRequestKind::kPreview, log.RecordAndHold("preview-running"));
  ASSERT_TRUE(log.WaitUntilHeld());

  const auto apply   = worker.Submit(SearchRequestKind::kApply, log.Record("apply"));
  const auto preview = worker.Submit(SearchRequestKind::kPreview, log.Record("preview-newest"));
  log.Release();
  ASSERT_TRUE(log.WaitForRunCount(3));

  // Submit order is kept across kinds, and neither kind removed the other.
  EXPECT_EQ(log.Ran(), (std::vector<std::string>{"preview-running", "apply", "preview-newest"}));
  EXPECT_TRUE(worker.IsCurrent(SearchRequestKind::kApply, apply));
  EXPECT_TRUE(worker.IsCurrent(SearchRequestKind::kPreview, preview));
  EXPECT_NE(apply, preview);
}

TEST(SearchRequestWorkerTest, RunningRequestBecomesStaleWhenANewerRequestArrives) {
  JobLog              log;
  SearchRequestWorker worker;
  const auto running = worker.Submit(SearchRequestKind::kPreview, log.RecordAndHold("old"));
  ASSERT_TRUE(log.WaitUntilHeld());
  EXPECT_TRUE(worker.IsCurrent(SearchRequestKind::kPreview, running));

  const auto newest = worker.Submit(SearchRequestKind::kPreview, log.Record("new"));
  // The owner checks this before it applies the old result.
  EXPECT_FALSE(worker.IsCurrent(SearchRequestKind::kPreview, running));
  EXPECT_TRUE(worker.IsCurrent(SearchRequestKind::kPreview, newest));

  log.Release();
  ASSERT_TRUE(log.WaitForRunCount(2));
  EXPECT_EQ(log.Ran(), (std::vector<std::string>{"old", "new"}));
}

TEST(SearchRequestWorkerTest, InvalidateDropsThePendingRequestAndMarksTheRunningOneStale) {
  JobLog              log;
  SearchRequestWorker worker;
  const auto running = worker.Submit(SearchRequestKind::kApply, log.RecordAndHold("running"));
  ASSERT_TRUE(log.WaitUntilHeld());
  const auto pending = worker.Submit(SearchRequestKind::kApply, log.Record("pending"));
  const auto preview = worker.Submit(SearchRequestKind::kPreview, log.Record("preview"));

  worker.Invalidate(SearchRequestKind::kApply);
  EXPECT_FALSE(worker.IsCurrent(SearchRequestKind::kApply, running));
  EXPECT_FALSE(worker.IsCurrent(SearchRequestKind::kApply, pending));
  EXPECT_TRUE(worker.IsCurrent(SearchRequestKind::kPreview, preview));

  log.Release();
  ASSERT_TRUE(log.WaitForRunCount(2));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(log.Ran(), (std::vector<std::string>{"running", "preview"}));
  EXPECT_FALSE(worker.IsCurrent(SearchRequestKind::kApply, 0));
}

TEST(SearchRequestWorkerTest, DestructorDropsPendingRequestsAndWaitsForTheRunningJob) {
  JobLog log;
  auto   worker = std::make_unique<SearchRequestWorker>();
  worker->Submit(SearchRequestKind::kPreview, log.RecordAndHold("running"));
  ASSERT_TRUE(log.WaitUntilHeld());
  worker->Submit(SearchRequestKind::kApply, log.Record("pending"));

  std::thread release_later([&log]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.Release();
  });
  worker.reset();  // Returns only after "running" returned.
  release_later.join();

  EXPECT_EQ(log.Ran(), (std::vector<std::string>{"running"}));
}

}  // namespace
}  // namespace alcedo::ui
