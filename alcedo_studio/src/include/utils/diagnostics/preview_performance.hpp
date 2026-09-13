//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "utils/diagnostics/preview_performance_record.hpp"

namespace alcedo {

class IPreviewMonotonicClock {
 public:
  virtual ~IPreviewMonotonicClock() = default;
  [[nodiscard]] virtual auto NowNs() const -> std::int64_t = 0;
};

class SteadyPreviewMonotonicClock final : public IPreviewMonotonicClock {
 public:
  [[nodiscard]] auto NowNs() const -> std::int64_t override;
};

}  // namespace alcedo

namespace alcedo::diag {

namespace detail {
[[nodiscard]] auto PreviewPerformanceModeAtomic() -> std::atomic<PreviewPerformanceMode>&;
}

/**
 * @brief True when Summary or Detail is active. Off is a relaxed atomic load only.
 */
[[nodiscard]] inline auto PreviewPerformanceEnabled() -> bool {
  return detail::PreviewPerformanceModeAtomic().load(std::memory_order_relaxed) !=
         PreviewPerformanceMode::Off;
}

/**
 * @brief Low-overhead preview timing owner. Off creates no samples, strings, or queue slots.
 *
 * Hot-path notes never format text or write files. Completed records go to a bounded
 * ring; a background thread writes one app-log duration line per second. A full ring
 * drops the diagnostic event, counts the loss, and leaves render unblocked.
 *
 * Process start turns Detail on. Tests call SetMode(Off) or ResetForTesting.
 * Optional `ALCEDO_PREVIEW_PERF_LOG` sets the output path. The log writes durations
 * in milliseconds. It does not write absolute monotonic clock values.
 *
 * @thread_safety Notes may run on the session owner, render worker, and Qt render
 *                thread. Off takes no lock.
 */
class PreviewPerformance {
 public:
  static void Initialize();
  static void Shutdown();
  static void ResetForTesting();

  static void SetMode(PreviewPerformanceMode mode);
  [[nodiscard]] static auto Mode() -> PreviewPerformanceMode;

  static void SetClock(std::shared_ptr<IPreviewMonotonicClock> clock);
  [[nodiscard]] static auto NowNs() -> std::int64_t;

  static void SetOutputPath(std::string path);
  static void SetQueueCapacityForTesting(std::size_t capacity);
  static void PauseWriterForTesting(bool paused);
  static void InstallRecordSink(std::function<void(const PreviewRequestRecord&)> sink);
  static void FlushWriter();

  [[nodiscard]] static auto EventsQueued() -> std::uint64_t;
  [[nodiscard]] static auto EventsLost() -> std::uint64_t;
  [[nodiscard]] static auto PendingSampleCount() -> std::size_t;
  [[nodiscard]] static auto InternCount() -> std::size_t;
  [[nodiscard]] static auto WrittenLog() -> std::string;

  static void NoteSubmit(std::uint64_t request_id, PreviewFrameRole role, PreviewQuality quality,
                         std::string_view reason, bool has_user_input);
  static void NoteInputTimes(std::uint64_t request_id, std::uint64_t sequence_id,
                             std::int64_t first_accepted_ns, std::int64_t latest_accepted_ns);
  static void NoteScheduled(std::uint64_t request_id);
  static void NoteProducerReady(std::uint64_t request_id);
  static void NotePresentWake(std::uint64_t request_id);
  static void NoteGuiUpdate();
  static void NoteRenderEnter();
  static void NoteConsumeBegin(std::uint64_t request_id);
  static void NoteDisplayed(std::uint64_t request_id);
  static void NoteTerminal(std::uint64_t request_id, PreviewTerminalOutcome outcome,
                           std::string_view reason);

  static void BindCurrentRequest(std::uint64_t request_id);
  static void ClearCurrentRequest();
  [[nodiscard]] static auto CurrentRequestId() -> std::uint64_t;

  static void BeginCpu(PreviewCpuStage stage);
  static void EndCpu(PreviewCpuStage stage);
  static void AddCpuDuration(std::uint64_t request_id, PreviewCpuStage stage, std::int64_t ns);

