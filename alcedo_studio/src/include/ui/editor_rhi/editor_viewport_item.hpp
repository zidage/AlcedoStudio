//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QQuickRhiItem>
#include <QString>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "ui/editor_rhi/frame_presentation_broker.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

class EditorViewportRenderer;

// Production QML viewport. It owns only thread-safe submission state; QRhi
// wrappers and native target adapters are owned by EditorViewportRenderer on
// the scene-graph render thread.
class EditorViewportItem : public QQuickRhiItem {
  Q_OBJECT
  Q_PROPERTY(QString backendName READ backendName NOTIFY DiagnosticsChanged)
  Q_PROPERTY(qulonglong imageGeneration READ imageGeneration WRITE setImageGeneration
                 NOTIFY ImageGenerationChanged)
  Q_PROPERTY(qulonglong targetGeneration READ targetGeneration NOTIFY DiagnosticsChanged)
  Q_PROPERTY(qulonglong lastPresentedImageGeneration READ lastPresentedImageGeneration
                 NOTIFY DiagnosticsChanged)
  Q_PROPERTY(qulonglong droppedStaleFrameCount READ droppedStaleFrameCount
                 NOTIFY DiagnosticsChanged)
  Q_PROPERTY(int liveTargetCount READ liveTargetCount NOTIFY DiagnosticsChanged)
  Q_PROPERTY(bool presentationAvailable READ presentationAvailable NOTIFY DiagnosticsChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY StatusChanged)

 public:
  explicit EditorViewportItem(QQuickItem* parent = nullptr);
  ~EditorViewportItem() override;

  [[nodiscard]] auto backendName() const -> QString;
  [[nodiscard]] auto imageGeneration() const -> qulonglong {
    return image_generation_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto targetGeneration() const -> qulonglong;
  [[nodiscard]] auto lastPresentedImageGeneration() const -> qulonglong;
  [[nodiscard]] auto droppedStaleFrameCount() const -> qulonglong;
  [[nodiscard]] auto liveTargetCount() const -> int;
  [[nodiscard]] auto presentationAvailable() const -> bool;
  [[nodiscard]] auto statusText() const -> QString;

  void setImageGeneration(qulonglong generation);

  // Pipeline-facing submission API. These methods are safe from worker
  // threads and never map a QRhi target or touch a QRhi resource.
  void submitFrame(const ViewerFrame& frame);
  auto tryAcquireWritableTarget() -> std::optional<WritableTargetLease>;
  auto submitCompletedFrame(const CompletedFrameLease& frame) -> bool;

  void setViewState(const ViewerViewState& state);
  [[nodiscard]] auto broker() const -> const std::shared_ptr<FramePresentationBroker>& {
    return broker_;
  }

  Q_INVOKABLE void requestRendererInvalidation();
  Q_INVOKABLE void cancelPendingFrames();

  // Called by the application composition root before loading QML. The
  // registration is idempotent and also makes visible-window QML tests use the
  // same real type.
  static void RegisterQmlType();

 signals:
  void DiagnosticsChanged();
  void ImageGenerationChanged();
  void StatusChanged();

 protected:
  auto createRenderer() -> QQuickRhiItemRenderer* override;
  void itemChange(ItemChange change, const ItemChangeData& value) override;

 private:
  friend class EditorViewportRenderer;

  [[nodiscard]] auto takePendingFrames() -> std::deque<ViewerFrame>;
  [[nodiscard]] auto viewStateSnapshot() const -> ViewerViewState;
  void setBackendName(const QString& name);
  void setStatusText(const QString& text);
  void notifyDiagnosticsChanged();

  std::shared_ptr<FramePresentationBroker> broker_;
  mutable std::mutex mutex_;
  std::deque<ViewerFrame> pending_frames_;
  ViewerViewState view_state_{};
  QString backend_name_ = QStringLiteral("uninitialized");
  QString status_text_ = QStringLiteral("waiting for a compatible frame");
  std::atomic<qulonglong> image_generation_{0};
};

void RegisterEditorViewportQmlTypes();

}  // namespace alcedo::editor_rhi
