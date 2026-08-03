//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace alcedo::editor_rhi {

// Startup-only graphics/pipeline pair. No hot-switch after the first QQuickWindow.
enum class EditorBackend {
  Cuda,    // Windows: CUDA pipeline + Qt Quick Direct3D 11
  OpenCl,  // Windows: OpenCL pipeline + Qt Quick OpenGL
  Metal,   // macOS: Metal pipeline + Qt Quick Metal
};

struct EditorBackendParseResult {
  std::optional<EditorBackend> backend;
  std::string                  error;
  bool                         present = false;  // true when --editor-backend was provided
};

// Accepts "cuda", "opencl", "metal" (case-insensitive). Empty string is invalid.
[[nodiscard]] auto ParseEditorBackendToken(std::string_view token)
    -> std::optional<EditorBackend>;

// Scans argv for --editor-backend=VALUE or --editor-backend VALUE.
[[nodiscard]] auto ParseEditorBackendArgs(int argc, char** argv) -> EditorBackendParseResult;

[[nodiscard]] auto ToString(EditorBackend backend) -> const char*;
[[nodiscard]] auto QtGraphicsApiName(EditorBackend backend) -> const char*;
[[nodiscard]] auto IsBackendSupportedOnThisPlatform(EditorBackend backend) -> bool;
[[nodiscard]] auto IsBackendAvailableInThisBuild(EditorBackend backend) -> bool;

// Default when packaging does not pass an explicit flag. CUDA preferred on Windows
// when built; Metal on macOS; OpenCL as Windows secondary.
[[nodiscard]] auto DefaultEditorBackendForPlatform() -> std::optional<EditorBackend>;

// Process-wide backend selected by ApplyEditorBackendBeforeWindow / production
// main. EditorViewportItem and DirectPresentQueue must use this value so
// the lease pool matches the active QRhi and pipeline backend.
void SetActiveEditorBackend(EditorBackend backend);
[[nodiscard]] auto ActiveEditorBackend() -> EditorBackend;
[[nodiscard]] auto HasActiveEditorBackend() -> bool;

}  // namespace alcedo::editor_rhi
