//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_viewport_item.hpp"

#include <QtQml/qqml.h>

#include <QMetaObject>
#include <QQuickWindow>
#include <QScreen>
#include <QThread>
#include <QVector2D>
#include <cmath>
#include <mutex>

#include "ui/edit_viewer/color_manager.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_renderer.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/diagnostics/render_e2e_timing.hpp"

using alcedo::diag::editorPresentLog;

namespace alcedo::editor_rhi {

EditorViewportItem::EditorViewportItem(QQuickItem* parent)
    : QQuickRhiItem(parent),
      present_queue_(std::make_shared<DirectPresentQueue>(ActiveEditorBackend())) {
  setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA32F);
  setMirrorVertically(false);
  setAlphaBlending(false);
  frame_sink_ = std::make_unique<DirectFrameSink>(this);
  connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* window) {
    attachWindow(window);
    if (window && isVisible() && window->visibility() != QWindow::Hidden &&
        window->visibility() != QWindow::Minimized) {
      resumePresentation();
    } else {
      suspendPresentation();
    }
  });
  connect(this, &QQuickItem::visibleChanged, this, [this] {
    if (isVisible() && window() && window()->visibility() != QWindow::Hidden &&
        window()->visibility() != QWindow::Minimized) {
      // synchronize() is the render-thread acknowledgement that turns the
      // consumer back on. Merely becoming visible is not sufficient.
      resumePresentation();
      applyDisplayConfig();
    } else {
      suspendPresentation();
      // The CAMetalLayer is shared by the entire QML window. Leaving the
      // editor or entering its empty state must restore ordinary SDR chrome.
      resetWindowDisplayConfig();
    }
  });
  // Parent may already place this item in a window before windowChanged fires.
  if (window()) {
    attachWindow(window());
  }
  // The queue remains unavailable until EditorViewportRenderer::synchronize()
  // runs on the scene-graph thread. This prevents a producer from waiting on a
  // window that is exposed but has not created this item's renderer yet.
  present_queue_->SetConsumerAvailable(false);
}

EditorViewportItem::~EditorViewportItem() {
  detachWindow();
  present_queue_->Shutdown();
  setStatusText(QStringLiteral("viewport shutting down"));
}

void EditorViewportItem::RegisterQmlType() { RegisterEditorViewportQmlTypes(); }

void RegisterEditorViewportQmlTypes() {
  static std::once_flag once;
  std::call_once(once, [] {
    qmlRegisterModule("Alcedo.Main", 1, 0);
    qmlRegisterType<EditorViewportItem>("Alcedo.Main", 1, 0, "EditorViewportItem");
  });
  // Overlay + interaction types share the same module and are idempotent.
  RegisterEditorOverlayQmlTypes();
}

auto EditorViewportItem::backendName() const -> QString {
  std::lock_guard lock(mutex_);
  return backend_name_;
}

auto EditorViewportItem::targetGeneration() const -> qulonglong {
  return present_queue_->DiagnosticsSnapshot().target_generation;
}

auto EditorViewportItem::lastPresentedSessionEpoch() const -> qulonglong {
  return present_queue_->DiagnosticsSnapshot().last_composed_session_epoch;
}

auto EditorViewportItem::lastPresentedRequestId() const -> qulonglong {
  return present_queue_->DiagnosticsSnapshot().last_composed_request_id;
}

auto EditorViewportItem::presentedFrameCount() const -> qulonglong {
  return present_queue_->DiagnosticsSnapshot().composed_frame_count;
}

auto EditorViewportItem::droppedStaleFrameCount() const -> qulonglong {
  return present_queue_->DiagnosticsSnapshot().dropped_stale_frame_count;
}

auto EditorViewportItem::liveTargetCount() const -> int {
  return static_cast<int>(present_queue_->DiagnosticsSnapshot().live_target_count);
}

