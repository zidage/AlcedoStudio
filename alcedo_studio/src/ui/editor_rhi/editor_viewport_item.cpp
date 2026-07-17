//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_viewport_item.hpp"

#include <QMetaObject>
#include <QtQml/qqml.h>

#include <mutex>

#include "ui/editor_rhi/editor_viewport_renderer.hpp"

namespace alcedo::editor_rhi {

EditorViewportItem::EditorViewportItem(QQuickItem* parent)
    : QQuickRhiItem(parent), broker_(std::make_shared<FramePresentationBroker>()) {
  setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA32F);
  setMirrorVertically(false);
  setAlphaBlending(false);
  RegisterQmlType();
}

EditorViewportItem::~EditorViewportItem() {
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

void EditorViewportItem::setImageGeneration(qulonglong generation) {
  const auto previous = image_generation_.exchange(generation, std::memory_order_acq_rel);
  if (previous == generation) {
    return;
  }
  broker_->InvalidateImageGeneration(generation);
  emit ImageGenerationChanged();
  notifyDiagnosticsChanged();
  update();
}

void EditorViewportItem::submitFrame(const ViewerFrame& frame) {
  if (!frame) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    pending_frames_.push_back(frame);
    while (pending_frames_.size() > 8) {
      pending_frames_.pop_front();
    }
  }
  update();
}

auto EditorViewportItem::tryAcquireWritableTarget() -> std::optional<WritableTargetLease> {
  return broker_->TryAcquireWritableTarget();
}

auto EditorViewportItem::submitCompletedFrame(const CompletedFrameLease& frame) -> bool {
  const bool accepted = broker_->SubmitCompletedFrame(frame);
  if (accepted) {
    update();
  }
  return accepted;
}

void EditorViewportItem::setViewState(const ViewerViewState& state) {
  {
    std::lock_guard lock(mutex_);
    view_state_ = state;
  }
  update();
}

void EditorViewportItem::requestRendererInvalidation() {
  broker_->InvalidateTargetGeneration();
  update();
}

void EditorViewportItem::cancelPendingFrames() {
  broker_->InvalidateImageGeneration(imageGeneration());
  {
    std::lock_guard lock(mutex_);
    pending_frames_.clear();
  }
  update();
}

auto EditorViewportItem::createRenderer() -> QQuickRhiItemRenderer* {
  return new EditorViewportRenderer();
}

void EditorViewportItem::itemChange(ItemChange change, const ItemChangeData& value) {
  QQuickRhiItem::itemChange(change, value);
  if (change == ItemVisibleHasChanged) {
    broker_->SetConsumerAvailable(value.boolValue);
    notifyDiagnosticsChanged();
  }
}

auto EditorViewportItem::takePendingFrames() -> std::deque<ViewerFrame> {
  std::lock_guard lock(mutex_);
  auto frames = std::move(pending_frames_);
  pending_frames_.clear();
  return frames;
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
  QMetaObject::invokeMethod(this, [this] { emit DiagnosticsChanged(); },
                            Qt::QueuedConnection);
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
  QMetaObject::invokeMethod(this, [this] { emit DiagnosticsChanged(); }, Qt::QueuedConnection);
}

}  // namespace alcedo::editor_rhi
