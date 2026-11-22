//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX2 experiment: Qt Quick on Vulkan with a VkDevice that the program creates
// (QQuickGraphicsDevice::fromDeviceObjects), against Qt Quick on Direct3D 11 with the same scene.
// Experiment code: it does not merge.
//
// vx2_viewport --api=vulkan|d3d11 --case=drag|resize|stress|grab|formats|adapters [options]
// vx2_viewport --compare a.png b.png
#include <windows.h>
#include <dxgi1_6.h>

#include <vulkan/vulkan.h>

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QQuickGraphicsConfiguration>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <QTimer>
#include <QVulkanInstance>
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vx2_common.hpp"
#include "vx2_item.hpp"
#include "vx2_vulkan.hpp"

using namespace vx2;

namespace {

struct Args {
  std::string api       = "vulkan";
  std::string scenario  = "drag";
  std::string device;
  std::string queue     = "separate";
  bool        mutex     = true;
  bool        validation = false;
  bool        sync_validation = false;
  bool        api_dump  = false;
  bool        minimal_features = false;
  int         screen    = -1;
  double      seconds   = 8.0;
  int         width     = 2560;
  int         height    = 1600;
  int         work      = 0;
  bool        lut       = true;
  bool        flip_correction = true;
  std::string out;
  std::string lock_span = "frame";
  bool        continuous = false;
  std::string compare_a, compare_b;
};

auto Value(const std::string& arg, const char* name) -> std::optional<std::string> {
  const std::string prefix = std::string(name) + "=";
  if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  return std::nullopt;
}

auto OnOff(const std::string& v) -> bool { return v == "on" || v == "1" || v == "true"; }

auto Parse(int argc, char** argv) -> Args {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "--compare" && i + 2 < argc) {
      a.compare_a = argv[++i];
      a.compare_b = argv[++i];
    } else if (auto v = Value(s, "--api")) a.api = *v;
    else if (auto v2 = Value(s, "--case")) a.scenario = *v2;
    else if (auto v3 = Value(s, "--device")) a.device = *v3;
    else if (auto v4 = Value(s, "--queue")) a.queue = *v4;
    else if (auto v5 = Value(s, "--mutex")) a.mutex = OnOff(*v5);
    else if (auto v6 = Value(s, "--validation")) a.validation = OnOff(*v6);
    else if (auto v7 = Value(s, "--sync-validation")) a.sync_validation = OnOff(*v7);
    else if (auto v8 = Value(s, "--api-dump")) a.api_dump = OnOff(*v8);
    else if (auto v9 = Value(s, "--minimal-features")) a.minimal_features = OnOff(*v9);
    else if (auto va = Value(s, "--screen")) a.screen = std::stoi(*va);
    else if (auto vb = Value(s, "--seconds")) a.seconds = std::stod(*vb);
    else if (auto vc = Value(s, "--image")) {
      const auto x = vc->find('x');
      a.width      = std::stoi(vc->substr(0, x));
      a.height     = std::stoi(vc->substr(x + 1));
    } else if (auto vd = Value(s, "--work")) a.work = std::stoi(*vd);
    else if (auto ve = Value(s, "--lut")) a.lut = OnOff(*ve);
    else if (auto vf = Value(s, "--flip-correction")) a.flip_correction = OnOff(*vf);
    else if (auto vg = Value(s, "--out")) a.out = *vg;
    else if (auto vh = Value(s, "--lock-span")) a.lock_span = *vh;
    else if (auto vi = Value(s, "--produce")) a.continuous = *vi == "continuous";
    else {
      Log("ERROR unknown argument %s", s.c_str());
      std::exit(2);
    }
  }
  return a;
}

auto Narrow(const wchar_t* text) -> std::string {
  char buffer[512]{};
  WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer), nullptr, nullptr);
  return buffer;
}

