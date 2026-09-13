//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QVariantMap>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "app/editor_session_types.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/editor_node_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/editor_rhi/editor_startup.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/qt_test_plugin_paths.hpp"
#include "utils/diagnostics/preview_performance.hpp"
#include "utils/diagnostics/preview_performance_format.hpp"

namespace alcedo::ui::test {
namespace {

#ifdef __APPLE__
editor_rhi::EditorBackend g_backend = editor_rhi::EditorBackend::Metal;
#else
editor_rhi::EditorBackend g_backend = editor_rhi::EditorBackend::Cuda;
#endif
editor_rhi::EditorStartupResult g_startup{};

constexpr std::uint32_t kInteractiveMaxLongEdge = 2560;
constexpr auto          kSliderPeriod           = std::chrono::milliseconds(8);
constexpr auto          kTrajectoryLength       = std::chrono::seconds(10);
constexpr int           kRepeatCount            = 3;

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

auto WaitForImportFinished(ApplicationModuleHost& host) -> bool {
  const bool done =
      WaitUntil([&] { return !host.import_export()->ImportRunning(); }, std::chrono::minutes(2));
  ProcessEvents(200);
  return done;
}

auto CollectCiRawFiles(std::size_t max_count = 1) -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (!std::filesystem::exists(root)) {
    return paths;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_regular_file() && is_supported_file(entry.path())) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.size() > max_count) {
    paths.resize(max_count);
  }
  return paths;
}

auto PreviewDumpDirectory() -> std::filesystem::path {
  auto cwd = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    if (std::filesystem::exists(cwd / "CMakePresets.json")) {
      auto dir = cwd / "build" / "tmp" / "preview_performance";
      std::filesystem::create_directories(dir);
      return dir;
    }
    const auto parent = cwd.parent_path();
    if (parent == cwd) {
      break;
    }
    cwd = parent;
  }
  auto dir = std::filesystem::current_path() / "build" / "tmp" / "preview_performance";
  std::filesystem::create_directories(dir);
  return dir;
}

auto ModeName(diag::PreviewPerformanceMode mode) -> const char* {
  switch (mode) {
    case diag::PreviewPerformanceMode::Off:
      return "Off";
    case diag::PreviewPerformanceMode::Summary:
      return "Summary";
    case diag::PreviewPerformanceMode::Detail:
      return "Detail";
  }
  return "Off";
}

struct FeltSample {
  std::int64_t qml_ns          = 0;
  std::int64_t e2e_ns          = 0;
  std::int64_t sched_ns        = 0;
  std::int64_t sink_ns         = 0;
  std::int64_t swap_ns         = 0;
  std::int64_t present_wait_ns = 0;
  std::int64_t encode_ns       = 0;
};

auto IsFeltPresented(const diag::PreviewRequestRecord& record) -> bool {
  return record.outcome == diag::PreviewTerminalOutcome::Presented && !record.incomplete &&
         record.has_user_input && record.frame_role == diag::PreviewFrameRole::InteractivePrimary &&
         record.qml_latest_write_ns > 0 && record.frame_swapped_ns > 0;
}

auto MakeFeltSample(const diag::PreviewRequestRecord& record) -> FeltSample {
  FeltSample sample;
  sample.qml_ns   = diag::PreviewDurationNs(record.frame_swapped_ns, record.qml_latest_write_ns);
  sample.e2e_ns   = diag::PreviewDurationNs(record.displayed_ns, record.submit_ns);
  sample.sched_ns = diag::PreviewDurationNs(record.worker_start_ns, record.scheduled_ns);
  sample.sink_ns  = diag::PreviewDurationNs(record.producer_ready_ns, record.sink_submit_ns);
  sample.swap_ns  = diag::PreviewDurationNs(record.frame_swapped_ns, record.imported_ns);
  sample.present_wait_ns =
      diag::PreviewDurationNs(record.imported_ns, record.producer_ready_ns);
  sample.encode_ns = record.cpu.encode_ns;
  return sample;
}

