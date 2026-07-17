//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QQuickRhiItem>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "ui/editor_rhi/frame_presentation_broker.hpp"
#include "ui/viewer/viewer_view_state.hpp"

class QQuickWindow;

namespace alcedo::editor_rhi {

class EditorViewportRenderer;
class LeaseFrameSink;

// Production QML viewport. It owns only thread-safe submission state; QRhi
// wrappers and native target adapters are owned by EditorViewportRenderer on
// the scene-graph render thread.
//
// There is no host-upload presentation path. Pipeline workers acquire leases
// via tryAcquireWritableTarget / submitCompletedFrame (or LeaseFrameSink).
class EditorViewportItem : public QQuickRhiItem {
  Q_OBJECT
  Q_PROPERTY(QString backendName READ backendName NOTIFY DiagnosticsChanged)
  // Durable image id (DB). Distinct from imageGeneration (session counter).
  Q_PROPERTY(qulonglong imageIdentity READ imageIdentity WRITE setImageIdentity
                 NOTIFY ImageIdentityChanged)
  // Monotonic session generation; advances on every open/switch including A→B→A.
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
  [[nodiscard]] auto imageIdentity() const -> qulonglong {
    return image_identity_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto imageGeneration() const -> qulonglong {
    return image_generation_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto targetGeneration() const -> qulonglong;
  [[nodiscard]] auto lastPresentedImageGeneration() const -> qulonglong;
  [[nodiscard]] auto droppedStaleFrameCount() const -> qulonglong;
  [[nodiscard]] auto liveTargetCount() const -> int;
  [[nodiscard]] auto presentationAvailable() const -> bool;
  [[nodiscard]] auto statusText() const -> QString;

  void setImageIdentity(qulonglong identity);
  void setImageGeneration(qulonglong generation);
  // Atomically advances session generation for a new focus (A→B or A→A reopen).
  Q_INVOKABLE void beginImageSession(qulonglong imageIdentity);

  // Pipeline-facing submission API. Safe from worker threads; never maps a
  // QRhi target or touches a QRhi resource. update() is always queued to GUI.
  auto tryAcquireWritableTarget(const WritableTargetRequest& request)
      -> std::optional<WritableTargetLease>;
  auto tryAcquireWritableTarget() -> std::optional<WritableTargetLease>;
  auto submitCompletedFrame(const CompletedFrameLease& frame) -> bool;
  void abandonProducerWrite(const WritableTargetLease& lease);

  void setViewState(const ViewerViewState& state);
  // QML/session bridge: update zoom/pan without touching native targets or
  // the frame broker. Does not recreate the viewport texture.
  Q_INVOKABLE void setViewTransform(float zoom, float panX, float panY);
  [[nodiscard]] auto broker() const -> const std::shared_ptr<FramePresentationBroker>& {
    return broker_;
  }

  // IFrameSink bridge used by the edit pipeline. Owned by the item; safe to
  // keep while the viewport exists.
  [[nodiscard]] auto frameSink() -> LeaseFrameSink*;

  Q_INVOKABLE void requestRendererInvalidation();
  Q_INVOKABLE void cancelPendingFrames();
  // Re-evaluate window exposure / minimize state (also used by tests after
  // programmatic hide/show where Qt may not emit the expected signals promptly).
  Q_INVOKABLE void refreshPresentationAvailability();
  // Request a render-thread pass only when content/state actually changed.
  void requestPresentUpdate();

  // Called by the application composition root before loading QML. The
  // registration is idempotent and also makes visible-window QML tests use the
  // same real type.
  static void RegisterQmlType();

 signals:
  void DiagnosticsChanged();
  void ImageIdentityChanged();
  void ImageGenerationChanged();
  void StatusChanged();
  void TargetSizeRequested(int width, int height);

 protected:
  auto createRenderer() -> QQuickRhiItemRenderer* override;
  void itemChange(ItemChange change, const ItemChangeData& value) override;
  void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

 private:
  friend class EditorViewportRenderer;
  friend class LeaseFrameSink;

  [[nodiscard]] auto viewStateSnapshot() const -> ViewerViewState;
  void setBackendName(const QString& name);
  void setStatusText(const QString& text);
  void notifyDiagnosticsChanged();
  void refreshConsumerAvailability();
  void attachWindow(QQuickWindow* window);
  void detachWindow();
  void onWindowVisibilityChanged();
  void onWindowSceneGraphInvalidated();
  void onWindowSceneGraphInitialized();
  void requestPresentUpdateOnGuiThread();

  std::shared_ptr<FramePresentationBroker> broker_;
  std::unique_ptr<LeaseFrameSink> frame_sink_;
  mutable std::mutex mutex_;
  ViewerViewState view_state_{};
  QString backend_name_ = QStringLiteral("uninitialized");
  QString status_text_ = QStringLiteral("waiting for a compatible frame");
  std::atomic<qulonglong> image_identity_{0};
  std::atomic<qulonglong> image_generation_{0};
  QQuickWindow* attached_window_ = nullptr;
  bool scene_graph_ready_ = false;
  bool update_pending_ = false;
  bool last_diagnostics_available_ = false;
  qulonglong last_diag_target_gen_ = 0;
  qulonglong last_diag_dropped_ = 0;
  int last_diag_live_targets_ = 0;
};

void RegisterEditorViewportQmlTypes();

}  // namespace alcedo::editor_rhi
