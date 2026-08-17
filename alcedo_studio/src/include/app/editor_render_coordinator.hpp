//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
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

/// Forward completion installed by the coordinator at Schedule time.
/// Production adapters invoke this instead of holding a reverse coordinator pointer.
using EditorPipelineScheduleCompletion =
    std::function<void(bool success, std::string message)>;

/// Small scheduler seam used by production and deterministic coordinator tests.
class IEditorPipelineSchedulerPort {
 public:
  virtual ~IEditorPipelineSchedulerPort() = default;
  /// Returns a scheduler-side job id, or 0 on immediate failure.
  /// `on_complete` is the only control-plane path back to the coordinator.
  virtual auto Schedule(const EditorRenderRequest& request,
                        EditorPipelineScheduleCompletion on_complete = {}) -> std::uint64_t = 0;
  virtual void Cancel(std::uint64_t scheduler_job_id) = 0;
  virtual void WaitForSessionIdle(std::uint64_t /*session_epoch*/) {}
  /// Bind stable render inputs for the open/switched image (epoch + identity +
  /// presentation sink id). Production loads image/buffer/pipeline once; fakes no-op.
  virtual void BindSessionContext(std::uint64_t /*epoch*/, sl_element_id_t /*element_id*/,
                                  image_id_t /*image_id*/,
                                  PresentationSinkId /*presentation_sink_id*/ = 0) {}
  /// Drop the bound session render context (close / pre-switch reset).
  virtual void ClearSessionContext() {}
};

/// Application-layer owner of editor render coalesce + single-flight scheduling.
///
/// Pending work is three fixed quality slots (interactive / quality / detail).
/// Same-slot submit overwrites the prior pending entry and emits `Replaced`.
/// `ScheduleNext` selects interactive > quality > detail in constant time.
/// This is the only production component allowed to call the editor pipeline
/// scheduler. Session service, adjustment models, and viewport controllers submit
/// EditorRenderIntent values here and never receive a PipelineScheduler pointer.
class EditorRenderCoordinator final : public IEditorRenderSubmitPort {
 public:
  using ResultObserver = std::function<void(const EditorRenderResult&)>;

  /// Ladder slots derived from `EditorRenderQuality` (not string keys).
  enum class QualitySlot : std::uint8_t {
    Interactive = 0,
    Quality     = 1,
    Detail      = 2,
  };
  static constexpr std::size_t kQualitySlotCount = 3;

  explicit EditorRenderCoordinator(std::shared_ptr<IEditorPipelineSchedulerPort> scheduler);
  ~EditorRenderCoordinator() override = default;

  void SetResultObserver(ResultObserver observer);

  /// Active image-load request. Older pending intents for other loads are removed.
  void SetActiveImageLoadRequest(std::uint64_t image_load_request_id) override;

  /// Forward open/switch session render context bind to the production adapter.
  void BindSessionRenderContext(std::uint64_t epoch, sl_element_id_t element_id,
                                image_id_t image_id,
                                PresentationSinkId presentation_sink_id = 0) override;
  void ClearSessionRenderContext() override;

  /// Replace the pipeline scheduler seam after construction. Production hosts
  /// set the scheduler once via the constructor; focused harnesses may swap in
  /// a recording/completing fake so tests never grow production Dispatch branches.
  void SetPipelineSchedulerPort(std::shared_ptr<IEditorPipelineSchedulerPort> scheduler);