auto EditorViewportItem::presentationAvailable() const -> bool {
  return present_queue_->DiagnosticsSnapshot().consumer_available;
}

auto EditorViewportItem::statusText() const -> QString {
  std::lock_guard lock(mutex_);
  return status_text_;
}

auto EditorViewportItem::displayConfig() const -> ViewerDisplayConfig {
  std::lock_guard lock(mutex_);
  return display_config_;
}

auto EditorViewportItem::extendedDynamicRangeRequested() const -> bool {
  const auto config = displayConfig();
  return config.encoding_eotf == ColorUtils::EOTF::ST2084 ||
         config.encoding_eotf == ColorUtils::EOTF::HLG;
}

void EditorViewportItem::setDisplayConfig(const ViewerDisplayConfig& config) {
  {
    std::lock_guard lock(mutex_);
    if (display_config_ == config) {
      return;
    }
    display_config_ = config;
  }
  window_color_space_applied_.store(false, std::memory_order_release);
  auto apply = [this] {
    emit DisplayConfigChanged();
    applyDisplayConfig();
  };
  if (thread() == QThread::currentThread()) {
    apply();
  } else {
    QMetaObject::invokeMethod(this, std::move(apply), Qt::QueuedConnection);
  }
}

void EditorViewportItem::setImageIdentity(qulonglong identity) {
  const auto previous = image_identity_.exchange(identity, std::memory_order_acq_rel);
  if (previous == identity) {
    return;
  }
  emit ImageIdentityChanged();
}

void EditorViewportItem::setSessionEpoch(qulonglong epoch) {
  const auto previous = session_epoch_.exchange(epoch, std::memory_order_acq_rel);
  if (previous == epoch) {
    return;
  }
  present_queue_->InvalidateSessionEpoch(epoch, imageIdentity());
  emit SessionEpochChanged();
  notifyDiagnosticsChanged();
  requestPresentUpdate();
}

void EditorViewportItem::beginImageSession(qulonglong imageIdentity) {
  setImageIdentity(imageIdentity);
  const auto next = session_epoch_.load(std::memory_order_acquire) + 1;
  session_epoch_.store(next, std::memory_order_release);
  present_queue_->InvalidateSessionEpoch(next, imageIdentity);
  if (frame_sink_) {
    frame_sink_->ClearPendingImportedFrames();
  }
  emit SessionEpochChanged();
  notifyDiagnosticsChanged();
  requestPresentUpdate();
}

void EditorViewportItem::setViewState(const ViewerViewState& state) {
  view_state_push_count_.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard lock(mutex_);
    view_state_ = state;
  }
  if (frame_sink_) {
    frame_sink_->SetViewState(state);
  }
  requestPresentUpdateOnGuiThread();
}

void EditorViewportItem::setViewTransform(float zoom, float panX, float panY) {
  ViewerViewState copy;
  {
    std::lock_guard lock(mutex_);
    auto&           transform = view_state_.snapshot.view_transform;
    if (std::abs(transform.zoom - zoom) <= 1.0e-6f &&
        std::abs(transform.pan.x() - panX) <= 1.0e-6f &&
        std::abs(transform.pan.y() - panY) <= 1.0e-6f) {
      return;
    }
    transform.zoom = zoom;
    transform.pan  = QVector2D(panX, panY);
    copy           = view_state_;
  }
  if (frame_sink_) {
    frame_sink_->SetViewState(copy);
  }
  requestPresentUpdateOnGuiThread();
}

auto EditorViewportItem::frameSink() -> DirectFrameSink* { return frame_sink_.get(); }

void EditorViewportItem::requestRendererInvalidation() {
  present_queue_->InvalidateTargetGeneration();
  requestPresentUpdate();
}

void EditorViewportItem::cancelPendingFrames() {
  present_queue_->InvalidateSessionEpoch(sessionEpoch(), imageIdentity());
  if (frame_sink_) {
    frame_sink_->ClearPendingImportedFrames();
  }
  requestPresentUpdate();
}

