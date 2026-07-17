//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_backend.hpp"

#include <atomic>
#include <cctype>
#include <cstring>

namespace alcedo::editor_rhi {
namespace {

std::atomic<int> g_active_backend{-1};

auto ToLowerAscii(std::string_view in) -> std::string {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

auto FindArgValue(int argc, char** argv, std::string_view option_name)
    -> std::optional<std::string_view> {
  const std::string opt_eq = std::string(option_name) + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i] ? argv[i] : "");
    if (arg == option_name) {
      if (i + 1 < argc && argv[i + 1]) {
        return std::string_view(argv[i + 1]);
      }
      return std::nullopt;
    }
    if (arg.rfind(opt_eq, 0) == 0) {
      return arg.substr(opt_eq.size());
    }
  }
  return std::nullopt;
}

}  // namespace

auto ParseEditorBackendToken(std::string_view token) -> std::optional<EditorBackend> {
  const std::string lower = ToLowerAscii(token);
  if (lower == "cuda") {
    return EditorBackend::Cuda;
  }
  if (lower == "opencl") {
    return EditorBackend::OpenCl;
  }
  if (lower == "metal") {
    return EditorBackend::Metal;
  }
  return std::nullopt;
}

auto ParseEditorBackendArgs(int argc, char** argv) -> EditorBackendParseResult {
  EditorBackendParseResult result;
  const auto               value = FindArgValue(argc, argv, "--editor-backend");
  if (!value.has_value()) {
    return result;
  }
  result.present = true;
  if (value->empty()) {
    result.error = "empty --editor-backend value";
    return result;
  }
  result.backend = ParseEditorBackendToken(*value);
  if (!result.backend.has_value()) {
    result.error = "invalid --editor-backend value \"" + std::string(*value) +
                   "\"; expected cuda, opencl, or metal";
  }
  return result;
}

auto ToString(EditorBackend backend) -> const char* {
  switch (backend) {
    case EditorBackend::Cuda:
      return "cuda";
    case EditorBackend::OpenCl:
      return "opencl";
    case EditorBackend::Metal:
      return "metal";
  }
  return "unknown";
}

auto QtGraphicsApiName(EditorBackend backend) -> const char* {
  switch (backend) {
    case EditorBackend::Cuda:
      return "Direct3D11";
    case EditorBackend::OpenCl:
      return "OpenGL";
    case EditorBackend::Metal:
      return "Metal";
  }
  return "unknown";
}

auto IsBackendSupportedOnThisPlatform(EditorBackend backend) -> bool {
#if defined(_WIN32)
  return backend == EditorBackend::Cuda || backend == EditorBackend::OpenCl;
#elif defined(__APPLE__)
  return backend == EditorBackend::Metal;
#else
  (void)backend;
  return false;
#endif
}

auto IsBackendAvailableInThisBuild(EditorBackend backend) -> bool {
  if (!IsBackendSupportedOnThisPlatform(backend)) {
    return false;
  }
  switch (backend) {
    case EditorBackend::Cuda:
#if defined(HAVE_CUDA)
      return true;
#else
      return false;
#endif
    case EditorBackend::OpenCl:
#if defined(HAVE_OPENCL)
      return true;
#else
      return false;
#endif
    case EditorBackend::Metal:
#if defined(HAVE_METAL)
      return true;
#else
      return false;
#endif
  }
  return false;
}

auto DefaultEditorBackendForPlatform() -> std::optional<EditorBackend> {
#if defined(_WIN32)
#if defined(HAVE_CUDA)
  return EditorBackend::Cuda;
#elif defined(HAVE_OPENCL)
  return EditorBackend::OpenCl;
#else
  return std::nullopt;
#endif
#elif defined(__APPLE__)
#if defined(HAVE_METAL)
  return EditorBackend::Metal;
#else
  return std::nullopt;
#endif
#else
  return std::nullopt;
#endif
}

void SetActiveEditorBackend(EditorBackend backend) {
  g_active_backend.store(static_cast<int>(backend), std::memory_order_release);
}

auto ActiveEditorBackend() -> EditorBackend {
  const int value = g_active_backend.load(std::memory_order_acquire);
  if (value < 0) {
    if (const auto def = DefaultEditorBackendForPlatform()) {
      return *def;
    }
    return EditorBackend::Cuda;
  }
  return static_cast<EditorBackend>(value);
}

auto HasActiveEditorBackend() -> bool {
  return g_active_backend.load(std::memory_order_acquire) >= 0;
}

}  // namespace alcedo::editor_rhi