  [[nodiscard]] auto image_load_request_id() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return active_image_load_request_id_;
  }

  auto Submit(const EditorRenderIntent& intent) -> EditorRenderResult override;

  /// Cancel every pending/in-flight request for an image-load request (image switch).
  void CancelSession(std::uint64_t image_load_request_id) override;
  void CancelSession(std::uint64_t image_load_request_id,
                     SessionIdleCallback on_idle) override;
  void CancelSessionAndWait(std::uint64_t image_load_request_id) override;
  /// Queue behind the in-flight frame: drop not-yet-started pending for this
  /// session, then wait for the running job (and its present handoff) to finish.
  /// Does not cancel work already in Apply/present.
  void WaitForSessionIdle(std::uint64_t image_load_request_id) override;

  /// Cancel one request by id (token or explicit). Starts the next runnable request.
  auto CancelRequest(std::uint64_t request_id) -> bool;

  /// Mark the blocking scheduler call complete and start the next request.
  /// `schedule_next_from_pool=false` defers ScheduleNext (present handoff);
  /// production calls true only from a scheduler-pool callback tail.
  void NotifySchedulerCompleted(std::uint64_t request_id, bool success, std::string message = {},
                                bool schedule_next_from_pool = true);

  /// Process pending slots: schedule at most one job when idle.
  void Pump();

  [[nodiscard]] auto pending_count() const -> std::size_t {
    std::scoped_lock lock(mutex_);
    return CountOccupiedSlots();
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
    diag.pending_count = CountOccupiedSlots();
    diag.inflight_reason =
        inflight_ ? std::make_optional(inflight_->request.intent.reason) : std::nullopt;
    diag.replaced_count              = replaced_count_;
    diag.cancelled_count             = cancelled_count_;
    diag.last_error                  = last_error_;
    diag.image_load_request_id       = active_image_load_request_id_;
    diag.last_rejection_reason       = last_rejection_reason_;
    diag.last_rejected_render_reason = last_rejected_render_reason_;
    diag.last_ready_frame_role       = last_ready_frame_role_;
    diag.last_ready_render_reason    = last_ready_render_reason_;
    diag.accepted_count              = accepted_count_;
    diag.failed_count                = failed_count_;
    diag.ready_count                 = ready_count_;
    return diag;
  }

  /// Maps quality to the fixed coalesce slot (Interactive / Quality / Detail).
  [[nodiscard]] static auto SlotIndexForQuality(EditorRenderQuality quality) -> std::size_t;

 private:
  struct PendingEntry {
    EditorRenderRequest request;
    std::uint64_t       scheduler_job_id = 0;
  };

  auto AcceptOrReject(const EditorRenderIntent& intent, std::string* message) const -> bool;
  /// Overwrite `slots_[slot]`; emit Replaced for any previous occupant.
  void PlaceInSlot(std::size_t slot, PendingEntry entry);
  /// Cancel pending/in-flight work whose image_load_request_id does not match
  /// active_image_load_request_id_. Returns the running scheduler job to stop.
  auto CancelObsoleteForImageLoadMismatch() -> std::uint64_t;
  [[nodiscard]] auto IsObsolete(const EditorRenderIntent& intent) const -> bool;
  void               Emit(EditorRenderResult result);
  void               DeliverPendingResults();
  void               ScheduleNext();
  [[nodiscard]] auto CountOccupiedSlots() const -> std::size_t;
  /// First occupied slot in interactive > quality > detail order, or nullopt.
  [[nodiscard]] auto SelectNextSlotIndex() const -> std::optional<std::size_t>;
  /// Drop cancelled / epoch-obsolete entries from all slots (O(slot count)).
  void ScrubPendingSlots();
  [[nodiscard]] auto HasSessionWork(std::uint64_t image_load_request_id) const -> bool;
  auto TakeIdleCallbacks(std::uint64_t image_load_request_id)
      -> std::vector<SessionIdleCallback>;

  std::shared_ptr<IEditorPipelineSchedulerPort> scheduler_;
  ResultObserver                                observer_;
  std::uint64_t                                 active_image_load_request_id_ = 0;
  std::uint64_t                                 next_request_id_              = 1;
  std::uint64_t                                 last_scheduled_request_id_    = 0;
  /// At most one pending request per quality ladder role.
  std::array<std::optional<PendingEntry>, kQualitySlotCount> slots_{};
  std::optional<PendingEntry>                                inflight_;
  std::vector<EditorRenderResult>                            results_;
  std::vector<EditorRenderResult>                            pending_delivery_;
  std::unordered_set<std::uint64_t>                          terminal_request_ids_;
  struct PendingIdleCallback {
    std::uint64_t       image_load_request_id = 0;
    SessionIdleCallback callback;
  };
  std::vector<PendingIdleCallback> idle_callbacks_;
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