  static void BeginPass(std::string_view owner, PreviewPassKind kind, std::uint32_t ordinal,
                        std::string_view mask_id);
  static void SetPassState(PreviewExecutionState state);
  static void EndPass();

  static void BeginSubStage(PreviewSubStageKind kind);
  static void SetSubStageState(PreviewExecutionState state);
  static void EndSubStage();

  static void NoteDevelopDecode(const PreviewDevelopDecodeParams& params);
  static void NoteDevelopLayout(PreviewDevelopLayout layout);
  static void NoteResourceSnapshot(const PreviewResourceSnapshot& snapshot);

  /**
   * @brief Identity of the innermost open pass or sub-stage on this thread.
   *
   * Invalid when timing is Off or no request/pass is bound. Backends record
   * native timestamps against this target and later call @ref NoteGpuDuration.
   */
  [[nodiscard]] static auto CurrentGpuSampleTarget() -> PreviewGpuSampleTarget;

  /**
   * @brief Attach a resolved native GPU duration to a still-pending request.
   *
   * Skipped, aliased, and disabled passes keep @c Unavailable and ignore
   * @p gpu_ns. Looks up the pending sample by @p request_id, not TLS.
   */
  static void NoteGpuDuration(std::uint64_t request_id, std::uint8_t pass_index, bool is_sub,
                              std::uint8_t sub_index, std::int64_t gpu_ns,
                              PreviewGpuTimeStatus status);

  /**
   * @brief Attach a whole-submission GPU duration when per-pass samples are unavailable.
   */
  static void NoteGpuRequestDuration(std::uint64_t request_id, std::int64_t gpu_ns,
                                     PreviewGpuTimeStatus status);
};

class PreviewCpuInterval {
 public:
  explicit PreviewCpuInterval(PreviewCpuStage stage) : stage_(stage) {
    if (!PreviewPerformanceEnabled()) {
      return;
    }
    active_ = true;
    PreviewPerformance::BeginCpu(stage_);
  }
  ~PreviewCpuInterval() {
    if (active_) {
      PreviewPerformance::EndCpu(stage_);
    }
  }
  PreviewCpuInterval(const PreviewCpuInterval&)            = delete;
  auto operator=(const PreviewCpuInterval&) -> PreviewCpuInterval& = delete;

 private:
  PreviewCpuStage stage_;
  bool            active_ = false;
};

class PreviewPassInterval {
 public:
  PreviewPassInterval(std::string_view owner, PreviewPassKind kind, std::uint32_t ordinal = 0,
                      std::string_view mask_id = {}) {
    if (!PreviewPerformanceEnabled()) {
      return;
    }
    active_ = true;
    PreviewPerformance::BeginPass(owner, kind, ordinal, mask_id);
  }
  ~PreviewPassInterval() {
    if (active_) {
      PreviewPerformance::EndPass();
    }
  }
  void SetState(PreviewExecutionState state) {
    if (active_) {
      PreviewPerformance::SetPassState(state);
    }
  }
  PreviewPassInterval(const PreviewPassInterval&)            = delete;
  auto operator=(const PreviewPassInterval&) -> PreviewPassInterval& = delete;

 private:
  bool active_ = false;
};

class PreviewSubStageInterval {
 public:
  explicit PreviewSubStageInterval(PreviewSubStageKind kind) {
    if (!PreviewPerformanceEnabled()) {
      return;
    }
    active_ = true;
    PreviewPerformance::BeginSubStage(kind);
  }
  ~PreviewSubStageInterval() {
    if (active_) {
      PreviewPerformance::EndSubStage();
    }
  }
  void SetState(PreviewExecutionState state) {
    if (active_) {
      PreviewPerformance::SetSubStageState(state);
    }
  }
  PreviewSubStageInterval(const PreviewSubStageInterval&)            = delete;
  auto operator=(const PreviewSubStageInterval&) -> PreviewSubStageInterval& = delete;

 private:
  bool active_ = false;
};

}  // namespace alcedo::diag
