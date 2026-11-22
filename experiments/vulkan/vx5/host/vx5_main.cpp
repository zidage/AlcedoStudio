//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX5 experiment: video memory budget and device loss (plan section 6). Experiment code: it does
// not merge.
//
// vx5_memory --case=budget|hold|lost|tdr --device=<part of name> [options]
//   budget: --block-mib=256 --over-mib=2048 --max-mib=0 --priority=none|same|split
//           --pageable=on|off --hold-seconds=5 --work-mib=1536 --min-free-mib=6144
//   hold:   --hold-mib=3072 --hold-seconds=30  (a second process that holds video memory)
//   lost:   the VX2 viewport (Qt Quick on the display device, the producer on the pipeline
//           device); at 2 s a kernel on the pipeline device reads an address with no memory.
//           --fault-address=fixed|freed|0x...
//   tdr:    the same viewport; at 2 s the program runs "dxcap -forcetdr" (a GPU reset of Windows).
//   both:   --observe-ms=1000 (time between the loss and the restore) --after-seconds=2
//           --display-restore=recreate|reuse --screen=N --validation=on|off
#include <windows.h>

#include <vulkan/vulkan.h>

#include <QFile>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QVulkanInstance>
#include <QtGui/rhi/qshader.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vx2_common.hpp"
#include "vx2_item.hpp"
#include "vx2_vulkan.hpp"
#include "vx5_budget.hpp"
#include "vx5_compute.hpp"

using namespace vx2;
using vx5::Block;
using vx5::Compute;

namespace {

struct Args {
  std::string      scenario = "budget";
  std::string      device;
  std::string      spv_dir;
  bool             validation = false;
  int              screen     = -1;
  vx5::BudgetArgs  budget;
  bool             pageable     = false;
  int              hold_mib     = 3072;
  double           hold_seconds = 30.0;
  std::string      fault_address = "fixed";
  int              observe_ms    = 1000;
  double           after_seconds = 2.0;
  std::string      display_restore = "recreate";
};

auto Value(const std::string& arg, const char* name) -> std::optional<std::string> {
  const std::string prefix = std::string(name) + "=";
  if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  return std::nullopt;
}

auto OnOff(const std::string& v) -> bool { return v == "on" || v == "1" || v == "true"; }

auto Parse(int argc, char** argv) -> Args {
  Args a;
  bool hold_seconds_set = false;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (auto v = Value(s, "--case")) a.scenario = *v;
    else if (auto v1 = Value(s, "--device")) a.device = *v1;
    else if (auto v2 = Value(s, "--spv")) a.spv_dir = *v2;
    else if (auto v3 = Value(s, "--validation")) a.validation = OnOff(*v3);
    else if (auto v4 = Value(s, "--screen")) a.screen = std::stoi(*v4);
    else if (auto v5 = Value(s, "--block-mib")) a.budget.block_mib = std::stoi(*v5);
    else if (auto v6 = Value(s, "--over-mib")) a.budget.over_mib = std::stoi(*v6);
    else if (auto v7 = Value(s, "--max-mib")) a.budget.max_mib = std::stoi(*v7);
    else if (auto v8 = Value(s, "--priority")) a.budget.priority = *v8;
    else if (auto v9 = Value(s, "--pageable")) a.pageable = OnOff(*v9);
    else if (auto va = Value(s, "--hold-seconds")) {
      a.budget.hold_seconds = std::stod(*va);
      a.hold_seconds        = std::stod(*va);
      hold_seconds_set      = true;
    } else if (auto vb = Value(s, "--work-mib")) a.budget.work_mib = std::stoi(*vb);
    else if (auto vc = Value(s, "--min-free-mib")) a.budget.min_free_mib = std::stoi(*vc);
    else if (auto vd = Value(s, "--hold-mib")) a.hold_mib = std::stoi(*vd);
    else if (auto ve = Value(s, "--fault-address")) a.fault_address = *ve;
    else if (auto vf = Value(s, "--observe-ms")) a.observe_ms = std::stoi(*vf);
    else if (auto vg = Value(s, "--after-seconds")) a.after_seconds = std::stod(*vg);
    else if (auto vh = Value(s, "--display-restore")) a.display_restore = *vh;
    else if (auto vi = Value(s, "--display-probe")) a.budget.display_probe = OnOff(*vi);
    else {
      Log("ERROR unknown argument %s", s.c_str());
      std::exit(2);
    }
  }
  if (!hold_seconds_set && a.scenario == "budget") a.budget.hold_seconds = 5.0;
  return a;
}

