//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
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
  virtual ~IEditorPipelineSchedulerPort() = default;
  /// Returns a scheduler-side job id, or 0 on immediate failure.
  virtual auto Schedule(const EditorRenderRequest& request) -> std::uint64_t = 0;
  virtual void Cancel(std::uint64_t scheduler_job_id) = 0;
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

  void SetResultObserver(ResultObserver observer);

  /// Active image/session/view generations. Older intents are rejected on submit;
  /// advancing also cancels obsolete pending and in-flight work (Phase 5A-Fix).
  void SetActiveGenerations(std::uint64_t session_generation, std::uint64_t render_generation,
                            std::uint64_t view_generation) override;

  [[nodiscard]] auto session_generation() const -> std::uint64_t { return session_generation_; }
  [[nodiscard]] auto render_generation() const -> std::uint64_t { return render_generation_; }
  [[nodiscard]] auto view_generation() const -> std::uint64_t { return view_generation_; }

  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override;

  /// Cancel every pending/in-flight request for a session generation (image switch).
  void CancelSession(std::uint64_t session_generation) override;

  /// Cancel one request by id (token or explicit). Starts the next runnable request.
  auto CancelRequest(std::uint64_t request_id) -> bool;

  /// Drive completion from the scheduler port (tests and future production glue).
  void NotifySchedulerCompleted(std::uint64_t request_id, bool success, std::string message = {});
  void NotifyFrameSubmitted(std::uint64_t request_id);
  void NotifyFramePresented(std::uint64_t request_id);

  /// Process the pending queue: schedule at most one job when idle.
  void Pump();

  [[nodiscard]] auto pending_count() const -> std::size_t { return pending_.size(); }
  [[nodiscard]] auto has_inflight() const -> bool { return inflight_.has_value(); }
  [[nodiscard]] auto last_scheduled_request_id() const -> std::uint64_t {
    return last_scheduled_request_id_;
  }
  [[nodiscard]] auto results() const -> const std::vector<EditorRenderResult>& {
    return results_;
  }

 private:
  struct PendingEntry {
    EditorRenderRequest request;
    std::uint64_t       scheduler_job_id = 0;
  };

  auto AcceptOrReject(const EditorRenderIntent& intent, std::string* message) const -> bool;
  void ReplacePendingWithKey(const std::string& key, std::uint64_t except_request_id);
  void CancelObsoleteForActiveGenerations();
  [[nodiscard]] auto IsObsolete(const EditorRenderIntent& intent) const -> bool;
  void Emit(EditorRenderResult result);
  void ScheduleNext();
  [[nodiscard]] auto HasResultKind(std::uint64_t request_id, EditorRenderResultKind kind) const
      -> bool;
  [[nodiscard]] static auto PriorityRank(EditorRenderPriority priority) -> int;
  [[nodiscard]] static auto SelectNextIndex(const std::deque<PendingEntry>& pending) -> std::size_t;

  std::shared_ptr<IEditorPipelineSchedulerPort> scheduler_;
  ResultObserver                                observer_;
  std::uint64_t                                 session_generation_ = 0;
  std::uint64_t                                 render_generation_  = 0;
  std::uint64_t                                 view_generation_    = 0;
  std::uint64_t                                 next_request_id_    = 1;
  std::uint64_t                                 last_scheduled_request_id_ = 0;
  std::deque<PendingEntry>                      pending_;
  std::optional<PendingEntry>                   inflight_;
  std::vector<EditorRenderResult>               results_;
  std::unordered_set<std::uint64_t>             terminal_request_ids_;
};

}  // namespace alcedo