/// DXGI adapters and the outputs (monitors) that each one drives.
void LogAdapters() {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    Log("ERROR CreateDXGIFactory1");
    return;
  }
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    Log("DXGI adapter %u '%s' luid=%08x%08x flags=%u", i, Narrow(desc.Description).c_str(),
        static_cast<unsigned>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart, desc.Flags);
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC od{};
      output->GetDesc(&od);
      IDXGIOutput6* o6 = nullptr;
      std::string   hdr = "unknown";
      if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6)))) {
        DXGI_OUTPUT_DESC1 d1{};
        o6->GetDesc1(&d1);
        hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "HDR10 on" : "SDR";
        Log("DXGI   output %u %s desktop=(%ld,%ld)-(%ld,%ld) bits=%u colour=%s maxLuminance=%.0f",
            o, Narrow(od.DeviceName).c_str(), od.DesktopCoordinates.left, od.DesktopCoordinates.top,
            od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, d1.BitsPerColor, hdr.c_str(),
            d1.MaxLuminance);
        o6->Release();
      }
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
}

auto FindDxgiAdapter(const std::string& filter) -> std::optional<LUID> {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    return std::nullopt;
  }
  std::optional<LUID> result;
  IDXGIAdapter1*      adapter = nullptr;
  std::string         lower   = filter;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (UINT i = 0; !result && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::string name = Narrow(desc.Description);
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name.find(lower) != std::string::npos) result = desc.AdapterLuid;
    adapter->Release();
  }
  factory->Release();
  return result;
}

void LogScreens() {
  const auto screens = QGuiApplication::screens();
  for (int i = 0; i < screens.size(); ++i) {
    const QScreen* s = screens[i];
    Log("SCREEN %d name=%s model='%s' geometry=%dx%d+%d+%d dpr=%.2f refresh=%.2f", i,
        s->name().toUtf8().constData(), s->model().toUtf8().constData(), s->geometry().width(),
        s->geometry().height(), s->geometry().x(), s->geometry().y(), s->devicePixelRatio(),
        s->refreshRate());
  }
}

struct FrameRecord {
  std::int64_t  begin_ns     = 0;
  std::int64_t  rendering_ns = 0;
  std::int64_t  rendered_ns  = 0;
  std::int64_t  end_ns     = 0;
  std::uint64_t new_id     = 0;
  std::int64_t  request_ns = 0;
  QSize         pixels;
};

auto Compare(const std::string& a_path, const std::string& b_path) -> int {
  QImage a(QString::fromStdString(a_path)), b(QString::fromStdString(b_path));
  if (a.isNull() || b.isNull() || a.size() != b.size()) {
    Log("ERROR compare: images absent or of different size");
    return 2;
  }
  a = a.convertToFormat(QImage::Format_RGB888);
  b = b.convertToFormat(QImage::Format_RGB888);
  int  max_diff = 0, max_flip = 0;
  long differ = 0, differ_over1 = 0;
  for (int y = 0; y < a.height(); ++y) {
    const uchar* pa = a.constScanLine(y);
    const uchar* pb = b.constScanLine(y);
    const uchar* pf = b.constScanLine(a.height() - 1 - y);
    for (int x = 0; x < a.width() * 3; ++x) {
      const int d = std::abs(int(pa[x]) - int(pb[x]));
      max_diff    = std::max(max_diff, d);
      if (d > 0) ++differ;
      if (d > 1) ++differ_over1;
      max_flip = std::max(max_flip, std::abs(int(pa[x]) - int(pf[x])));
    }
  }
  std::printf("RESULT compare max_diff=%d values_differ=%ld values_differ_over_1=%ld of=%ld "
              "max_diff_if_b_flipped=%d\n",
              max_diff, differ, differ_over1, static_cast<long>(a.width()) * a.height() * 3,
              max_flip);
  return 0;
}

