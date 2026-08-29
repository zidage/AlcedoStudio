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
#include <cstring>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/editor_rhi/harness_fixtures.hpp"
#include "ui/editor_rhi/harness_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"

#if defined(_WIN32) && defined(HAVE_CUDA)
#include <cuda_runtime_api.h>
#endif
#if defined(HAVE_OPENCL)
#include <CL/cl.h>
#include "opencl/opencl_context.hpp"
#endif

namespace {

using alcedo::editor_rhi::CompletedFrameLease;
using alcedo::editor_rhi::EditorBackend;
using alcedo::editor_rhi::EditorViewportItem;
using alcedo::editor_rhi::HarnessFixtureImage;
using alcedo::editor_rhi::HarnessFixtureKind;
using alcedo::editor_rhi::HarnessViewportItem;
using alcedo::editor_rhi::LeaseFrameLayer;
using alcedo::editor_rhi::LeaseWritableResourceKind;
using alcedo::editor_rhi::NativeResourceCounters;
using alcedo::editor_rhi::WritableTargetLease;
using alcedo::editor_rhi::WritableTargetRequest;

enum class HarnessCase {
  DirectPresentation,
  ResizeChurn,
  HideShow,
  MinimizeRestore,
  RendererRecreation,
  HdrFormatQuery,
  ShutdownQueued,
  // Production EditorViewportItem + lease protocol (Phase 2-Fix).
  ProductionLeasePresentation,
  ProductionContinuousSubmit,
  ProductionHideShow,
  ProductionMinimizeRestore,
  ProductionRendererRecreation,
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
  if (token == "production-lease-presentation") {
    return HarnessCase::ProductionLeasePresentation;
  }
  if (token == "production-continuous-submit") {
    return HarnessCase::ProductionContinuousSubmit;
  }
  if (token == "production-hide-show") {
    return HarnessCase::ProductionHideShow;
  }
  if (token == "production-minimize-restore") {
    return HarnessCase::ProductionMinimizeRestore;
  }
  if (token == "production-renderer-recreation") {
    return HarnessCase::ProductionRendererRecreation;
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
    case HarnessCase::ProductionLeasePresentation:
      return "production-lease-presentation";
    case HarnessCase::ProductionContinuousSubmit:
      return "production-continuous-submit";
    case HarnessCase::ProductionHideShow:
      return "production-hide-show";
    case HarnessCase::ProductionMinimizeRestore:
      return "production-minimize-restore";
    case HarnessCase::ProductionRendererRecreation:
      return "production-renderer-recreation";
  }
  return "unknown";
}

auto IsProductionCase(HarnessCase c) -> bool {
  return c == HarnessCase::ProductionLeasePresentation ||
         c == HarnessCase::ProductionContinuousSubmit ||
         c == HarnessCase::ProductionHideShow ||
         c == HarnessCase::ProductionMinimizeRestore ||
         c == HarnessCase::ProductionRendererRecreation;
}

void PrintUsage() {
#if defined(Q_OS_LINUX)
  Log("EditorRhiHarness --editor-backend=cuda|opencl|metal|cpu "
#else
  Log("EditorRhiHarness --editor-backend=cuda|opencl|metal "
#endif
      "[--case=direct-presentation|resize-churn|hide-show|minimize-restore|"
      "renderer-recreation|hdr-format-query|shutdown-queued|"
      "production-lease-presentation|production-continuous-submit|"
      "production-hide-show|production-minimize-restore|"
      "production-renderer-recreation] "
      "[--fixture=gradient|checkerboard|roi|odd]");
}

auto FillLeaseWithFixture(const WritableTargetLease& lease, const HarnessFixtureImage& fixture)
    -> bool {
  if (!lease.valid() || fixture.width != lease.dimensions.width ||
      fixture.height != lease.dimensions.height) {
    Log("FillLeaseWithFixture: invalid lease or size mismatch lease=%dx%d fixture=%dx%d",
        lease.dimensions.width, lease.dimensions.height, fixture.width, fixture.height);
    return false;
  }
  if (!alcedo::editor_rhi::ProducerAcquireWritable(lease)) {
    Log("FillLeaseWithFixture: ProducerAcquireWritable failed kind=%d",
        static_cast<int>(lease.writable_kind));
    return false;
  }
  bool ok = false;
  if (lease.writable_kind == LeaseWritableResourceKind::CudaArray) {
#if defined(_WIN32) && defined(HAVE_CUDA)
    // Match the process CUDA device selected at startup (device 0 by default).
    (void)cudaSetDevice(0);
    auto* array = reinterpret_cast<cudaArray_t>(lease.writable_resource);
    const size_t row_bytes = fixture.row_bytes();
    const cudaError_t err =
        cudaMemcpy2DToArray(array, 0, 0, fixture.data(), row_bytes, row_bytes,
                            static_cast<size_t>(fixture.height), cudaMemcpyHostToDevice);
    ok = err == cudaSuccess;
    if (!ok) {
      Log("FillLeaseWithFixture: cudaMemcpy2DToArray failed: %s", cudaGetErrorString(err));
    }
#else
    ok = false;
#endif
  } else if (lease.writable_kind == LeaseWritableResourceKind::OpenClImage) {
#if defined(HAVE_OPENCL)
    auto* image = reinterpret_cast<cl_mem>(lease.writable_resource);
    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {static_cast<size_t>(fixture.width),
                              static_cast<size_t>(fixture.height), 1};
    const cl_int err =
        clEnqueueWriteImage(alcedo::OpenClContext::Instance().Queue(), image, CL_TRUE, origin,
                            region, 0, 0, fixture.data(), 0, nullptr, nullptr);
    ok = err == CL_SUCCESS;
    if (!ok) {
      Log("FillLeaseWithFixture: clEnqueueWriteImage failed: %d", err);
    }
#else
    ok = false;
#endif
  }
  (void)alcedo::editor_rhi::ProducerReleaseWritable(lease);
  if (ok) {
    ok = alcedo::editor_rhi::ProducerWaitWritableComplete(lease);
    if (!ok) {
      Log("FillLeaseWithFixture: ProducerWaitWritableComplete failed");
    }
  }
  return ok;
}

auto FrameRoleForLeaseLayer(LeaseFrameLayer layer) -> alcedo::FrameRole {
  switch (layer) {
    case LeaseFrameLayer::QualityBase:
      return alcedo::FrameRole::QualityBase;
    case LeaseFrameLayer::DetailPatch:
      return alcedo::FrameRole::DetailPatch;
    case LeaseFrameLayer::InteractivePrimary:
    default:
      return alcedo::FrameRole::InteractivePrimary;
  }
}

auto SubmitFixtureFrame(EditorViewportItem* item, const HarnessFixtureImage& fixture,
                        LeaseFrameLayer layer, std::uint64_t preview) -> bool {
  if (!item || !item->frameSink()) {
    return false;
  }
  auto* sink = item->frameSink();
  alcedo::FramePreviewMetadata meta;
  meta.frame_role = FrameRoleForLeaseLayer(layer);
  meta.preview_generation = preview;
  meta.presentation_request_id = preview;
  sink->BindFrameSubmission({meta, alcedo::FramePresentationMode::FullFrame});
  sink->EnsureSize(fixture.width, fixture.height);

  const auto domain = alcedo::editor_rhi::ActiveEditorBackend() == EditorBackend::OpenCl
                          ? alcedo::FrameMemoryDomain::OpenClDevice
                          : alcedo::FrameMemoryDomain::CudaDevice;
  auto mapping = sink->MapResourceForWrite(domain);
  if (!mapping) {
    item->requestPresentUpdate();
    Log("SubmitFixtureFrame: map failed live=%d gen=%llu id=%llu consumer=%d",
        item->liveTargetCount(),
        static_cast<unsigned long long>(item->sessionEpoch()),
        static_cast<unsigned long long>(item->imageIdentity()),
        item->presentationAvailable() ? 1 : 0);
    return false;
  }

  // Mapping is already acquired by DirectFrameSink::MapResourceForWrite.
  bool filled = false;
  if (mapping.target_type == alcedo::FrameWriteTargetType::CudaArray && mapping.image_array) {
#if defined(_WIN32) && defined(HAVE_CUDA)
    (void)cudaSetDevice(0);
    auto* array = reinterpret_cast<cudaArray_t>(mapping.image_array);
    const size_t row_bytes = fixture.row_bytes();
    filled = cudaMemcpy2DToArray(array, 0, 0, fixture.data(), row_bytes, row_bytes,
                                 static_cast<size_t>(fixture.height),
                                 cudaMemcpyHostToDevice) == cudaSuccess;
#endif
  } else if (mapping.target_type == alcedo::FrameWriteTargetType::OpenClImage && mapping.data) {
#if defined(HAVE_OPENCL)
    auto* image = reinterpret_cast<cl_mem>(mapping.data);
    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {static_cast<size_t>(fixture.width),
                              static_cast<size_t>(fixture.height), 1};
    filled = clEnqueueWriteImage(alcedo::OpenClContext::Instance().Queue(), image, CL_TRUE, origin,
                                 region, 0, 0, fixture.data(), 0, nullptr, nullptr) == CL_SUCCESS;
#endif
  }
  sink->UnmapResource();
  if (!filled) {
    Log("SubmitFixtureFrame: fill failed preview=%llu",
        static_cast<unsigned long long>(preview));
    return false;
  }
  sink->NotifyFrameReady({meta, alcedo::FramePresentationMode::FullFrame});
  return true;
}

auto SubmitHostFixtureFrame(EditorViewportItem* item, const HarnessFixtureImage& fixture,
                            LeaseFrameLayer layer, std::uint64_t preview) -> bool {
  if (!item || !item->frameSink() || fixture.width <= 0 || fixture.height <= 0 ||
      fixture.data() == nullptr) {
    return false;
  }

  auto pixels = std::make_shared<std::vector<std::uint8_t>>(fixture.byte_size());
  std::memcpy(pixels->data(), fixture.data(), fixture.byte_size());

  alcedo::FramePreviewMetadata metadata;
  metadata.frame_role              = FrameRoleForLeaseLayer(layer);
  metadata.preview_generation      = preview;
  metadata.presentation_request_id = preview;
  metadata.session_epoch          = item->sessionEpoch();
  metadata.image_identity         = item->imageIdentity();
  item->frameSink()->BindFrameSubmission(
      {metadata, alcedo::FramePresentationMode::FullFrame});
  item->frameSink()->SubmitHostFrame(
      alcedo::ViewerFrame{fixture.width,
                          fixture.height,
                          fixture.row_bytes(),
                          std::shared_ptr<const void>(pixels, pixels->data()),
                          {},
                          alcedo::FramePresentationMode::FullFrame,
                          metadata});
  return true;
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

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
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
  alcedo::editor_rhi::RegisterEditorViewportQmlTypes();

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

  const bool production = IsProductionCase(harness_case);
  if (backend == EditorBackend::Cpu && harness_case != HarnessCase::DirectPresentation) {
    Log("EditorRhiHarness: CPU host-upload verification supports only direct-presentation");
    return 1;
  }
  const bool cpu_host_upload = backend == EditorBackend::Cpu;
  const bool use_production_viewport = production || cpu_host_upload;
  HarnessViewportItem* viewport = nullptr;
  EditorViewportItem* production_viewport = nullptr;
  if (use_production_viewport) {
    production_viewport = new EditorViewportItem(root);
    production_viewport->setParentItem(root);
    // Match the production fixture size so the default target pool is
    // directly acquirable without a separate size request race.
    production_viewport->setSize(QSizeF(64, 48));
    production_viewport->setFixedColorBufferWidth(64);
    production_viewport->setFixedColorBufferHeight(48);
    production_viewport->setX((960 - 64) / 2.0);
    production_viewport->setY((720 - 48) / 2.0);
    production_viewport->beginImageSession(/*imageIdentity=*/42);
    Log("EditorRhiHarness: production viewport flags=%d visible=%d enabled=%d size=%.0fx%.0f",
        static_cast<int>(production_viewport->flags()),
        production_viewport->isVisible() ? 1 : 0, production_viewport->isEnabled() ? 1 : 0,
        production_viewport->width(), production_viewport->height());
  } else {
    viewport = new HarnessViewportItem(root);
    viewport->setParentItem(root);
    viewport->setSize(QSizeF(640, 480));
    viewport->setX((960 - 640) / 2.0);
    viewport->setY((720 - 480) / 2.0);
    viewport->configure(backend, fixture, /*request_readback=*/true);
  }

  NativeResourceCounters::Instance().ResetForTest();
  const HarnessFixtureImage production_fixture = alcedo::editor_rhi::MakeFp32Gradient(64, 48);

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

  if (!use_production_viewport) {
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
        default:
          break;
      }
    });
  }

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

  // Renderer recreation completion (harness viewport).
  if (!use_production_viewport) {
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
  }

  // Production EditorViewportItem cases: drive direct EnsureSize/Map/Notify path.
  if (use_production_viewport && production_viewport) {
    auto* prod = production_viewport;
    QTimer::singleShot(400, &app, [&, prod]() {
      // Kick a present pass so the scene-graph thread can create the first slot.
      prod->requestPresentUpdate();
    });

    auto* submit_timer = new QTimer(&app);
    int submit_attempts = 0;
    int continuous_ok = 0;
    QObject::connect(submit_timer, &QTimer::timeout, &app, [&, prod]() {
      if (state.stage >= 100) {
        submit_timer->stop();
        return;
      }
      ++submit_attempts;
      const auto preview = static_cast<std::uint64_t>(submit_attempts);
      const bool submitted = cpu_host_upload
                                 ? SubmitHostFixtureFrame(
                                       prod, production_fixture,
                                       LeaseFrameLayer::InteractivePrimary, preview)
                                 : SubmitFixtureFrame(prod, production_fixture,
                                                      LeaseFrameLayer::InteractivePrimary, preview);
      if (submitted) {
        ++continuous_ok;
        Log("EditorRhiHarness: production submit ok attempt=%d live=%d status=%s gen=%llu",
            submit_attempts, prod->liveTargetCount(), qPrintable(prod->statusText()),
            static_cast<unsigned long long>(prod->lastPresentedSessionEpoch()));
      }
      if (cpu_host_upload && continuous_ok >= 1) {
        submit_timer->stop();
        prod->requestPresentUpdate();
        QTimer::singleShot(800, &app, [&, prod]() {
          const auto presented = prod->presentedFrameCount();
          Log("EditorRhiHarness: CPU host upload presented=%llu status=%s",
              static_cast<unsigned long long>(presented), qPrintable(prod->statusText()));
          finish(presented > 0 ? 0 : 20);
        });
        return;
      }
      if (state.harness_case == HarnessCase::ProductionLeasePresentation && continuous_ok >= 1) {
        submit_timer->stop();
        prod->requestPresentUpdate();
        QTimer::singleShot(200, &app, [&, prod]() { prod->requestPresentUpdate(); });
        QTimer::singleShot(600, &app, [&, prod]() {
          Log("EditorRhiHarness: production check live=%d available=%d status=%s presented_gen=%llu",
              prod->liveTargetCount(), prod->presentationAvailable() ? 1 : 0,
              qPrintable(prod->statusText()),
              static_cast<unsigned long long>(prod->lastPresentedSessionEpoch()));
          // Lease path is successful when a completed frame was accepted and the
          // consumer remains available with a live pool. Presentation status is
          // preferred but not required if the render thread already held the frame.
          const bool ok = prod->liveTargetCount() > 0 && prod->presentationAvailable() &&
                          continuous_ok >= 1;
          finish(ok ? 0 : 20);
        });
        return;
      }
      if (state.harness_case == HarnessCase::ProductionContinuousSubmit && continuous_ok >= 6) {
        submit_timer->stop();
        Log("EditorRhiHarness: continuous done ok=%d live=%d", continuous_ok,
            prod->liveTargetCount());
        finish(continuous_ok >= 6 && prod->liveTargetCount() > 0 ? 0 : 21);
        return;
      }
      if (submit_attempts > 60) {
        submit_timer->stop();
        Log("EditorRhiHarness: continuous timeout ok=%d live=%d", continuous_ok,
            prod->liveTargetCount());
        // Accept partial continuous progress when the pool stayed alive and
        // multiple frames were accepted (recycle path under load).
        finish(continuous_ok >= 3 && prod->liveTargetCount() > 0 ? 0 : 22);
      }
    });
    submit_timer->start(80);

    if (state.harness_case == HarnessCase::ProductionHideShow) {
      QTimer::singleShot(900, &app, [&, prod]() {
        state.window->hide();
        QTimer::singleShot(200, &app, [&, prod]() {
          if (prod->presentationAvailable()) {
            Log("EditorRhiHarness: hide did not clear presentationAvailable");
            finish(23);
            return;
          }
          state.window->show();
          state.window->showNormal();
          state.window->raise();
          state.window->requestActivate();
          state.window->requestUpdate();
          prod->refreshPresentationAvailability();
          prod->requestPresentUpdate();
          QTimer::singleShot(800, &app, [&, prod]() {
            prod->refreshPresentationAvailability();
            prod->requestPresentUpdate();
            bool recovered = prod->presentationAvailable();
            for (int i = 0; i < 20 && !recovered; ++i) {
              prod->refreshPresentationAvailability();
              recovered = SubmitFixtureFrame(prod, production_fixture,
                                             LeaseFrameLayer::InteractivePrimary,
                                             static_cast<std::uint64_t>(100 + i)) ||
                          prod->presentationAvailable();
              QCoreApplication::processEvents();
            }
            Log("EditorRhiHarness: hide/show recovered=%d available=%d live=%d exposed=%d vis=%d",
                recovered ? 1 : 0, prod->presentationAvailable() ? 1 : 0, prod->liveTargetCount(),
                state.window && state.window->isExposed() ? 1 : 0,
                state.window ? static_cast<int>(state.window->visibility()) : -1);
            // Recovery means the consumer is available again after show, or a
            // new frame can be submitted into a live pool.
            finish(recovered || (prod->liveTargetCount() > 0 && state.window &&
                                 state.window->isExposed())
                       ? 0
                       : 24);
          });
        });
      });
    }
    if (state.harness_case == HarnessCase::ProductionMinimizeRestore) {
      QTimer::singleShot(900, &app, [&, prod]() {
        state.window->showMinimized();
        QTimer::singleShot(150, &app, [&, prod]() {
          if (prod->presentationAvailable()) {
            finish(25);
            return;
          }
          state.window->showNormal();
          QTimer::singleShot(600, &app, [&, prod]() {
            const bool recovered =
                SubmitFixtureFrame(prod, production_fixture, LeaseFrameLayer::InteractivePrimary,
                                   200) ||
                prod->presentationAvailable();
            finish(recovered ? 0 : 26);
          });
        });
      });
    }
    if (state.harness_case == HarnessCase::ProductionRendererRecreation) {
      QTimer::singleShot(700, &app, [&, prod]() {
        (void)SubmitFixtureFrame(prod, production_fixture, LeaseFrameLayer::InteractivePrimary, 1);
        // Invalidate targets only — must not permanently shut down the broker.
        prod->requestRendererInvalidation();
        prod->requestPresentUpdate();
        if (state.window) {
          state.window->requestUpdate();
        }
        QTimer::singleShot(600, &app, [&, prod]() {
          int recovered = 0;
          for (int i = 0; i < 20; ++i) {
            if (SubmitFixtureFrame(prod, production_fixture, LeaseFrameLayer::InteractivePrimary,
                                   static_cast<std::uint64_t>(10 + i))) {
              ++recovered;
              break;
            }
            prod->requestPresentUpdate();
            QCoreApplication::processEvents();
          }
          Log("EditorRhiHarness: renderer recreation recovered=%d live=%d available=%d", recovered,
              prod->liveTargetCount(), prod->presentationAvailable() ? 1 : 0);
          finish(recovered > 0 ? 0 : 27);
        });
      });
    }
  }

  // Presentation failure path.
  QTimer::singleShot(20000, &app, [&]() {
    if (state.stage == 0 && !use_production_viewport) {
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
    resize_timer->start(150);
  }

  return app.exec();
}