struct FrameRecord {
  std::int64_t  end_ns     = 0;
  std::uint64_t new_id     = 0;
  std::int64_t  request_ns = 0;
};

void ReportWindow(const char* label, const std::vector<FrameRecord>& frames, std::int64_t t0,
                  std::int64_t t1, double vsync_ms) {
  std::vector<double> period;
  std::int64_t        first = 0, last = 0;
  std::size_t         fresh = 0;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    if (frames[i].end_ns < t0 || frames[i].end_ns > t1) continue;
    if (first == 0) first = frames[i - 1].end_ns;
    last = frames[i].end_ns;
    period.push_back((frames[i].end_ns - frames[i - 1].end_ns) * 1e-6);
    if (frames[i].new_id != 0) ++fresh;
  }
  long over = 0;
  for (double p : period) {
    if (p > 1.5 * vsync_ms) ++over;
  }
  double longest = 0;
  for (double p : period) longest = std::max(longest, p);
  // The longest time without a frame end, the edges of the window included.
  std::int64_t edge = 0;
  for (const auto& f : frames) {
    if (f.end_ns >= t0) {
      edge = f.end_ns - t0;
      break;
    }
  }
  if (frames.empty() || frames.back().end_ns < t0) edge = t1 - t0;
  std::int64_t tail = frames.empty() ? t1 - t0 : t1 - std::max(t0, std::min(t1, frames.back().end_ns));
  const double gap  = std::max({longest, edge * 1e-6, tail * 1e-6});
  std::printf("RESULT window=%s seconds=%.2f frames=%zu new_images=%zu periods_over_1.5_vsync=%ld "
              "longest_gap_ms=%.1f %s\n",
              label, (t1 - t0) * 1e-9, period.size(), fresh, over, gap,
              Format("period_ms", Summarize(period)).c_str());
}

/// Calls @p then on the GUI thread when @p ready returns true (polled each 2 ms), or @p late after
/// @p timeout_ms.
void When(QObject* owner, std::function<bool()> ready, std::function<void()> then, int timeout_ms,
          std::function<void()> late) {
  auto*              timer    = new QTimer(owner);
  const std::int64_t deadline = NowNs() + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
  timer->setInterval(2);
  QObject::connect(timer, &QTimer::timeout, owner, [=] {
    if (ready()) {
      timer->stop();
      timer->deleteLater();
      then();
    } else if (NowNs() > deadline) {
      timer->stop();
      timer->deleteLater();
      late();
    }
  });
  timer->start();
}

auto FillSpirv() -> std::vector<std::uint32_t> {
  QFile f(QStringLiteral(":/vx2/vx2_fill.comp.qsb"));
  f.open(QIODevice::ReadOnly);
  const QShader    shader = QShader::fromSerialized(f.readAll());
  const QByteArray spv    = shader.shader({QShader::SpirvShader, QShaderVersion(100)}).shader();
  std::vector<std::uint32_t> words(static_cast<std::size_t>(spv.size()) / 4);
  std::memcpy(words.data(), spv.constData(), words.size() * 4);
  return words;
}

