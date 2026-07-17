//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_viewport_item.hpp"

#include <QMetaObject>
#include <QQuickWindow>
#include <QVector2D>
#include <QtQml/qqml.h>

#include <cmath>
#include <mutex>

#include "ui/editor_rhi/editor_interaction_controller.hpp"
#include "ui/editor_rhi/editor_viewport_renderer.hpp"
#include "ui/editor_rhi/lease_frame_sink.hpp"

namespace alcedo::editor_rhi {

EditorViewportItem::EditorViewportItem(QQuickItem* parent)
    : QQuickRhiItem(parent),
      broker_(std::make_shared<FramePresentationBroker>(ActiveEditorBackend())) {
  setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA32F);
  setMirrorVertically(false);
  setAlphaBlending(false);
  frame_sink_ = std::make_unique<LeaseFrameSink>(this);
  RegisterQmlType();
  connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow* window) {
    attachWindow(window);
    refreshConsumerAvailability();
  });
  connect(this, &QQuickItem::visibleChanged, this, [this] { refreshConsumerAvailability(); });
  // Parent may already place this item in a window before windowChanged fires.
  if (window()) {
    attachWindow(window());
    refreshConsumerAvailability();
  }
}

EditorViewportItem::~EditorViewportItem() {
  detachWindow();
  broker_->Shutdown();
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
  return broker_->DiagnosticsSnapshot().target_generation;
}

auto EditorViewportItem::lastPresentedImageGeneration() const -> qulonglong {
  return broker_->DiagnosticsSnapshot().last_presented_image_generation;
}

auto EditorViewportItem::droppedStaleFrameCount() const -> qulonglong {
  return broker_->DiagnosticsSnapshot().dropped_stale_frame_count;
}

auto EditorViewportItem::liveTargetCount() const -> int {
  return static_cast<int>(broker_->DiagnosticsSnapshot().live_target_count);
}

auto EditorViewportItem::presentationAvailable() const -> bool {
  return broker_->DiagnosticsSnapshot().consumer_available;
}

auto EditorViewportItem::statusText() const -> QString {
  std::lock_guard lock(mutex_);
  return status_text_;
}

void EditorViewportItem::setImageIdentity(qulonglong identity) {
  const auto previous = image_identity_.exchange(identity, std::memory_order_acq_rel);
  if (previous == identity) {
    return;
  }
  emit ImageIdentityChanged();
}

void EditorViewportItem::setImageGeneration(qulonglong generation) {
  const auto previous = image_generation_.exchange(generation, std::memory_order_acq_rel);
  if (previous == generation) {
    return;
  }
  broker_->InvalidateImageGeneration(generation, imageIdentity());
  emit ImageGenerationChanged();
  notifyDiagnosticsChanged();
  requestPresentUpdate();
}

void EditorViewportItem::beginImageSession(qulonglong imageIdentity) {
  setImageIdentity(imageIdentity);
  const auto next = image_generation_.load(std::memory_order_acquire) + 1;
  image_generation_.store(next, std::memory_order_release);
  broker_->InvalidateImageGeneration(next, imageIdentity);
  emit ImageGenerationChanged();
  notifyDiagnosticsChanged();
  requestPresentUpdate();
}

auto EditorViewportItem::tryAcquireWritableTarget(const WritableTargetRequest& request)
    -> std::optional<WritableTargetLease> {
  return broker_->TryAcquireWritableTarget(request);
}

auto EditorViewportItem::tryAcquireWritableTarget() -> std::optional<WritableTargetLease> {
  return broker_->TryAcquireWritableTarget();
}

auto EditorViewportItem::submitCompletedFrame(const CompletedFrameLease& frame) -> bool {
  const bool accepted = broker_->SubmitCompletedFrame(frame);
  if (accepted) {
    requestPresentUpdateOnGuiThread();
  }
  return accepted;
}

void EditorViewportItem::abandonProducerWrite(const WritableTargetLease& lease) {
  broker_->AbandonProducerWrite(lease);
  notifyDiagnosticsChanged();
}

void EditorViewportItem::setViewState(const ViewerViewState& state) {
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
    auto& transform = view_state_.snapshot.view_transform;
    if (std::abs(transform.zoom - zoom) <= 1.0e-6f &&
        std::abs(transform.pan.x() - panX) <= 1.0e-6f &&
        std::abs(transform.pan.y() - panY) <= 1.0e-6f) {
      return;
    }
    transform.zoom = zoom;
    transform.pan = QVector2D(panX, panY);
    copy = view_state_;
  }
  if (frame_sink_) {
    frame_sink_->SetViewState(copy);
  }
  requestPresentUpdateOnGuiThread();
}

auto EditorViewportItem::frameSink() -> LeaseFrameSink* { return frame_sink_.get(); }

void EditorViewportItem::requestRendererInvalidation() {
  broker_->InvalidateTargetGeneration();
  requestPresentUpdate();
}

