//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <vector>

namespace alcedo::ui {

/// Phase 2 (Step 4) — a ref-counted "project DB write barrier" held by export
/// (and other DB-stabilizing reads) so background image-analysis result commits
/// defer until the barrier releases. While the barrier is held, finished
/// analysis results queue in memory instead of writing to the project DB; when
/// the count drops to zero the `on_release_` callback fires, which drains the
/// `AnalysisResultWriteQueue` (see below).
///
/// Threading: all Acquire/Release calls happen on the UI/module-host thread
/// (export start/finish and the analysis Finish callback are all marshalled
/// there via `QMetaObject::invokeMethod QueuedConnection`), so the count needs
/// no mutex. `Release()` when the count is already zero is a defensive no-op.
class ProjectDbWriteBarrier {
 public:
  void Acquire() { ++count_; }
  void Release() {
    if (count_ > 0) {
      --count_;
      if (count_ == 0 && on_release_) {
        on_release_();
      }
    }
  }
  bool IsHeld() const { return count_ > 0; }
  int  Count() const { return count_; }
  void SetOnRelease(std::function<void()> cb) { on_release_ = std::move(cb); }

 private:
  int                   count_ = 0;
  std::function<void()> on_release_;
};

/// Phase 2 (Step 4) — a small deferred-write queue used by
/// `AlbumImageAnalysisSink`. `Submit(op)` runs `op` immediately when the barrier
/// is not held, or queues it for `Drain()` when it is. `Drain()` runs every
/// pending op in submission order, then fires every registered drain-complete
/// callback (FIFO) — used to defer a background task's `FinishTask` until its
/// queued writes actually commit, so the task's interaction locks stay held
/// across the export-barrier gap and a user can't edit a just-analyzed image's
/// rating in the window before the queued AI rating overwrites it.
class AnalysisResultWriteQueue {
 public:
  explicit AnalysisResultWriteQueue(ProjectDbWriteBarrier& barrier) : barrier_(barrier) {}

  // If the barrier is held, queue `op` for `Drain()`; otherwise run it now.
  void Submit(std::function<void()> op);
  // Run every pending op in order, then fire every drain-complete callback in
  // registration order. Clears both vectors.
  void Drain();
  bool IsPending() const { return !pending_.empty(); }
  // Append a callback fired at the end of the next `Drain()`. Used by
  // `ImageAnalysisController::Finish` to defer `FinishTask` until writes commit.
  void SetOnDrainComplete(std::function<void()> cb);

 private:
  ProjectDbWriteBarrier&             barrier_;
  std::vector<std::function<void()>> pending_;
  std::vector<std::function<void()>> drain_completes_;
};

}  // namespace alcedo::ui