//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QtQml>

#include <cstdarg>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/harness_fixtures.hpp"
#include "ui/editor_rhi/harness_viewport_item.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"

namespace {

using alcedo::editor_rhi::EditorBackend;
using alcedo::editor_rhi::HarnessFixtureKind;
using alcedo::editor_rhi::HarnessViewportItem;
using alcedo::editor_rhi::NativeResourceCounters;

enum class HarnessCase {
  DirectPresentation,
  ResizeChurn,
  HideShow,
  MinimizeRestore,
  RendererRecreation,
  HdrFormatQuery,
  ShutdownQueued,
};

void Log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
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

auto ParseCase(std::string_view token) -> std::optional<HarnessCase> {
  if (token == "direct-presentation") {
    return HarnessCase::DirectPresentation;
  }
  if (token == "resize-churn") {
    return HarnessCase::ResizeChurn;
  }
  if (token == "hide-show") {
    return HarnessCase::HideShow;
  }
  if (token == "minimize-restore") {
    return HarnessCase::MinimizeRestore;
  }
  if (token == "renderer-recreation") {
    return HarnessCase::RendererRecreation;
  }
  if (token == "hdr-format-query") {
    return HarnessCase::HdrFormatQuery;
  }
  if (token == "shutdown-queued") {
    return HarnessCase::ShutdownQueued;
  }
  return std::nullopt;
}

auto CaseName(HarnessCase c) -> const char* {
  switch (c) {
    case HarnessCase::DirectPresentation:
      return "direct-presentation";
    case HarnessCase::ResizeChurn:
      return "resize-churn";
    case HarnessCase::HideShow:
      return "hide-show";
    case HarnessCase::MinimizeRestore:
      return "minimize-restore";
    case HarnessCase::RendererRecreation:
      return "renderer-recreation";
    case HarnessCase::HdrFormatQuery:
      return "hdr-format-query";
    case HarnessCase::ShutdownQueued:
      return "shutdown-queued";
  }
  return "unknown";
}

void PrintUsage() {
  Log("EditorRhiHarness --editor-backend=cuda|opencl|metal "
      "[--case=direct-presentation|resize-churn|hide-show|minimize-restore|"
      "renderer-recreation|hdr-format-query|shutdown-queued] "
      "[--fixture=gradient|checkerboard|roi|odd]");
}

auto CompareReadback(HarnessViewportItem* viewport) -> int {
  if (!viewport) {
    return 2;
  }
  const auto expected = viewport->ExpectedFixture();
  auto       actual   = viewport->TakeReadback();
  if (!actual.has_value()) {
    Log("EditorRhiHarness: no readback available");
    return 3;
  }
  const float err = alcedo::editor_rhi::MaxAbsPixelError(
      expected, actual->data(), actual->width, actual->height, actual->row_bytes());
  if (err < 0.0f) {
    Log("EditorRhiHarness: readback dimension mismatch expected=%dx%d got=%dx%d", expected.width,
        expected.height, actual->width, actual->height);
    return 4;
  }
  Log("EditorRhiHarness: max_abs_pixel_error=%.6f tolerance=%.6f", err,
      alcedo::editor_rhi::kHarnessPixelAbsTolerance);
  if (err > alcedo::editor_rhi::kHarnessPixelAbsTolerance) {
    Log("EditorRhiHarness: pixel compare failed");
    return 5;
  }
  return 0;
}

struct HarnessState {
  QQuickWindow*        window   = nullptr;
  QQuickItem*          root     = nullptr;
  HarnessViewportItem* viewport = nullptr;
  HarnessCase          harness_case = HarnessCase::DirectPresentation;
  EditorBackend        backend  = EditorBackend::Cuda;
  int                  exit_code = 0;
  int                  stage     = 0;
  std::uint64_t        frames_at_stage = 0;
  int                  resize_i  = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
  using namespace alcedo::editor_rhi;
  setvbuf(stderr, nullptr, _IONBF, 0);
  setvbuf(stdout, nullptr, _IONBF, 0);

  const auto backend_parse = ParseEditorBackendArgs(argc, argv);
  if (backend_parse.present && !backend_parse.error.empty()) {
    Log("EditorRhiHarness: %s", backend_parse.error.c_str());
    PrintUsage();
    return 1;
  }

  EditorBackend backend;
  if (backend_parse.backend.has_value()) {
    backend = *backend_parse.backend;
  } else if (const auto def = DefaultEditorBackendForPlatform()) {
    backend = *def;
    Log("EditorRhiHarness: using default backend %s", ToString(backend));
  } else {
    Log("EditorRhiHarness: no --editor-backend and no default for this platform");
    PrintUsage();
    return 1;
  }

