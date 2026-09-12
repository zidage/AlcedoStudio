//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/preview_performance.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace alcedo {

auto SteadyPreviewMonotonicClock::NowNs() const -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace alcedo

namespace alcedo::diag {
namespace {

constexpr std::size_t kMaxPassesPerRequest     = 32;
constexpr std::size_t kMaxSubStagesPerPass     = 12;
constexpr std::size_t kMaxOpenIntervals        = 16;
constexpr std::size_t kDefaultPendingCapacity  = 64;
constexpr std::size_t kDefaultQueueCapacity    = 32;

enum class OpenKind : std::uint8_t { Cpu, Pass, Sub };

struct OpenInterval {
  OpenKind          kind  = OpenKind::Cpu;
  PreviewCpuStage   cpu   = PreviewCpuStage::Encode;
  std::int64_t      start_ns = 0;
  std::uint8_t      pass_index = 0;
};

struct PendingSub {
  PreviewSubStageKind   kind  = PreviewSubStageKind::Pointwise;
  PreviewExecutionState state = PreviewExecutionState::Executed;
  std::int64_t          cpu_ns = 0;
};

struct PendingPass {
  std::uint16_t         owner_id = 0;
  std::uint16_t         mask_id  = 0;
  PreviewPassKind       kind     = PreviewPassKind::UploadRaw;
  std::uint32_t         ordinal  = 0;
  PreviewExecutionState state    = PreviewExecutionState::Executed;
  std::int64_t          cpu_ns   = 0;
  std::uint8_t          sub_count = 0;
  PendingSub            subs[kMaxSubStagesPerPass]{};
};

struct PendingSample {
  std::uint64_t              request_id         = 0;
  std::uint64_t              input_sequence_id  = 0;
  PreviewFrameRole           frame_role         = PreviewFrameRole::InteractivePrimary;
  PreviewQuality             quality            = PreviewQuality::Interactive;
  std::string                reason;
  bool                       has_user_input     = false;
  bool                       incomplete         = false;
  std::int64_t               first_accepted_ns  = 0;
  std::int64_t               latest_accepted_ns = 0;
  std::int64_t               submit_ns          = 0;
  std::int64_t               scheduled_ns       = 0;
  std::int64_t               producer_ready_ns  = 0;
  std::int64_t               present_wake_ns    = 0;
  std::int64_t               consume_begin_ns   = 0;
  std::int64_t               displayed_ns       = 0;
  std::uint64_t              qt_frame           = 0;
  PreviewCpuStageTimes       cpu{};
  std::uint8_t               pass_count         = 0;
  PendingPass                passes[kMaxPassesPerRequest]{};
  std::uint8_t               open_count         = 0;
  OpenInterval               open[kMaxOpenIntervals]{};
  bool                       has_develop_decode = false;
  PreviewDevelopDecodeParams develop{};
  bool                       has_resources      = false;
  PreviewResourceSnapshot    resources{};
};

auto CpuField(PreviewCpuStageTimes& times, PreviewCpuStage stage) -> std::int64_t& {
  switch (stage) {
    case PreviewCpuStage::Apply:
      return times.apply_ns;
    case PreviewCpuStage::Invalidation:
      return times.invalidation_ns;
    case PreviewCpuStage::PlanKey:
      return times.plan_key_ns;
    case PreviewCpuStage::PlanLookup:
      return times.plan_lookup_ns;
    case PreviewCpuStage::PlanCompile:
      return times.plan_compile_ns;
    case PreviewCpuStage::Allocation:
      return times.allocation_ns;
    case PreviewCpuStage::Encode:
      return times.encode_ns;
    case PreviewCpuStage::Submit:
      return times.submit_ns;
    case PreviewCpuStage::Wait:
      return times.wait_ns;
  }
  return times.encode_ns;
}

void SubtractChildren(PendingSample& sample) {
  std::int64_t pass_total = 0;
  for (std::uint8_t p = 0; p < sample.pass_count; ++p) {
    auto&      pass      = sample.passes[p];
    std::int64_t sub_total = 0;
    for (std::uint8_t s = 0; s < pass.sub_count; ++s) {
      sub_total += pass.subs[s].cpu_ns;
    }
    if (pass.cpu_ns > sub_total) {
      pass.cpu_ns -= sub_total;
    } else if (sub_total > 0) {
      pass.cpu_ns = 0;
    }
    pass_total += pass.cpu_ns + sub_total;
  }
  if (sample.cpu.encode_ns > pass_total) {
    sample.cpu.encode_ns -= pass_total;
  } else if (pass_total > 0 && sample.cpu.encode_ns > 0) {
    sample.cpu.encode_ns = 0;
  }
}

auto QuantileNs(std::vector<std::int64_t> values, const double fraction) -> std::int64_t {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(
      std::min(values.size() - 1, static_cast<std::size_t>(fraction * (values.size() - 1))));
  return values[index];
}

auto FormatSummary(const std::vector<std::int64_t>& e2e_ns, const std::uint64_t presented,
                   const std::uint64_t lost) -> std::string {
  std::ostringstream out;
  out << "#preview_perf_summary samples=" << e2e_ns.size() << " presented=" << presented
      << " lost=" << lost << " p50_e2e_ns=" << QuantileNs(e2e_ns, 0.50)
      << " p95_e2e_ns=" << QuantileNs(e2e_ns, 0.95)
      << " p99_e2e_ns=" << QuantileNs(e2e_ns, 0.99);
  std::int64_t max_ns = 0;
  for (const auto value : e2e_ns) {
    max_ns = std::max(max_ns, value);
  }
  out << " max_e2e_ns=" << max_ns << "\n";
  return out.str();
}

auto FormatRecord(const PreviewRequestRecord& record) -> std::string {
  std::ostringstream out;
  out << "#preview_perf v1\n";
  out << "request id=" << record.request_id << " sequence=" << record.input_sequence_id
      << " role=" << PreviewFrameRoleName(record.frame_role)
      << " quality=" << PreviewQualityName(record.quality) << " reason="
      << (record.reason.empty() ? "?" : record.reason)
      << " has_user_input=" << (record.has_user_input ? 1 : 0)
      << " incomplete=" << (record.incomplete ? 1 : 0) << "\n";
  out << "input first_accepted_ns=" << record.first_accepted_ns
      << " latest_accepted_ns=" << record.latest_accepted_ns << "\n";
  out << "cpu apply_ns=" << record.cpu.apply_ns << " invalidation_ns=" << record.cpu.invalidation_ns
      << " plan_key_ns=" << record.cpu.plan_key_ns << " plan_lookup_ns=" << record.cpu.plan_lookup_ns
      << " plan_compile_ns=" << record.cpu.plan_compile_ns
      << " allocation_ns=" << record.cpu.allocation_ns << " encode_ns=" << record.cpu.encode_ns
      << " submit_ns=" << record.cpu.submit_ns << " wait_ns=" << record.cpu.wait_ns << "\n";
  for (const auto& pass : record.passes) {
    out << "pass owner=" << (pass.owner.empty() ? "?" : pass.owner)
        << " kind=" << PreviewPassKindName(pass.kind) << " ordinal=" << pass.ordinal
        << " state=" << PreviewExecutionStateName(pass.state) << " cpu_ns=" << pass.cpu_ns
        << " gpu=unavailable";
    if (!pass.mask_id.empty()) {
      out << " mask=" << pass.mask_id;
    }
    out << "\n";
    for (const auto& sub : pass.sub_stages) {
      out << "  sub kind=" << PreviewSubStageKindName(sub.kind)
          << " state=" << PreviewExecutionStateName(sub.state) << " cpu_ns=" << sub.cpu_ns << "\n";
    }
  }
  if (record.has_develop_decode) {
    const auto& d = record.develop;
    out << "develop decode_res=" << PreviewDecodeResName(d.decode_res)
        << " cfa=" << PreviewCfaKindName(d.cfa)
        << " demosaic=" << PreviewDemosaicMethodName(d.demosaic)
        << " highlights_reconstruct=" << (d.highlights_reconstruct ? 1 : 0)
        << " downsample_passes=" << static_cast<unsigned>(d.downsample_passes) << " host="
        << d.host_width << "x" << d.host_height << " develop=" << d.develop_width << "x"
        << d.develop_height << " full_ref=" << d.full_ref_width << "x" << d.full_ref_height
        << " upload_rgb=" << (d.upload_rgb ? 1 : 0)
        << " layout=" << PreviewDevelopLayoutName(d.layout) << "\n";
  }
  if (record.has_resources) {
    const auto& r = record.resources;
    out << "resource texture_used_bytes=" << r.texture_used_bytes
        << " texture_leased_bytes=" << r.texture_leased_bytes
        << " texture_unleased_bytes=" << r.texture_unleased_bytes
        << " texture_entry_count=" << r.texture_entry_count
        << " texture_allocation_count=" << r.texture_allocation_count
        << " texture_peak_used_bytes=" << r.texture_peak_used_bytes
        << " transient_used_bytes=" << r.transient_used_bytes
        << " transient_capacity_bytes=" << r.transient_capacity_bytes
        << " published_image_count=" << r.published_image_count
        << " write_image_count=" << r.write_image_count << " value_bytes=" << r.value_bytes
        << " value_count=" << r.value_count;
    if (r.device_memory_valid) {
      out << " device_used_bytes=" << r.device_used_bytes
          << " device_free_bytes=" << r.device_free_bytes
          << " device_total_bytes=" << r.device_total_bytes;
    }
    out << "\n";
  }
  out << "present qt_frame=" << record.qt_frame << " displayed_ns=" << record.displayed_ns
      << " submit_ns=" << record.submit_ns;
  if (record.has_user_input && record.first_accepted_ns > 0 && record.displayed_ns > 0) {
    out << " input_to_present_ns=" << (record.displayed_ns - record.latest_accepted_ns);
  } else {
    out << " input_to_present_ns=n/a";
  }
  out << "\n";
  out << "terminal outcome=" << PreviewTerminalOutcomeName(record.outcome);
  if (!record.terminal_reason.empty()) {
    out << " reason=" << record.terminal_reason;
  }
  out << " gpu=unavailable\n";
  return out.str();
}

struct State {
  std::atomic<PreviewPerformanceMode> mode{PreviewPerformanceMode::Off};
  std::shared_ptr<IPreviewMonotonicClock> clock = std::make_shared<SteadyPreviewMonotonicClock>();

