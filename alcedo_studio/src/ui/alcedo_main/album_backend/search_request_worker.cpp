//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/search_request_worker.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace alcedo::ui {
namespace {

auto KindIndex(SearchRequestKind kind) -> std::size_t { return static_cast<std::size_t>(kind); }

}  // namespace

SearchRequestWorker::SearchRequestWorker() : thread_([this]() { Run(); }) {}

SearchRequestWorker::~SearchRequestWorker() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    pending_.clear();
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

auto SearchRequestWorker::Submit(SearchRequestKind kind, Job job) -> std::uint64_t {
  std::uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    generation                          = ++last_generation_;
    newest_generation_[KindIndex(kind)] = generation;
    RemovePendingLocked(kind);
    if (!stopping_) {
      pending_.push_back(PendingRequest{kind, generation, std::move(job)});
    }
  }
  wake_.notify_one();
  return generation;
}

void SearchRequestWorker::Invalidate(SearchRequestKind kind) {
  std::lock_guard lock(mutex_);
  newest_generation_[KindIndex(kind)] = ++last_generation_;
  RemovePendingLocked(kind);
}

auto SearchRequestWorker::IsCurrent(SearchRequestKind kind, std::uint64_t generation) const
    -> bool {
  std::lock_guard lock(mutex_);
  return generation != 0 && newest_generation_[KindIndex(kind)] == generation;
}

void SearchRequestWorker::RemovePendingLocked(SearchRequestKind kind) {
  pending_.erase(
      std::remove_if(pending_.begin(), pending_.end(),
                     [kind](const PendingRequest& request) { return request.kind_ == kind; }),
      pending_.end());
}

void SearchRequestWorker::Run() {
  for (;;) {
    PendingRequest request;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this]() { return stopping_ || !pending_.empty(); });
      if (stopping_) {
        return;
      }
      request = std::move(pending_.front());
      pending_.pop_front();
    }
    request.job_(request.generation_);
  }
}

}  // namespace alcedo::ui
