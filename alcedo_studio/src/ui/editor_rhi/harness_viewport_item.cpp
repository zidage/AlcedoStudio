//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/harness_viewport_item.hpp"

#include "ui/editor_rhi/harness_viewport_renderer.hpp"

namespace alcedo::editor_rhi {

HarnessViewportItem::HarnessViewportItem(QQuickItem* parent) : QQuickRhiItem(parent) {
  setColorBufferFormat(QQuickRhiItem::TextureFormat::RGBA32F);
  // OpenGL RHI textures are often origin-flipped relative to the item; keep false
  // and correct UV orientation in the harness vertex data so readback matches
  // the producer fixture layout (top-left origin in our generators).
  setMirrorVertically(false);
  setAlphaBlending(false);
}

void HarnessViewportItem::configure(EditorBackend backend, HarnessFixtureKind fixture_kind,
                                    bool request_readback) {
  backend_          = backend;
  fixture_kind_     = fixture_kind;
  request_readback_ = request_readback;
  expected_         = MakeFixture(fixture_kind);
  // Pin the pre-composition color buffer to fixture pixel size so readback is 1:1.
  if (expected_.width > 0 && expected_.height > 0) {
    setFixedColorBufferWidth(expected_.width);
    setFixedColorBufferHeight(expected_.height);
  }
  update();
}

auto HarnessViewportItem::statusText() const -> QString {
  std::lock_guard lock(mutex_);
  return status_text_;
}

auto HarnessViewportItem::presentationOk() const -> bool {
  std::lock_guard lock(mutex_);
  return presentation_ok_;
}

auto HarnessViewportItem::readbackReady() const -> bool {
  std::lock_guard lock(mutex_);
  return readback_ready_;
}

auto HarnessViewportItem::TakeReadback() -> std::optional<HarnessFixtureImage> {
  std::lock_guard lock(mutex_);
  auto            out = std::move(readback_);
  readback_.reset();
  readback_ready_ = false;
  return out;
}

auto HarnessViewportItem::ExpectedFixture() const -> HarnessFixtureImage {
  return expected_;
}

void HarnessViewportItem::requestLifecycleResize(int width, int height) {
  resize_request_w_.store(width);
  resize_request_h_.store(height);
  setWidth(width);
  setHeight(height);
  update();
}

void HarnessViewportItem::requestRendererInvalidate() {
  invalidate_request_.store(true);
  update();
}

auto HarnessViewportItem::RenderFrameCount() const -> std::uint64_t {
  return render_frame_count_.load();
}

auto HarnessViewportItem::LastError() const -> QString {
  std::lock_guard lock(mutex_);
  return last_error_;
}

auto HarnessViewportItem::createRenderer() -> QQuickRhiItemRenderer* {
  return new HarnessViewportRenderer();
}

void HarnessViewportItem::setStatusText(const QString& text) {
  {
    std::lock_guard lock(mutex_);
    if (status_text_ == text) {
      return;
    }
    status_text_ = text;
  }
  QMetaObject::invokeMethod(this, [this]() { emit statusTextChanged(); }, Qt::QueuedConnection);
}

void HarnessViewportItem::setPresentationOk(bool ok) {
  {
    std::lock_guard lock(mutex_);
    if (presentation_ok_ == ok) {
      return;
    }
    presentation_ok_ = ok;
  }
  QMetaObject::invokeMethod(this, [this]() { emit presentationOkChanged(); }, Qt::QueuedConnection);
}

void HarnessViewportItem::setReadbackReady(bool ready) {
  {
    std::lock_guard lock(mutex_);
    if (readback_ready_ == ready) {
      return;
    }
    readback_ready_ = ready;
  }
  QMetaObject::invokeMethod(this, [this]() { emit readbackReadyChanged(); }, Qt::QueuedConnection);
}

void HarnessViewportItem::setLastError(const QString& error) {
  std::lock_guard lock(mutex_);
  last_error_ = error;
}

void HarnessViewportItem::publishReadback(HarnessFixtureImage image) {
  {
    std::lock_guard lock(mutex_);
    readback_       = std::move(image);
    readback_ready_ = true;
  }
  QMetaObject::invokeMethod(this, [this]() { emit readbackReadyChanged(); }, Qt::QueuedConnection);
}

void HarnessViewportItem::noteFramePresented() {
  render_frame_count_.fetch_add(1);
  QMetaObject::invokeMethod(this, [this]() { emit harnessFramePresented(); },
                            Qt::QueuedConnection);
}

}  // namespace alcedo::editor_rhi