struct TrajectoryStats {
  std::vector<std::int64_t> qml_ns;
  std::vector<std::int64_t> e2e_ns;
  std::vector<std::int64_t> sched_ns;
  std::vector<std::int64_t> sink_ns;
  std::vector<std::int64_t> swap_ns;
  std::vector<std::int64_t> encode_ns;
  std::uint64_t             presented        = 0;
  std::uint64_t             cancelled        = 0;
  std::uint64_t             coalesced        = 0;
  std::uint64_t             failed           = 0;
  std::uint64_t             dropped          = 0;
  std::uint64_t             stale            = 0;
  std::uint32_t             render_width     = 0;
  std::uint32_t             render_height    = 0;
  bool                      develop_skipped  = false;
  const diag::PreviewRequestRecord* slowest  = nullptr;
  std::int64_t                      slowest_qml_ns = -1;
};

auto BuildStats(const std::vector<diag::PreviewRequestRecord>& records) -> TrajectoryStats {
  TrajectoryStats stats;
  for (const auto& record : records) {
    switch (record.outcome) {
      case diag::PreviewTerminalOutcome::Presented:
        ++stats.presented;
        break;
      case diag::PreviewTerminalOutcome::Cancelled:
        ++stats.cancelled;
        break;
      case diag::PreviewTerminalOutcome::Coalesced:
        ++stats.coalesced;
        break;
      case diag::PreviewTerminalOutcome::Failed:
        ++stats.failed;
        break;
      case diag::PreviewTerminalOutcome::Dropped:
        ++stats.dropped;
        break;
      case diag::PreviewTerminalOutcome::Stale:
        ++stats.stale;
        break;
    }
    if (!IsFeltPresented(record)) {
      continue;
    }
    if (stats.render_width == 0 && record.render_width > 0) {
      stats.render_width  = record.render_width;
      stats.render_height = record.render_height;
    }
    const auto sample = MakeFeltSample(record);
    stats.qml_ns.push_back(sample.qml_ns);
    stats.e2e_ns.push_back(sample.e2e_ns);
    stats.sched_ns.push_back(sample.sched_ns);
    stats.sink_ns.push_back(sample.sink_ns);
    stats.swap_ns.push_back(sample.swap_ns);
    stats.encode_ns.push_back(sample.encode_ns);
    if (sample.qml_ns > stats.slowest_qml_ns) {
      stats.slowest_qml_ns = sample.qml_ns;
      stats.slowest        = &record;
    }
    for (const auto& pass : record.passes) {
      if ((pass.kind == diag::PreviewPassKind::UploadRaw ||
           pass.kind == diag::PreviewPassKind::UploadRgb) &&
          pass.state != diag::PreviewExecutionState::Executed) {
        stats.develop_skipped = true;
      }
    }
  }
  return stats;
}

void DumpQuantiles(std::ostream& out, std::string_view name, const std::vector<std::int64_t>& values) {
  out << "  " << name << " n=" << values.size()
      << " p50=" << diag::FormatPreviewMilliseconds(diag::PreviewQuantileNs(values, 0.50))
      << " p95=" << diag::FormatPreviewMilliseconds(diag::PreviewQuantileNs(values, 0.95))
      << " p99=" << diag::FormatPreviewMilliseconds(diag::PreviewQuantileNs(values, 0.99))
      << " max=" << diag::FormatPreviewMilliseconds(diag::PreviewMaxNs(values)) << "\n";
}

void DumpPassList(std::ostream& out, const diag::PreviewRequestRecord& record) {
  out << "    request=" << record.request_id << " reason=" << record.reason
      << " outcome=" << diag::PreviewTerminalOutcomeName(record.outcome)
      << " render=" << record.render_width << "x" << record.render_height << "\n";
  for (const auto& pass : record.passes) {
    out << "      " << pass.owner << " " << diag::PreviewPassKindName(pass.kind)
        << " state=" << diag::PreviewExecutionStateName(pass.state)
        << " cpu_ms=" << diag::FormatPreviewMilliseconds(pass.cpu_ns)
        << " gpu=" << diag::PreviewGpuTimeStatusName(pass.gpu_status)
        << " gpu_ms=" << diag::FormatPreviewMilliseconds(pass.gpu_ns) << "\n";
  }
}

class RecordCollector {
 public:
  void Attach() {
    diag::PreviewPerformance::InstallRecordSink([this](const diag::PreviewRequestRecord& record) {
      std::lock_guard lock(mutex_);
      records_.push_back(record);
    });
  }
  void Detach() { diag::PreviewPerformance::InstallRecordSink({}); }
  auto Snapshot() const -> std::vector<diag::PreviewRequestRecord> {
    std::lock_guard lock(mutex_);
    return records_;
  }
  void Clear() {
    std::lock_guard lock(mutex_);
    records_.clear();
  }

