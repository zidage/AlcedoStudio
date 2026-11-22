//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: long dispatch on the shared physical device. The VX2 viewport (Qt Quick on the
// display VkDevice, the preview producer on the pipeline VkDevice) runs while a 45 megapixel render
// or a timeout kernel runs on the pipeline VkDevice. Experiment code: it does not merge.
//
// vx3_load --case=kernels|render|spin|idle --device=<part of name> [options]
//   --image=8256x5504 --blur-radius=32 --blur-pairs=3
//   --split=whole|op|budget --budget-ms=N --inflight=1|2 --repeat=N --window=on|off
//   --spin=wide|long|many --spin-ms=N --spin-count=N --screen=N --validation=on|off
#include <windows.h>

#include <vulkan/vulkan.h>

#include <QFile>
#include <QGuiApplication>
#include <QQuickGraphicsDevice>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QVulkanInstance>
#include <QtGui/rhi/qshader.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vx2_common.hpp"
#include "vx2_item.hpp"
#include "vx2_vulkan.hpp"
#include "vx3_load.hpp"

using namespace vx2;
using vx3::Gpu;

namespace {

struct Args {
  std::string scenario = "kernels";
  std::string device;
  std::string split    = "whole";
  std::string spin     = "wide";
  std::string spv_dir;
  double      budget_ms  = 16.0;
  double      spin_ms    = 3000.0;
  int         spin_count = 300;
  int         inflight   = 1;
  int         repeat     = 1;
  int         width      = 8256;
  int         height     = 5504;
  int         blur_radius = 32;
  int         blur_pairs  = 3;
  int         screen      = -1;
  bool        window      = true;
  bool        validation  = false;
  double      idle_seconds = 4.0;
  /// Long edge of the preview render on the pipeline device while the export runs: 2560 (drag),
  /// 3840 (settle), 0 (none). The preview has the aspect of the export image.
  int         preview     = 0;
  /// Time from the start of one preview request to the next: 16.7 ms (a drag at 60 Hz) or longer.
  double      preview_interval_ms = 16.7;
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
    if (auto v = Value(s, "--case")) a.scenario = *v;
    else if (auto v1 = Value(s, "--device")) a.device = *v1;
    else if (auto v2 = Value(s, "--split")) a.split = *v2;
    else if (auto v3 = Value(s, "--budget-ms")) a.budget_ms = std::stod(*v3);
    else if (auto v4 = Value(s, "--inflight")) a.inflight = std::stoi(*v4);
    else if (auto v5 = Value(s, "--repeat")) a.repeat = std::stoi(*v5);
    else if (auto v6 = Value(s, "--image")) {
      const auto x = v6->find('x');
      a.width      = std::stoi(v6->substr(0, x));
      a.height     = std::stoi(v6->substr(x + 1));
    } else if (auto v7 = Value(s, "--blur-radius")) a.blur_radius = std::stoi(*v7);
    else if (auto v8 = Value(s, "--blur-pairs")) a.blur_pairs = std::stoi(*v8);
    else if (auto v9 = Value(s, "--screen")) a.screen = std::stoi(*v9);
    else if (auto va = Value(s, "--window")) a.window = OnOff(*va);
    else if (auto vb = Value(s, "--validation")) a.validation = OnOff(*vb);
    else if (auto vc = Value(s, "--spin")) a.spin = *vc;
    else if (auto vd = Value(s, "--spin-ms")) a.spin_ms = std::stod(*vd);
    else if (auto ve = Value(s, "--spin-count")) a.spin_count = std::stoi(*ve);
    else if (auto vf = Value(s, "--spv")) a.spv_dir = *vf;
    else if (auto vg = Value(s, "--idle-seconds")) a.idle_seconds = std::stod(*vg);
    else if (auto vh = Value(s, "--preview")) a.preview = std::stoi(*vh);
    else if (auto vi = Value(s, "--preview-interval-ms")) a.preview_interval_ms = std::stod(*vi);
    else {
      Log("ERROR unknown argument %s", s.c_str());
      std::exit(2);
    }
  }
  return a;
}