void EditorViewportItem::suspendPresentation() {
  presentation_requested_.store(false, std::memory_order_release);
  present_queue_->SetConsumerAvailable(false);
  notifyDiagnosticsChanged();
}

void EditorViewportItem::resumePresentation() {
  presentation_requested_.store(true, std::memory_order_release);
  requestPresentUpdate();
}

void EditorViewportItem::requestPresentUpdate() { requestPresentUpdateOnGuiThread(); }

void EditorViewportItem::prepareForAdjustmentFrame() {
  adjustment_frame_requested_.store(true, std::memory_order_release);
  adjustment_frame_request_count_.fetch_add(1, std::memory_order_acq_rel);
  requestPresentUpdateOnGuiThread();
}

void EditorViewportItem::refreshPresentationAvailability() {
  if (isVisible() && window() && window()->visibility() != QWindow::Hidden &&
      window()->visibility() != QWindow::Minimized) {
    resumePresentation();
  } else {
    suspendPresentation();
  }
}

void EditorViewportItem::requestPresentUpdateOnGuiThread() {
  // Worker threads must not call QQuickItem::update() directly.
  auto request = [this] {
    // QQuickItem already coalesces update requests. A separate sticky flag
    // can remain set while the item is hidden and suppress the first update
    // after exposure, stranding a producer waiting for its native target.
    update();
    if (QQuickWindow* w = window()) {
      w->requestUpdate();
    }
    // Stamp every Ready frame waiting on this GUI wake (P0 present split).
    diag::NoteRenderE2eGuiUpdate();
  };
  if (thread() == QThread::currentThread()) {
    request();
  } else {
    QMetaObject::invokeMethod(this, std::move(request), Qt::QueuedConnection);
  }
}

auto EditorViewportItem::createRenderer() -> QQuickRhiItemRenderer* {
  // Renderer destruction must not shut down the present queue; the item keeps
  // it for scene-graph recreation.
  qCDebug(editorPresentLog,
          "[EditorPresent] creating QQuickRhiItem renderer image=%llu epoch=%llu",
          static_cast<unsigned long long>(imageIdentity()),
          static_cast<unsigned long long>(sessionEpoch()));
  return new EditorViewportRenderer();
}

void EditorViewportItem::attachWindow(QQuickWindow* window) {
  if (attached_window_ == window) {
    return;
  }
  detachWindow();
  attached_window_ = window;
  if (!attached_window_) {
    suspendPresentation();
    return;
  }
  window_visibility_connection_ = connect(
      attached_window_, &QWindow::visibilityChanged, this, [this](QWindow::Visibility visibility) {
        if (visibility == QWindow::Hidden || visibility == QWindow::Minimized) {
          suspendPresentation();
        } else {
          // The next synchronize() confirms that the renderer is live.
          resumePresentation();
          applyDisplayConfig();
        }
      });
  window_screen_connection_ =
      connect(attached_window_, &QWindow::screenChanged, this, [this](QScreen*) {
        window_color_space_applied_.store(false, std::memory_order_release);
        applyDisplayConfig();
      });
  // sceneGraphInvalidated is emitted on the render thread. Direct connection
  // is required so blocked producers are released before Qt destroys QRhi
  // resources; the queue operation itself is thread-safe.
  scene_graph_invalidated_connection_ = connect(
      attached_window_, &QQuickWindow::sceneGraphInvalidated, this,
      [queue = present_queue_] { queue->SetConsumerAvailable(false); }, Qt::DirectConnection);
  scene_graph_initialized_connection_ = connect(
      attached_window_, &QQuickWindow::sceneGraphInitialized, this,
      [this] {
        refreshPresentationAvailability();
        applyDisplayConfig();
      },
      Qt::QueuedConnection);
  applyDisplayConfig();
}

