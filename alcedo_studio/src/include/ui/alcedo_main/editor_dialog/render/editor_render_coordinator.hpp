//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QTimer>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>

#include "app/pipeline_service.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/render_controller.hpp"
#include "ui/alcedo_main/editor_dialog/state.hpp"

namespace alcedo {
class ImageBuffer;
class QtEditViewer;
}  // namespace alcedo

namespace alcedo::ui {

class SpinnerWidget;

class EditorRenderCoordinator {
 public:
  struct Dependencies {
    QObject*                           timer_parent = nullptr;
    std::shared_ptr<PipelineGuard>     pipeline_guard;
    std::shared_ptr<PipelineScheduler> scheduler;
    PipelineTask*                      base_task = nullptr;
    AdjustmentState*                   state     = nullptr;
  };

  struct Callbacks {
    std::function<QtEditViewer*()>              viewer;
    std::function<SpinnerWidget*()>             spinner;
    std::function<ControlPanelKind()>           active_panel;
    std::function<bool()>                       needs_full_frame_preview_after_geometry_commit;
    std::function<void()>                       clear_full_frame_preview_after_geometry_commit;
    std::function<void(const AdjustmentState&)> apply_state_to_pipeline;
    std::function<bool()>                       refresh_color_temp_runtime_state;
    std::function<void()>                       sync_color_temp_controls;
  };

  EditorRenderCoordinator(Dependencies dependencies, Callbacks callbacks);

  void AdvancePreviewGeneration();
  void InvalidateDetailPreviewState();
  auto BuildPreviewMetadata(RenderType render_type) const -> FramePreviewMetadata;
  auto IsDetailPreviewGeometryFallbackActive() const -> bool;
  auto WantsDetailPreviewFromViewport() const -> bool;
  auto CanScheduleDetailPreview() const -> bool;
  void MaybeScheduleDetailPreviewRenderFromViewport();

  void EnsureQualityPreviewTimer();
  void EnsureDetailPreviewTimer();
  void TriggerQualityPreviewRenderFromPipeline();
  void ScheduleQualityPreviewRenderFromPipeline();
  void ScheduleDetailPreviewRenderFromViewport();
  void TriggerDetailPreviewRenderFromViewport();

  auto CanSubmitFastPreviewNow() const -> bool;
  void EnsureFastPreviewSubmitTimer();
  void ArmFastPreviewSubmitTimer();
  void EnqueueRenderRequest(const AdjustmentState&      snapshot,
                            const FramePreviewMetadata& frame_metadata, bool apply_state,
                            bool use_viewport_region = true);
  void RequestRender(bool use_viewport_region = true, bool bump_preview_generation = true);
  void RequestRenderWithoutApplyingState(bool use_viewport_region     = true,
                                         bool bump_preview_generation = false);

  void EnsurePollTimer();
  void PollInflight();
  void StartNext();
  void OnRenderFinished(bool render_succeeded);
  // Re-enqueues a preview request whose presentation was skipped because the
  // present target was not yet sized. Fired on a short debounce timer so the UI
  // thread has a chance to finish the asynchronous render-target resize first;
  // this is robust to whether the resize completed before or after the render
  // finished (a signal-only trigger would orphan the replay when the resize
  // completed first).
  void EnsureResizeReplayTimer();
  void OnResizeReplayTick();

 private:
  static constexpr std::chrono::milliseconds kQualityPreviewDebounceInterval =
      controllers::render::kQualityPreviewDebounceInterval;
  static constexpr std::chrono::milliseconds kViewportDetailDebounceInterval{120};
  // Debounce before retrying a skipped (present-target-not-ready) render, and
  // the cap on retries to avoid a runaway loop if the target never becomes ready.
  static constexpr int kResizeReplayIntervalMs   = 16;
  static constexpr int kMaxResizeReplayAttempts  = 12;

  auto                                       CurrentViewer() const -> QtEditViewer*;
  auto                                       CurrentSpinner() const -> SpinnerWidget*;
  auto                                       CurrentActivePanel() const -> ControlPanelKind;

  Dependencies                               dependencies_;
  Callbacks                                  callbacks_;

  QTimer*                                    poll_timer_                              = nullptr;
  QTimer*                                    detail_preview_timer_                    = nullptr;
  QTimer*                                    quality_preview_timer_                   = nullptr;
  QTimer*                                    fast_preview_submit_timer_               = nullptr;
  QTimer*                                    resize_replay_timer_                     = nullptr;
  int                                        resize_replay_attempts_                  = 0;
  bool                                       inflight_                                = false;
  bool                                       detail_preview_waiting_for_quality_base_ = false;

  std::optional<std::future<std::shared_ptr<ImageBuffer>>> inflight_future_{};
  std::optional<PendingRenderRequest>                      inflight_request_{};
  std::optional<PendingRenderRequest>                      pending_fast_preview_request_{};
  std::optional<PendingRenderRequest>                      pending_quality_base_render_request_{};
  std::optional<PendingRenderRequest>                      pending_detail_render_request_{};
  // A preview request whose presentation was skipped because the present target
  // was not yet sized. Held until OnRenderTargetReady() replays it. Cleared by
  // AdvancePreviewGeneration() so stale requests are not replayed after edits.
  std::optional<PendingRenderRequest>                      pending_resize_replay_{};

  std::chrono::steady_clock::time_point                    last_fast_preview_submit_time_{};
  std::uint64_t                                            preview_generation_ = 0;
  std::uint64_t                                            detail_serial_      = 0;
  std::uint64_t latest_quality_base_generation_ready_                          = 0;
};

}  // namespace alcedo::ui