  std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::uint64_t, PendingSample> pending;
  std::vector<std::string> intern_names{std::string{}};
  std::deque<PreviewRequestRecord> queue;
  std::size_t queue_capacity = kDefaultQueueCapacity;
  std::uint64_t events_queued = 0;
  std::uint64_t events_lost   = 0;
  std::int64_t last_gui_update_ns   = 0;
  std::int64_t last_render_enter_ns = 0;
  std::uint64_t qt_frame            = 0;
  bool writer_paused = false;
  bool writer_busy   = false;
  bool stop_writer   = false;
  bool writer_started = false;
  std::thread writer;
  std::function<void(const PreviewRequestRecord&)> sink;
  std::string output_path;
  std::string written_log;
  std::ofstream file;
  std::vector<std::int64_t> summary_e2e_ns;
  std::uint64_t summary_presented = 0;

  ~State() { StopWriter(); }

  void StopWriter() {
    {
      std::lock_guard lock(mutex);
      stop_writer = true;
      writer_paused = false;
    }
    cv.notify_all();
    if (writer.joinable()) {
      writer.join();
    }
    writer_started = false;
    stop_writer    = false;
  }

  void EnsureWriterLocked() {
    if (writer_started) {
      return;
    }
    stop_writer     = false;
    writer_started  = true;
    writer          = std::thread([this] { WriterLoop(); });
  }