void EditorViewportItem::detachWindow(bool reset_display) {
  if (reset_display) {
    resetWindowDisplayConfig();
  }
  QObject::disconnect(window_visibility_connection_);
  QObject::disconnect(window_screen_connection_);
  QObject::disconnect(scene_graph_invalidated_connection_);
  QObject::disconnect(scene_graph_initialized_connection_);
  window_visibility_connection_       = {};
  window_screen_connection_           = {};
  scene_graph_invalidated_connection_ = {};
  scene_graph_initialized_connection_ = {};
  attached_window_                    = nullptr;
}

void EditorViewportItem::applyDisplayConfig() {
  if (!attached_window_ || !isVisible() || thread() != QThread::currentThread()) {
    return;
  }
  const auto native_handle = reinterpret_cast<void*>(attached_window_->winId());
  if (!native_handle) {
    return;
  }
  const bool applied  = ColorManager::ApplyWindowColorSpace(native_handle, displayConfig());
  const bool previous = window_color_space_applied_.exchange(applied, std::memory_order_acq_rel);
  if (previous != applied) {
    emit DisplayConfigChanged();
  }
}

void EditorViewportItem::resetWindowDisplayConfig() {
  if (!attached_window_ || thread() != QThread::currentThread()) {
    return;
  }
  const auto native_handle = reinterpret_cast<void*>(attached_window_->winId());
  if (native_handle) {
    (void)ColorManager::ApplyWindowColorSpace(native_handle, ViewerDisplayConfig{});
  }
  const bool previous = window_color_space_applied_.exchange(false, std::memory_order_acq_rel);
  if (previous) {
    emit DisplayConfigChanged();
  }
}

auto EditorViewportItem::viewStateSnapshot() const -> ViewerViewState {
  std::lock_guard lock(mutex_);
  return view_state_;
}

void EditorViewportItem::setBackendName(const QString& name) {
  {
    std::lock_guard lock(mutex_);
    if (backend_name_ == name) {
      return;
    }
    backend_name_ = name;
  }
  QMetaObject::invokeMethod(this, [this] { emit DiagnosticsChanged(); }, Qt::QueuedConnection);
}

void EditorViewportItem::setStatusText(const QString& text) {
  {
    std::lock_guard lock(mutex_);
    if (status_text_ == text) {
      return;
    }
    status_text_ = text;
  }
  QMetaObject::invokeMethod(this, [this] { emit StatusChanged(); }, Qt::QueuedConnection);
}

void EditorViewportItem::notifyDiagnosticsChanged() {
  const auto diag                  = present_queue_->DiagnosticsSnapshot();
  const bool available             = diag.consumer_available;
  const auto target_gen            = diag.target_generation;
  const auto presented_image_gen   = diag.last_composed_session_epoch;
  const auto presented_request_id  = diag.last_composed_request_id;
  const auto presented_frame_count = diag.composed_frame_count;
  const auto dropped               = diag.dropped_stale_frame_count;
  const int  live                  = static_cast<int>(diag.live_target_count);
  if (available == last_diagnostics_available_ && target_gen == last_diag_target_gen_ &&
      presented_image_gen == last_diag_presented_session_epoch_ &&
      presented_request_id == last_diag_presented_request_id_ &&
      presented_frame_count == last_diag_presented_frame_count_ && dropped == last_diag_dropped_ &&
      live == last_diag_live_targets_) {
    return;
  }
  last_diagnostics_available_      = available;
  last_diag_target_gen_            = target_gen;
  last_diag_presented_session_epoch_   = presented_image_gen;
  last_diag_presented_request_id_  = presented_request_id;
  last_diag_presented_frame_count_ = presented_frame_count;
  last_diag_dropped_               = dropped;
  last_diag_live_targets_          = live;
  QMetaObject::invokeMethod(this, [this] { emit DiagnosticsChanged(); }, Qt::QueuedConnection);
}

}  // namespace alcedo::editor_rhi