struct FrameRecord {
  std::int64_t  end_ns     = 0;
  std::uint64_t new_id     = 0;
  std::int64_t  request_ns = 0;
};

/// Statistics of the frames whose end is in [t0, t1].
void ReportWindow(const char* label, const std::vector<FrameRecord>& frames,
                  const std::vector<std::pair<std::int64_t, double>>& gui_late, std::int64_t t0,
                  std::int64_t t1, double vsync_ms) {
  std::vector<double> period, latency, late;
  std::int64_t        first = 0, last = 0;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    if (frames[i].end_ns < t0 || frames[i].end_ns > t1) continue;
    if (first == 0) first = frames[i - 1].end_ns;
    last = frames[i].end_ns;
    period.push_back((frames[i].end_ns - frames[i - 1].end_ns) * 1e-6);
    if (frames[i].new_id != 0) latency.push_back((frames[i].end_ns - frames[i].request_ns) * 1e-6);
  }
  for (const auto& [ns, ms] : gui_late) {
    if (ns >= t0 && ns <= t1) late.push_back(ms);
  }
  const double elapsed = (last - first) * 1e-6;
  const long   missed  = period.empty() ? 0 : std::lround(elapsed / vsync_ms) - static_cast<long>(period.size());
  long         over    = 0;
  for (double p : period) {
    if (p > 1.5 * vsync_ms) ++over;
  }
  std::printf("RESULT window=%s seconds=%.2f frames=%zu missed_vsyncs=%ld periods_over_1.5_vsync=%ld\n",
              label, (t1 - t0) * 1e-9, period.size(), missed, over);
  std::printf("RESULT window=%s %s\n", label, Format("period_ms", Summarize(period)).c_str());
  // The display image of the producer (a fill kernel): request to the end of the frame that shows it.
  std::printf("RESULT window=%s %s\n", label, Format("display_image_latency_ms", Summarize(latency)).c_str());
  std::printf("RESULT window=%s %s\n", label, Format("gui_late_ms", Summarize(late)).c_str());
}

/// One preview render: request (start of the wait for the queue) to the fence.
struct PreviewSample {
  std::int64_t start_ns   = 0;
  double       latency_ms = 0;
  double       gpu_ms     = 0;
};

void ReportPreview(const char* label, const std::vector<PreviewSample>& samples, std::int64_t t0,
                   std::int64_t t1) {
  std::vector<double> latency, gpu;
  for (const auto& s : samples) {
    if (s.start_ns < t0 || s.start_ns > t1) continue;
    latency.push_back(s.latency_ms);
    gpu.push_back(s.gpu_ms);
  }
  std::printf("RESULT preview=%s %s\n", label, Format("preview_render_latency_ms", Summarize(latency)).c_str());
  std::printf("RESULT preview=%s %s\n", label, Format("preview_render_gpu_ms", Summarize(gpu)).c_str());
}

void ReportRun(const char* label, const vx3::RunRecord& run) {
  std::vector<double> gpu, wall, gaps;
  double              gpu_sum = 0;
  for (std::size_t i = 0; i < run.chunks.size(); ++i) {
    const auto& c = run.chunks[i];
    if (c.complete_ns == 0) continue;
    gpu.push_back(c.gpu_ms);
    gpu_sum += c.gpu_ms;
    wall.push_back((c.complete_ns - c.submit_ns) * 1e-6);
  }
  const double total = (run.end_ns - run.begin_ns) * 1e-6;
  std::printf("RESULT run=%s submissions=%zu wall_ms=%.2f gpu_sum_ms=%.2f idle_ms=%.2f device_lost=%d error='%s'\n",
              label, run.chunks.size(), total, gpu_sum, total - gpu_sum, run.device_lost ? 1 : 0,
              run.error.c_str());
  std::printf("RESULT run=%s %s\n", label, Format("submission_gpu_ms", Summarize(gpu)).c_str());
  std::printf("RESULT run=%s %s\n", label, Format("submission_wall_ms", Summarize(wall)).c_str());
}

