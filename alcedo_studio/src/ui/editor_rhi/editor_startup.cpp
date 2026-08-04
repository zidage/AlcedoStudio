//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_startup.hpp"

#include <QtGui/rhi/qrhi.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <memory>
#include <sstream>

#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
#include <QtGui/qopenglcontext_platform.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <GL/gl.h>
#include <windows.h>

#include "opencl/opencl_runtime.hpp"
#endif

namespace alcedo::editor_rhi {
namespace {

#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
class OpenClGlSharingBootstrap {
 public:
  auto Initialize() -> bool {
    if (initialized_) {
      return true;
    }

    context_ = std::make_unique<QOpenGLContext>();
    if (auto* global_share_context = QOpenGLContext::globalShareContext()) {
      context_->setShareContext(global_share_context);
      context_->setFormat(global_share_context->format());
    }
    if (!context_->create()) {
      error_ = "failed to create hidden OpenGL context for OpenCL sharing";
      context_.reset();
      return false;
    }

    surface_ = std::make_unique<QOffscreenSurface>();
    surface_->setFormat(context_->format());
    surface_->create();
    if (!surface_->isValid() || !context_->makeCurrent(surface_.get())) {
      error_ = "failed to make hidden OpenGL context current for OpenCL sharing";
      surface_.reset();
      context_.reset();
      return false;
    }

    auto* native_context = context_->nativeInterface<QNativeInterface::QWGLContext>();
    HGLRC hglrc          = native_context ? native_context->nativeContext() : nullptr;
    HDC   hdc            = wglGetCurrentDC();
    if (hglrc == nullptr || hdc == nullptr) {
      error_ = "failed to resolve WGL context handles for OpenCL sharing";
      context_->doneCurrent();
      surface_.reset();
      context_.reset();
      return false;
    }

    alcedo::OpenClInitializationOptions options;
    options.gl_context        = hglrc;
    options.gl_device_context = hdc;
    initialized_              = alcedo::TryInitializeOpenClRuntime(options);
    context_->doneCurrent();

    if (!initialized_) {
      error_ = "TryInitializeOpenClRuntime failed with OpenGL sharing options";
      surface_.reset();
      context_.reset();
    }
    return initialized_;
  }

  [[nodiscard]] auto error() const -> const std::string& { return error_; }

 private:
  std::unique_ptr<QOpenGLContext>    context_;
  std::unique_ptr<QOffscreenSurface> surface_;
  bool                               initialized_ = false;
  std::string                        error_;
};

// Keep bootstrap alive for process lifetime so the share group remains valid.
auto SharedOpenClGlBootstrap() -> OpenClGlSharingBootstrap& {
  static OpenClGlSharingBootstrap bootstrap;
  return bootstrap;
}
#endif

}  // namespace

auto ApplyEditorBackendBeforeWindow(EditorBackend backend) -> EditorStartupResult {
  EditorStartupResult result;
  result.diagnostics.backend         = backend;
  result.diagnostics.backend_name    = ToString(backend);
  result.diagnostics.qt_graphics_api = QtGraphicsApiName(backend);

  if (!IsBackendSupportedOnThisPlatform(backend)) {
    result.error =
        std::string("editor backend ") + ToString(backend) + " is not supported on this platform";
    return result;
  }
  if (!IsBackendAvailableInThisBuild(backend)) {
    result.error = std::string("editor backend ") + ToString(backend) +
                   " is not available in this build (missing compile feature)";
    return result;
  }

  switch (backend) {
    case EditorBackend::Cuda: {
#if defined(_WIN32) && defined(HAVE_CUDA)
      QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);

      const auto discovery = DiscoverCudaAdapters();
      if (!discovery.ok || !discovery.preferred.has_value()) {
        result.error = discovery.error.empty() ? "CUDA adapter discovery failed" : discovery.error;
        return result;
      }
      result.cuda_adapter = discovery.preferred;

      const auto dxgi     = FindDxgiAdapterForLuid(discovery.preferred->luid);
      if (!dxgi.has_value()) {
        result.error = "CUDA preferred device LUID has no matching DXGI adapter";
        return result;
      }

      result.diagnostics.cuda_device_index   = discovery.preferred->device_index;
      result.diagnostics.cuda_device_name    = discovery.preferred->name;
      result.diagnostics.cuda_luid           = DescribeLuid(discovery.preferred->luid);
      result.diagnostics.dxgi_luid           = DescribeLuid(dxgi->luid);
      result.diagnostics.adapter_description = dxgi->description;
      result.diagnostics.notes =
          "CUDA/D3D11 adapter LUID matched before first QQuickWindow; "
          "bind via QQuickGraphicsDevice::fromAdapter on the window";
      result.ok = true;
      SetActiveEditorBackend(backend);
      return result;
#else
      result.error = "CUDA backend requires Windows + HAVE_CUDA";
      return result;
#endif
    }
    case EditorBackend::OpenCl: {
#if defined(Q_OS_WIN) && defined(HAVE_OPENCL)
      // The composition root must enable sharing before constructing the
      // application. Reapplying this attribute here is too late and produces a
      // misleading Qt warning even when the pre-application setup was correct.
      if (!QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts)) {
        result.error = "OpenCL backend requires Qt::AA_ShareOpenGLContexts before QGuiApplication";
        return result;
      }
      QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

      auto& bootstrap = SharedOpenClGlBootstrap();
      if (!bootstrap.Initialize()) {
        result.error = bootstrap.error().empty() ? "OpenCL/OpenGL sharing bootstrap failed"
                                                 : bootstrap.error();
        return result;
      }
      result.diagnostics.opencl_gl_sharing = true;
      result.diagnostics.notes =
          "OpenCL initialized with OpenGL sharing; Qt Quick uses OpenGL render loop";
      result.ok = true;
      SetActiveEditorBackend(backend);
      return result;
#else
      result.error = "OpenCL backend requires Windows + HAVE_OPENCL";
      return result;
#endif
    }
    case EditorBackend::Metal: {
#if defined(__APPLE__) && defined(HAVE_METAL)
      QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
      result.diagnostics.notes =
          "Metal API selected; shared-texture presentation qualified by the macOS RHI harness";
      result.ok = true;
      SetActiveEditorBackend(backend);
      return result;
#else
      result.error = "Metal backend requires macOS + HAVE_METAL";
      return result;
#endif
    }
  }

  result.error = "unknown editor backend";
  return result;
}

