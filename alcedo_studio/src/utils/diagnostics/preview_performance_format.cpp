//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/preview_performance_format.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace alcedo::diag {
namespace {

auto FormatWallTimestamp() -> std::string {
  const auto now    = std::chrono::system_clock::now();
  const auto second = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(now - second);
  const auto t      = std::chrono::system_clock::to_time_t(now);
  std::tm    local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
      << ms.count();
  return out.str();
}

auto FormatThreadId() -> std::string {
  std::ostringstream out;
  out.imbue(std::locale::classic());
#if defined(_WIN32)
  out << std::hex << GetCurrentThreadId();
#elif defined(__APPLE__)
  std::uint64_t thread_id = 0;
  pthread_threadid_np(nullptr, &thread_id);
  out << std::hex << thread_id;
#else
  out << std::hex << static_cast<unsigned long>(syscall(SYS_gettid));
#endif
  return out.str();
}

}  // namespace

auto PreviewQuantileNs(std::vector<std::int64_t> values, const double fraction) -> std::int64_t {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      std::min(values.size() - 1, static_cast<std::size_t>(fraction * (values.size() - 1))));
  return values[index];
}

auto PreviewMaxNs(const std::vector<std::int64_t>& values) -> std::int64_t {
  std::int64_t max_ns = 0;
  for (const auto value : values) {
    max_ns = std::max(max_ns, value);
  }
  return max_ns;
}

auto FormatPreviewMilliseconds(const std::int64_t nanoseconds) -> std::string {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(2) << (static_cast<double>(nanoseconds) / 1'000'000.0);
  return out.str();
}

auto FormatPreviewMegabytes(const std::size_t bytes) -> std::string {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0));
  return out.str();
}

auto FormatPreviewLogPrefix() -> std::string {
  return FormatWallTimestamp() + " [INFO] [tid=" + FormatThreadId() + "] [alcedo.preview.perf] ";
}

auto FormatPreviewSlowest(const PreviewRequestRecord& record) -> std::string {
  const auto e2e_ns = PreviewDurationNs(record.displayed_ns, record.submit_ns);
  std::ostringstream out;
  out << "slowest id=" << record.request_id << " "
      << (record.reason.empty() ? "?" : record.reason) << " "
      << PreviewFrameRoleName(record.frame_role) << " e2e_ms=" << FormatPreviewMilliseconds(e2e_ns);
  if (record.has_user_input && record.latest_accepted_ns > 0) {
    out << " input_ms="
        << FormatPreviewMilliseconds(PreviewDurationNs(record.displayed_ns, record.latest_accepted_ns));
  }
  if (record.has_user_input && record.qml_latest_write_ns > 0 && record.frame_swapped_ns > 0) {
    out << " qml_ms="
        << FormatPreviewMilliseconds(
               PreviewDurationNs(record.frame_swapped_ns, record.qml_latest_write_ns));
  }
  if (record.extra_schedule_wait_ns > 0) {
    out << " extra_sched_ms=" << FormatPreviewMilliseconds(record.extra_schedule_wait_ns);
  }
  if (record.present_wake_ns > 0 && record.gui_update_ns > 0) {
    out << " ready_to_gui_ms="
        << FormatPreviewMilliseconds(
               PreviewDurationNs(record.gui_update_ns, record.present_wake_ns));
  }
  if (record.gui_update_ns > 0 && record.imported_ns > 0) {
    out << " gui_to_import_ms="
        << FormatPreviewMilliseconds(PreviewDurationNs(record.imported_ns, record.gui_update_ns));
  }
  if (record.imported_ns > 0 && record.frame_swapped_ns > 0) {
    out << " import_to_swap_ms="
        << FormatPreviewMilliseconds(
               PreviewDurationNs(record.frame_swapped_ns, record.imported_ns));
  }
  if (record.worker_start_ns > 0 && record.scheduled_ns > 0) {
    out << " sched_ms="
        << FormatPreviewMilliseconds(PreviewDurationNs(record.worker_start_ns, record.scheduled_ns));
  }
  if (record.producer_ready_ns > 0 && record.sink_submit_ns > 0) {
    out << " sink_ms="
        << FormatPreviewMilliseconds(
               PreviewDurationNs(record.producer_ready_ns, record.sink_submit_ns));
  }
  out << " apply_ms=" << FormatPreviewMilliseconds(record.cpu.apply_ns)
      << " encode_ms=" << FormatPreviewMilliseconds(record.cpu.encode_ns)
      << " wait_ms=" << FormatPreviewMilliseconds(record.cpu.wait_ns);
  if (record.has_develop_decode) {
    const auto& d = record.develop;
    out << " develop=" << PreviewDecodeResName(d.decode_res) << " " << PreviewCfaKindName(d.cfa)
        << " " << PreviewDemosaicMethodName(d.demosaic) << " " << d.develop_width << "x"
        << d.develop_height;
  }
  if (record.render_width > 0 && record.render_height > 0) {
    out << " render=" << record.render_width << "x" << record.render_height;
  }
  for (const auto& pass : record.passes) {
    out << " | " << (pass.owner.empty() ? "?" : pass.owner) << " "
        << PreviewPassKindName(pass.kind) << "=" << FormatPreviewMilliseconds(pass.cpu_ns);
    if (pass.gpu_status == PreviewGpuTimeStatus::Available) {
      out << " gpu_ms=" << FormatPreviewMilliseconds(pass.gpu_ns);
    }
    for (const auto& sub : pass.sub_stages) {
      out << " " << PreviewSubStageKindName(sub.kind) << "="
          << FormatPreviewMilliseconds(sub.cpu_ns);
      if (sub.gpu_status == PreviewGpuTimeStatus::Available) {
        out << " gpu_ms=" << FormatPreviewMilliseconds(sub.gpu_ns);
      }
    }
  }
  if (record.has_resources) {
    out << " | texture_mb=" << FormatPreviewMegabytes(record.resources.texture_used_bytes)
        << " peak_mb=" << FormatPreviewMegabytes(record.resources.texture_peak_used_bytes)
        << " allocs=" << record.resources.texture_allocation_count;
  }
  return out.str();
}

