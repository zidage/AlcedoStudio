//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <condition_variable>
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
// drains them after imported QRhi wrappers have been destroyed and any
// ProducerWriting lease has been abandoned by the producer.
class FramePresentationBroker final {
 public:
  struct Diagnostics {
    EditorBackend backend = EditorBackend::Cuda;
    std::uint64_t target_generation = 0;
    std::uint64_t image_generation = 0;
    std::uint64_t image_identity = 0;
    std::uint64_t last_presented_image_generation = 0;
    std::uint64_t last_presented_request_id = 0;
    std::uint64_t presented_frame_count = 0;
    std::uint64_t dropped_stale_frame_count = 0;
    std::size_t live_target_count = 0;
    std::size_t available_target_count = 0;
    std::size_t producer_writing_count = 0;
    bool consumer_available = true;
  };

  explicit FramePresentationBroker(EditorBackend backend);

  [[nodiscard]] auto backend() const -> EditorBackend { return backend_; }

  // Render-thread side. Returns false for invalid or stale targets. Multiple
  // targets may share one target generation (the normal ring-buffer case).
  auto PublishWritableTarget(WritableTargetLease lease) -> bool;

  // Producer side. Non-blocking: hidden, saturated, or size-mismatched
  // viewports return no lease so a pipeline worker can cancel or reschedule.
  [[nodiscard]] auto TryAcquireWritableTarget(const WritableTargetRequest& request)
      -> std::optional<WritableTargetLease>;
  // Producer-side equivalent of the legacy BlockingQueuedConnection resize
  // handshake. Waits for an explicit publish/failure/lifecycle result; it does
  // not guess render-thread latency with a timer.
  [[nodiscard]] auto WaitAcquireWritableTarget(const WritableTargetRequest& request)
      -> std::optional<WritableTargetLease>;
  [[nodiscard]] auto HasWritableTarget(const WritableTargetRequest& request) const -> bool;

  // Legacy helper used by older unit tests: acquire any Available target of the
  // current generation. Prefer the request form in production code.
  [[nodiscard]] auto TryAcquireWritableTarget() -> std::optional<WritableTargetLease>;

  // Producer abandons a write without submitting (cancel, size change mid-write).
  // Marks producer complete and queues the target for release when invalid, or
  // returns it to Available when still current.
  void AbandonProducerWrite(const WritableTargetLease& lease);

  auto SubmitCompletedFrame(CompletedFrameLease frame) -> bool;

  // Render-thread side. Only the newest compatible frame (by preview_generation,
  // then detail_serial, then submission sequence) is returned. A zero field in
  // expected is a wildcard for that generation component, except image_identity
  // which is checked when non-zero.
  [[nodiscard]] auto ConsumeNewestCompletedFrame(TargetGeneration expected,
                                                 LeaseFrameLayer layer)
      -> std::optional<CompletedFrameLease>;
  void CompleteRendererConsumption(const CompletedFrameLease& frame);
  // Render-thread acknowledgement. This marks a compatible imported frame as
  // actually sampled without recycling its target while it remains displayed.
  auto AcknowledgeFramePresented(const CompletedFrameLease& frame) -> bool;

  // Lifecycle operations are safe from the GUI or producer thread. They only
  // mutate broker state and enqueue native-release intents for targets that are
  // not currently ProducerWriting.
  void SetConsumerAvailable(bool available);
  void InvalidateTargetGeneration();
  void InvalidateImageGeneration(std::uint64_t image_generation, std::uint64_t image_identity = 0);
  void Shutdown();

  // Must run on the scene-graph render thread. Each returned lease must be
  // destroyed by the adapter that created it, after QRhi wrappers are gone and
  // the lifetime token reports can_destroy() (or cancel with no consumers).
  [[nodiscard]] auto DrainReleasedTargets() -> std::vector<WritableTargetLease>;

  // Outstanding producer size requests that the render thread should fulfill.
  void NoteTargetRequest(const WritableTargetRequest& request);
  void FailTargetRequest(const WritableTargetRequest& request);
  [[nodiscard]] auto DrainTargetRequests() -> std::vector<WritableTargetRequest>;

  [[nodiscard]] auto CurrentTargetGeneration() const -> std::uint64_t;
  [[nodiscard]] auto CurrentImageGeneration() const -> std::uint64_t;
  [[nodiscard]] auto CurrentImageIdentity() const -> std::uint64_t;
  [[nodiscard]] auto DiagnosticsSnapshot() const -> Diagnostics;

 private:
  enum class TargetState : std::uint8_t {
    Available,
    ProducerWriting,
    RendererConsuming,
    PendingRelease,  // cancelled or invalidated; waiting for producer abandon
  };

  struct TargetRecord {
    WritableTargetLease lease{};
    TargetState state = TargetState::Available;
    bool release_when_idle = false;
  };

  struct CompletedRecord {
    CompletedFrameLease frame{};
    std::uint64_t sequence = 0;
  };

  static auto SameTarget(const WritableTargetLease& lhs, const WritableTargetLease& rhs) -> bool;
  static auto GenerationMatches(const TargetGeneration& actual,
                                const TargetGeneration& expected) -> bool;
  static auto IsNewerFrame(const CompletedFrameLease& candidate,
                           const CompletedFrameLease& current,
                           std::uint64_t candidate_sequence,
                           std::uint64_t current_sequence) -> bool;

  void QueueTargetForReleaseLocked(WritableTargetLease lease);
  void DropCompletedForTargetLocked(const WritableTargetLease& lease, bool recycle_to_available);
  void InvalidateLocked(bool bump_target_generation);
  void RecycleOrReleaseTargetLocked(TargetRecord& record);
  auto FindTargetLocked(const WritableTargetLease& lease) -> TargetRecord*;
  auto TryAcquireWritableTargetLocked(const WritableTargetRequest& request)
      -> std::optional<WritableTargetLease>;
  static auto SameRequestTarget(const WritableTargetRequest& lhs,
                                const WritableTargetRequest& rhs) -> bool;
  auto AcceptsFrameLocked(const CompletedFrameLease& frame) const -> bool;

  mutable std::mutex mutex_;
  std::condition_variable target_ready_;
  EditorBackend backend_;
  bool consumer_available_ = true;
  bool shutdown_ = false;
  std::uint64_t current_target_generation_ = 0;
  std::uint64_t current_image_generation_ = 0;
  std::uint64_t current_image_identity_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t last_presented_image_generation_ = 0;
  std::uint64_t last_presented_request_id_ = 0;
  std::uint64_t presented_frame_count_ = 0;
  std::uint64_t dropped_stale_frame_count_ = 0;
  // Per-layer high-water marks so late older edits cannot replace newer results.
  std::array<std::uint64_t, 3> last_accepted_preview_generation_{};
  std::array<std::uint64_t, 3> last_accepted_detail_serial_{};
  std::deque<TargetRecord> targets_;
  std::deque<CompletedRecord> completed_;
  std::deque<WritableTargetLease> release_queue_;
  std::deque<WritableTargetRequest> pending_requests_;
  std::deque<WritableTargetRequest> failed_requests_;
};

}  // namespace alcedo::editor_rhi
