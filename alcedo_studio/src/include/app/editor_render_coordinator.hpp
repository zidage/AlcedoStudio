//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"

namespace alcedo {

/// Small scheduler seam used by production and deterministic coordinator tests.
class IEditorPipelineSchedulerPort {
 public:
  virtual ~IEditorPipelineSchedulerPort()                                    = default;
  /// Returns a scheduler-side job id, or 0 on immediate failure.
  virtual auto Schedule(const EditorRenderRequest& request) -> std::uint64_t = 0;
  virtual void Cancel(std::uint64_t scheduler_job_id)                        = 0;
  virtual void WaitForSessionIdle(std::uint64_t /*session_generation*/) {}
};

/// Application-layer owner of the editor render request queue.
///
/// This is the only production component allowed to call the editor pipeline
/// scheduler. Session service, adjustment models, and viewport controllers submit
/// EditorRenderIntent values here and never receive a PipelineScheduler pointer.
class EditorRenderCoordinator final : public IEditorRenderSubmitPort {
 public:
  using ResultObserver = std::function<void(const EditorRenderResult&)>;

  explicit EditorRenderCoordinator(std::shared_ptr<IEditorPipelineSchedulerPort> scheduler);
  ~EditorRenderCoordinator() override = default;

  void SetResultObserver(ResultObserver observer);

  /// Active image-load request. Older pending intents for other loads are removed.
  void SetActiveImageLoadRequest(std::uint64_t image_load_request_id) override;

  [[nodiscard]] auto image_load_request_id() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return active_image_load_request_id_;
  }

  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override;

  /// Cancel every pending/in-flight request for an image-load request (image switch).
  void CancelSession(std::uint64_t image_load_request_id) override;
  void CancelSessionAndWait(std::uint64_t image_load_request_id) override;

  /// Cancel one request by id (token or explicit). Starts the next runnable request.
  auto CancelRequest(std::uint64_t request_id) -> bool;

  /// Mark the blocking scheduler call complete and start the next request.
  void NotifySchedulerCompleted(std::uint64_t request_id, bool success, std::string message = {});

  /// Process the pending queue: schedule at most one job when idle.
  void Pump();

  [[nodiscard]] auto pending_count() const -> std::size_t {
    std::scoped_lock lock(mutex_);
    return pending_.size();
  }
  [[nodiscard]] auto has_inflight() const -> bool {
    std::scoped_lock lock(mutex_);
    return inflight_.has_value();
  }
  [[nodiscard]] auto last_scheduled_request_id() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return last_scheduled_request_id_;
  }
  [[nodiscard]] auto results() const -> std::vector<EditorRenderResult> {
    std::scoped_lock lock(mutex_);
    return results_;
  }
  /// Aggregate busy/reason/rejection summary for QML without exposing pipeline tasks.
  [[nodiscard]] auto diagnostics() const -> EditorRenderCoordinatorDiagnostics override {
    std::scoped_lock                   lock(mutex_);
    EditorRenderCoordinatorDiagnostics diag;
    diag.has_inflight  = inflight_.has_value();
    diag.pending_count = pending_.size();
    diag.inflight_reason =
        inflight_ ? std::make_optional(inflight_->request.intent.reason) : std::nullopt;
    diag.replaced_count               = replaced_count_;
    diag.cancelled_count              = cancelled_count_;
    diag.last_error                   = last_error_;
    diag.image_load_request_id        = active_image_load_request_id_;
    diag.last_rejection_reason        = last_rejection_reason_;
    diag.last_rejected_render_reason  = last_rejected_render_reason_;
    diag.last_ready_frame_role    = last_ready_frame_role_;
    diag.last_ready_render_reason = last_ready_render_reason_;
    diag.accepted_count               = accepted_count_;
    diag.failed_count                 = failed_count_;
    diag.ready_count                  = ready_count_;
    return diag;
  }

 private:
  struct PendingEntry {
    EditorRenderRequest request;
    std::uint64_t       scheduler_job_id = 0;
  };

  auto AcceptOrReject(const EditorRenderIntent& intent, std::string* message) const -> bool;
  void ReplacePendingWithKey(const std::string& key, std::uint64_t except_request_id);
  /// Cancel pending/in-flight work whose image_load_request_id does not match
  /// active_image_load_request_id_. Returns the running scheduler job to stop.
  auto CancelObsoleteForImageLoadMismatch() -> std::uint64_t;
  [[nodiscard]] auto IsObsolete(const EditorRenderIntent& intent) const -> bool;
  void               Emit(EditorRenderResult result);
  void               DeliverPendingResults();
  void               ScheduleNext();
  [[nodiscard]] static auto PriorityRank(EditorRenderPriority priority) -> int;
  [[nodiscard]] static auto SelectNextIndex(const std::deque<PendingEntry>& pending) -> std::size_t;

  std::shared_ptr<IEditorPipelineSchedulerPort> scheduler_;
  ResultObserver                                observer_;
  std::uint64_t                                 active_image_load_request_id_ = 0;
  std::uint64_t                                 next_request_id_           = 1;
  std::uint64_t                                 last_scheduled_request_id_ = 0;
  std::deque<PendingEntry>                      pending_;
  std::optional<PendingEntry>                   inflight_;
  std::vector<EditorRenderResult>               results_;
  std::vector<EditorRenderResult>               pending_delivery_;
  std::unordered_set<std::uint64_t>             terminal_request_ids_;
  // Diagnostics for the QML spinner/progress/error surface.
  std::size_t                                   replaced_count_  = 0;
  std::size_t                                   cancelled_count_ = 0;
  std::size_t                                   accepted_count_  = 0;
  std::size_t                                   failed_count_    = 0;
  std::size_t                                   ready_count_     = 0;
  std::string                                   last_error_;
  std::string                                   last_rejection_reason_;
  std::optional<EditorRenderReason>             last_rejected_render_reason_;
  std::optional<FrameRole>                      last_ready_frame_role_;
  std::optional<EditorRenderReason>             last_ready_render_reason_;
  mutable std::mutex                            mutex_;
  bool                                          delivery_in_progress_ = false;
};

}  // namespace alcedo
