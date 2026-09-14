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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/editor_session_types.hpp"
#include "edit/graph/graph_ids.hpp"
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
constexpr float         kExposureWriteStart     = 0.15f;
constexpr float         kExposureWriteStep      = 0.02f;
constexpr float         kExposureWrapMax        = 1.80f;
constexpr float         kExposureWrapMin        = 0.10f;
constexpr float         kLlfShadows             = 18.0f;
constexpr float         kLlfHighlights          = -12.0f;

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
  std::vector<std::int64_t> gpu_ns;
  std::vector<std::int64_t> last_grade_gpu_ns;
  std::vector<std::int64_t> drt_gpu_ns;
  std::vector<std::int64_t> llf_gpu_ns;
  std::uint64_t             presented        = 0;
  std::uint64_t             cancelled        = 0;
  std::uint64_t             coalesced        = 0;
  std::uint64_t             failed           = 0;
  std::uint64_t             dropped          = 0;
  std::uint64_t             stale            = 0;
  std::uint32_t             render_width     = 0;
  std::uint32_t             render_height    = 0;
  std::uint32_t             develop_width    = 0;
  std::uint32_t             develop_height   = 0;
  bool                      develop_skipped  = false;
  const diag::PreviewRequestRecord* slowest  = nullptr;
  const diag::PreviewRequestRecord* median   = nullptr;
  const diag::PreviewRequestRecord* first    = nullptr;
  std::vector<const diag::PreviewRequestRecord*> felt;
  std::int64_t                      slowest_qml_ns = -1;
};

auto LastExecutedGradeGpuNs(const diag::PreviewRequestRecord& record) -> std::int64_t {
  std::int64_t gpu_ns = 0;
  bool         found  = false;
  for (const auto& pass : record.passes) {
    if (pass.kind == diag::PreviewPassKind::PrimaryColorGrade &&
        pass.state == diag::PreviewExecutionState::Executed &&
        pass.gpu_status == diag::PreviewGpuTimeStatus::Available) {
      gpu_ns = pass.gpu_ns;
      found  = true;
    }
  }
  return found ? gpu_ns : 0;
}

auto PassGpuNs(const diag::PreviewRequestRecord& record, diag::PreviewPassKind kind)
    -> std::int64_t {
  for (const auto& pass : record.passes) {
    if (pass.kind == kind && pass.state == diag::PreviewExecutionState::Executed &&
        pass.gpu_status == diag::PreviewGpuTimeStatus::Available) {
      return pass.gpu_ns;
    }
  }
  return 0;
}

auto IsLlfSubStage(diag::PreviewSubStageKind kind) -> bool {
  switch (kind) {
    case diag::PreviewSubStageKind::LlfExtract:
    case diag::PreviewSubStageKind::LlfPyramid:
    case diag::PreviewSubStageKind::LlfRemap:
    case diag::PreviewSubStageKind::LlfSelect:
    case diag::PreviewSubStageKind::LlfCollapse:
    case diag::PreviewSubStageKind::LlfApply:
    case diag::PreviewSubStageKind::LlfSampleCanonical:
      return true;
    default:
      return false;
  }
}

auto LlfGpuNs(const diag::PreviewRequestRecord& record) -> std::int64_t {
  std::int64_t gpu_ns = 0;
  for (const auto& pass : record.passes) {
    if (pass.state != diag::PreviewExecutionState::Executed) {
      continue;
    }
    for (const auto& sub : pass.sub_stages) {
      if (IsLlfSubStage(sub.kind) && sub.gpu_status == diag::PreviewGpuTimeStatus::Available) {
        gpu_ns += sub.gpu_ns;
      }
    }
  }
  return gpu_ns;
}

auto HasExecutedLlf(const diag::PreviewRequestRecord& record) -> bool {
  for (const auto& pass : record.passes) {
    if (pass.state != diag::PreviewExecutionState::Executed) {
      continue;
    }
    for (const auto& sub : pass.sub_stages) {
      if (IsLlfSubStage(sub.kind) && sub.state == diag::PreviewExecutionState::Executed) {
        return true;
      }
    }
  }
  return false;
}