void EditorViewportItem::cancelPendingFrames() {
  broker_->InvalidateImageGeneration(imageGeneration(), imageIdentity());
  requestPresentUpdate();
}

void EditorViewportItem::requestPresentUpdate() {
  requestPresentUpdateOnGuiThread();
}

void EditorViewportItem::refreshPresentationAvailability() {
  refreshConsumerAvailability();
}

void EditorViewportItem::requestPresentUpdateOnGuiThread() {
  // Worker threads must not call QQuickItem::update() directly.
  QMetaObject::invokeMethod(
      this,
      [this] {
        if (update_pending_) {
          return;
        }
        update_pending_ = true;
        update();
        if (QQuickWindow* w = window()) {
          w->requestUpdate();
        }
      },
      Qt::QueuedConnection);
}

auto EditorViewportItem::createRenderer() -> QQuickRhiItemRenderer* {
  // Renderer destruction must not shut down the broker; the item keeps it for
  // scene-graph recreation.
  return new EditorViewportRenderer();
}

void EditorViewportItem::itemChange(ItemChange change, const ItemChangeData& value) {
  QQuickRhiItem::itemChange(change, value);
  if (change == ItemVisibleHasChanged || change == ItemSceneChange) {
    refreshConsumerAvailability();
  }
}

void EditorViewportItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
  QQuickRhiItem::geometryChange(newGeometry, oldGeometry);
  if (newGeometry.size() != oldGeometry.size()) {
    requestPresentUpdate();
  }
}

void EditorViewportItem::attachWindow(QQuickWindow* window) {
  if (attached_window_ == window) {
    return;
  }
  detachWindow();
  attached_window_ = window;
  if (!attached_window_) {
    return;
  }
  connect(attached_window_, &QWindow::visibilityChanged, this,
          &EditorViewportItem::onWindowVisibilityChanged);
  connect(attached_window_, &QWindow::activeChanged, this,
          &EditorViewportItem::onWindowVisibilityChanged);
  connect(attached_window_, &QQuickWindow::sceneGraphInvalidated, this,
          &EditorViewportItem::onWindowSceneGraphInvalidated);
  connect(attached_window_, &QQuickWindow::sceneGraphInitialized, this,
          &EditorViewportItem::onWindowSceneGraphInitialized);
  scene_graph_ready_ = true;
}

void EditorViewportItem::detachWindow() {
  if (!attached_window_) {
    return;
  }
  disconnect(attached_window_, nullptr, this, nullptr);
  attached_window_ = nullptr;
  scene_graph_ready_ = false;
}

void EditorViewportItem::onWindowVisibilityChanged() { refreshConsumerAvailability(); }

void EditorViewportItem::onWindowSceneGraphInvalidated() {
  scene_graph_ready_ = false;
  // Do not permanently shut down the broker; only mark the consumer unavailable
  // so producers stop writing. Targets are released; a new renderer recreates them.
  broker_->SetConsumerAvailable(false);
  notifyDiagnosticsChanged();
}

void EditorViewportItem::onWindowSceneGraphInitialized() {
  scene_graph_ready_ = true;
  refreshConsumerAvailability();
  requestPresentUpdate();
}

void EditorViewportItem::refreshConsumerAvailability() {
  bool available = isVisible() && opacity() > 0.0;
  if (attached_window_) {
    const auto visibility = attached_window_->visibility();
    const bool window_ok = attached_window_->isExposed() &&
                           visibility != QWindow::Hidden &&
                           visibility != QWindow::Minimized;
    // Simple hide/show does not always re-emit sceneGraphInitialized. Once the
    // window is exposed again, treat the consumer as ready so the pool can
    // top up and producers can resume.
    if (window_ok) {
      scene_graph_ready_ = true;
    }
    available = available && window_ok && scene_graph_ready_;
  } else {
    available = false;
  }
  broker_->SetConsumerAvailable(available);
  if (available) {
    requestPresentUpdate();
  }
  notifyDiagnosticsChanged();
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
  const auto diag = broker_->DiagnosticsSnapshot();
  const bool available = diag.consumer_available;
  const auto target_gen = diag.target_generation;
  const auto dropped = diag.dropped_stale_frame_count;
  const int live = static_cast<int>(diag.live_target_count);
  if (available == last_diagnostics_available_ && target_gen == last_diag_target_gen_ &&
      dropped == last_diag_dropped_ && live == last_diag_live_targets_) {
    return;
  }
  last_diagnostics_available_ = available;
  last_diag_target_gen_ = target_gen;
  last_diag_dropped_ = dropped;
  last_diag_live_targets_ = live;
  QMetaObject::invokeMethod(this, [this] { emit DiagnosticsChanged(); }, Qt::QueuedConnection);
}

}  // namespace alcedo::editor_rhi