  void WriterLoop() {
    for (;;) {
      std::vector<PreviewRequestRecord> batch;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] {
          return stop_writer || (!writer_paused && !queue.empty());
        });
        if (stop_writer && queue.empty()) {
          return;
        }
        if (writer_paused) {
          continue;
        }
        writer_busy = true;
        while (!queue.empty()) {
          batch.push_back(std::move(queue.front()));
          queue.pop_front();
        }
      }
      for (const auto& record : batch) {
        const auto mode_now = mode.load(std::memory_order_relaxed);
        std::string text;
        {
          std::lock_guard lock(mutex);
          if (record.outcome == PreviewTerminalOutcome::Presented && !record.incomplete &&
              record.displayed_ns > record.submit_ns) {
            ++summary_presented;
            summary_e2e_ns.push_back(record.displayed_ns - record.submit_ns);
          }
          if (mode_now == PreviewPerformanceMode::Summary) {
            text = FormatSummary(summary_e2e_ns, summary_presented, events_lost);
          } else {
            text = FormatRecord(record);
          }
          written_log += text;
          if (!output_path.empty()) {
            if (!file.is_open()) {
              file.open(output_path, std::ios::out | std::ios::app);
            }
            if (file.is_open()) {
              file << text << std::flush;
            }
          }
        }
        std::function<void(const PreviewRequestRecord&)> local_sink;
        {
          std::lock_guard lock(mutex);
          local_sink = sink;
        }
        if (local_sink) {
          local_sink(record);
        }
      }
      {
        std::lock_guard lock(mutex);
        writer_busy = false;
      }
      cv.notify_all();
    }
  }

  auto InternLocked(std::string_view name) -> std::uint16_t {
    if (name.empty()) {
      return 0;
    }
    for (std::uint16_t i = 1; i < intern_names.size(); ++i) {
      if (intern_names[i] == name) {
        return i;
      }
    }
    intern_names.emplace_back(name);
    return static_cast<std::uint16_t>(intern_names.size() - 1);
  }

  auto InternNameLocked(std::uint16_t id) const -> std::string {
    if (id == 0 || id >= intern_names.size()) {
      return {};
    }
    return intern_names[id];
  }

  auto FindLocked(std::uint64_t request_id) -> PendingSample* {
    const auto it = pending.find(request_id);
    if (it == pending.end()) {
      return nullptr;
    }
    return &it->second;
  }

  auto CurrentSampleLocked() -> PendingSample* { return FindLocked(tls_request); }

  void CompleteLocked(PendingSample sample, PreviewTerminalOutcome outcome,
                      std::string_view reason, std::int64_t now_ns) {
    SubtractChildren(sample);
    PreviewRequestRecord record;
    record.request_id         = sample.request_id;
    record.input_sequence_id  = sample.input_sequence_id;
    record.frame_role         = sample.frame_role;
    record.quality            = sample.quality;
    record.reason             = std::move(sample.reason);
    record.has_user_input     = sample.has_user_input;
    record.incomplete         = sample.incomplete;
    record.first_accepted_ns  = sample.first_accepted_ns;
    record.latest_accepted_ns = sample.latest_accepted_ns;
    record.submit_ns          = sample.submit_ns;
    record.scheduled_ns       = sample.scheduled_ns;
    record.producer_ready_ns  = sample.producer_ready_ns;
    record.present_wake_ns    = sample.present_wake_ns;
    record.consume_begin_ns   = sample.consume_begin_ns;
    record.displayed_ns       = sample.displayed_ns != 0 ? sample.displayed_ns : now_ns;
    record.qt_frame           = sample.qt_frame;
    record.cpu                = sample.cpu;
    record.has_develop_decode = sample.has_develop_decode;
    record.develop            = sample.develop;
    record.has_resources      = sample.has_resources;
    record.resources          = sample.resources;
    record.outcome            = outcome;
    record.terminal_reason    = std::string(reason);
    record.gpu_status         = PreviewGpuTimeStatus::Unavailable;
    record.passes.reserve(sample.pass_count);
    for (std::uint8_t p = 0; p < sample.pass_count; ++p) {
      const auto& src = sample.passes[p];
      PreviewPassRecord pass;
      pass.owner      = InternNameLocked(src.owner_id);
      pass.mask_id    = InternNameLocked(src.mask_id);
      pass.kind       = src.kind;
      pass.ordinal    = src.ordinal;
      pass.state      = src.state;
      pass.cpu_ns     = src.cpu_ns;
      pass.gpu_status = PreviewGpuTimeStatus::Unavailable;
      pass.sub_stages.reserve(src.sub_count);
      for (std::uint8_t s = 0; s < src.sub_count; ++s) {
        PreviewSubStageRecord sub;
        sub.kind   = src.subs[s].kind;
        sub.state  = src.subs[s].state;
        sub.cpu_ns = src.subs[s].cpu_ns;
        pass.sub_stages.push_back(sub);
      }
      record.passes.push_back(std::move(pass));
    }
    if (queue.size() >= queue_capacity) {
      ++events_lost;
      record.incomplete = true;
      return;
    }
    queue.push_back(std::move(record));
    ++events_queued;
    cv.notify_one();
  }

  static thread_local std::uint64_t tls_request;
};