  HarnessCase harness_case = HarnessCase::DirectPresentation;
  if (const auto case_arg = FindArgValue(argc, argv, "--case")) {
    const auto parsed = ParseCase(*case_arg);
    if (!parsed) {
      Log("EditorRhiHarness: unknown --case value");
      PrintUsage();
      return 1;
    }
    harness_case = *parsed;
  }

  HarnessFixtureKind fixture = HarnessFixtureKind::Fp32Gradient;
  if (const auto fixture_arg = FindArgValue(argc, argv, "--fixture")) {
    const auto parsed = ParseHarnessFixtureKind(*fixture_arg);
    if (!parsed) {
      Log("EditorRhiHarness: unknown --fixture value");
      return 1;
    }
    fixture = *parsed;
  }

  Log("EditorRhiHarness: backend=%s case=%s fixture=%s raw_fixture=%s", ToString(backend),
      CaseName(harness_case), ToString(fixture), SmallRealRawFixtureRelativePath().c_str());

#if defined(Q_OS_WIN)
  if (backend == EditorBackend::OpenCl) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }
#endif

  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("EditorRhiHarness"));
  QCoreApplication::setOrganizationName(QStringLiteral("Alcedo"));
  // Keep the event loop alive after window.close() so finish() can report exit code.
  app.setQuitOnLastWindowClosed(false);
  Log("EditorRhiHarness: QGuiApplication created");

  const EditorStartupResult startup = ApplyEditorBackendBeforeWindow(backend);
  if (!startup.ok) {
    Log("EditorRhiHarness: startup failed before window: %s", startup.error.c_str());
    return 10;
  }
  Log("EditorRhiHarness: startup ok api=%s adapter=%s cuda_device=%d luid=%s opencl_gl=%d",
      startup.diagnostics.qt_graphics_api.c_str(),
      startup.diagnostics.adapter_description.c_str(), startup.diagnostics.cuda_device_index,
      startup.diagnostics.cuda_luid.c_str(), startup.diagnostics.opencl_gl_sharing ? 1 : 0);
  if (!startup.diagnostics.notes.empty()) {
    Log("EditorRhiHarness: %s", startup.diagnostics.notes.c_str());
  }

  qmlRegisterType<HarnessViewportItem>("Alcedo.EditorRhiHarness", 1, 0, "HarnessViewportItem");

  QQuickWindow window;
  BindEditorGraphicsToWindow(&window, startup);
  window.setTitle(QStringLiteral("EditorRhiHarness"));
  window.resize(960, 720);
  window.setColor(QColor(16, 16, 20));

  auto* root = new QQuickItem;
  root->setParentItem(window.contentItem());
  root->setSize(window.size());
  QObject::connect(&window, &QQuickWindow::widthChanged, root,
                   [root, &window]() { root->setWidth(window.width()); });
  QObject::connect(&window, &QQuickWindow::heightChanged, root,
                   [root, &window]() { root->setHeight(window.height()); });

  auto* viewport = new HarnessViewportItem(root);
  viewport->setParentItem(root);
  viewport->setSize(QSizeF(640, 480));
  viewport->setX((960 - 640) / 2.0);
  viewport->setY((720 - 480) / 2.0);
  viewport->configure(backend, fixture, /*request_readback=*/true);

  NativeResourceCounters::Instance().ResetForTest();

  HarnessState state;
  state.window       = &window;
  state.root         = root;
  state.viewport     = viewport;
  state.harness_case = harness_case;
  state.backend      = backend;

  // Hard timeout so a stuck GPU path cannot hang CI forever.
  QTimer::singleShot(45000, &app, [&state]() {
    if (state.exit_code == 0 && state.stage < 100) {
      Log("EditorRhiHarness: hard timeout");
      state.exit_code = 99;
      QCoreApplication::exit(state.exit_code);
    }
  });

  auto finish = [&](int code) {
    if (state.stage >= 100) {
      return;
    }
    state.exit_code = code;
    state.stage     = 100;
    // Do not delete QQuickRhiItem synchronously from the GUI thread while the
    // threaded scene graph may still be rendering — that deadlocks. Close the
    // window and let Qt destroy items with the QObject tree at process exit.
    if (state.window) {
      state.window->close();
    }
    // Allow one render-thread invalidate cycle, then report counters and quit.
    QTimer::singleShot(250, &app, [&state]() {
      const auto live = NativeResourceCounters::Instance().LiveTotal();
      Log("EditorRhiHarness: live_resources_at_teardown total=%lld shared=%lld imported=%lld "
          "extmem=%lld opencl_images=%lld",
          static_cast<long long>(live),
          static_cast<long long>(NativeResourceCounters::Instance().LiveSharedTextures()),
          static_cast<long long>(NativeResourceCounters::Instance().LiveImportedQRhiTextures()),
          static_cast<long long>(NativeResourceCounters::Instance().LiveExternalMemories()),
          static_cast<long long>(NativeResourceCounters::Instance().LiveOpenClImages()));
      // Live counts may remain non-zero until process destruction of the window
      // tree; treat that as a soft diagnostic here. Hard leak fail is reserved
      // for a rising counter across repeated cases in CI soak jobs.
      if (state.exit_code == 0) {
        Log("EditorRhiHarness: PASS backend=%s case=%s", ToString(state.backend),
            CaseName(state.harness_case));
      } else {
        Log("EditorRhiHarness: FAIL code=%d backend=%s case=%s", state.exit_code,
            ToString(state.backend), CaseName(state.harness_case));
      }
      QCoreApplication::exit(state.exit_code);
    });
  };

  QObject::connect(viewport, &HarnessViewportItem::harnessFramePresented, &app, [&]() {
    if (state.stage != 0 || !state.viewport) {
      return;
    }
    if (!state.viewport->presentationOk()) {
      return;
    }
    Log("EditorRhiHarness: presentation ok frames=%llu status=%s",
        static_cast<unsigned long long>(state.viewport->RenderFrameCount()),
        qPrintable(state.viewport->statusText()));
    state.stage = 1;

    if (state.harness_case == HarnessCase::HdrFormatQuery) {
      const std::string diag = QueryOpenGlHdrFormatSupportDiagnostic();
      std::fputs(diag.c_str(), stderr);
      std::fflush(stderr);
    }

    switch (state.harness_case) {
      case HarnessCase::DirectPresentation:
      case HarnessCase::HdrFormatQuery:
      case HarnessCase::ShutdownQueued: {
        QTimer::singleShot(500, &app, [&]() {
          if (!state.viewport) {
            return;
          }
          if (!state.viewport->readbackReady()) {
            // Give readback one more frame cycle.
            QTimer::singleShot(500, &app, [&]() {
              finish(state.viewport ? CompareReadback(state.viewport) : 2);
            });
            return;
          }
          finish(CompareReadback(state.viewport));
        });
        break;
      }
      case HarnessCase::ResizeChurn: {
        state.resize_i = 0;
        state.stage    = 2;
        break;
      }
      case HarnessCase::HideShow: {
        state.window->hide();
        QTimer::singleShot(100, state.window, [w = state.window]() { w->show(); });
        QTimer::singleShot(800, &app, [&]() {
          finish(state.viewport && state.viewport->presentationOk() ? 0 : 13);
        });
        break;
      }
      case HarnessCase::MinimizeRestore: {
        state.window->showMinimized();
        QTimer::singleShot(100, state.window, [w = state.window]() { w->showNormal(); });
        QTimer::singleShot(800, &app, [&]() {
          finish(state.viewport && state.viewport->presentationOk() ? 0 : 14);
        });
        break;
      }
      case HarnessCase::RendererRecreation: {
        state.frames_at_stage = state.viewport->RenderFrameCount();
        state.viewport->requestRendererInvalidate();
        state.stage = 3;
        break;
      }
    }
  });

  // Resize churn driven by timer while stage==2.
  auto* resize_timer = new QTimer(&app);
  QObject::connect(resize_timer, &QTimer::timeout, &app, [&]() {
    if (state.stage != 2 || !state.viewport || !state.window) {
      return;
    }
    if (state.resize_i >= 8) {
      resize_timer->stop();
      finish(state.viewport->presentationOk() ? 0 : 12);
      return;
    }
    const int w = 320 + (state.resize_i % 4) * 40;
    const int h = 240 + (state.resize_i % 3) * 30;
    state.viewport->requestLifecycleResize(w, h);
    state.window->resize(w + 100, h + 100);
    ++state.resize_i;
  });

  // Renderer recreation completion.
  QObject::connect(viewport, &HarnessViewportItem::harnessFramePresented, &app, [&]() {
    if (state.stage != 3 || !state.viewport) {
      return;
    }
    if (state.viewport->RenderFrameCount() > state.frames_at_stage + 1) {
      state.stage = 4;
      QTimer::singleShot(300, &app, [&]() {
        finish(state.viewport ? CompareReadback(state.viewport) : 2);
      });
    }
  });

  // Presentation failure path.
  QTimer::singleShot(20000, &app, [&]() {
    if (state.stage == 0) {
      if (state.viewport && !state.viewport->LastError().isEmpty()) {
        Log("EditorRhiHarness: viewport error: %s", qPrintable(state.viewport->LastError()));
      }
      Log("EditorRhiHarness: presentation did not become ready (frames=%llu)",
          static_cast<unsigned long long>(state.viewport ? state.viewport->RenderFrameCount()
                                                         : 0));
      finish(11);
    }
  });

  window.show();
  Log("EditorRhiHarness: window shown, entering event loop");
  if (harness_case == HarnessCase::ResizeChurn) {
    // Start resize timer after first present; it no-ops until stage==2.
    resize_timer->start(150);
  }

  return app.exec();
}