 private:
  mutable std::mutex                      mutex_;
  std::vector<diag::PreviewRequestRecord> records_;
};

class EditorPreviewPresentTrajectoryTest : public ApplicationModuleHostTestFixture {};

struct Harness {
  ApplicationModuleHost              host;
  QQuickWindow                       window;
  editor_rhi::EditorViewportItem*    viewport = nullptr;
  EditorNodeController               nodes;
  RecordCollector                    collector;
  uint                               element_id = 0;
  uint                               image_id   = 0;

  ~Harness() {
    collector.Detach();
    if (viewport != nullptr) {
      viewport->endInteractivePresentLoop();
      viewport->cancelPendingFrames();
      viewport->suspendPresentation();
    }
    if (host.editor_session() != nullptr) {
      (void)WaitUntil([&] { return !host.editor_session()->render_busy(); },
                      std::chrono::seconds(15));
      host.editor_session()->Finalize(false);
      (void)WaitUntil([&] { return !host.editor_session()->has_image(); },
                      std::chrono::seconds(20));
    }
    window.close();
    ProcessEvents(200);
    host.Shutdown();
  }
};

auto LongEdge(std::uint32_t width, std::uint32_t height) -> std::uint32_t {
  return std::max(width, height);
}

void SyncPresentationSize(EditorSessionController* session, editor_rhi::EditorViewportItem* viewport,
                          QQuickWindow& window) {
  const qreal dpr = window.devicePixelRatio();
  session->updatePresentationTargetSize(
      std::max(1, qRound(viewport->width() * dpr)),
      std::max(1, qRound(viewport->height() * dpr)));
}