/// Per-op calibration table, grouped by op name.
void ReportKernels(const std::vector<vx3::Op>& ops, std::uint32_t width, std::uint32_t height) {
  struct Group {
    std::string cls;
    int         count = 0;
    double      ms    = 0;
    double      max   = 0;
    double      pixels = 0;
  };
  std::map<std::string, Group> groups;
  std::map<std::string, double> classes;
  double total = 0;
  for (const auto& op : ops) {
    auto& g = groups[op.name];
    g.cls   = op.cls;
    g.count++;
    g.ms += op.ms;
    g.max = std::max(g.max, op.ms);
    g.pixels += double(op.width) * op.height;
    classes[op.cls] += op.ms;
    total += op.ms;
  }
  for (const auto& [name, g] : groups) {
    std::printf("RESULT kernel=%s class=%s dispatches=%d total_ms=%.3f max_ms=%.3f ms_per_mp=%.4f\n",
                name.c_str(), g.cls.c_str(), g.count, g.ms, g.max, g.ms / (g.pixels * 1e-6));
  }
  for (const auto& [cls, ms] : classes) std::printf("RESULT class=%s total_ms=%.3f\n", cls.c_str(), ms);
  std::printf("RESULT render image=%ux%u ops=%zu total_ms=%.3f\n", width, height, ops.size(), total);
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  QGuiApplication app(argc, argv);
  const std::string spv_dir =
      args.spv_dir.empty() ? QCoreApplication::applicationDirPath().toStdString() + "/spv" : args.spv_dir;

  VulkanOptions o;
  o.device_filter = args.device;
  o.validation    = args.validation;
  o.split_device  = true;  // B5: pipeline device and display device
  o.separate_queue = false;
  std::unique_ptr<VulkanContext> ctx;
  try {
    ctx = std::make_unique<VulkanContext>(o);
  } catch (const std::exception& e) {
    Log("ERROR %s", e.what());
    return 1;
  }
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(ctx->physical, &props);
  Log("DEVICE '%s' driver=%u maxComputeWorkGroupCount=%u,%u timestampPeriod=%.3f", props.deviceName,
      props.driverVersion, props.limits.maxComputeWorkGroupCount[0],
      props.limits.maxComputeWorkGroupCount[1], props.limits.timestampPeriod);

  auto gpu = std::make_unique<Gpu>(*ctx, spv_dir);
  vx3::Render render;
  vx3::Spin   spin;
  std::vector<vx3::Op>    ops;
  std::vector<vx3::Chunk> chunks;
  try {
    if (args.scenario == "kernels" || args.scenario == "render") {
      render.Build(*gpu, static_cast<std::uint32_t>(args.width), static_cast<std::uint32_t>(args.height),
                   args.blur_radius, args.blur_pairs);
      gpu->RunOnce(render.init);
      gpu->Calibrate(render.ops, 3);
      Log("RENDER image=%dx%d llf=%ux%u levels=%d samples=%d blur_radius=%d blur_pairs=%d "
          "allocated_mib=%llu",
          args.width, args.height, render.llf_width, render.llf_height, render.llf_levels,
          render.llf_samples, args.blur_radius, args.blur_pairs,
          static_cast<unsigned long long>(gpu->BytesAllocated() >> 20));
      ops    = render.ops;
      chunks = vx3::Plan(ops, args.split, args.budget_ms);
      double max_estimate = 0;
      for (const auto& c : chunks) max_estimate = std::max(max_estimate, c.estimate_ms);
      std::printf("RESULT plan split=%s budget_ms=%.1f submissions=%zu largest_estimate_ms=%.2f\n",
                  args.split.c_str(), args.budget_ms, chunks.size(), max_estimate);
    } else if (args.scenario == "spin") {
      spin.Build(*gpu, args.spin, args.spin_ms, args.spin_count);
      ops = spin.ops;
      vx3::Chunk all;
      for (std::size_t i = 0; i < ops.size(); ++i) all.bands.push_back({static_cast<int>(i), 0, ops[i].height});
      chunks.push_back(all);
    } else if (args.scenario != "idle") {
      Log("ERROR unknown case %s", args.scenario.c_str());
      return 2;
    }
  } catch (const std::exception& e) {
    Log("ERROR setup: %s", e.what());
    return 1;
  }

  // The preview render: the same ops at the preview size, on a Gpu of its own (command pool, query
  // pool, fences) that shares the pipeline queue. The blur radius scales with the image, as a radius
  // in millimetres of film does.
  std::unique_ptr<Gpu>    preview_gpu;
  vx3::Render             preview;
  std::vector<vx3::Chunk> preview_chunks;
  if (args.preview > 0 && args.scenario != "kernels") {
    try {
      preview_gpu       = std::make_unique<Gpu>(*ctx, spv_dir);
      const double s    = static_cast<double>(args.preview) / std::max(args.width, args.height);
      const auto   pw   = static_cast<std::uint32_t>(std::lround(args.width * s));
      const auto   ph   = static_cast<std::uint32_t>(std::lround(args.height * s));
      const int    pr   = std::max(1, static_cast<int>(std::lround(args.blur_radius * s)));
      preview.Build(*preview_gpu, pw, ph, pr, args.blur_pairs);
      preview_gpu->RunOnce(preview.init);
      preview_gpu->Calibrate(preview.ops, 3);
      preview_chunks = vx3::Plan(preview.ops, "whole", 0);
      Log("PREVIEW image=%ux%u blur_radius=%d interval_ms=%.1f gpu_ms=%.2f", pw, ph, pr,
          args.preview_interval_ms, preview_chunks[0].estimate_ms);
    } catch (const std::exception& e) {
      Log("ERROR preview setup: %s", e.what());
      return 1;
    }
  }

  if (args.scenario == "kernels") {
    ReportKernels(ops, static_cast<std::uint32_t>(args.width), static_cast<std::uint32_t>(args.height));
    for (const char* mode : {"whole", "op"}) {
      const auto plan = vx3::Plan(ops, mode, 0);
      for (int r = 0; r < 2; ++r) {
        const auto run = gpu->Execute(ops, plan, 1, 120'000'000'000ll);
        if (r == 1) ReportRun(mode, run);
      }
    }
    for (double budget : {4.0, 8.0, 16.0, 33.0, 100.0}) {
      const auto plan = vx3::Plan(ops, "budget", budget);
      for (int inflight : {1, 2}) {
        const auto run = gpu->Execute(ops, plan, inflight, 120'000'000'000ll);
        char       label[64];
        std::snprintf(label, sizeof(label), "budget%.0f_inflight%d", budget, inflight);
        ReportRun(label, run);
      }
    }
    render.Release(*gpu);
    gpu.reset();
    std::printf("RESULT %s\n", ctx->ValidationReport().c_str());
    return ctx->ValidationErrors() > 0 ? 3 : 0;
  }

  if (!args.window) {
    for (int r = 0; r < args.repeat; ++r) {
      const auto run = gpu->Execute(ops, chunks, args.inflight, 120'000'000'000ll);
      ReportRun(args.split.c_str(), run);
      if (run.device_lost) break;
    }
    render.Release(*gpu);
    spin.Release(*gpu);
    gpu.reset();
    std::printf("RESULT %s\n", ctx->ValidationReport().c_str());
    return 0;
  }

  // --- window: Qt Quick on the display device, the preview producer on the pipeline device ---
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
  const int                img_w = 2560, img_h = 1600;
  const std::vector<float> fixture = MakeFixture(img_w, img_h);
  std::unique_ptr<Producer> producer;
  {
    QFile f(QStringLiteral(":/vx2/vx2_fill.comp.qsb"));
    f.open(QIODevice::ReadOnly);
    const QShader    shader = QShader::fromSerialized(f.readAll());
    const QByteArray spv    = shader.shader({QShader::SpirvShader, QShaderVersion(100)}).shader();
    std::vector<std::uint32_t> words(static_cast<std::size_t>(spv.size()) / 4);
    std::memcpy(words.data(), spv.constData(), words.size() * 4);
    try {
      producer = std::make_unique<Producer>(*ctx, fixture, img_w, img_h, 0, words);
    } catch (const std::exception& e) {
      Log("ERROR producer: %s", e.what());
      return 1;
    }
  }
  auto* window = new QQuickWindow();
  window->setScreen(screen);
  window->setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint);
  window->setTitle(QStringLiteral("alcedo VX3"));
  window->setVulkanInstance(&qvk);
  window->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceObjects(
      ctx->physical, ctx->device, static_cast<int>(ctx->family), static_cast<int>(ctx->qt_queue_index)));
  const QRect avail = screen->availableGeometry();
  window->setGeometry(avail.x() + 40, avail.y() + 40, 1280, 800);
  auto* item = new Vx2Item(window->contentItem());
  item->setSize(QSizeF(1280, 800));
  Vx2Item::Config cfg;
  cfg.source          = PhotoSource::VulkanProducer;
  cfg.producer        = producer.get();
  cfg.fixture         = &fixture;
  cfg.width           = img_w;
  cfg.height          = img_h;
  cfg.flip_correction = true;
  item->Configure(cfg);
  Log("SCREEN '%s' refresh=%.2f", screen->name().toUtf8().constData(), screen->refreshRate());

  std::mutex               records_mutex;
  std::vector<FrameRecord> records;
  Producer*                raw_prod = producer.get();
  QObject::connect(
      window, &QQuickWindow::afterFrameEnd, window,
      [&, raw_prod] {
        const std::uint64_t n = ++Vx2Item::submitted_frames;
        raw_prod->SetSubmittedFrames(n);
        FrameRecord r;
        r.end_ns       = NowNs();
        r.new_id       = Vx2Item::marks.new_id;
        r.request_ns   = Vx2Item::marks.new_request_ns;
        Vx2Item::marks = {};
        std::lock_guard<std::mutex> lock(records_mutex);
        records.push_back(r);
      },
      Qt::DirectConnection);
  // Drag: a preview request for each frame. The window also draws each vsync without a new image
  // (an animated UI over a long render), so the frame period shows the display device and the
  // preview latency shows the wait of the preview on the pipeline queue.
  std::uint64_t request_count = 0;
  QObject::connect(window, &QQuickWindow::frameSwapped, item, [&] {
    producer->Request(0.6f + 0.4f * static_cast<float>(std::sin(request_count++ * 0.05)), NowNs());
    item->update();
  }, Qt::QueuedConnection);
  producer->Start([item] { QMetaObject::invokeMethod(item, [item] { item->update(); }, Qt::QueuedConnection); });
  std::vector<std::pair<std::int64_t, double>> gui_late;
  std::int64_t                                 gui_last = 0;
  QTimer                                       gui_probe;
  gui_probe.setTimerType(Qt::PreciseTimer);
  QObject::connect(&gui_probe, &QTimer::timeout, window, [&] {
    const std::int64_t now = NowNs();
    if (gui_last != 0) gui_late.emplace_back(now, std::max(0.0, (now - gui_last) * 1e-6 - 2.0));
    gui_last = now;
  });

  // The load starts 2 s after the window (1 s warm-up, 1 s idle reference) and the program ends
  // 1 s after the load.
  std::atomic<std::int64_t> load_begin{0}, load_end{0}, idle_begin{0};
  std::vector<vx3::RunRecord> runs;
  std::thread                 load;
  // Preview thread: one preview render for each interval (a drag requests a new preview each
  // frame; newest wins, so a slow render skips the requests in between).
  std::atomic<bool>          preview_stop{false};
  std::vector<PreviewSample> preview_samples;
  std::thread                preview_thread;
  QTimer::singleShot(0, window, [&] {
    window->show();
    producer->Request(1.0f, NowNs());
    gui_probe.start(2);
  });
  QTimer::singleShot(1000, window, [&] {
    idle_begin = NowNs();
    if (!preview_gpu) return;
    preview_thread = std::thread([&] {
      const auto interval = static_cast<std::int64_t>(args.preview_interval_ms * 1e6);
      while (!preview_stop) {
        const std::int64_t start = NowNs();
        const auto         run   = preview_gpu->Execute(preview.ops, preview_chunks, 1, 120'000'000'000ll);
        if (!run.error.empty()) {
          Log("ERROR preview render: %s", run.error.c_str());
          break;
        }
        preview_samples.push_back({start, (run.end_ns - start) * 1e-6, run.chunks[0].gpu_ms});
        const std::int64_t next = start + interval;
        while (!preview_stop && NowNs() < next) std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  });
  const int idle_ms = static_cast<int>(args.idle_seconds * 1000);
  QTimer::singleShot(2000, window, [&] {
    if (args.scenario == "idle") {
      load_begin = NowNs();
      QTimer::singleShot(idle_ms, window, [&] {
        load_end = NowNs();
        app.quit();
      });
      return;
    }
    load = std::thread([&] {
      load_begin = NowNs();
      for (int r = 0; r < args.repeat; ++r) {
        runs.push_back(gpu->Execute(ops, chunks, args.inflight, 120'000'000'000ll));
        if (!runs.back().error.empty()) break;
      }
      load_end = NowNs();
      QMetaObject::invokeMethod(window, [&] { QTimer::singleShot(1000, window, [&] { app.quit(); }); },
                                Qt::QueuedConnection);
    });
  });
  app.exec();
  if (load.joinable()) load.join();
  preview_stop = true;
  if (preview_thread.joinable()) preview_thread.join();
  window->hide();
  producer->Stop();

  std::vector<FrameRecord> frames;
  {
    std::lock_guard<std::mutex> lock(records_mutex);
    frames = records;
  }
  const double vsync_ms = 1000.0 / screen->refreshRate();
  std::printf("RESULT case=%s device='%s' screen='%s' refresh=%.2f split=%s budget_ms=%.1f inflight=%d "
              "repeat=%d spin=%s producer_failed=%d renderer_errors=%d\n",
              args.scenario.c_str(), ctx->device_name.c_str(), screen->name().toUtf8().constData(),
              screen->refreshRate(), args.split.c_str(), args.budget_ms, args.inflight, args.repeat,
              args.spin.c_str(), producer->Failed() ? 1 : 0, Vx2Item::errors.load());
  ReportWindow("idle", frames, gui_late, idle_begin, load_begin, vsync_ms);
  ReportWindow("load", frames, gui_late, load_begin, load_end, vsync_ms);
  if (preview_gpu) {
    std::printf("RESULT preview_size=%ux%u interval_ms=%.1f\n", preview.width, preview.height,
                args.preview_interval_ms);
    ReportPreview("idle", preview_samples, idle_begin, load_begin);
    ReportPreview("load", preview_samples, load_begin, load_end);
  }
  for (std::size_t r = 0; r < runs.size(); ++r) {
    char label[32];
    std::snprintf(label, sizeof(label), "%zu", r);
    ReportRun(label, runs[r]);
  }
  delete window;
  producer.reset();
  render.Release(*gpu);
  spin.Release(*gpu);
  gpu.reset();
  if (preview_gpu) {
    preview.Release(*preview_gpu);
    preview_gpu.reset();
  }
  std::printf("RESULT %s\n", ctx->ValidationReport().c_str());
  std::fflush(stdout);
  return ctx->ValidationErrors() > 0 ? 3 : 0;
}