void BindEditorGraphicsToWindow(QQuickWindow* window, const EditorStartupResult& startup) {
  if (!window || !startup.ok) {
    return;
  }
#if defined(_WIN32) && defined(HAVE_CUDA)
  if (startup.diagnostics.backend == EditorBackend::Cuda && startup.cuda_adapter.has_value()) {
    const LUID& luid = startup.cuda_adapter->luid;
    window->setGraphicsDevice(QQuickGraphicsDevice::fromAdapter(
        static_cast<quint32>(luid.LowPart), static_cast<qint32>(luid.HighPart)));
  }
#else
  (void)startup;
#endif
}

auto QueryOpenGlHdrFormatSupportDiagnostic() -> std::string {
  std::ostringstream oss;
  oss << "OpenGL HDR format support probe (Phase 9 input, not a renderer switch):\n";

  QRhiGles2InitParams gl_params;
  gl_params.format = QSurfaceFormat::defaultFormat();
  QOffscreenSurface surface;
  surface.setFormat(gl_params.format);
  surface.create();
  if (!surface.isValid()) {
    oss << "  status=unavailable reason=offscreen_surface_invalid\n";
    return oss.str();
  }
  gl_params.fallbackSurface = &surface;

  std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &gl_params));
  if (!rhi) {
    oss << "  status=unavailable reason=failed_to_create_opengl_qrhi\n";
    return oss.str();
  }

  const bool rgba16f = rhi->isTextureFormatSupported(QRhiTexture::RGBA16F);
  auto       report  = [&](QRhiSwapChain::Format format, const char* name) {
    bool format_ok = false;
    if (QRhiSwapChain* sc = rhi->newSwapChain()) {
      format_ok = sc->isFormatSupported(format);
      delete sc;
    }
    oss << "  format=" << name << " isFormatSupported=" << (format_ok ? "true" : "false")
        << " rgba16f_texture=" << (rgba16f ? "true" : "false") << "\n";
  };

  report(QRhiSwapChain::SDR, "SDR");
  report(QRhiSwapChain::HDRExtendedSrgbLinear, "HDRExtendedSrgbLinear");
  report(QRhiSwapChain::HDR10, "HDR10");
  report(QRhiSwapChain::HDRExtendedDisplayP3Linear, "HDRExtendedDisplayP3Linear");
  oss << "  note=record_on_representative_NVIDIA_AMD_HDR_and_SDR_displays\n";
  return oss.str();
}

}  // namespace alcedo::editor_rhi