thread_local std::uint64_t State::tls_request = 0;

auto Global() -> State& {
  static State state;
  return state;
}

auto ParseMode(std::string_view value) -> PreviewPerformanceMode {
  if (value.empty() || value == "off" || value == "0") {
    return PreviewPerformanceMode::Off;
  }
  if (value == "summary") {
    return PreviewPerformanceMode::Summary;
  }
  if (value == "detail" || value == "1") {
    return PreviewPerformanceMode::Detail;
  }
  return PreviewPerformanceMode::Off;
}

}  // namespace

namespace detail {

auto PreviewPerformanceModeAtomic() -> std::atomic<PreviewPerformanceMode>& {
  return Global().mode;
}

}  // namespace detail

void PreviewPerformance::InitializeFromEnvironment() {
  const char* mode_env = std::getenv("ALCEDO_PREVIEW_PERF");
  const auto  mode     = ParseMode(mode_env == nullptr ? "" : mode_env);
  SetMode(mode);
  const char* path_env = std::getenv("ALCEDO_PREVIEW_PERF_LOG");
  if (path_env != nullptr && path_env[0] != '\0') {
    SetOutputPath(path_env);
  }
}

void PreviewPerformance::Shutdown() {
  auto& state = Global();
  state.StopWriter();
  std::lock_guard lock(state.mutex);
  state.pending.clear();
  state.queue.clear();
  state.intern_names.assign(1, std::string{});
  state.events_queued = 0;
  state.events_lost   = 0;
  state.written_log.clear();
  state.summary_e2e_ns.clear();
  state.summary_presented = 0;
  if (state.file.is_open()) {
    state.file.close();
  }
  state.mode.store(PreviewPerformanceMode::Off, std::memory_order_relaxed);
  State::tls_request = 0;
}

