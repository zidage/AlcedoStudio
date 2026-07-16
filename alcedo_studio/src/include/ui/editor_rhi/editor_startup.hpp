//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>

#include "ui/editor_rhi/editor_backend.hpp"

#if defined(_WIN32) && defined(HAVE_CUDA)
#include "ui/editor_rhi/cuda_adapter_discovery.hpp"
#endif

class QQuickWindow;

namespace alcedo::editor_rhi {

struct EditorStartupDiagnostics {
  EditorBackend backend = EditorBackend::Cuda;
  std::string   backend_name;
  std::string   qt_graphics_api;
  std::string   adapter_description;
  std::string   cuda_device_name;
  int           cuda_device_index = -1;
  std::string   cuda_luid;
  std::string   dxgi_luid;
  bool          opencl_gl_sharing = false;
  std::string   notes;
};

struct EditorStartupResult {
  bool                     ok = false;
  std::string              error;
  EditorStartupDiagnostics diagnostics{};
#if defined(_WIN32) && defined(HAVE_CUDA)
  std::optional<CudaAdapterInfo> cuda_adapter;
#endif
};

// Applies graphics API and platform interop setup before any QQuickWindow exists.
//
// Must be called after QGuiApplication (or QApplication) construction so Qt platform
// plugins are loaded, and before the first QQuickWindow / QQmlApplicationEngine load
// that creates a window.
//
// On CUDA: discovers CUDA LUID, finds matching DXGI adapter, records adapter for
// QQuickGraphicsDevice::fromAdapter on the first QQuickWindow.
// On OpenCL: bootstraps a shareable OpenGL context topology and initializes OpenCL
// with GL sharing when available.
// On Metal: selects Metal API (macOS only); full feasibility is Phase 8.
[[nodiscard]] auto ApplyEditorBackendBeforeWindow(EditorBackend backend) -> EditorStartupResult;

// Apply QQuickWindow::setGraphicsApi and optional QQuickGraphicsDevice adapter binding.
// Call immediately before creating/showing the first QQuickWindow. window may be null
// when only the process-wide API is needed; adapter binding requires a QQuickWindow*.
// For CUDA, pass the startup result so fromAdapter uses the discovered LUID.
void BindEditorGraphicsToWindow(QQuickWindow* window, const EditorStartupResult& startup);

// Query OpenGL HDR swapchain format support after a window/RHI exists (Phase 9 input).
// Returns a multi-line diagnostic; does not change renderers.
[[nodiscard]] auto QueryOpenGlHdrFormatSupportDiagnostic() -> std::string;

}  // namespace alcedo::editor_rhi
