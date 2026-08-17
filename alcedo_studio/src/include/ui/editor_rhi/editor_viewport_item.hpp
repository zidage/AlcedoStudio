//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaObject>
#include <QQuickRhiItem>
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

class EditorViewportRenderer;
class DirectFrameSink;

// Production QML viewport. Thin QML + render-thread boundary only: it owns no
// pipeline scheduler and exposes no backend selection or host-upload path.
// Native target creation, mapping, ready-queue, and teardown live in
// DirectPresentQueue; QRhi import is owned by EditorViewportRenderer.
//
// Production call sequence (pipeline worker):
//   EnsureSize → MapResourceForWrite → GPU write → UnmapResource → NotifyFrameReady
class EditorViewportItem : public QQuickRhiItem {
  Q_OBJECT
  Q_PROPERTY(QString backendName READ backendName NOTIFY DiagnosticsChanged)
  // Durable image id (DB). Distinct from sessionEpoch (ImageLoadRequestId stamp).
  Q_PROPERTY(qulonglong imageIdentity READ imageIdentity WRITE setImageIdentity NOTIFY
                 ImageIdentityChanged)
  // Presentation stamp of the active ImageLoadRequestId; set by the session
  // controller on every open/switch including A→B→A.
  Q_PROPERTY(qulonglong sessionEpoch READ sessionEpoch WRITE setSessionEpoch NOTIFY
                 SessionEpochChanged)
  Q_PROPERTY(qulonglong targetGeneration READ targetGeneration NOTIFY DiagnosticsChanged)
  Q_PROPERTY(qulonglong lastPresentedSessionEpoch READ lastPresentedSessionEpoch NOTIFY
                 DiagnosticsChanged)
  Q_PROPERTY(
      qulonglong lastPresentedRequestId READ lastPresentedRequestId NOTIFY DiagnosticsChanged)
  Q_PROPERTY(qulonglong presentedFrameCount READ presentedFrameCount NOTIFY DiagnosticsChanged)
  Q_PROPERTY(
      qulonglong droppedStaleFrameCount READ droppedStaleFrameCount NOTIFY DiagnosticsChanged)
  Q_PROPERTY(int liveTargetCount READ liveTargetCount NOTIFY DiagnosticsChanged)
  Q_PROPERTY(bool presentationAvailable READ presentationAvailable NOTIFY DiagnosticsChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY StatusChanged)
  Q_PROPERTY(bool extendedDynamicRangeRequested READ extendedDynamicRangeRequested NOTIFY
                 DisplayConfigChanged)
  Q_PROPERTY(bool windowColorSpaceApplied READ windowColorSpaceApplied NOTIFY DisplayConfigChanged)

 public:
  explicit EditorViewportItem(QQuickItem* parent = nullptr);
  ~EditorViewportItem() override;