void PreviewPerformance::ResetForTesting() {
  Shutdown();
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  state.queue_capacity      = kDefaultQueueCapacity;
  state.writer_paused       = false;
  state.sink                = {};
  state.output_path.clear();
  state.last_gui_update_ns   = 0;
  state.last_render_enter_ns = 0;
  state.qt_frame             = 0;
  state.clock = std::make_shared<SteadyPreviewMonotonicClock>();
}

void PreviewPerformance::SetMode(PreviewPerformanceMode mode) {
  auto& state = Global();
  state.mode.store(mode, std::memory_order_relaxed);
  if (mode == PreviewPerformanceMode::Off) {
    return;
  }
  std::lock_guard lock(state.mutex);
  state.EnsureWriterLocked();
}

auto PreviewPerformance::Mode() -> PreviewPerformanceMode {
  return Global().mode.load(std::memory_order_relaxed);
}

void PreviewPerformance::SetClock(std::shared_ptr<IPreviewMonotonicClock> clock) {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  state.clock = clock ? std::move(clock) : std::make_shared<SteadyPreviewMonotonicClock>();
}

auto PreviewPerformance::NowNs() -> std::int64_t {
  auto& state = Global();
  std::shared_ptr<IPreviewMonotonicClock> clock;
  {
    std::lock_guard lock(state.mutex);
    clock = state.clock;
  }
  return clock ? clock->NowNs() : 0;
}