auto CountExecutedGrades(const diag::PreviewRequestRecord& record) -> int {
  int count = 0;
  for (const auto& pass : record.passes) {
    if (pass.kind == diag::PreviewPassKind::PrimaryColorGrade &&
        pass.state == diag::PreviewExecutionState::Executed) {
      ++count;
    }
  }
  return count;
}

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
    if (stats.develop_width == 0 && record.has_develop_decode) {
      stats.develop_width  = record.develop.develop_width;
      stats.develop_height = record.develop.develop_height;
    }
    const auto sample = MakeFeltSample(record);
    stats.qml_ns.push_back(sample.qml_ns);
    stats.e2e_ns.push_back(sample.e2e_ns);
    stats.sched_ns.push_back(sample.sched_ns);
    stats.sink_ns.push_back(sample.sink_ns);
    stats.swap_ns.push_back(sample.swap_ns);
    stats.encode_ns.push_back(sample.encode_ns);
    stats.gpu_ns.push_back(record.gpu_ns);
    stats.last_grade_gpu_ns.push_back(LastExecutedGradeGpuNs(record));
    stats.drt_gpu_ns.push_back(PassGpuNs(record, diag::PreviewPassKind::Drt));
    stats.llf_gpu_ns.push_back(LlfGpuNs(record));
    stats.felt.push_back(&record);
    if (stats.first == nullptr) {
      stats.first = &record;
    }
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
  if (!stats.felt.empty()) {
    auto ordered = stats.felt;
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
      return diag::PreviewDurationNs(a->frame_swapped_ns, a->qml_latest_write_ns) <
             diag::PreviewDurationNs(b->frame_swapped_ns, b->qml_latest_write_ns);
    });
    stats.median = ordered[ordered.size() / 2];
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
      << " render=" << record.render_width << "x" << record.render_height
      << " gpu_ms=" << diag::FormatPreviewMilliseconds(record.gpu_ns)
      << " executed_grades=" << CountExecutedGrades(record) << "\n";
  if (record.has_develop_decode) {
    out << "      develop_plane=" << record.develop.develop_width << "x"
        << record.develop.develop_height
        << " cfa=" << diag::PreviewCfaKindName(record.develop.cfa)
        << " decode=" << diag::PreviewDecodeResName(record.develop.decode_res)
        << " demosaic=" << diag::PreviewDemosaicMethodName(record.develop.demosaic) << "\n";
  }
  for (const auto& pass : record.passes) {
    out << "      " << pass.owner << " " << diag::PreviewPassKindName(pass.kind)
        << " state=" << diag::PreviewExecutionStateName(pass.state)
        << " cpu_ms=" << diag::FormatPreviewMilliseconds(pass.cpu_ns)
        << " gpu=" << diag::PreviewGpuTimeStatusName(pass.gpu_status)
        << " gpu_ms=" << diag::FormatPreviewMilliseconds(pass.gpu_ns) << "\n";
    for (const auto& sub : pass.sub_stages) {
      out << "        " << diag::PreviewSubStageKindName(sub.kind)
          << " cpu_ms=" << diag::FormatPreviewMilliseconds(sub.cpu_ns)
          << " gpu=" << diag::PreviewGpuTimeStatusName(sub.gpu_status)
          << " gpu_ms=" << diag::FormatPreviewMilliseconds(sub.gpu_ns) << "\n";
    }
  }
}

