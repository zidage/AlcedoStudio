//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/render_e2e_timing.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace alcedo::diag {
namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
  Clock::time_point                submit_at{};
  std::optional<Clock::time_point> scheduled_at;
  std::optional<Clock::time_point> producer_ready_at;
  std::optional<Clock::time_point> present_wake_at;
  std::optional<Clock::time_point> gui_update_at;
  std::optional<Clock::time_point> render_enter_at;
  std::optional<Clock::time_point> consume_begin_at;
  std::string                      reason;
  std::string                      quality;
  std::string                      role;
};

// Bound in-flight samples so a present-path miss cannot grow without limit
// during a long interactive session.
constexpr std::size_t kMaxPendingSamples = 256;

std::mutex                                g_mutex;
std::unordered_map<std::uint64_t, Sample> g_samples;

auto MsBetween(const Clock::time_point start, const Clock::time_point end) -> double {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void EraseLocked(const std::uint64_t request_id) {
  g_samples.erase(request_id);
}

void PruneIfNeededLocked() {
  if (g_samples.size() <= kMaxPendingSamples) {
    return;
  }
  // Drop the lowest request ids first (oldest under monotonic allocator).
  while (g_samples.size() > kMaxPendingSamples / 2) {
    auto oldest = g_samples.begin();
    for (auto it = g_samples.begin(); it != g_samples.end(); ++it) {
      if (it->first < oldest->first) {
        oldest = it;
      }
    }
    g_samples.erase(oldest);
  }
}

auto FindSampleLocked(const std::uint64_t request_id) -> Sample* {
  const auto it = g_samples.find(request_id);
  if (it == g_samples.end()) {
    return nullptr;
  }
  return &it->second;
}

void EnsurePresentChainPrefix(Sample* sample, const Clock::time_point now) {
  if (sample == nullptr) {
    return;
  }
  if (!sample->scheduled_at.has_value()) {
    sample->scheduled_at = sample->submit_at;
  }
  if (!sample->producer_ready_at.has_value()) {
    sample->producer_ready_at = now;
  }
  if (!sample->present_wake_at.has_value()) {
    sample->present_wake_at = sample->producer_ready_at;
  }
}

}  // namespace

void NoteRenderE2eSubmit(const std::uint64_t request_id, const std::string_view reason,
                         const std::string_view quality, const std::string_view role) {
  if (request_id == 0) {
    return;
  }
  Sample sample;
  sample.submit_at = Clock::now();
  sample.reason    = std::string(reason);
  sample.quality   = std::string(quality);
  sample.role      = std::string(role);

  std::lock_guard lock(g_mutex);
  g_samples.insert_or_assign(request_id, std::move(sample));
  PruneIfNeededLocked();
}

void NoteRenderE2eScheduled(const std::uint64_t request_id) {
  if (request_id == 0) {
    return;
  }
  const auto      now = Clock::now();
  std::lock_guard lock(g_mutex);
  Sample*         sample = FindSampleLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  if (!sample->scheduled_at.has_value()) {
    sample->scheduled_at = now;
  }
}

void NoteRenderE2eProducerReady(const std::uint64_t request_id) {
  if (request_id == 0) {
    return;
  }
  const auto      now = Clock::now();
  std::lock_guard lock(g_mutex);
  Sample*         sample = FindSampleLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  if (!sample->scheduled_at.has_value()) {
    sample->scheduled_at = sample->submit_at;
  }
  if (!sample->producer_ready_at.has_value()) {
    sample->producer_ready_at = now;
  }
}

void NoteRenderE2ePresentWake(const std::uint64_t request_id) {
  if (request_id == 0) {
    return;
  }
  const auto      now = Clock::now();
  std::lock_guard lock(g_mutex);
  Sample*         sample = FindSampleLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  EnsurePresentChainPrefix(sample, now);
  if (!sample->present_wake_at.has_value()) {
    sample->present_wake_at = now;
  }
}

void NoteRenderE2eGuiUpdate() {
  const auto      now = Clock::now();
  std::lock_guard lock(g_mutex);
  for (auto& [request_id, sample] : g_samples) {
    (void)request_id;
    // Only frames that already asked for a redraw; ignore bare submit samples.
    if (!sample.present_wake_at.has_value() || sample.gui_update_at.has_value()) {
      continue;
    }
    sample.gui_update_at = now;
  }
}