void PreviewPerformance::SetOutputPath(std::string path) {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  if (state.file.is_open()) {
    state.file.close();
  }
  state.output_path = std::move(path);
}

void PreviewPerformance::SetQueueCapacityForTesting(std::size_t capacity) {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  state.queue_capacity = capacity == 0 ? 1 : capacity;
}

void PreviewPerformance::PauseWriterForTesting(bool paused) {
  auto& state = Global();
  {
    std::lock_guard lock(state.mutex);
    state.writer_paused = paused;
  }
  state.cv.notify_all();
}

void PreviewPerformance::InstallRecordSink(std::function<void(const PreviewRequestRecord&)> sink) {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  state.sink = std::move(sink);
}

void PreviewPerformance::FlushWriter() {
  auto& state = Global();
  std::unique_lock lock(state.mutex);
  if (!state.writer_started) {
    return;
  }
  state.writer_paused = false;
  state.cv.notify_all();
  state.cv.wait(lock, [&] { return state.queue.empty() && !state.writer_busy; });
}

auto PreviewPerformance::EventsQueued() -> std::uint64_t {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  return state.events_queued;
}

auto PreviewPerformance::EventsLost() -> std::uint64_t {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  return state.events_lost;
}

auto PreviewPerformance::PendingSampleCount() -> std::size_t {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  return state.pending.size();
}

auto PreviewPerformance::InternCount() -> std::size_t {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  return state.intern_names.size() > 0 ? state.intern_names.size() - 1 : 0;
}

auto PreviewPerformance::WrittenLog() -> std::string {
  auto& state = Global();
  std::lock_guard lock(state.mutex);
  return state.written_log;
}

void PreviewPerformance::NoteSubmit(const std::uint64_t request_id, const PreviewFrameRole role,
                                    const PreviewQuality quality, const std::string_view reason,
                                    const bool has_user_input) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&            state = Global();
  const auto       now   = NowNs();
  std::lock_guard  lock(state.mutex);
  if (state.pending.size() >= kDefaultPendingCapacity &&
      state.pending.find(request_id) == state.pending.end()) {
    ++state.events_lost;
    return;
  }
  PendingSample sample;
  sample.request_id     = request_id;
  sample.frame_role     = role;
  sample.quality        = quality;
  sample.reason         = std::string(reason);
  sample.has_user_input = has_user_input;
  sample.submit_ns      = now;
  state.pending.insert_or_assign(request_id, std::move(sample));
}

void PreviewPerformance::NoteInputTimes(const std::uint64_t request_id,
                                        const std::uint64_t sequence_id,
                                        const std::int64_t first_accepted_ns,
                                        const std::int64_t latest_accepted_ns) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.FindLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  sample->input_sequence_id  = sequence_id;
  sample->first_accepted_ns  = first_accepted_ns;
  sample->latest_accepted_ns = latest_accepted_ns;
}

void PreviewPerformance::NoteScheduled(const std::uint64_t request_id) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.FindLocked(request_id);
  if (sample == nullptr || sample->scheduled_ns != 0) {
    return;
  }
  sample->scheduled_ns = now;
}