void PreviewWindowAccum::AddPresentedIntervals(const PreviewRequestRecord& record) {
  const auto interval_ns = PreviewDurationNs(record.displayed_ns, record.submit_ns);
  if (interval_ns <= 0) {
    return;
  }
  e2e_ns.push_back(interval_ns);
  if (record.has_user_input && record.latest_accepted_ns > 0) {
    input_ns.push_back(PreviewDurationNs(record.displayed_ns, record.latest_accepted_ns));
  }
  if (record.has_user_input && record.qml_latest_write_ns > 0 && record.frame_swapped_ns > 0) {
    qml_ns.push_back(PreviewDurationNs(record.frame_swapped_ns, record.qml_latest_write_ns));
  }
  apply_ns.push_back(record.cpu.apply_ns);
  encode_ns.push_back(record.cpu.encode_ns);
  wait_ns.push_back(record.cpu.wait_ns);
  if (record.extra_schedule_wait_ns > 0) {
    extra_schedule_ns.push_back(record.extra_schedule_wait_ns);
  }
  const auto ready_to_gui = PreviewDurationNs(record.gui_update_ns, record.present_wake_ns);
  if (ready_to_gui > 0) {
    ready_to_gui_ns.push_back(ready_to_gui);
  }
  const auto gui_to_import = PreviewDurationNs(record.imported_ns, record.gui_update_ns);
  if (gui_to_import > 0) {
    gui_to_import_ns.push_back(gui_to_import);
  }
  const auto import_to_swap = PreviewDurationNs(record.frame_swapped_ns, record.imported_ns);
  if (import_to_swap > 0) {
    import_to_swap_ns.push_back(import_to_swap);
  }
  if (interval_ns > slowest_e2e_ns) {
    slowest_e2e_ns = interval_ns;
    slowest        = record;
  }
}

auto FormatPreviewWindow(const PreviewWindowAccum& window, const std::uint64_t lost) -> std::string {
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - window.start)
                              .count();
  std::ostringstream out;
  out << FormatPreviewLogPrefix() << "window_ms=" << elapsed_ms << " presented=" << window.presented
      << " dropped=" << window.dropped << " cancelled=" << window.cancelled
      << " failed=" << window.failed << " coalesced=" << window.coalesced
      << " stale=" << window.stale << " lost=" << lost
      << " e2e_ms p50=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.e2e_ns, 0.50))
      << " p95=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.e2e_ns, 0.95))
      << " max=" << FormatPreviewMilliseconds(PreviewMaxNs(window.e2e_ns))
      << " input_ms p50=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.input_ns, 0.50))
      << " p95=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.input_ns, 0.95))
      << " max=" << FormatPreviewMilliseconds(PreviewMaxNs(window.input_ns));
  if (!window.qml_ns.empty()) {
    out << " qml_ms p50=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.qml_ns, 0.50))
        << " p95=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.qml_ns, 0.95))
        << " max=" << FormatPreviewMilliseconds(PreviewMaxNs(window.qml_ns));
  }
  out << " apply_ms p50=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.apply_ns, 0.50))
      << " encode_ms p50=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.encode_ns, 0.50))
      << " wait_ms p50=" << FormatPreviewMilliseconds(PreviewQuantileNs(window.wait_ns, 0.50));
  if (!window.extra_schedule_ns.empty()) {
    out << " extra_sched_ms p50="
        << FormatPreviewMilliseconds(PreviewQuantileNs(window.extra_schedule_ns, 0.50))
        << " max=" << FormatPreviewMilliseconds(PreviewMaxNs(window.extra_schedule_ns));
  }
  if (!window.ready_to_gui_ns.empty()) {
    out << " ready_to_gui_ms p50="
        << FormatPreviewMilliseconds(PreviewQuantileNs(window.ready_to_gui_ns, 0.50));
  }
  if (!window.gui_to_import_ns.empty()) {
    out << " gui_to_import_ms p50="
        << FormatPreviewMilliseconds(PreviewQuantileNs(window.gui_to_import_ns, 0.50));
  }
  if (!window.import_to_swap_ns.empty()) {
    out << " import_to_swap_ms p50="
        << FormatPreviewMilliseconds(PreviewQuantileNs(window.import_to_swap_ns, 0.50));
  }
  if (window.slowest.has_value()) {
    out << " | " << FormatPreviewSlowest(*window.slowest);
  }
  out << "\n";
  return out.str();
}

}  // namespace alcedo::diag