auto DriveExposureSlider(EditorSessionController* session, std::chrono::milliseconds duration)
    -> int {
  const auto start = std::chrono::steady_clock::now();
  int        writes = 0;
  float      ev     = 0.15f;
  while (std::chrono::steady_clock::now() - start < duration) {
    ev += 0.02f;
    if (ev > 1.80f) {
      ev = 0.10f;
    }
    if (session->submitWrite(QStringLiteral("exposure"), EditorScalarWrite{ev}, false)) {
      ++writes;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
    std::this_thread::sleep_for(kSliderPeriod);
  }
  (void)session->submitWrite(QStringLiteral("exposure"), EditorScalarWrite{ev}, true);
  ++writes;
  return writes;
}

TEST_F(EditorPreviewPresentTrajectoryTest, SubmitWriteHotExposureCompletesAtFrameSwapped) {
  if (!g_startup.ok) {
    GTEST_SKIP() << g_startup.error;
  }
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    GTEST_SKIP() << "Product present needs the native Windows QPA";
  }
  if (g_backend != editor_rhi::EditorBackend::Cuda) {
    GTEST_SKIP() << "CUDA present trajectory only";
  }
  const auto raw_files = CollectCiRawFiles(1);
  if (raw_files.empty()) {
    GTEST_SKIP() << "CI RAW fixture is required";
  }

  Harness harness;
  harness.host.project()->SetRuntimeAcceleratorPreference(AcceleratorBackendPreference::CUDA);
  ASSERT_TRUE(CreateTestProject(harness.host));
  harness.host.import_export()->StartImport(PathsToQStringList(raw_files));
  ASSERT_TRUE(WaitForImportFinished(harness.host));
  ASSERT_EQ(harness.host.import_export()->ImportFailed(), 0);
  ASSERT_GE(harness.host.library()->Thumbnails().size(), 1);
  const QVariantMap item = harness.host.library()->Thumbnails().at(0).toMap();
  harness.element_id     = item.value("elementId").toUInt();
  harness.image_id       = item.value("imageId").toUInt();
  ASSERT_GT(harness.element_id, 0u);
  ASSERT_GT(harness.image_id, 0u);

  editor_rhi::BindEditorGraphicsToWindow(&harness.window, g_startup);
  harness.window.resize(1280, 720);
  harness.viewport = new editor_rhi::EditorViewportItem(harness.window.contentItem());
  harness.viewport->setSize(QSizeF(1280, 720));
  harness.viewport->setVisible(true);
  auto* session = harness.host.editor_session();
  ASSERT_NE(session, nullptr);
  session->bindPresentationViewport(harness.viewport);
  QObject::connect(harness.viewport, &editor_rhi::EditorViewportItem::targetSizeRequested, session,
                   [session](int width, int height) {
                     session->updatePresentationTargetSize(width, height);
                   });
  harness.window.show();
  harness.window.requestActivate();
  ProcessEvents(200);
  SyncPresentationSize(session, harness.viewport, harness.window);
  ASSERT_TRUE(WaitUntil([&] { return harness.viewport->presentationAvailable(); },
                        std::chrono::seconds(15)))
      << harness.viewport->statusText().toStdString();

  diag::PreviewPerformance::ResetForTesting();
  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
  harness.collector.Attach();

  harness.host.workspace_router()->OpenEditor(harness.element_id, harness.image_id);
  ASSERT_TRUE(WaitUntil(
      [&] {
        return harness.host.editor_session_service()->state() == EditorSessionState::Interactive &&
               session->can_edit() && harness.viewport->presentedFrameCount() > 0;
      },
      std::chrono::minutes(2)))
      << "state=" << EditorSessionStateName(harness.host.editor_session_service()->state())
      << " status=" << harness.viewport->statusText().toStdString();
  ASSERT_TRUE(WaitUntil([&] { return !session->render_busy(); }, std::chrono::seconds(30)));

  ASSERT_TRUE(session->submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.35f}, false));
  ASSERT_TRUE(session->submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.55f}, false));
  ASSERT_TRUE(WaitUntil(
      [&] {
        diag::PreviewPerformance::FlushWriter();
        const auto records = harness.collector.Snapshot();
        return std::any_of(records.begin(), records.end(), IsFeltPresented);
      },
      std::chrono::seconds(45)))
      << "status=" << harness.viewport->statusText().toStdString()
      << " presented=" << harness.viewport->presentedFrameCount()
      << " pending=" << diag::PreviewPerformance::PendingSampleCount();
  (void)session->submitWrite(QStringLiteral("exposure"), EditorScalarWrite{0.55f}, true);

  diag::PreviewPerformance::FlushWriter();
  const auto records = harness.collector.Snapshot();
  const auto stats   = BuildStats(records);
  ASSERT_FALSE(stats.qml_ns.empty());
  EXPECT_EQ(LongEdge(stats.render_width, stats.render_height), kInteractiveMaxLongEdge)
      << stats.render_width << "x" << stats.render_height;
  EXPECT_GT(diag::PreviewQuantileNs(stats.qml_ns, 0.50),
            diag::PreviewQuantileNs(stats.encode_ns, 0.50));
  EXPECT_GE(diag::PreviewQuantileNs(stats.sink_ns, 0.50), 0);
  EXPECT_GE(diag::PreviewQuantileNs(stats.sched_ns, 0.50), 0);
  EXPECT_TRUE(stats.develop_skipped);
}