void DumpFeltFrameCsv(std::ostream& csv, std::string_view run_label,
                      const diag::PreviewRequestRecord& record) {
  const auto sample = MakeFeltSample(record);
  csv << run_label << ',' << record.request_id << ','
      << diag::FormatPreviewMilliseconds(sample.qml_ns) << ','
      << diag::FormatPreviewMilliseconds(sample.e2e_ns) << ','
      << diag::FormatPreviewMilliseconds(record.gpu_ns) << ','
      << diag::FormatPreviewMilliseconds(sample.encode_ns) << ','
      << diag::FormatPreviewMilliseconds(sample.sink_ns) << ','
      << diag::FormatPreviewMilliseconds(sample.swap_ns) << ','
      << diag::FormatPreviewMilliseconds(LastExecutedGradeGpuNs(record)) << ','
      << diag::FormatPreviewMilliseconds(PassGpuNs(record, diag::PreviewPassKind::Drt)) << ','
      << diag::FormatPreviewMilliseconds(LlfGpuNs(record)) << ',' << CountExecutedGrades(record)
      << ',' << record.render_width << 'x' << record.render_height << '\n';
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
  float      ev     = kExposureWriteStart;
  while (std::chrono::steady_clock::now() - start < duration) {
    ev += kExposureWriteStep;
    if (ev > kExposureWrapMax) {
      ev = kExposureWrapMin;
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

auto ConnectEightCleanColorGrades(EditorNodeController& nodes, QString* last_grade) -> bool {
  QString predecessor = QStringLiteral("grade.primary");
  for (int i = 0; i < 7; ++i) {
    if (!nodes.addCleanColorGrade()) {
      return false;
    }
    *last_grade = nodes.selected_node_id_string();
    if (!nodes.requestConnect(predecessor, *last_grade)) {
      return false;
    }
    predecessor = *last_grade;
  }
  return nodes.requestConnect(*last_grade, QStringLiteral("drt"));
}

auto EnableLlfOnSelectedGrade(EditorSessionController* session) -> bool {
  if (!session->submitWrite(QStringLiteral("shadows"), EditorScalarWrite{kLlfShadows}, false)) {
    return false;
  }
  if (!session->enqueueNodeSwitchBoundary()) {
    return false;
  }
  if (!session->submitWrite(QStringLiteral("highlights"), EditorScalarWrite{kLlfHighlights},
                            false)) {
    return false;
  }
  return session->enqueueNodeSwitchBoundary();
}

struct TrajectoryRunResult {
  std::string                           label;
  diag::PreviewPerformanceMode          mode              = diag::PreviewPerformanceMode::Detail;
  int                                   writes            = 0;
  std::int64_t                          wall_ms           = 0;
  qulonglong                            presented_frames  = 0;
  std::uint64_t                         events_lost       = 0;
  TrajectoryStats                       stats;
  std::vector<diag::PreviewRequestRecord> records;
};

auto RunExposureTrajectory(Harness& harness, EditorSessionController* session, std::string label,
                           diag::PreviewPerformanceMode mode)
    -> std::optional<TrajectoryRunResult> {
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
    return std::nullopt;
  }
  TrajectoryRunResult run;
  run.label            = std::move(label);
  run.mode             = mode;
  run.writes           = writes;
  run.wall_ms          = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - wall_start)
                    .count();
  run.presented_frames = harness.viewport->presentedFrameCount() - frames_before;
  run.events_lost      = diag::PreviewPerformance::EventsLost();
  run.records          = harness.collector.Snapshot();
  run.stats            = BuildStats(run.records);
  return run;
}

void WriteTrajectoryDump(std::ostream& out, std::ostream& csv,
                         const std::vector<TrajectoryRunResult>& runs) {
  for (const auto& run : runs) {
    out << "\n[" << run.label << "] mode=" << ModeName(run.mode) << " writes=" << run.writes
        << " wall_ms=" << run.wall_ms << " viewport_frames=" << run.presented_frames
        << " records=" << run.records.size() << " presented=" << run.stats.presented
        << " cancelled=" << run.stats.cancelled << " coalesced=" << run.stats.coalesced
        << " failed=" << run.stats.failed << " dropped=" << run.stats.dropped
        << " stale=" << run.stats.stale << " lost=" << run.events_lost
        << " render=" << run.stats.render_width << "x" << run.stats.render_height
        << " develop=" << run.stats.develop_width << "x" << run.stats.develop_height
        << " develop_skipped=" << (run.stats.develop_skipped ? "yes" : "no") << "\n";
    DumpQuantiles(out, "qml_ms", run.stats.qml_ns);
    DumpQuantiles(out, "e2e_ms", run.stats.e2e_ns);
    DumpQuantiles(out, "gpu_ms", run.stats.gpu_ns);
    DumpQuantiles(out, "last_grade_gpu_ms", run.stats.last_grade_gpu_ns);
    DumpQuantiles(out, "llf_gpu_ms", run.stats.llf_gpu_ns);
    DumpQuantiles(out, "drt_gpu_ms", run.stats.drt_gpu_ns);
    DumpQuantiles(out, "sched_ms", run.stats.sched_ns);
    DumpQuantiles(out, "sink_ms", run.stats.sink_ns);
    DumpQuantiles(out, "swap_ms", run.stats.swap_ns);
    DumpQuantiles(out, "encode_ms", run.stats.encode_ns);
    if (run.stats.first != nullptr) {
      out << "  first_presented_pass_trace:\n";
      DumpPassList(out, *run.stats.first);
    }
    if (run.stats.median != nullptr) {
      out << "  p50_qml_pass_trace:\n";
      DumpPassList(out, *run.stats.median);
    }
    if (run.stats.slowest != nullptr) {
      out << "  slowest_qml_pass_trace:\n";
      DumpPassList(out, *run.stats.slowest);
    }
    for (const auto* record : run.stats.felt) {
      DumpFeltFrameCsv(csv, run.label, *record);
    }
  }
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
  QString last_grade;
  ASSERT_TRUE(ConnectEightCleanColorGrades(harness.nodes, &last_grade))
      << harness.nodes.last_error().toStdString();
  EXPECT_FALSE(harness.nodes.incomplete_draft());
  EXPECT_EQ(harness.nodes.backbone_node_ids().size(), 10);
  harness.nodes.selectNode(last_grade);
  ProcessEvents(100);
  ASSERT_TRUE(WaitUntil([&] { return !session->render_busy(); }, std::chrono::seconds(60)));

  const auto dump_path = PreviewDumpDirectory() / "cuda_interactive_2560_present_table.txt";
  const auto csv_path  = PreviewDumpDirectory() / "cuda_interactive_2560_present_frames.csv";
  std::ofstream out(dump_path, std::ios::trunc);
  std::ofstream csv(csv_path, std::ios::trunc);
  out << "CUDA Interactive 2560 product present trajectory\n";
  out << "kind=slider_interactive_frame_trace\n";
  out << "path=submitWrite -> admit -> schedule -> worker -> sink -> import -> frameSwapped\n";
  out << "cfa=Bayer source=" << raw_files.front().filename().string() << "\n";
  out << "decode_res=FULL interactive_max_edge=" << kInteractiveMaxLongEdge << "\n";
  out << "grades=8 selected_node=last_color_grade field=exposure llf=off\n";
  out << "exposure_start=" << kExposureWriteStart << " step=" << kExposureWriteStep
      << " wrap_min=" << kExposureWrapMin << " wrap_max=" << kExposureWrapMax << "\n";
  out << "slider_period_ms=" << kSliderPeriod.count()
      << " trajectory_s=" << kTrajectoryLength.count()
      << " session_cache=kept settled_write=true_on_loop_end\n";
  out << "runs=3x Detail last-exposure slider, 1x Summary same slider, 1x Off same slider\n";
  csv << "run,request_id,qml_ms,e2e_ms,gpu_ms,encode_ms,sink_ms,swap_ms,last_grade_gpu_ms,"
         "drt_gpu_ms,llf_gpu_ms,executed_grades,render\n";

  std::vector<TrajectoryRunResult> runs;
  for (int i = 0; i < kRepeatCount; ++i) {
    auto run = RunExposureTrajectory(harness, session, "detail_repeat_" + std::to_string(i + 1),
                                     diag::PreviewPerformanceMode::Detail);
    ASSERT_TRUE(run.has_value()) << "Detail trajectory " << (i + 1) << " did not become idle";
    runs.push_back(std::move(*run));
  }
  {
    auto run = RunExposureTrajectory(harness, session, "summary_once",
                                     diag::PreviewPerformanceMode::Summary);
    ASSERT_TRUE(run.has_value()) << "Summary trajectory did not become idle";
    runs.push_back(std::move(*run));
  }
  {
    auto run =
        RunExposureTrajectory(harness, session, "off_once", diag::PreviewPerformanceMode::Off);
    ASSERT_TRUE(run.has_value()) << "Off trajectory did not become idle";
    runs.push_back(std::move(*run));
  }

  WriteTrajectoryDump(out, csv, runs);
  out.flush();
  csv.flush();

  const auto& first = runs.front();
  ASSERT_FALSE(first.stats.qml_ns.empty());
  for (const auto& run : runs) {
    if (run.mode == diag::PreviewPerformanceMode::Off) {
      continue;
    }
    EXPECT_FALSE(run.stats.qml_ns.empty()) << run.label;
    EXPECT_GT(run.stats.presented, 0u) << run.label;
    EXPECT_EQ(LongEdge(run.stats.render_width, run.stats.render_height), kInteractiveMaxLongEdge)
        << run.label;
    if (run.stats.median != nullptr) {
      EXPECT_FALSE(HasExecutedLlf(*run.stats.median)) << run.label;
    }
  }
  EXPECT_EQ(LongEdge(first.stats.render_width, first.stats.render_height), kInteractiveMaxLongEdge);
  EXPECT_GT(diag::PreviewQuantileNs(first.stats.qml_ns, 0.50),
            diag::PreviewQuantileNs(first.stats.encode_ns, 0.50));
  EXPECT_GE(diag::PreviewQuantileNs(first.stats.sink_ns, 0.50), 0);
  EXPECT_GE(diag::PreviewQuantileNs(first.stats.sched_ns, 0.50), 0);
  EXPECT_TRUE(first.stats.develop_skipped);
  EXPECT_EQ(diag::PreviewPerformance::EventsLost(), 0u);
}

TEST_F(EditorPreviewPresentTrajectoryTest,
       Interactive2560PresentTrajectoryDumpLastGradeLlfEnabled) {
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
  QString last_grade;
  ASSERT_TRUE(ConnectEightCleanColorGrades(harness.nodes, &last_grade))
      << harness.nodes.last_error().toStdString();
  EXPECT_FALSE(harness.nodes.incomplete_draft());
  EXPECT_EQ(harness.nodes.backbone_node_ids().size(), 10);
  harness.nodes.selectNode(last_grade);
  ProcessEvents(100);
  ASSERT_TRUE(WaitUntil(
      [&] {
        return !session->render_busy() && !harness.viewport->interactivePresentLoopActive();
      },
      std::chrono::seconds(60)));
  harness.nodes.selectNode(last_grade);
  ProcessEvents(50);
  ASSERT_TRUE(session->enqueueNodeSwitchBoundary());
  const auto document = session->pipeline_document();
  ASSERT_NE(document, nullptr);
  std::string target_error;
  const auto  shadows_target = alcedo::CompleteSelectedNodeParameterTarget(
      *document, NodeId{last_grade.toStdString()}, "shadows", &target_error);
  ASSERT_TRUE(shadows_target.has_value()) << target_error;
  ASSERT_TRUE(EnableLlfOnSelectedGrade(session))
      << "can_edit=" << session->can_edit()
      << " selected=" << harness.nodes.selected_node_id_string().toStdString()
      << " last_grade=" << last_grade.toStdString()
      << " last_error=" << session->last_error().toStdString()
      << " status=" << harness.viewport->statusText().toStdString();

  const auto dump_path = PreviewDumpDirectory() / "cuda_interactive_2560_present_llf_table.txt";
  const auto csv_path  = PreviewDumpDirectory() / "cuda_interactive_2560_present_llf_frames.csv";
  std::ofstream out(dump_path, std::ios::trunc);
  std::ofstream csv(csv_path, std::ios::trunc);
  out << "CUDA Interactive 2560 product present trajectory with LLF enabled\n";
  out << "kind=slider_interactive_frame_trace\n";
  out << "path=submitWrite -> admit -> schedule -> worker -> sink -> import -> frameSwapped\n";
  out << "cfa=Bayer source=" << raw_files.front().filename().string() << "\n";
  out << "decode_res=FULL interactive_max_edge=" << kInteractiveMaxLongEdge << "\n";
  out << "grades=8 selected_node=last_color_grade field=exposure llf=enabled\n";
  out << "llf_shadows=" << kLlfShadows << " llf_highlights=" << kLlfHighlights << "\n";
  out << "exposure_start=" << kExposureWriteStart << " step=" << kExposureWriteStep
      << " wrap_min=" << kExposureWrapMin << " wrap_max=" << kExposureWrapMax << "\n";
  out << "slider_period_ms=" << kSliderPeriod.count()
      << " trajectory_s=" << kTrajectoryLength.count()
      << " session_cache=kept settled_write=true_on_loop_end\n";
  out << "runs=3x Detail last-exposure slider after last-grade Shadows/Highlights\n";
  csv << "run,request_id,qml_ms,e2e_ms,gpu_ms,encode_ms,sink_ms,swap_ms,last_grade_gpu_ms,"
         "drt_gpu_ms,llf_gpu_ms,executed_grades,render\n";

  std::vector<TrajectoryRunResult> runs;
  for (int i = 0; i < kRepeatCount; ++i) {
    auto run = RunExposureTrajectory(harness, session, "detail_repeat_" + std::to_string(i + 1),
                                     diag::PreviewPerformanceMode::Detail);
    ASSERT_TRUE(run.has_value()) << "Detail LLF trajectory " << (i + 1) << " did not become idle";
    runs.push_back(std::move(*run));
  }

  WriteTrajectoryDump(out, csv, runs);
  out.flush();
  csv.flush();

  const auto& first = runs.front();
  ASSERT_FALSE(first.stats.qml_ns.empty());
  ASSERT_NE(first.stats.median, nullptr);
  EXPECT_TRUE(HasExecutedLlf(*first.stats.median));
  EXPECT_GT(diag::PreviewQuantileNs(first.stats.llf_gpu_ns, 0.50), 0);
  for (const auto& run : runs) {
    EXPECT_FALSE(run.stats.qml_ns.empty()) << run.label;
    EXPECT_GT(run.stats.presented, 0u) << run.label;
    EXPECT_EQ(LongEdge(run.stats.render_width, run.stats.render_height), kInteractiveMaxLongEdge)
        << run.label;
    ASSERT_NE(run.stats.median, nullptr) << run.label;
    EXPECT_TRUE(HasExecutedLlf(*run.stats.median)) << run.label;
    EXPECT_GT(diag::PreviewQuantileNs(run.stats.llf_gpu_ns, 0.50), 0) << run.label;
    EXPECT_TRUE(run.stats.develop_skipped) << run.label;
  }
  EXPECT_GT(diag::PreviewQuantileNs(first.stats.qml_ns, 0.50),
            diag::PreviewQuantileNs(first.stats.encode_ns, 0.50));
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