void PreviewPerformance::NoteProducerReady(const std::uint64_t request_id) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.FindLocked(request_id);
  if (sample == nullptr || sample->producer_ready_ns != 0) {
    return;
  }
  if (sample->scheduled_ns == 0) {
    sample->scheduled_ns = sample->submit_ns;
  }
  sample->producer_ready_ns = now;
}

void PreviewPerformance::NotePresentWake(const std::uint64_t request_id) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.FindLocked(request_id);
  if (sample == nullptr || sample->present_wake_ns != 0) {
    return;
  }
  if (sample->producer_ready_ns == 0) {
    sample->producer_ready_ns = now;
  }
  sample->present_wake_ns = now;
}

void PreviewPerformance::NoteGuiUpdate() {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  state.last_gui_update_ns = now;
}

void PreviewPerformance::NoteRenderEnter() {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  ++state.qt_frame;
  state.last_render_enter_ns = now;
}

void PreviewPerformance::NoteConsumeBegin(const std::uint64_t request_id) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.FindLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  sample->consume_begin_ns = now;
  sample->qt_frame         = state.qt_frame;
}

void PreviewPerformance::NoteDisplayed(const std::uint64_t request_id) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto it = state.pending.find(request_id);
  if (it == state.pending.end()) {
    return;
  }
  PendingSample sample = std::move(it->second);
  state.pending.erase(it);
  sample.displayed_ns = now;
  if (sample.qt_frame == 0) {
    sample.qt_frame = state.qt_frame;
  }
  state.CompleteLocked(std::move(sample), PreviewTerminalOutcome::Presented, {}, now);
}

void PreviewPerformance::NoteTerminal(const std::uint64_t request_id,
                                      const PreviewTerminalOutcome outcome,
                                      const std::string_view reason) {
  if (!PreviewPerformanceEnabled() || request_id == 0) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto it = state.pending.find(request_id);
  if (it == state.pending.end()) {
    return;
  }
  PendingSample sample = std::move(it->second);
  state.pending.erase(it);
  state.CompleteLocked(std::move(sample), outcome, reason, now);
}

void PreviewPerformance::BindCurrentRequest(const std::uint64_t request_id) {
  State::tls_request = request_id;
}

void PreviewPerformance::ClearCurrentRequest() { State::tls_request = 0; }

auto PreviewPerformance::CurrentRequestId() -> std::uint64_t { return State::tls_request; }

void PreviewPerformance::BeginCpu(const PreviewCpuStage stage) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr || sample->open_count >= kMaxOpenIntervals) {
    return;
  }
  sample->open[sample->open_count++] = OpenInterval{OpenKind::Cpu, stage, now, 0};
}

void PreviewPerformance::AddCpuDuration(const std::uint64_t request_id, const PreviewCpuStage stage,
                                        const std::int64_t ns) {
  if (!PreviewPerformanceEnabled() || request_id == 0 || ns <= 0) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.FindLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  CpuField(sample->cpu, stage) += ns;
}

void PreviewPerformance::EndCpu(const PreviewCpuStage stage) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr) {
    return;
  }
  for (std::uint8_t i = sample->open_count; i > 0; --i) {
    auto& frame = sample->open[i - 1];
    if (frame.kind == OpenKind::Cpu && frame.cpu == stage) {
      CpuField(sample->cpu, stage) += now - frame.start_ns;
      for (std::uint8_t j = i - 1; j + 1 < sample->open_count; ++j) {
        sample->open[j] = sample->open[j + 1];
      }
      --sample->open_count;
      return;
    }
  }
}

