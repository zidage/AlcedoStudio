//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace alcedo::ui {

/// Kind of a search request. A request replaces the pending request of its own kind only, so
/// a dialog preview never drops a pending apply and the reverse.
enum class SearchRequestKind : std::uint8_t {
  kPreview = 0,  ///< Search dialog page: typing preview, paging, and explicit submit.
  kApply   = 1,  ///< Thumbnail grid page and stats for the applied search.
};

/// One worker thread that runs search requests in submit order with latest-wins coalescing.
///
/// Why the generation exists: the search dialog starts a preview request after each 140 ms
/// typing pause, and one request takes 50–60 ms (preview) to 110–135 ms (apply) in the debug
/// build on a 1000-file library. A new request can therefore arrive while an older one of the
/// same kind waits in the queue or runs on the worker:
///
/// - A pending request is removed when a newer request of its kind arrives, so it never runs.
/// - The running request cannot stop in the middle of its SQL. Its result is posted to the UI
///   thread after the newer request was submitted; the owner calls IsCurrent before it applies
///   the result and drops it when a newer request (or Invalidate) has replaced it.
///
/// Thread use: Submit, Invalidate, and IsCurrent may be called from any thread; the owner
/// calls them from the UI thread. Jobs run on the worker thread only and must not touch UI
/// objects; they post their result back to the owner's thread.
class SearchRequestWorker {
 public:
  /// Receives the generation that Submit returned for this request. Must not throw: it runs
  /// on the worker thread, and the job reports its own failures in its result.
  using Job = std::function<void(std::uint64_t generation)>;

  SearchRequestWorker();
  /// Drops the pending requests, waits for the running job to return, and joins the thread.
  ~SearchRequestWorker();

  SearchRequestWorker(const SearchRequestWorker&)            = delete;
  SearchRequestWorker& operator=(const SearchRequestWorker&) = delete;

  /// Queue @p job as the newest request of @p kind. A pending request of the same kind is
  /// removed without running. Returns the request generation (unique across kinds, > 0).
  auto                 Submit(SearchRequestKind kind, Job job) -> std::uint64_t;
  /// Remove the pending request of @p kind and mark the running one stale, so no request of
  /// this kind submitted before the call reports IsCurrent afterwards.
  void                 Invalidate(SearchRequestKind kind);
  /// True when @p generation is the newest request of @p kind and was not invalidated.
  [[nodiscard]] auto   IsCurrent(SearchRequestKind kind, std::uint64_t generation) const -> bool;

 private:
  struct PendingRequest {
    SearchRequestKind kind_       = SearchRequestKind::kPreview;
    std::uint64_t     generation_ = 0;
    Job               job_{};
  };

  void                         RemovePendingLocked(SearchRequestKind kind);
  void                         Run();

  mutable std::mutex           mutex_;
  std::condition_variable      wake_;
  std::deque<PendingRequest>   pending_{};
  std::array<std::uint64_t, 2> newest_generation_{};
  std::uint64_t                last_generation_ = 0;
  bool                         stopping_        = false;
  std::thread                  thread_;  // Last member: starts after the state above exists.
};

}  // namespace alcedo::ui