/// The grabbed window against the CPU form of the composite: texel, clamp, display table
/// (trilinear in double precision), 8-bit code.
void CheckGrab(const QImage& grab, const std::vector<float>& fixture, int w, int h,
               const DisplayLut* lut) {
  const QImage img = grab.convertToFormat(QImage::Format_RGB888);
  if (img.width() != w || img.height() != h) {
    Log("ERROR grab size %dx%d, expected %dx%d", img.width(), img.height(), w, h);
    return;
  }
  auto expect = [&](int x, int y, int c) {
    const float* t = &fixture[(static_cast<std::size_t>(y) * w + x) * 4];
    double       v = 0;
    if (lut) {
      v = SampleLut(*lut, t[0], t[1], t[2])[c];
    } else {
      v = std::clamp(static_cast<double>(t[c]), 0.0, 1.0);
    }
    return v * 255.0;
  };
  double max_err = 0, max_err_flipped = 0;
  long   over1 = 0;
  for (int y = 0; y < h; ++y) {
    const uchar* p = img.constScanLine(y);
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < 3; ++c) {
        const double e = std::abs(p[x * 3 + c] - expect(x, y, c));
        const double f = std::abs(p[x * 3 + c] - expect(x, h - 1 - y, c));
        max_err        = std::max(max_err, e);
        max_err_flipped = std::max(max_err_flipped, f);
        if (e > 1.0) ++over1;
      }
    }
  }
  std::printf("RESULT grab_vs_cpu max_code_error=%.3f values_over_1_code=%ld "
              "max_code_error_if_flipped=%.3f top_left=(%d,%d,%d)\n",
              max_err, over1, max_err_flipped, img.constScanLine(0)[0], img.constScanLine(0)[1],
              img.constScanLine(0)[2]);
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  if (!args.compare_a.empty()) {
    QGuiApplication app(argc, argv);
    return Compare(args.compare_a, args.compare_b);
  }
  const bool vulkan = args.api == "vulkan";
  if (!vulkan && args.api != "d3d11") {
    Log("ERROR --api must be vulkan or d3d11");
    return 2;
  }
  const bool grab_case = args.scenario == "grab";
  const int  img_w     = grab_case ? 960 : args.width;
  const int  img_h     = grab_case ? 720 : args.height;

  QGuiApplication app(argc, argv);
  LogScreens();
  if (args.scenario == "adapters") {
    LogAdapters();
    VulkanOptions o;
    o.separate_queue = false;
    VulkanContext probe(o);  // logs every physical device with its LUID
    return 0;
  }

  std::unique_ptr<VulkanContext> ctx;
  QVulkanInstance                qvk;
  if (vulkan) {
    VulkanOptions o;
    o.device_filter    = args.device;
    o.validation       = args.validation;
    o.sync_validation  = args.sync_validation;
    o.api_dump         = args.api_dump;
    o.separate_queue   = args.queue == "separate";
    o.split_device     = args.queue == "split";
    o.queue_mutex      = args.mutex;
    o.minimal_features = args.minimal_features;
    try {
      ctx = std::make_unique<VulkanContext>(o);
    } catch (const std::exception& e) {
      Log("ERROR %s", e.what());
      return 1;
    }
    ctx->LogFormatFeatures();
    QByteArrayList exts;
    for (const auto& e : ctx->instance_extensions) exts << QByteArray::fromStdString(e);
    qvk.setVkInstance(ctx->instance);
    qvk.setExtensions(exts);
    if (!qvk.create()) {
      Log("ERROR QVulkanInstance::create with the program instance");
      return 1;
    }
    Log("QT QVulkanInstance extensions seen by Qt: %s",
        qvk.extensions().join(' ').constData());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
  } else {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
  }

  const auto screens = QGuiApplication::screens();
  QScreen*   screen  = args.screen >= 0 && args.screen < screens.size() ? screens[args.screen]
                                                                         : QGuiApplication::primaryScreen();
  const double dpr = screen->devicePixelRatio();

  // The GDI device name of the monitor (\\.\DISPLAYn); QScreen::name() can be the model name.
  std::wstring gdi_device;
  if (auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>()) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(native->handle(), &info)) gdi_device = info.szDevice;
  }
  Log("SCREEN selected '%s' gdi=%s dpr=%.2f refresh=%.2f", screen->name().toUtf8().constData(),
      Narrow(gdi_device.c_str()).c_str(), dpr, screen->refreshRate());

  std::shared_ptr<DisplayLut> lut;
  if (args.lut) {
    try {
      lut = std::make_shared<DisplayLut>(BuildDisplayLut(gdi_device, 65));
      Log("LUT profile='%s' path=%s edge=65", lut->description.c_str(),
          Narrow(lut->profile_path.c_str()).c_str());
    } catch (const std::exception& e) {
      Log("ERROR display table: %s", e.what());
      return 1;
    }
  }

  const std::vector<float> fixture = MakeFixture(img_w, img_h);
  std::unique_ptr<Producer> producer;
  if (vulkan) {
    QFile f(QStringLiteral(":/vx2/vx2_fill.comp.qsb"));
    f.open(QIODevice::ReadOnly);
    const QShader    shader = QShader::fromSerialized(f.readAll());
    const QByteArray spv    = shader.shader({QShader::SpirvShader, QShaderVersion(100)}).shader();
    std::vector<std::uint32_t> words(static_cast<std::size_t>(spv.size()) / 4);
    std::memcpy(words.data(), spv.constData(), words.size() * 4);
    try {
      producer = std::make_unique<Producer>(*ctx, fixture, img_w, img_h, args.work, words);
    } catch (const std::exception& e) {
      Log("ERROR producer: %s", e.what());
      return 1;
    }
  }

  auto* window = new QQuickWindow();
  window->setScreen(screen);
  window->setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint);
  window->setTitle(QStringLiteral("alcedo VX2"));
  if (vulkan) {
    window->setVulkanInstance(&qvk);
    window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
        ctx->physical, ctx->device, static_cast<int>(ctx->family),
        static_cast<int>(ctx->qt_queue_index)));
  } else if (!args.device.empty()) {
    const auto luid = FindDxgiAdapter(args.device);
    if (!luid) {
      Log("ERROR no DXGI adapter matches '%s'", args.device.c_str());
      return 1;
    }
    window->setGraphicsDevice(QQuickGraphicsDevice::fromAdapter(luid->LowPart, luid->HighPart));
  }

  // Logical size: in the grab case one device pixel for each photo pixel.
  int win_w = 1280, win_h = 800;
  if (grab_case) {
    win_w = static_cast<int>(std::lround(img_w / dpr));
    win_h = static_cast<int>(std::lround(img_h / dpr));
    if (std::abs(win_w * dpr - img_w) > 1e-6 || std::abs(win_h * dpr - img_h) > 1e-6) {
      Log("ERROR grab: %dx%d is not a whole logical size at dpr %.2f", img_w, img_h, dpr);
      return 1;
    }
  }
  const QRect avail = screen->availableGeometry();
  window->setGeometry(avail.x() + 40, avail.y() + 40, win_w, win_h);

  auto* item = new Vx2Item(window->contentItem());
  item->setSize(QSizeF(win_w, win_h));
  QObject::connect(window, &QWindow::widthChanged, item, [item](int w) { item->setWidth(w); });
  QObject::connect(window, &QWindow::heightChanged, item, [item](int h) { item->setHeight(h); });
  Vx2Item::Config cfg;
  cfg.source          = vulkan ? PhotoSource::VulkanProducer : PhotoSource::RhiCompute;
  cfg.producer        = producer.get();
  cfg.fixture         = &fixture;
  cfg.width           = img_w;
  cfg.height          = img_h;
  cfg.work            = args.work;
  cfg.lut             = lut;
  cfg.flip_correction = args.flip_correction;
  item->Configure(cfg);

  // Render thread: frame marks, the queue lock of plan section 4.2, the frame count.
  std::mutex               records_mutex;
  std::vector<FrameRecord> records;
  thread_local bool         lock_held   = false;
  thread_local std::int64_t begin_ns    = 0;
  thread_local std::int64_t rendering_ns = 0;
  thread_local std::int64_t rendered_ns = 0;
  VulkanContext*            raw_ctx     = ctx.get();
  Producer*                 raw_prod    = producer.get();
  // frame: beforeFrameBegin to afterFrameEnd (plan section 4.2). submit: afterRendering to
  // afterFrameEnd, which holds QRhi::endFrame (queue submit and present) and not the wait for the
  // frame slot and the swapchain image in QRhi::beginFrame.
  const bool lock_frame  = args.lock_span == "frame";
  auto       take_lock   = [raw_ctx] {
    if (raw_ctx && raw_ctx->use_mutex && !lock_held) {
      raw_ctx->queue_mutex.lock();
      lock_held = true;
    }
  };
  QObject::connect(
      window, &QQuickWindow::beforeFrameBegin, window,
      [=] {
        begin_ns = NowNs();
        if (lock_frame) take_lock();
      },
      Qt::DirectConnection);
  QObject::connect(
      window, &QQuickWindow::beforeRendering, window, [] { rendering_ns = NowNs(); },
      Qt::DirectConnection);
  QObject::connect(
      window, &QQuickWindow::afterRendering, window,
      [=] {
        rendered_ns = NowNs();
        if (!lock_frame) take_lock();
      },
      Qt::DirectConnection);
  QObject::connect(
      window, &QQuickWindow::afterFrameEnd, window,
      [&, raw_ctx, raw_prod, window] {
        const std::int64_t end = NowNs();
        if (lock_held) {
          raw_ctx->queue_mutex.unlock();
          lock_held = false;
        }
        const std::uint64_t n = ++Vx2Item::submitted_frames;
        if (raw_prod) raw_prod->SetSubmittedFrames(n);
        FrameRecord r;
        r.begin_ns   = begin_ns;
        r.rendering_ns = rendering_ns;
        r.rendered_ns  = rendered_ns;
        r.end_ns     = end;
        r.new_id     = Vx2Item::marks.new_id;
        r.request_ns = Vx2Item::marks.new_request_ns;
        r.pixels     = window->swapChain() ? window->swapChain()->currentPixelSize() : QSize();
        Vx2Item::marks = {};
        std::lock_guard<std::mutex> lock(records_mutex);
        records.push_back(r);
      },
      Qt::DirectConnection);

  // Content requests: on each frame (continuous drag), from the GUI thread.
  const bool drag   = args.scenario == "drag" || args.scenario == "stress";
  const bool resize = args.scenario == "resize" || args.scenario == "stress";
  std::uint64_t request_count = 0;
  auto request = [&] {
    const float gain = 0.6f + 0.4f * static_cast<float>(std::sin(request_count * 0.05));
    ++request_count;
    const float g = grab_case ? 1.0f : gain;
    if (producer) {
      producer->Request(g, NowNs());
      // Continuous production: the window still renders each vsync (an animated UI over a
      // long render), so the frame times show whether Qt keeps vsync while the GPU is busy.
      if (args.continuous) item->update();
    } else {
      item->RequestGain(g, NowNs());
    }
  };
  if (producer) {
    producer->SetContinuous(args.continuous);
    producer->Start([item] { QMetaObject::invokeMethod(item, [item] { item->update(); }, Qt::QueuedConnection); });
  }
  // GUI thread probe: a 2 ms timer. Its lateness is the time that the GUI thread cannot handle an
  // input event (the sync of the threaded render loop blocks the GUI thread).
  std::vector<double> gui_late;
  std::int64_t        gui_last = 0;
  QTimer              gui_probe;
  gui_probe.setTimerType(Qt::PreciseTimer);
  QObject::connect(&gui_probe, &QTimer::timeout, window, [&] {
    const std::int64_t now = NowNs();
    if (gui_last != 0) gui_late.push_back(std::max(0.0, (now - gui_last) * 1e-6 - 2.0));
    gui_last = now;
  });
  if (drag) {
    QObject::connect(window, &QQuickWindow::frameSwapped, item, [&] { request(); },
                     Qt::QueuedConnection);
  }

  struct ResizeRequest {
    std::int64_t ns;
    QSize        pixels;
  };
  std::vector<ResizeRequest> resize_requests;
  QTimer resize_timer;
  resize_timer.setTimerType(Qt::PreciseTimer);
  int resize_step = 0;
  QObject::connect(&resize_timer, &QTimer::timeout, window, [&] {
    // Triangle wave between 900x560 and 1500x940 logical pixels, 60 steps each way.
    const int    k = resize_step++ % 120;
    const double t = k < 60 ? k / 60.0 : (120 - k) / 60.0;
    const int    w = static_cast<int>(900 + 600 * t);
    const int    h = static_cast<int>(560 + 380 * t);
    resize_requests.push_back({NowNs(), QSize(static_cast<int>(std::lround(w * dpr)),
                                              static_cast<int>(std::lround(h * dpr)))});
    window->resize(w, h);
  });

  bool surface_logged = false;
  QObject::connect(window, &QQuickWindow::frameSwapped, window, [&] {
    if (surface_logged) return;
    surface_logged = true;
    QRhi*          rhi = window->rhi();
    QRhiSwapChain* sc  = window->swapChain();
    if (rhi) {
      Log("QT rhi backend=%s device='%s' yUpInNDC=%d yUpInFramebuffer=%d", rhi->backendName(),
          rhi->driverInfo().deviceName.constData(), rhi->isYUpInNDC() ? 1 : 0,
          rhi->isYUpInFramebuffer() ? 1 : 0);
    }
    if (sc) {
      Log("QT swapchain format=%d (0 SDR) pixels=%dx%d sampleCount=%d", static_cast<int>(sc->format()),
          sc->currentPixelSize().width(), sc->currentPixelSize().height(), sc->sampleCount());
      const QRhiSwapChainHdrInfo hdr = sc->hdrInfo();
      Log("QT hdrInfo limitsType=%d maxLuminance=%.1f (or maxColorComponentValue) sdrWhite=%.1f",
          static_cast<int>(hdr.limitsType), hdr.limits.luminanceInNits.maxLuminance,
          hdr.sdrWhiteLevel);
      Log("QT isFormatSupported HDRExtendedSrgbLinear=%d HDR10=%d HDRExtendedDisplayP3Linear=%d",
          sc->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear) ? 1 : 0,
          sc->isFormatSupported(QRhiSwapChain::HDR10) ? 1 : 0,
          sc->isFormatSupported(QRhiSwapChain::HDRExtendedDisplayP3Linear) ? 1 : 0);
    }
    if (ctx) ctx->LogSurface(QVulkanInstance::surfaceForWindow(window));
  }, Qt::QueuedConnection);

  int exit_code = 0;
  QTimer::singleShot(0, window, [&] {
    window->show();
    request();
    if (resize) resize_timer.start(16);
    gui_probe.start(2);
  });

  if (grab_case) {
    QTimer::singleShot(2500, window, [&] {
      const QImage img = window->grabWindow();
      if (!args.out.empty()) img.save(QString::fromStdString(args.out));
      Log("GRAB %dx%d format=%d saved=%s", img.width(), img.height(), static_cast<int>(img.format()),
          args.out.c_str());
      CheckGrab(img, fixture, img_w, img_h, lut.get());
      app.quit();
    });
  } else {
    QTimer::singleShot(static_cast<int>(args.seconds * 1000), window, [&] {
      resize_timer.stop();
      app.quit();
    });
  }

  app.exec();
  window->hide();
  if (producer) producer->Stop();

  // Statistics.
  std::vector<FrameRecord> frames;
  {
    std::lock_guard<std::mutex> lock(records_mutex);
    frames = records;
  }
  const double vsync_ms = 1000.0 / screen->refreshRate();
  std::vector<double> period, period_rendering, cpu, begin_wait, record, end_frame, between, latency;
  // The first second is warm-up (pipeline creation, first swapchain).
  const std::int64_t start = frames.empty() ? 0 : frames.front().end_ns + 1'000'000'000;
  std::int64_t       first_end = 0, last_end = 0;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    if (frames[i].end_ns < start) continue;
    if (first_end == 0) first_end = frames[i - 1].end_ns;
    last_end = frames[i].end_ns;
    period.push_back((frames[i].end_ns - frames[i - 1].end_ns) * 1e-6);
    period_rendering.push_back((frames[i].rendering_ns - frames[i - 1].rendering_ns) * 1e-6);
    cpu.push_back((frames[i].end_ns - frames[i].begin_ns) * 1e-6);
    begin_wait.push_back((frames[i].rendering_ns - frames[i].begin_ns) * 1e-6);
    record.push_back((frames[i].rendered_ns - frames[i].rendering_ns) * 1e-6);
    end_frame.push_back((frames[i].end_ns - frames[i].rendered_ns) * 1e-6);
    // Render thread outside the frame: the wait for the next sync request, and the swapchain
    // resize (createOrResize runs before beforeFrameBegin).
    between.push_back((frames[i].begin_ns - frames[i - 1].end_ns) * 1e-6);
    if (frames[i].new_id != 0) latency.push_back((frames[i].end_ns - frames[i].request_ns) * 1e-6);
  }
  // Frames that the window did not get in the measured time: vsync intervals minus frames.
  const double elapsed_ms  = (last_end - first_end) * 1e-6;
  const long   missed      = period.empty() ? 0
                                            : std::lround(elapsed_ms / vsync_ms) -
                                       static_cast<long>(period.size());
  long         late = 0;
  for (double p : period) {
    if (p > 1.5 * vsync_ms) ++late;
  }
  std::vector<double> resize_latency;
  long                resize_unmatched = 0;
  for (const auto& rq : resize_requests) {
    if (rq.ns < start) continue;
    bool found = false;
    for (const auto& f : frames) {
      if (f.end_ns > rq.ns && f.pixels == rq.pixels) {
        resize_latency.push_back((f.end_ns - rq.ns) * 1e-6);
        found = true;
        break;
      }
    }
    if (!found) ++resize_unmatched;
  }
  std::printf("RESULT case=%s api=%s device='%s' queue=%s mutex=%d lock_span=%s produce=%s "
              "screen='%s' refresh=%.2f image=%dx%d work=%d frames=%zu content_frames=%zu "
              "missed_vsyncs=%ld periods_over_1.5_vsync=%ld renderer_errors=%d\n",
              args.scenario.c_str(), args.api.c_str(),
              ctx ? ctx->device_name.c_str() : args.device.c_str(),
              ctx ? (ctx->split ? "split" : ctx->shared_queue ? "shared" : "separate") : "qt", ctx && ctx->use_mutex ? 1 : 0,
              args.lock_span.c_str(), args.continuous ? "continuous" : "on-request",
              screen->name().toUtf8().constData(), screen->refreshRate(), img_w, img_h, args.work,
              period.size(), latency.size(), missed, late, Vx2Item::errors.load());
  std::printf("RESULT %s\n", Format("period_ms", Summarize(period)).c_str());
  std::printf("RESULT %s\n", Format("period_at_rendering_ms", Summarize(period_rendering)).c_str());
  std::printf("RESULT %s\n", Format("render_thread_ms", Summarize(cpu)).c_str());
  std::printf("RESULT %s\n", Format("begin_frame_ms", Summarize(begin_wait)).c_str());
  std::printf("RESULT %s\n", Format("record_ms", Summarize(record)).c_str());
  std::printf("RESULT %s\n", Format("end_frame_ms", Summarize(end_frame)).c_str());
  std::printf("RESULT %s\n", Format("between_frames_ms", Summarize(between)).c_str());
  std::printf("RESULT %s\n", Format("gui_late_ms", Summarize(gui_late)).c_str());
  if (!latency.empty()) std::printf("RESULT %s\n", Format("content_latency_ms", Summarize(latency)).c_str());
  if (resize) {
    std::printf("RESULT %s resize_requests_unmatched=%ld\n",
                Format("resize_latency_ms", Summarize(resize_latency)).c_str(), resize_unmatched);
  }
  if (producer) {
    const auto gpu = producer->GpuTimesMs();
    std::printf("RESULT %s producer_frames_per_s=%.1f\n", Format("producer_gpu_ms", Summarize(gpu)).c_str(),
                gpu.size() / std::max(1e-3, args.seconds));
    std::printf("RESULT %s\n", Format("producer_lock_wait_ms", Summarize(producer->LockWaitsMs())).c_str());
  }
  delete window;
  producer.reset();
  if (ctx) {
    std::printf("RESULT %s\n", ctx->ValidationReport().c_str());
    if (ctx->ValidationErrors() > 0) exit_code = 3;
  }
  if (Vx2Item::errors.load() > 0) exit_code = 4;
  std::fflush(stdout);
  return exit_code;
}