/// Runs "dxcap -forcetdr" and waits for it. The exit code, or -1 when it does not start.
auto ForceTdr(double& ms) -> long {
  wchar_t             cmd[] = L"dxcap.exe -forcetdr";
  STARTUPINFOW        si{};
  PROCESS_INFORMATION pi{};
  si.cb                 = sizeof(si);
  const std::int64_t t0 = NowNs();
  if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    ms = 0;
    return -1;
  }
  WaitForSingleObject(pi.hProcess, 30'000);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  ms = (NowNs() - t0) * 1e-6;
  return static_cast<long>(code);
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  QGuiApplication app(argc, argv);
  const std::string spv_dir =
      args.spv_dir.empty() ? QCoreApplication::applicationDirPath().toStdString() + "/spv" : args.spv_dir;
  const std::string spv_path = spv_dir + "/vx5_mem.spv";

  VulkanOptions o;
  o.device_filter   = args.device;
  o.validation      = args.validation;
  o.split_device    = true;  // B5: pipeline device and display device
  o.separate_queue  = false;
  o.memory_priority = args.budget.priority != "none" || args.pageable;
  o.pageable        = args.pageable;
  o.device_fault    = true;
  std::unique_ptr<VulkanContext> ctx;
  try {
    ctx = std::make_unique<VulkanContext>(o);
  } catch (const std::exception& e) {
    Log("ERROR %s", e.what());
    return 1;
  }
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(ctx->physical, &props);
  Log("DEVICE '%s' driver=%u memory_priority=%d pageable=%d device_fault=%d", props.deviceName,
      props.driverVersion, ctx->memory_priority_enabled ? 1 : 0, ctx->pageable_enabled ? 1 : 0,
      ctx->device_fault_enabled ? 1 : 0);

  if (args.scenario == "budget" || args.scenario == "hold") {
    int code = 0;
    try {
      Compute compute(*ctx, ctx->pipe_device, spv_path);
      vx5::BudgetArgs b = args.budget;
      b.spv_path        = spv_path;
      code = args.scenario == "budget" ? vx5::RunBudget(*ctx, compute, b)
                                       : vx5::RunHold(*ctx, compute, args.hold_mib, args.hold_seconds);
    } catch (const std::exception& e) {
      Log("ERROR %s", e.what());
      code = 1;
    }
    std::printf("RESULT %s\n", ctx->ValidationReport().c_str());
    std::fflush(stdout);
    return code != 0 ? code : (ctx->ValidationErrors() > 0 ? 3 : 0);
  }
  if (args.scenario != "lost" && args.scenario != "tdr") {
    Log("ERROR unknown case %s", args.scenario.c_str());
    return 2;
  }

  // --- the viewport of VX2 and VX3 on the display device ---
  QVulkanInstance qvk;
  QByteArrayList  exts;
  for (const auto& e : ctx->instance_extensions) exts << QByteArray::fromStdString(e);
  qvk.setVkInstance(ctx->instance);
  qvk.setExtensions(exts);
  if (!qvk.create()) {
    Log("ERROR QVulkanInstance::create");
    return 1;
  }
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
  const auto screens = QGuiApplication::screens();
  QScreen*   screen  = args.screen >= 0 && args.screen < screens.size() ? screens[args.screen]
                                                                         : QGuiApplication::primaryScreen();
  const double vsync_ms = 1000.0 / screen->refreshRate();
  const int                  img_w = 2560, img_h = 1600;
  const std::vector<float>   fixture = MakeFixture(img_w, img_h);
  const std::vector<std::uint32_t> fill_spirv = FillSpirv();

  std::unique_ptr<Producer> producer;
  std::unique_ptr<Compute>  compute;
  Block                     out;
  try {
    producer = std::make_unique<Producer>(*ctx, fixture, img_w, img_h, 0, fill_spirv);
    compute  = std::make_unique<Compute>(*ctx, ctx->pipe_device, spv_path);
    double ms = 0;
    VkCheck(compute->Allocate(64 * 1024, -1.0f, out, ms), "vkAllocateMemory (fault output)");
  } catch (const std::exception& e) {
    Log("ERROR setup: %s", e.what());
    return 1;
  }

  // The producer that the render thread reports frames to, and the one that gets requests.
  std::mutex                producer_mutex;
  Producer*                 live_producer    = producer.get();
  Producer*                 current_producer = producer.get();
  std::mutex                records_mutex;
  std::vector<FrameRecord>  records;
  std::atomic<bool>         restore_armed{false};
  std::atomic<std::int64_t> first_new_frame_ns{0};
  std::atomic<int>          scene_graph_errors{0};
  QObject                   driver;

  struct View {
    QQuickWindow*     window = nullptr;
    QPointer<Vx2Item> item;
  };
  View view;
  auto make_view = [&](Producer* p) {
    View v;
    v.window = new QQuickWindow();
    v.window->setScreen(screen);
    v.window->setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint);
    v.window->setTitle(QStringLiteral("alcedo VX5"));
    v.window->setVulkanInstance(&qvk);
    v.window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
        ctx->physical, ctx->device, static_cast<int>(ctx->family), static_cast<int>(ctx->qt_queue_index)));
    const QRect avail = screen->availableGeometry();
    v.window->setGeometry(avail.x() + 40, avail.y() + 40, 1280, 800);
    v.item = new Vx2Item(v.window->contentItem());
    v.item->setSize(QSizeF(1280, 800));
    Vx2Item::Config cfg;
    cfg.source          = PhotoSource::VulkanProducer;
    cfg.producer        = p;
    cfg.fixture         = &fixture;
    cfg.width           = img_w;
    cfg.height          = img_h;
    cfg.flip_correction = true;
    v.item->Configure(cfg);
    QObject::connect(
        v.window, &QQuickWindow::afterFrameEnd, v.window,
        [&] {
          const std::uint64_t n = ++Vx2Item::submitted_frames;
          {
            std::lock_guard<std::mutex> lock(producer_mutex);
            if (live_producer) live_producer->SetSubmittedFrames(n);
          }
          FrameRecord r;
          r.end_ns       = NowNs();
          r.new_id       = Vx2Item::marks.new_id;
          r.request_ns   = Vx2Item::marks.new_request_ns;
          Vx2Item::marks = {};
          if (r.new_id != 0 && restore_armed && first_new_frame_ns == 0) first_new_frame_ns = r.end_ns;
          std::lock_guard<std::mutex> lock(records_mutex);
          records.push_back(r);
        },
        Qt::DirectConnection);
    QPointer<Vx2Item> item = v.item;
    std::uint64_t*    count = new std::uint64_t(0);
    QObject::connect(v.window, &QObject::destroyed, [count] { delete count; });
    QObject::connect(v.window, &QQuickWindow::frameSwapped, v.item, [&, item, count] {
      if (current_producer) {
        current_producer->Request(0.6f + 0.4f * static_cast<float>(std::sin((*count)++ * 0.05)), NowNs());
      }
      if (item) item->update();
    }, Qt::QueuedConnection);
    QObject::connect(v.window, &QQuickWindow::sceneGraphError, &driver,
                     [&](QQuickWindow::SceneGraphError error, const QString& message) {
                       scene_graph_errors++;
                       Log("QT sceneGraphError %d '%s' t=%lld", static_cast<int>(error),
                           message.toUtf8().constData(), static_cast<long long>(NowNs()));
                     });
    QObject::connect(v.window, &QQuickWindow::sceneGraphInvalidated, &driver, [] {
      Log("QT sceneGraphInvalidated t=%lld", static_cast<long long>(NowNs()));
    }, Qt::DirectConnection);
    QObject::connect(v.window, &QQuickWindow::sceneGraphInitialized, &driver, [] {
      Log("QT sceneGraphInitialized t=%lld", static_cast<long long>(NowNs()));
    }, Qt::DirectConnection);
    return v;
  };
  auto on_ready = [&](Producer* p) {
    p->Start([&] {
      QMetaObject::invokeMethod(&driver, [&] {
        if (view.item) view.item->update();
      }, Qt::QueuedConnection);
    });
  };
  view = make_view(producer.get());
  Log("SCREEN '%s' refresh=%.2f", screen->name().toUtf8().constData(), screen->refreshRate());

  // Watchdog: a fence of each device that is never submitted. vkGetFenceStatus returns
  // VK_ERROR_DEVICE_LOST after a loss on drivers that report it there.
  std::mutex               watch_mutex;
  VkFence                  watch_display = VK_NULL_HANDLE, watch_pipe = VK_NULL_HANDLE;
  std::atomic<std::int64_t> display_lost_ns{0}, pipe_lost_ns{0};
  std::atomic<int>          display_status{0}, pipe_status{0};
  auto make_watch_fences = [&] {
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCheck(vkCreateFence(ctx->device, &f, nullptr, &watch_display), "vkCreateFence (watch)");
    VkCheck(vkCreateFence(ctx->pipe_device, &f, nullptr, &watch_pipe), "vkCreateFence (watch)");
  };
  make_watch_fences();
  std::atomic<bool> watch_stop{false};
  std::thread       watchdog([&] {
    while (!watch_stop) {
      {
        std::lock_guard<std::mutex> lock(watch_mutex);
        if (watch_display != VK_NULL_HANDLE) {
          const VkResult d = vkGetFenceStatus(ctx->device, watch_display);
          const VkResult p = vkGetFenceStatus(ctx->pipe_device, watch_pipe);
          if (d != VK_NOT_READY && display_lost_ns == 0) {
            display_lost_ns = NowNs();
            display_status  = d;
            Log("WATCH display device fence status %d t=%lld", static_cast<int>(d),
                static_cast<long long>(NowNs()));
          }
          if (p != VK_NOT_READY && pipe_lost_ns == 0) {
            pipe_lost_ns = NowNs();
            pipe_status  = p;
            Log("WATCH pipeline device fence status %d t=%lld", static_cast<int>(p),
                static_cast<long long>(NowNs()));
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  auto drop_watch_fences = [&] {
    std::lock_guard<std::mutex> lock(watch_mutex);
    vkDestroyFence(ctx->device, watch_display, nullptr);
    vkDestroyFence(ctx->pipe_device, watch_pipe, nullptr);
    watch_display = watch_pipe = VK_NULL_HANDLE;
  };

  std::int64_t idle_begin = 0, load_begin = 0, detect_ns = 0, restore_begin = 0, restored_ns = 0,
               end_ns = 0;
  std::atomic<std::int64_t> detect_atomic{0};
  std::atomic<int>          detect_result{0};
  std::string               detect_how;
  std::mutex                detect_mutex;
  std::thread               load;
  int                       exit_code = 0;
  auto fail = [&](const char* what) {
    Log("ERROR restore: %s", what);
    exit_code = 4;
    end_ns    = NowNs();
    app.quit();
  };

  // The end: a touch pass on the restored pipeline device, then quit.
  auto finish = [&] {
    restored_ns = first_new_frame_ns.load();
    std::printf("RESULT restore first_new_frame_after_detect_ms=%.1f\n", (restored_ns - detect_ns) * 1e-6);
    QTimer::singleShot(static_cast<int>(args.after_seconds * 1000), &driver, [&] {
      try {
        Block  b;
        double ms = 0;
        VkCheck(compute->Allocate(256ull << 20, -1.0f, b, ms), "vkAllocateMemory (check)");
        VkCheck(compute->Fill(b), "fill (check)");
        std::vector<double> t;
        VkCheck(compute->Touch({&b}, t), "touch (check)");
        std::printf("RESULT check touch_gbps=%.1f\n", vx5::Gbps(b.size, t[0]));
        compute->Free(b);
      } catch (const std::exception& e) {
        Log("ERROR check: %s", e.what());
        exit_code = 5;
      }
      end_ns = NowNs();
      app.quit();
    });
  };

  // Restore of the pipeline device while Qt Quick keeps the display device.
  auto restore_pipeline = [&] {
    restore_begin = NowNs();
    view.item->SwitchProducer(nullptr);
    std::uint64_t gen = 0;
    (void)view.item->PendingProducer(gen);
    const std::uint64_t frames_at = Vx2Item::submitted_frames.load();
    When(&driver, [=] { return Vx2Item::switch_generation_applied >= gen && Vx2Item::submitted_frames >= frames_at + 4; },
         [&] {
           const std::int64_t t_drop = NowNs();
           {
             std::lock_guard<std::mutex> lock(producer_mutex);
             live_producer    = nullptr;
             current_producer = nullptr;
           }
           try {
             producer->Stop();
             producer.reset();
             compute->Free(out);
             compute.reset();
             drop_watch_fences();
             const std::int64_t t_objects = NowNs();
             ctx->RecreatePipelineDevice();
             const std::int64_t t_device = NowNs();
             {
               std::lock_guard<std::mutex> lock(watch_mutex);
               VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
               VkCheck(vkCreateFence(ctx->device, &f, nullptr, &watch_display), "vkCreateFence (watch)");
               VkCheck(vkCreateFence(ctx->pipe_device, &f, nullptr, &watch_pipe), "vkCreateFence (watch)");
               pipe_lost_ns = 0;
             }
             compute  = std::make_unique<Compute>(*ctx, ctx->pipe_device, spv_path);
             producer = std::make_unique<Producer>(*ctx, fixture, img_w, img_h, 0, fill_spirv, false);
             const std::int64_t t_producer = NowNs();
             std::printf("RESULT restore pipeline wait_for_qt_ms=%.1f destroy_ms=%.1f device_ms=%.1f "
                         "producer_ms=%.1f\n",
                         (t_drop - restore_begin) * 1e-6, (t_objects - t_drop) * 1e-6,
                         (t_device - t_objects) * 1e-6, (t_producer - t_device) * 1e-6);
           } catch (const std::exception& e) {
             Log("ERROR %s", e.what());
             fail("pipeline device recreate");
             return;
           }
           view.item->SwitchProducer(producer.get());
           std::uint64_t gen2 = 0;
           (void)view.item->PendingProducer(gen2);
           When(&driver,
                [=] {
                  return Vx2Item::switch_generation_applied >= gen2 &&
                         Vx2Item::submitted_frames >= Vx2Item::layouts_frame + 2;
                },
                [&] {
                  {
                    std::lock_guard<std::mutex> lock(producer_mutex);
                    live_producer    = producer.get();
                    current_producer = producer.get();
                  }
                  restore_armed = true;
                  on_ready(producer.get());
                  producer->Request(1.0f, NowNs());
                  When(&driver, [&] { return first_new_frame_ns != 0; }, finish, 5000,
                       [&] { fail("no frame of the new producer"); });
                },
                5000, [&] { fail("the layouts frame of the new producer"); });
         },
         5000, [&] { fail("Qt did not drop the old producer"); });
  };

  // Restore of both devices: Qt Quick's window goes, both devices are new, a new window starts
  // (recreate), or the same window gets the new device (reuse).
  auto restore_both = [&] {
    restore_begin = NowNs();
    {
      std::lock_guard<std::mutex> lock(producer_mutex);
      live_producer    = nullptr;
      current_producer = nullptr;
    }
    try {
      producer->Stop();
      const std::int64_t t0 = NowNs();
      if (args.display_restore == "reuse") {
        view.window->hide();
        view.window->releaseResources();
      } else {
        view.window->hide();
        delete view.window;
        view = {};
      }
      QCoreApplication::processEvents();
      const std::int64_t t_window = NowNs();
      producer.reset();
      compute->Free(out);
      compute.reset();
      drop_watch_fences();
      ctx->RecreatePipelineDevice();
      ctx->RecreateDisplayDevice();
      const std::int64_t t_devices = NowNs();
      {
        std::lock_guard<std::mutex> lock(watch_mutex);
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkCheck(vkCreateFence(ctx->device, &f, nullptr, &watch_display), "vkCreateFence (watch)");
        VkCheck(vkCreateFence(ctx->pipe_device, &f, nullptr, &watch_pipe), "vkCreateFence (watch)");
        display_lost_ns = 0;
        pipe_lost_ns    = 0;
      }
      compute = std::make_unique<Compute>(*ctx, ctx->pipe_device, spv_path);
      // recreate: no window draws now, so the producer sets the layouts on the display queue.
      const bool reuse = args.display_restore == "reuse";
      producer = std::make_unique<Producer>(*ctx, fixture, img_w, img_h, 0, fill_spirv, !reuse);
      const std::int64_t t_producer = NowNs();
      std::printf("RESULT restore both window_ms=%.1f devices_ms=%.1f producer_ms=%.1f\n",
                  (t_window - t0) * 1e-6, (t_devices - t_window) * 1e-6, (t_producer - t_devices) * 1e-6);
      if (reuse) {
        view.window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
            ctx->physical, ctx->device, static_cast<int>(ctx->family), static_cast<int>(ctx->qt_queue_index)));
        view.item->SwitchProducer(producer.get());
        view.window->show();
        std::uint64_t gen2 = 0;
        (void)view.item->PendingProducer(gen2);
        When(&driver,
             [=] {
               return Vx2Item::switch_generation_applied >= gen2 &&
                      Vx2Item::submitted_frames >= Vx2Item::layouts_frame + 2;
             },
             [&] {
               {
                 std::lock_guard<std::mutex> lock(producer_mutex);
                 live_producer    = producer.get();
                 current_producer = producer.get();
               }
               restore_armed = true;
               on_ready(producer.get());
               producer->Request(1.0f, NowNs());
               When(&driver, [&] { return first_new_frame_ns != 0; }, finish, 5000,
                    [&] { fail("no frame after the reuse of the window"); });
             },
             5000, [&] { fail("the reused window drew no layouts frame"); });
      } else {
        view = make_view(producer.get());
        {
          std::lock_guard<std::mutex> lock(producer_mutex);
          live_producer    = producer.get();
          current_producer = producer.get();
        }
        restore_armed = true;
        on_ready(producer.get());
        view.window->show();
        producer->Request(1.0f, NowNs());
        When(&driver, [&] { return first_new_frame_ns != 0; }, finish, 5000,
             [&] { fail("no frame in the new window"); });
      }
    } catch (const std::exception& e) {
      Log("ERROR %s", e.what());
      fail("both devices recreate");
    }
  };

  // After the detection: observe for observe_ms (the window keeps the last image of the lost
  // device), then restore.
  auto on_detect = [&] {
    detect_ns = detect_atomic.load();
    {
      std::lock_guard<std::mutex> lock(detect_mutex);
      std::printf("RESULT detect how='%s' vk=%d after_load_ms=%.1f\n", detect_how.c_str(),
                  detect_result.load(), (detect_ns - load_begin) * 1e-6);
    }
    std::printf("RESULT fault_info pipeline: %s\n", compute ? compute->FaultReport().c_str() : "-");
    QTimer::singleShot(args.observe_ms, &driver, [&] {
      // A third signal: Qt Quick ended no frame in the last 500 ms.
      std::int64_t last_frame = 0;
      {
        std::lock_guard<std::mutex> lock(records_mutex);
        if (!records.empty()) last_frame = records.back().end_ns;
      }
      const bool no_frames    = NowNs() - last_frame > 500'000'000ll;
      const bool display_lost = display_lost_ns != 0 || scene_graph_errors > 0 || no_frames;
      std::printf("RESULT before_restore display_lost=%d no_frames_500ms=%d display_status=%d pipe_lost_watch=%d "
                  "pipe_status=%d scene_graph_errors=%d producer_failed=%d qt_frames_after_detect=%zu\n",
                  display_lost ? 1 : 0, no_frames ? 1 : 0, display_status.load(), pipe_lost_ns != 0 ? 1 : 0,
                  pipe_status.load(), scene_graph_errors.load(), producer && producer->Failed() ? 1 : 0,
                  [&] {
                    std::lock_guard<std::mutex> lock(records_mutex);
                    std::size_t n = 0;
                    for (const auto& r : records) n += r.end_ns > detect_ns ? 1 : 0;
                    return n;
                  }());
      if (display_lost) {
        restore_both();
      } else {
        restore_pipeline();
      }
    });
  };

  QTimer::singleShot(0, &driver, [&] {
    view.window->show();
    producer->Request(1.0f, NowNs());
  });
  QTimer::singleShot(1000, &driver, [&] { idle_begin = NowNs(); });
  QTimer::singleShot(2000, &driver, [&] {
    load_begin = NowNs();
    load       = std::thread([&] {
      VkResult    r = VK_SUCCESS;
      std::string how;
      if (args.scenario == "lost") {
        VkDeviceAddress bad = 0x00007F0000000000ull;
        if (args.fault_address == "freed") {
          Block  freed;
          double ms = 0;
          VkCheck(compute->Allocate(64ull << 20, -1.0f, freed, ms), "vkAllocateMemory (freed)");
          bad = freed.address;
          compute->Free(freed);
        } else if (args.fault_address != "fixed") {
          bad = std::stoull(args.fault_address, nullptr, 16);
        }
        double wait = 0;
        r           = compute->Fault(bad, out.address, 10'000.0, wait);
        std::printf("RESULT fault address=0x%llx vk=%d wait_ms=%.1f\n", static_cast<unsigned long long>(bad),
                    static_cast<int>(r), wait);
        how = "fault fence";
        if (r == VK_SUCCESS) {
          // No loss at the fault: the pipeline device is checked with empty submissions.
          for (int i = 0; i < 20 && r == VK_SUCCESS; ++i) {
            r = compute->Ping(2000.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          how = r == VK_SUCCESS ? "no loss" : "ping after the fault";
        }
      } else {
        double     ms   = 0;
        const long code = ForceTdr(ms);
        std::printf("RESULT tdr dxcap_exit=%ld dxcap_ms=%.1f\n", code, ms);
        how = "ping after dxcap";
        for (int i = 0; i < 200 && r == VK_SUCCESS; ++i) {
          r = compute->Ping(2000.0);
          if (r == VK_SUCCESS) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (r == VK_SUCCESS) how = "no loss";
      }
      std::fflush(stdout);
      {
        std::lock_guard<std::mutex> lock(detect_mutex);
        detect_how = how;
      }
      detect_result = r;
      detect_atomic = NowNs();
      if (r == VK_SUCCESS) {
        QMetaObject::invokeMethod(&driver, [&] {
          detect_ns = detect_atomic.load();
          Log("RESULT no device loss");
          QTimer::singleShot(1000, &driver, [&] {
            end_ns = NowNs();
            app.quit();
          });
        }, Qt::QueuedConnection);
        return;
      }
      QMetaObject::invokeMethod(&driver, on_detect, Qt::QueuedConnection);
    });
  });
  // A run never takes more than 120 s (a GPU stall lasts until the Windows GPU timeout).
  QTimer::singleShot(120'000, &driver, [&] { fail("the run took 120 s"); });
  app.exec();
  if (load.joinable()) load.join();
  if (end_ns == 0) end_ns = NowNs();

  std::vector<FrameRecord> frames;
  {
    std::lock_guard<std::mutex> lock(records_mutex);
    frames = records;
  }
  std::printf("RESULT case=%s device='%s' screen='%s' refresh=%.2f display_restore=%s fault=%s "
              "renderer_errors=%d scene_graph_errors=%d\n",
              args.scenario.c_str(), ctx->device_name.c_str(), screen->name().toUtf8().constData(),
              screen->refreshRate(), args.display_restore.c_str(), args.fault_address.c_str(),
              Vx2Item::errors.load(), scene_graph_errors.load());
  if (idle_begin && load_begin) ReportWindow("idle", frames, idle_begin, load_begin, vsync_ms);
  if (load_begin && detect_ns) ReportWindow("to_detect", frames, load_begin, detect_ns, vsync_ms);
  if (detect_ns && restore_begin) ReportWindow("observe", frames, detect_ns, restore_begin, vsync_ms);
  if (restore_begin && restored_ns) ReportWindow("restore", frames, restore_begin, restored_ns, vsync_ms);
  if (restored_ns) ReportWindow("restored", frames, restored_ns, end_ns, vsync_ms);

  watch_stop = true;
  watchdog.join();
  {
    std::lock_guard<std::mutex> lock(producer_mutex);
    live_producer    = nullptr;
    current_producer = nullptr;
  }
  if (producer) producer->Stop();
  delete view.window;
  producer.reset();
  if (compute && out.buffer != VK_NULL_HANDLE) compute->Free(out);
  compute.reset();
  drop_watch_fences();
  std::printf("RESULT %s\n", ctx->ValidationReport().c_str());
  std::fflush(stdout);
  if (exit_code != 0) return exit_code;
  return ctx->ValidationErrors() > 0 ? 3 : 0;
}
