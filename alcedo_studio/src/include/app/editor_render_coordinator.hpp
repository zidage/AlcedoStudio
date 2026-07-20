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

/// Fakeable pipeline-scheduler seam. Production (Phase 5B+) enqueues real
/// PipelineScheduler tasks; Phase 5A tests prove policy without GPU or Qt.
class IEditorPipelineSchedulerPort {
 public:
  virtual ~IEditorPipelineSchedulerPort()                                    = default;
  /// Returns a scheduler-side job id, or 0 on immediate failure.
  virtual auto Schedule(const EditorRenderRequest& request) -> std::uint64_t = 0;
  virtual void Cancel(std::uint64_t scheduler_job_id)                        = 0;
  virtual void WaitForSessionIdle(std::uint64_t /*session_generation*/) {}
};

/// Application-layer editor render coordinator (Phase 5A).
///
/// This is the only production component allowed to call the editor pipeline
/// scheduler. Session service, adjustment models, and viewport controllers submit
/// EditorRenderIntent values here and never receive a PipelineScheduler pointer.
class EditorRenderCoordinator final : public IEditorRenderSubmitPort {
 public:
  using ResultObserver = std::function<void(const EditorRenderResult&)>;

  explicit EditorRenderCoordinator(std::shared_ptr<IEditorPipelineSchedulerPort> scheduler);
  ~EditorRenderCoordinator() override;

  void SetResultObserver(ResultObserver observer);

  /// Active image/session/view generations. Older pending intents are removed.
  /// Interactive adjustment bursts may preserve an already-running full frame
  /// so FAST_PREVIEW can finish while the newest pending request is coalesced.
  void SetActiveGenerations(std::uint64_t session_generation, std::uint64_t render_generation,
                            std::uint64_t view_generation,
                            EditorRenderSupersessionPolicy policy =
                                EditorRenderSupersessionPolicy::CancelObsolete) override;

  [[nodiscard]] auto session_generation() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return session_generation_;
  }
  [[nodiscard]] auto render_generation() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return render_generation_;
  }
  [[nodiscard]] auto view_generation() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return view_generation_;
  }

  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override;

  /// Cancel every pending/in-flight request for a session generation (image switch).
  void CancelSession(std::uint64_t session_generation) override;
  void CancelSessionAndWait(std::uint64_t session_generation) override;

  /// Cancel one request by id (token or explicit). Starts the next runnable request.
  auto CancelRequest(std::uint64_t request_id) -> bool;

  /// Drive completion from the scheduler port (tests and future production glue).
  void NotifySchedulerCompleted(std::uint64_t request_id, bool success, std::string message = {});
  void NotifyFrameSubmitted(std::uint64_t request_id);
  void NotifyFramePresented(std::uint64_t request_id);

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
  /// Phase 5D/5E: aggregate busy/reason/rejection summary for QML. Never
  /// exposes pipeline task objects.
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
    diag.session_generation           = session_generation_;
    diag.render_generation            = render_generation_;
    diag.view_generation              = view_generation_;
    diag.last_rejection_reason        = last_rejection_reason_;
    diag.last_rejected_render_reason  = last_rejected_render_reason_;
    diag.last_submitted_frame_role    = last_submitted_frame_role_;
    diag.last_submitted_render_reason = last_submitted_render_reason_;
    diag.accepted_count               = accepted_count_;
    diag.failed_count                 = failed_count_;
    diag.presented_count              = presented_count_;
    return diag;
  }

 private:
  struct CancellationCallbackGate {
    std::mutex               mutex;
    EditorRenderCoordinator* owner = nullptr;
  };

  struct PendingEntry {
    EditorRenderRequest request;
    std::uint64_t       scheduler_job_id = 0;
  };

  auto AcceptOrReject(const EditorRenderIntent& intent, std::string* message) const -> bool;
  void ReplacePendingWithKey(const std::string& key, std::uint64_t except_request_id);
  /// Cancel obsolete pending/in-flight work under mutex_. Returns a scheduler
  /// job id that must be cancelled only after releasing mutex_ (token callbacks
  /// re-enter CancelRequest).
  auto CancelObsoleteForActiveGenerations(EditorRenderSupersessionPolicy policy)
      -> std::uint64_t;
  [[nodiscard]] auto IsObsolete(const EditorRenderIntent& intent) const -> bool;
  void               Emit(EditorRenderResult result);
  void               DeliverPendingResults();
  void               ScheduleNext();
  [[nodiscard]] auto HasResultKind(std::uint64_t request_id, EditorRenderResultKind kind) const
      -> bool;
  [[nodiscard]] static auto PriorityRank(EditorRenderPriority priority) -> int;
  [[nodiscard]] static auto SelectNextIndex(const std::deque<PendingEntry>& pending) -> std::size_t;

  std::shared_ptr<IEditorPipelineSchedulerPort> scheduler_;
  ResultObserver                                observer_;
  std::uint64_t                                 session_generation_        = 0;
  std::uint64_t                                 render_generation_         = 0;
  std::uint64_t                                 view_generation_           = 0;
  std::uint64_t                                 next_request_id_           = 1;
  std::uint64_t                                 last_scheduled_request_id_ = 0;
  std::deque<PendingEntry>                      pending_;
  std::optional<PendingEntry>                   inflight_;
  std::vector<EditorRenderResult>               results_;
  std::vector<EditorRenderResult>               pending_delivery_;
  std::unordered_set<std::uint64_t>             terminal_request_ids_;
  std::shared_ptr<CancellationCallbackGate>     cancellation_gate_;
  // Phase 5D/5E diagnostics counters (QML spinner/progress/error surface).
  std::size_t                                   replaced_count_  = 0;
  std::size_t                                   cancelled_count_ = 0;
  std::size_t                                   accepted_count_  = 0;
  std::size_t                                   failed_count_    = 0;
  std::size_t                                   presented_count_ = 0;
  std::string                                   last_error_;
  std::string                                   last_rejection_reason_;
  std::optional<EditorRenderReason>             last_rejected_render_reason_;
  std::optional<FrameRole>                      last_submitted_frame_role_;
  std::optional<EditorRenderReason>             last_submitted_render_reason_;
  mutable std::mutex                            mutex_;
  bool                                          delivery_in_progress_ = false;
};

}  // namespace alcedo