void PreviewPerformance::BeginPass(const std::string_view owner, const PreviewPassKind kind,
                                   const std::uint32_t ordinal, const std::string_view mask_id) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr || sample->pass_count >= kMaxPassesPerRequest ||
      sample->open_count >= kMaxOpenIntervals) {
    return;
  }
  auto& pass      = sample->passes[sample->pass_count];
  pass            = PendingPass{};
  pass.owner_id   = state.InternLocked(owner);
  pass.mask_id    = state.InternLocked(mask_id);
  pass.kind       = kind;
  pass.ordinal    = ordinal;
  pass.state      = PreviewExecutionState::Executed;
  sample->open[sample->open_count++] =
      OpenInterval{OpenKind::Pass, PreviewCpuStage::Encode, now, sample->pass_count};
  ++sample->pass_count;
}

void PreviewPerformance::SetPassState(const PreviewExecutionState state_value) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr || sample->pass_count == 0) {
    return;
  }
  sample->passes[sample->pass_count - 1].state = state_value;
}

void PreviewPerformance::EndPass() {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr) {
    return;
  }
  for (std::uint8_t i = sample->open_count; i > 0; --i) {
    auto& frame = sample->open[i - 1];
    if (frame.kind == OpenKind::Pass) {
      sample->passes[frame.pass_index].cpu_ns += now - frame.start_ns;
      for (std::uint8_t j = i - 1; j + 1 < sample->open_count; ++j) {
        sample->open[j] = sample->open[j + 1];
      }
      --sample->open_count;
      return;
    }
  }
}

void PreviewPerformance::BeginSubStage(const PreviewSubStageKind kind) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr || sample->pass_count == 0 || sample->open_count >= kMaxOpenIntervals) {
    return;
  }
  auto& pass = sample->passes[sample->pass_count - 1];
  if (pass.sub_count >= kMaxSubStagesPerPass) {
    return;
  }
  auto& sub   = pass.subs[pass.sub_count];
  sub         = PendingSub{};
  sub.kind    = kind;
  sub.state   = PreviewExecutionState::Executed;
  sample->open[sample->open_count++] = OpenInterval{
      OpenKind::Sub, PreviewCpuStage::Encode, now,
      static_cast<std::uint8_t>(sample->pass_count - 1)};
  ++pass.sub_count;
}

void PreviewPerformance::SetSubStageState(const PreviewExecutionState state_value) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr || sample->pass_count == 0) {
    return;
  }
  auto& pass = sample->passes[sample->pass_count - 1];
  if (pass.sub_count == 0) {
    return;
  }
  pass.subs[pass.sub_count - 1].state = state_value;
}

void PreviewPerformance::EndSubStage() {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  const auto      now   = NowNs();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr) {
    return;
  }
  for (std::uint8_t i = sample->open_count; i > 0; --i) {
    auto& frame = sample->open[i - 1];
    if (frame.kind == OpenKind::Sub) {
      auto& pass = sample->passes[frame.pass_index];
      if (pass.sub_count > 0) {
        pass.subs[pass.sub_count - 1].cpu_ns += now - frame.start_ns;
      }
      for (std::uint8_t j = i - 1; j + 1 < sample->open_count; ++j) {
        sample->open[j] = sample->open[j + 1];
      }
      --sample->open_count;
      return;
    }
  }
}

void PreviewPerformance::NoteDevelopDecode(const PreviewDevelopDecodeParams& params) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr) {
    return;
  }
  sample->has_develop_decode = true;
  sample->develop            = params;
}

void PreviewPerformance::NoteDevelopLayout(const PreviewDevelopLayout layout) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr || !sample->has_develop_decode) {
    return;
  }
  sample->develop.layout = layout;
}

void PreviewPerformance::NoteResourceSnapshot(const PreviewResourceSnapshot& snapshot) {
  if (!PreviewPerformanceEnabled()) {
    return;
  }
  auto&           state = Global();
  std::lock_guard lock(state.mutex);
  auto*           sample = state.CurrentSampleLocked();
  if (sample == nullptr) {
    return;
  }
  sample->has_resources = true;
  sample->resources     = snapshot;
}

}  // namespace alcedo::diag