#ifndef _DEBUG
TEST_F(EditorPreviewPresentTrajectoryTest,
       Interactive2560PresentTrajectoryDumpSubmitWriteToFrameSwapped) {
  if (!g_startup.ok) {
    GTEST_SKIP() << g_startup.error;
  }
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    GTEST_SKIP() << "Product present needs the native Windows QPA";
  }
  if (g_backend != editor_rhi::EditorBackend::Cuda) {
    GTEST_SKIP() << "CUDA present trajectory only";
  }
  const auto raw_files = CollectCiRawFiles(1);
  if (raw_files.empty()) {
    GTEST_SKIP() << "CI RAW fixture is required";
  }

  Harness harness;
  harness.host.project()->SetRuntimeAcceleratorPreference(AcceleratorBackendPreference::CUDA);
  ASSERT_TRUE(CreateTestProject(harness.host));
  harness.host.import_export()->StartImport(PathsToQStringList(raw_files));
  ASSERT_TRUE(WaitForImportFinished(harness.host));
  ASSERT_EQ(harness.host.import_export()->ImportFailed(), 0);
  ASSERT_GE(harness.host.library()->Thumbnails().size(), 1);
  const QVariantMap item = harness.host.library()->Thumbnails().at(0).toMap();
  harness.element_id     = item.value("elementId").toUInt();
  harness.image_id       = item.value("imageId").toUInt();

  editor_rhi::BindEditorGraphicsToWindow(&harness.window, g_startup);
  harness.window.resize(1280, 720);
  harness.viewport = new editor_rhi::EditorViewportItem(harness.window.contentItem());
  harness.viewport->setSize(QSizeF(1280, 720));
  harness.viewport->setVisible(true);
  auto* session = harness.host.editor_session();
  session->bindPresentationViewport(harness.viewport);
  QObject::connect(harness.viewport, &editor_rhi::EditorViewportItem::targetSizeRequested, session,
                   [session](int width, int height) {
                     session->updatePresentationTargetSize(width, height);
                   });
  harness.window.show();
  harness.window.requestActivate();
  ProcessEvents(200);
  SyncPresentationSize(session, harness.viewport, harness.window);
  ASSERT_TRUE(WaitUntil([&] { return harness.viewport->presentationAvailable(); },
                        std::chrono::seconds(15)))
      << harness.viewport->statusText().toStdString();

  diag::PreviewPerformance::ResetForTesting();
  diag::PreviewPerformance::SetMode(diag::PreviewPerformanceMode::Detail);
  harness.collector.Attach();

  harness.host.workspace_router()->OpenEditor(harness.element_id, harness.image_id);
  ASSERT_TRUE(WaitUntil(
      [&] {
        return harness.host.editor_session_service()->state() == EditorSessionState::Interactive &&
               session->can_edit() && harness.viewport->presentedFrameCount() > 0;
      },
      std::chrono::minutes(2)));
  ASSERT_TRUE(WaitUntil([&] { return !session->render_busy(); }, std::chrono::seconds(60)));

  harness.nodes.set_editor_session(session);
  ASSERT_TRUE(harness.nodes.has_snapshot());
  ASSERT_TRUE(harness.nodes.can_add_color_grade()) << harness.nodes.last_error().toStdString();
  QString predecessor = QStringLiteral("grade.primary");
  QString last_grade;
  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(harness.nodes.addCleanColorGrade()) << harness.nodes.last_error().toStdString();
    last_grade = harness.nodes.selected_node_id_string();
    ASSERT_TRUE(harness.nodes.requestConnect(predecessor, last_grade))
        << harness.nodes.last_error().toStdString();
    predecessor = last_grade;
  }
  ASSERT_TRUE(harness.nodes.requestConnect(last_grade, QStringLiteral("drt")))
      << harness.nodes.last_error().toStdString();
  EXPECT_FALSE(harness.nodes.incomplete_draft());
  EXPECT_EQ(harness.nodes.backbone_node_ids().size(), 10);
  harness.nodes.selectNode(last_grade);
  ProcessEvents(100);
  ASSERT_TRUE(WaitUntil([&] { return !session->render_busy(); }, std::chrono::seconds(60)));

  const auto dump_path = PreviewDumpDirectory() / "cuda_interactive_2560_present_table.txt";
  std::ofstream out(dump_path, std::ios::trunc);
  out << "CUDA Interactive 2560 product present trajectory\n";
  out << "path=submitWrite -> admit -> schedule -> worker -> sink -> import -> frameSwapped\n";
  out << "cfa=Bayer source=" << raw_files.front().filename().string()
      << " grades=8 target=last_exposure slider_period_ms=" << kSliderPeriod.count() << "\n";

  struct RunResult {
    std::string                       label;
    diag::PreviewPerformanceMode      mode = diag::PreviewPerformanceMode::Detail;
    int                               writes = 0;
    std::int64_t                      wall_ms = 0;
    qulonglong                        presented_frames = 0;
    std::uint64_t                     events_lost = 0;
    TrajectoryStats                   stats;
    std::vector<diag::PreviewRequestRecord> records;
  };
  std::vector<RunResult> runs;

  auto run_once = [&](std::string label, diag::PreviewPerformanceMode mode) -> bool {
    diag::PreviewPerformance::SetMode(mode);
    harness.collector.Clear();
    const auto frames_before = harness.viewport->presentedFrameCount();
    const auto wall_start    = std::chrono::steady_clock::now();
    const int  writes        = DriveExposureSlider(session, kTrajectoryLength);
    const bool idle          = WaitUntil(
        [&] {
          return !session->render_busy() && !harness.viewport->interactivePresentLoopActive();
        },
        std::chrono::seconds(30));
    diag::PreviewPerformance::FlushWriter();
    if (!idle) {
      return false;
    }
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - wall_start)
                             .count();
    RunResult run;
    run.label            = std::move(label);
    run.mode             = mode;
    run.writes           = writes;
    run.wall_ms          = wall_ms;
    run.presented_frames = harness.viewport->presentedFrameCount() - frames_before;
    run.events_lost      = diag::PreviewPerformance::EventsLost();
    run.records          = harness.collector.Snapshot();
    run.stats            = BuildStats(run.records);
    runs.push_back(std::move(run));
    return true;
  };

  for (int i = 0; i < kRepeatCount; ++i) {
    ASSERT_TRUE(run_once("detail_repeat_" + std::to_string(i + 1),
                         diag::PreviewPerformanceMode::Detail))
        << "Detail trajectory " << (i + 1) << " did not become idle";
  }
  ASSERT_TRUE(run_once("summary_once", diag::PreviewPerformanceMode::Summary));
  ASSERT_TRUE(run_once("off_once", diag::PreviewPerformanceMode::Off));

  for (const auto& run : runs) {
    out << "\n[" << run.label << "] mode=" << ModeName(run.mode) << " writes=" << run.writes
        << " wall_ms=" << run.wall_ms << " viewport_frames=" << run.presented_frames
        << " records=" << run.records.size() << " presented=" << run.stats.presented
        << " cancelled=" << run.stats.cancelled << " coalesced=" << run.stats.coalesced
        << " failed=" << run.stats.failed << " dropped=" << run.stats.dropped
        << " stale=" << run.stats.stale << " lost=" << run.events_lost
        << " render=" << run.stats.render_width << "x" << run.stats.render_height
        << " develop_skipped=" << (run.stats.develop_skipped ? "yes" : "no") << "\n";
    DumpQuantiles(out, "qml_ms", run.stats.qml_ns);
    DumpQuantiles(out, "e2e_ms", run.stats.e2e_ns);
    DumpQuantiles(out, "sched_ms", run.stats.sched_ns);
    DumpQuantiles(out, "sink_ms", run.stats.sink_ns);
    DumpQuantiles(out, "swap_ms", run.stats.swap_ns);
    DumpQuantiles(out, "encode_ms", run.stats.encode_ns);
    if (run.stats.slowest != nullptr) {
      out << "  slowest_qml:\n";
      DumpPassList(out, *run.stats.slowest);
    }
  }
  out.flush();

  const auto& first = runs.front();
  ASSERT_FALSE(first.stats.qml_ns.empty());
  EXPECT_EQ(LongEdge(first.stats.render_width, first.stats.render_height), kInteractiveMaxLongEdge);
  EXPECT_GT(diag::PreviewQuantileNs(first.stats.qml_ns, 0.50),
            diag::PreviewQuantileNs(first.stats.encode_ns, 0.50));
  EXPECT_GE(diag::PreviewQuantileNs(first.stats.sink_ns, 0.50), 0);
  EXPECT_GE(diag::PreviewQuantileNs(first.stats.sched_ns, 0.50), 0);
  EXPECT_TRUE(first.stats.develop_skipped);
  EXPECT_EQ(diag::PreviewPerformance::EventsLost(), 0u);
}
#endif

}  // namespace
}  // namespace alcedo::ui::test

int main(int argc, char** argv) {
#ifdef _WIN32
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", QByteArray("windows"));
  }
#endif
  alcedo::ui::test::ConfigureQtPluginPaths(argc > 0 ? argv[0] : nullptr,
                                           /*force_offscreen=*/false);

  const QByteArray requested = qgetenv("ALCEDO_TEST_EDITOR_BACKEND").toLower();
  if (requested == "metal") {
    alcedo::ui::test::g_backend = alcedo::editor_rhi::EditorBackend::Metal;
  } else if (requested == "opencl") {
    alcedo::ui::test::g_backend = alcedo::editor_rhi::EditorBackend::OpenCl;
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }

  QApplication app(argc, argv);
  alcedo::ui::test::g_startup =
      alcedo::editor_rhi::ApplyEditorBackendBeforeWindow(alcedo::ui::test::g_backend);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
