//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"

namespace alcedo::editor_rhi {

// Thread-safe producer/consumer boundary between a pipeline worker and the
// Qt Quick scene-graph render thread. Native resources are never destroyed by
// broker methods. Invalidation only queues release intents; the render thread
// drains them after imported QRhi wrappers have been destroyed.
class FramePresentationBroker final {
 public:
  struct Diagnostics {
    EditorBackend backend = EditorBackend::Cuda;
    std::uint64_t target_generation = 0;
    std::uint64_t last_presented_image_generation = 0;
    std::uint64_t dropped_stale_frame_count = 0;
    std::size_t live_target_count = 0;
    bool consumer_available = true;
  };

  explicit FramePresentationBroker(EditorBackend backend = EditorBackend::Cuda);

  // Render-thread side. Returns false for invalid or stale targets. Multiple
  // targets may share one target generation (the normal ring-buffer case).
  auto PublishWritableTarget(WritableTargetLease lease) -> bool;

  // Producer side. This is deliberately non-blocking: a hidden or saturated
  // viewport returns no lease so a pipeline worker can cancel or reschedule.
  [[nodiscard]] auto TryAcquireWritableTarget() -> std::optional<WritableTargetLease>;
  auto SubmitCompletedFrame(CompletedFrameLease frame) -> bool;

  // Render-thread side. Only the newest compatible frame is returned. A zero
  // field in expected is a wildcard for that generation component.
  [[nodiscard]] auto ConsumeNewestCompletedFrame(
      TargetGeneration expected, LeaseFrameLayer layer)
      -> std::optional<CompletedFrameLease>;
  void CompleteRendererConsumption(const CompletedFrameLease& frame);

  // Lifecycle operations are safe from the GUI or producer thread. They only
  // mutate broker state and enqueue native-release intents.
  void SetConsumerAvailable(bool available);
  void InvalidateTargetGeneration();
  void InvalidateImageGeneration(std::uint64_t image_generation);
  void Shutdown();

  // Must run on the scene-graph render thread. Each returned lease must be
  // destroyed by the adapter that created it, after QRhi wrappers are gone.
  [[nodiscard]] auto DrainReleasedTargets() -> std::vector<WritableTargetLease>;

  [[nodiscard]] auto CurrentTargetGeneration() const -> std::uint64_t;
  [[nodiscard]] auto CurrentImageGeneration() const -> std::uint64_t;
  [[nodiscard]] auto DiagnosticsSnapshot() const -> Diagnostics;

 private:
  enum class TargetState : std::uint8_t {
    Available,
    ProducerWriting,
    RendererConsuming,
  };

  struct TargetRecord {
    WritableTargetLease lease{};
    TargetState state = TargetState::Available;
  };

  struct CompletedRecord {
    CompletedFrameLease frame{};
    std::uint64_t sequence = 0;
  };

  static auto SameTarget(const WritableTargetLease& lhs,
                         const WritableTargetLease& rhs) -> bool;
  static auto GenerationMatches(const TargetGeneration& actual,
                                const TargetGeneration& expected) -> bool;

  void QueueTargetForReleaseLocked(const WritableTargetLease& lease);
  void DropCompletedForTargetLocked(const WritableTargetLease& lease);
  void InvalidateLocked(std::optional<std::uint64_t> image_generation);

  mutable std::mutex mutex_;
  EditorBackend backend_ = EditorBackend::Cuda;
  bool consumer_available_ = true;
  bool shutdown_ = false;
  std::uint64_t current_target_generation_ = 0;
  std::uint64_t current_image_generation_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t last_presented_image_generation_ = 0;
  std::uint64_t dropped_stale_frame_count_ = 0;
  std::deque<TargetRecord> targets_;
  std::deque<CompletedRecord> completed_;
  std::deque<WritableTargetLease> release_queue_;
};

}  // namespace alcedo::editor_rhi
