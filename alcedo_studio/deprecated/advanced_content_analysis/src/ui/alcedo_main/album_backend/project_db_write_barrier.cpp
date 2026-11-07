//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/project_db_write_barrier.hpp"

#include <utility>

namespace alcedo::ui {

void AnalysisResultWriteQueue::Submit(std::function<void()> op) {
  if (barrier_.IsHeld()) {
    pending_.push_back(std::move(op));
  } else {
    op();
  }
}

void AnalysisResultWriteQueue::Drain() {
  // Move pending out before iterating so a re-entrant Submit (which, during
  // Drain, sees the barrier released and runs immediately) can't reallocate the
  // vector mid-iteration. During Drain the barrier is at zero, so a re-entrant
  // Submit runs immediately rather than appending — this just guards against a
  // future caller that drains while held.
  auto local = std::move(pending_);
  pending_.clear();
  for (auto& op : local) {
    op();
  }
  // Fire drain-complete callbacks in FIFO order. Each was registered by an
  // analysis job's Finish to defer FinishTask until its writes committed.
  auto completes = std::move(drain_completes_);
  drain_completes_.clear();
  for (auto& cb : completes) {
    cb();
  }
}

void AnalysisResultWriteQueue::SetOnDrainComplete(std::function<void()> cb) {
  drain_completes_.push_back(std::move(cb));
}

}  // namespace alcedo::ui