void NoteRenderE2eRenderEnter() {
  const auto      now = Clock::now();
  std::lock_guard lock(g_mutex);
  for (auto& [request_id, sample] : g_samples) {
    (void)request_id;
    if (!sample.present_wake_at.has_value() || sample.render_enter_at.has_value()) {
      continue;
    }
    if (!sample.gui_update_at.has_value()) {
      // Render can run without a dedicated wake stamp on some paths; keep the
      // chain monotonic for printing.
      sample.gui_update_at = sample.present_wake_at;
    }
    sample.render_enter_at = now;
  }
}

void NoteRenderE2eConsumeBegin(const std::uint64_t request_id) {
  if (request_id == 0) {
    return;
  }
  const auto      now = Clock::now();
  std::lock_guard lock(g_mutex);
  Sample*         sample = FindSampleLocked(request_id);
  if (sample == nullptr) {
    return;
  }
  EnsurePresentChainPrefix(sample, now);
  if (!sample->gui_update_at.has_value()) {
    sample->gui_update_at = sample->present_wake_at;
  }
  if (!sample->render_enter_at.has_value()) {
    sample->render_enter_at = now;
  }
  if (!sample->consume_begin_at.has_value()) {
    sample->consume_begin_at = now;
  }
}

void NoteRenderE2eDisplayed(const std::uint64_t request_id) {
  if (request_id == 0) {
    return;
  }
  const auto now = Clock::now();

  Sample sample;
  {
    std::lock_guard lock(g_mutex);
    const auto      it = g_samples.find(request_id);
    if (it == g_samples.end()) {
      return;
    }
    sample = std::move(it->second);
    g_samples.erase(it);
  }

  const double total_ms       = MsBetween(sample.submit_at, now);
  const auto   scheduled      = sample.scheduled_at.value_or(sample.submit_at);
  const auto   producer_ready = sample.producer_ready_at.value_or(now);
  const auto   present_wake   = sample.present_wake_at.value_or(producer_ready);
  const auto   gui_update     = sample.gui_update_at.value_or(present_wake);
  const auto   render_enter   = sample.render_enter_at.value_or(sample.consume_begin_at.value_or(now));
  const auto   consume_begin  = sample.consume_begin_at.value_or(render_enter);

  const double queue_ms    = MsBetween(sample.submit_at, scheduled);
  const double pipeline_ms = MsBetween(scheduled, producer_ready);
  const double present_ms  = MsBetween(producer_ready, now);
  const double wake_ms     = MsBetween(producer_ready, present_wake);
  const double gui_wait_ms = MsBetween(present_wake, gui_update);
  const double sg_wait_ms  = MsBetween(gui_update, render_enter);
  // Render-thread work after the frame starts: target fulfill + createFrom.
  // consume_begin is retained for diagnostics if fulfill ever grows.
  const double import_ms = MsBetween(render_enter, now);
  const double fps       = total_ms > 0.0 ? (1000.0 / total_ms) : 0.0;

  std::cout << std::fixed << std::setprecision(2) << "[RENDER_E2E] request=" << request_id
            << " reason=" << (sample.reason.empty() ? "?" : sample.reason)
            << " quality=" << (sample.quality.empty() ? "?" : sample.quality)
            << " role=" << (sample.role.empty() ? "?" : sample.role) << " total=" << total_ms
            << "ms queue=" << queue_ms << "ms pipeline=" << pipeline_ms
            << "ms present=" << present_ms << "ms (wake=" << wake_ms << "ms gui_wait=" << gui_wait_ms
            << "ms sg_wait=" << sg_wait_ms << "ms import=" << import_ms << "ms) (~"
            << std::setprecision(1) << fps << " fps)" << std::endl;

  (void)consume_begin;
}

void NoteRenderE2eTerminal(const std::uint64_t request_id, const std::string_view /*outcome*/) {
  if (request_id == 0) {
    return;
  }
  std::lock_guard lock(g_mutex);
  EraseLocked(request_id);
}

}  // namespace alcedo::diag
