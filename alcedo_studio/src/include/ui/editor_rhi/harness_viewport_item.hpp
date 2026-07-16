//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QQuickRhiItem>
#include <QString>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/harness_fixtures.hpp"

namespace alcedo::editor_rhi {

// Minimal QQuickRhiItem used by EditorRhiHarness. Renders an RGBA32F viewport fed
// by native CUDA/D3D11 or OpenCL/OpenGL shared textures with no host presentation copy.
class HarnessViewportItem : public QQuickRhiItem {
  Q_OBJECT
  Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
  Q_PROPERTY(bool presentationOk READ presentationOk NOTIFY presentationOkChanged)
  Q_PROPERTY(bool readbackReady READ readbackReady NOTIFY readbackReadyChanged)

 public:
  explicit HarnessViewportItem(QQuickItem* parent = nullptr);

  void configure(EditorBackend backend, HarnessFixtureKind fixture_kind, bool request_readback);

  [[nodiscard]] auto statusText() const -> QString;
  [[nodiscard]] auto presentationOk() const -> bool;
  [[nodiscard]] auto readbackReady() const -> bool;

  // Thread-safe snapshot of the last pre-composition readback (RGBA32F).
  [[nodiscard]] auto TakeReadback() -> std::optional<HarnessFixtureImage>;
  [[nodiscard]] auto ExpectedFixture() const -> HarnessFixtureImage;

  void requestLifecycleResize(int width, int height);
  void requestRendererInvalidate();

  // Observed by the harness main loop for lifecycle cases.
  [[nodiscard]] auto RenderFrameCount() const -> std::uint64_t;
  [[nodiscard]] auto LastError() const -> QString;

 signals:
  void statusTextChanged();
  void presentationOkChanged();
  void readbackReadyChanged();
  void harnessFramePresented();

 protected:
  auto createRenderer() -> QQuickRhiItemRenderer* override;

 private:
  friend class HarnessViewportRenderer;

  void setStatusText(const QString& text);
  void setPresentationOk(bool ok);
  void setReadbackReady(bool ready);
  void setLastError(const QString& error);
  void publishReadback(HarnessFixtureImage image);
  void noteFramePresented();

  EditorBackend      backend_          = EditorBackend::Cuda;
  HarnessFixtureKind fixture_kind_     = HarnessFixtureKind::Fp32Gradient;
  bool               request_readback_ = true;
  HarnessFixtureImage expected_{};

  mutable std::mutex mutex_;
  QString            status_text_ = QStringLiteral("idle");
  QString            last_error_;
  bool               presentation_ok_ = false;
  bool               readback_ready_  = false;
  std::optional<HarnessFixtureImage> readback_;

  std::atomic<std::uint64_t> render_frame_count_{0};
  std::atomic<int>           resize_request_w_{0};
  std::atomic<int>           resize_request_h_{0};
  std::atomic<bool>          invalidate_request_{false};
};

}  // namespace alcedo::editor_rhi