  [[nodiscard]] auto backendName() const -> QString;
  [[nodiscard]] auto imageIdentity() const -> qulonglong {
    return image_identity_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto sessionEpoch() const -> qulonglong {
    return session_epoch_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto targetGeneration() const -> qulonglong;
  [[nodiscard]] auto lastPresentedSessionEpoch() const -> qulonglong;
  [[nodiscard]] auto lastPresentedRequestId() const -> qulonglong;
  [[nodiscard]] auto presentedFrameCount() const -> qulonglong;
  [[nodiscard]] auto droppedStaleFrameCount() const -> qulonglong;
  [[nodiscard]] auto liveTargetCount() const -> int;
  [[nodiscard]] auto presentationAvailable() const -> bool;
  [[nodiscard]] auto statusText() const -> QString;
  [[nodiscard]] auto displayConfig() const -> ViewerDisplayConfig;
  [[nodiscard]] auto extendedDynamicRangeRequested() const -> bool;
  [[nodiscard]] auto windowColorSpaceApplied() const -> bool {
    return window_color_space_applied_.load(std::memory_order_acquire);
  }

  void               setImageIdentity(qulonglong identity);
  void               setSessionEpoch(qulonglong epoch);
  // Whole-window display state for the unified QML scene. The session
  // controller supplies the adjustment snapshot immediately; Metal frames
  // repeat the exact producer configuration when they arrive.
  void               setDisplayConfig(const ViewerDisplayConfig& config);
  // Harness helper: set identity and bump a local session_epoch. Production
  // uses setSessionEpoch(image_load_request_id) from the session controller.
  Q_INVOKABLE void   beginImageSession(qulonglong imageIdentity);

  void               setViewState(const ViewerViewState& state);
  // QML/session bridge: update zoom/pan without touching native targets.
  Q_INVOKABLE void   setViewTransform(float zoom, float panX, float panY);
  [[nodiscard]] auto present_queue() const -> const std::shared_ptr<DirectPresentQueue>& {
    return present_queue_;
  }
  // Diagnostics: how many times setViewState was entered (tests assert once
  // per user input sequence / metrics change).
  [[nodiscard]] auto viewStatePushCount() const -> int {
    return view_state_push_count_.load(std::memory_order_acquire);
  }

  // Production IFrameSink. Owned by the item; safe while the viewport exists.
  [[nodiscard]] auto frameSink() -> DirectFrameSink*;

  Q_INVOKABLE void   requestRendererInvalidation();
  Q_INVOKABLE void   cancelPendingFrames();
  // Stop producer handshakes before a session close tears down this QML tree.
  // This is reversible: resumePresentation or a visibility refresh re-enables
  // a reused viewport.
  void               suspendPresentation();
  void               resumePresentation();
  // Re-evaluate window exposure / minimize state (also used by tests after
  // programmatic hide/show where Qt may not emit the expected signals promptly).
  Q_INVOKABLE void   refreshPresentationAvailability();
  // Request a render-thread pass only when content/state actually changed.
  void               requestPresentUpdate();
  // Called synchronously from the GUI-thread adjustment submit path. Marks the
  // item dirty before the pointer event returns, so Qt Quick's next scene-graph
  // frame can recycle stale presentation slots even if a worker-thread ready
  // notification is still queued behind continuous input events.
  void               prepareForAdjustmentFrame();
  // Arm a vsync-sampled consume: every window present re-dirties this item so
  // ConsumeNewestReady runs on the next scene-graph tick. Used while a slider
  // or trackball drag submits unsettled patches.
  void               beginInteractivePresentLoop();
  // Stop the vsync consume and request one more pass for the last Ready frame.
  void               endInteractivePresentLoop();
  // Re-request a scene-graph frame while the interactive present loop is armed.
  // QQuickWindow::afterRendering queues this onto the GUI thread. Tests may
  // call it directly.
  void               continueInteractivePresentLoop();

  [[nodiscard]] auto interactivePresentLoopActive() const -> bool {
    return interactive_present_loop_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto interactivePresentLoopTickCount() const -> std::uint64_t {
    return interactive_present_loop_tick_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto adjustmentFrameRequestCount() const -> std::uint64_t {
    return adjustment_frame_request_count_.load(std::memory_order_acquire);
  }

  // Called by the application composition root before loading QML. The
  // registration is idempotent and also makes visible-window QML tests use the
  // same real type.
  static void RegisterQmlType();

 signals:
  void DiagnosticsChanged();
  void ImageIdentityChanged();
  void SessionEpochChanged();
  void StatusChanged();
  void DisplayConfigChanged();
  // camelCase for QML handler onTargetSizeRequested.
  void targetSizeRequested(int width, int height);

 protected:
  auto createRenderer() -> QQuickRhiItemRenderer* override;

 private:
  friend class EditorViewportRenderer;
  friend class DirectFrameSink;

  [[nodiscard]] auto viewStateSnapshot() const -> ViewerViewState;
  [[nodiscard]] auto presentationRequested() const -> bool {
    return presentation_requested_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto takeAdjustmentFrameRequest() -> bool {
    return adjustment_frame_requested_.exchange(false, std::memory_order_acq_rel);
  }
  void                                stopInteractivePresentLoop();
  void                                setBackendName(const QString& name);
  void                                setStatusText(const QString& text);
  void                                notifyDiagnosticsChanged();
  void                                attachWindow(QQuickWindow* window);
  void                                detachWindow(bool reset_display = true);
  void                                requestPresentUpdateOnGuiThread();
  void                                applyDisplayConfig();
  void                                resetWindowDisplayConfig();

  std::shared_ptr<DirectPresentQueue> present_queue_;
  std::unique_ptr<DirectFrameSink>    frame_sink_;
  mutable std::mutex                  mutex_;
  ViewerViewState                     view_state_{};
  ViewerDisplayConfig                 display_config_{};
  QString                             backend_name_ = QStringLiteral("uninitialized");
  QString                    status_text_ = QStringLiteral("waiting for a compatible frame");
  std::atomic<qulonglong>    image_identity_{0};
  std::atomic<qulonglong>    session_epoch_{0};
  std::atomic<int>           view_state_push_count_{0};
  std::atomic<bool>          presentation_requested_{false};
  std::atomic<bool>          adjustment_frame_requested_{false};
  std::atomic<std::uint64_t> adjustment_frame_request_count_{0};
  std::atomic<bool>          interactive_present_loop_{false};
  std::atomic<std::uint64_t> interactive_present_loop_tick_count_{0};
  QQuickWindow*              attached_window_ = nullptr;
  QMetaObject::Connection    window_visibility_connection_;
  QMetaObject::Connection    window_screen_connection_;
  QMetaObject::Connection    scene_graph_invalidated_connection_;
  QMetaObject::Connection    scene_graph_initialized_connection_;
  QMetaObject::Connection    after_rendering_connection_;
  std::atomic<bool>          window_color_space_applied_{false};
  bool                       last_diagnostics_available_      = false;
  qulonglong                 last_diag_target_gen_            = 0;
  qulonglong                 last_diag_presented_session_epoch_   = 0;
  qulonglong                 last_diag_presented_request_id_  = 0;
  qulonglong                 last_diag_presented_frame_count_ = 0;
  qulonglong                 last_diag_dropped_               = 0;
  int                        last_diag_live_targets_          = 0;
};

void RegisterEditorViewportQmlTypes();

}  // namespace alcedo::editor_rhi
