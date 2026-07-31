//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"

namespace alcedo::editor_rhi {

// Fixed three-slot direct-present queue restored from the pre-unified-QML
// RhiEditViewerSurface path. Ownership is exclusive per slot; there is no
// application-wide lease protocol or dual-sided completion map.
//
// Slot states:
//   Available        -> free for prepare/write
//   ProducerWriting  -> mapped by the pipeline worker
//   Ready            -> written, waiting for the render thread
//   RendererReading  -> imported/sampled by QQuickRhiItemRenderer
//
// Producer path: EnsureSize/Prepare -> Map -> write -> Unmap -> NotifyReady
// Render path:   create requested natives -> ConsumeNewestReady -> draw -> Recycle
class DirectPresentQueue final {
 public:
  static constexpr int kSlotCount = 3;

  enum class SlotState : std::uint8_t {
    Available = 0,
    ProducerWriting,
    Ready,
    RendererReading,
  };

  struct SizeRequest {
    int                width            = 0;
    int                height           = 0;
    int                preferred_slot   = -1;
    std::uint64_t      image_generation = 0;
    std::uint64_t      image_identity   = 0;
    std::uint64_t      layer_generation = 0;
    FrameRole          frame_role       = FrameRole::InteractivePrimary;

    [[nodiscard]] auto valid() const -> bool {
      return width > 0 && height > 0 && image_generation != 0;
    }
  };

  struct SlotNative {
    EditorBackend             backend           = EditorBackend::Cuda;
    LeaseNativeHandleKind     handle_kind       = LeaseNativeHandleKind::None;
    LeaseWritableResourceKind writable_kind     = LeaseWritableResourceKind::None;
    std::uintptr_t            native_handle     = 0;
    std::uintptr_t            writable_resource = 0;
    std::uintptr_t            sync_object       = 0;
    std::uint64_t             sync_value        = 0;
    // Opaque adapter cookie so the render thread can destroy the resource.
    std::uintptr_t            adapter_cookie    = 0;

    [[nodiscard]] auto        valid() const -> bool {
      return native_handle != 0 && writable_resource != 0 &&
             handle_kind != LeaseNativeHandleKind::None &&
             writable_kind != LeaseWritableResourceKind::None;
    }
  };

  struct SlotSnapshot {
    int                   index  = -1;
    SlotState             state  = SlotState::Available;
    int                   width  = 0;
    int                   height = 0;
    SlotNative            native{};
    FramePresentationMode presentation_mode = FramePresentationMode::FullFrame;
    FramePreviewMetadata  preview_metadata{};
    std::uint64_t         image_generation = 0;
    std::uint64_t         image_identity   = 0;
    std::uint64_t         sequence         = 0;
  };

  struct ReadyFrame {
    SlotSnapshot slot{};
  };

  struct Diagnostics {
    EditorBackend backend                        = EditorBackend::Cuda;
    std::uint64_t target_generation              = 0;
    std::uint64_t image_generation               = 0;
    std::uint64_t image_identity                 = 0;
    std::uint64_t last_composed_image_generation = 0;
    std::uint64_t last_composed_request_id       = 0;
    std::uint64_t composed_frame_count           = 0;
    std::uint64_t dropped_stale_frame_count      = 0;
    std::size_t   live_target_count              = 0;
    std::size_t   available_count                = 0;
    std::size_t   producer_writing_count         = 0;
    std::size_t   ready_count                    = 0;
    bool          consumer_available             = true;
  };

  struct PrepareResult {
    int  slot_index  = -1;
    bool need_create = false;
    bool ok          = false;
  };

  explicit DirectPresentQueue(EditorBackend backend);

  [[nodiscard]] auto backend() const -> EditorBackend { return backend_; }

  // Producer: select a write slot (legacy SelectDirectPresentWriteSlot rules).
  // Does not block. When need_create is true, the producer must NoteSizeRequest
  // and WaitForWritableSlot after requesting a render-thread update.
  [[nodiscard]] auto PrepareWrite(int width, int height, std::uint64_t image_generation,
                                  std::uint64_t image_identity) -> PrepareResult;

  void               NoteSizeRequest(const SizeRequest& request);
  void               FailSizeRequest(const SizeRequest& request);
  [[nodiscard]] auto DrainSizeRequests() -> std::vector<SizeRequest>;

  // Producer wait for an explicit create/fail/invalidate/shutdown result.
  // Never uses a timer as a correctness mechanism.
  [[nodiscard]] auto WaitForWritableSlot(const SizeRequest& request) -> std::optional<int>;

  // Worker-thread producers: wait briefly for synchronize() to enable the
  // consumer. The timeout prevents a hidden/unloaded viewport from retaining a
  // pipeline worker indefinitely. GUI-thread callers must not use this.
  [[nodiscard]] auto WaitUntilConsumerAvailable(std::chrono::milliseconds timeout) -> bool;

  // Render thread publishes a fully created native into a selected slot.
  auto               PublishCreatedSlot(int slot_index, int width, int height, SlotNative native,
                                        std::uint64_t image_generation, std::uint64_t image_identity) -> bool;

  // Producer write lifecycle (mutex held across Map until Unmap, matching the
  // old surface lock that spans map→write→unmap).
  [[nodiscard]] auto BeginWrite(int slot_index) -> std::optional<SlotSnapshot>;
  void               EndWrite(int slot_index);
  void               NotifyReady(int slot_index, FramePresentationMode mode,
                                 const FramePreviewMetadata& metadata);
  void               AbandonWrite(int slot_index);

  // Render-thread consumption. Newest compatible ready frame wins per layer;
  // older undisplayed ready frames for that layer are superseded and recycled.
  [[nodiscard]] auto ConsumeNewestReady(FrameRole layer, std::uint64_t image_generation,
                                        std::uint64_t image_identity) -> std::optional<ReadyFrame>;
  // After QRhi no longer samples the slot (layer replaced or renderer teardown).
  void               CompleteRendererRead(int slot_index);

  // One-shot Qt Quick window composition event for the first compatible frame
  // of the current image session. Returns true only the first successful call
  // for that image generation.
  auto AcknowledgeFirstComposition(std::uint64_t request_id, std::uint64_t image_generation,
                                   std::uint64_t image_identity) -> bool;
  // Diagnostic: every composed primary frame increments composed_frame_count.
  void NoteFrameComposed(std::uint64_t request_id, std::uint64_t image_generation,
                         std::uint64_t image_identity);

  void SetConsumerAvailable(bool available);
  void InvalidateImageGeneration(std::uint64_t image_generation, std::uint64_t image_identity = 0);
  void InvalidateTargetGeneration();
  void Shutdown();

  // Render thread drains released natives that must be destroyed after QRhi
  // wrappers are gone.
  [[nodiscard]] auto DrainReleasedNatives() -> std::vector<SlotNative>;

  [[nodiscard]] auto CurrentTargetGeneration() const -> std::uint64_t;
  [[nodiscard]] auto CurrentImageGeneration() const -> std::uint64_t;
  [[nodiscard]] auto CurrentImageIdentity() const -> std::uint64_t;
  [[nodiscard]] auto DiagnosticsSnapshot() const -> Diagnostics;
  [[nodiscard]] auto SlotAt(int index) const -> std::optional<SlotSnapshot>;
  [[nodiscard]] auto HasWritableSlot(int width, int height, std::uint64_t image_generation,
                                     std::uint64_t image_identity) const -> bool;

 private:
  struct Slot {
    SlotState             state  = SlotState::Available;
    int                   width  = 0;
    int                   height = 0;
    SlotNative            native{};
    FramePresentationMode presentation_mode = FramePresentationMode::FullFrame;
    FramePreviewMetadata  preview_metadata{};
    std::uint64_t         image_generation = 0;
    std::uint64_t         image_identity   = 0;
    std::uint64_t         sequence         = 0;
  };

  [[nodiscard]] auto IsValidSlot(int index) const -> bool;
  [[nodiscard]] auto SnapshotLocked(int index) const -> SlotSnapshot;
  void               RecycleSlotLocked(int index, bool queue_native_release);
  void               InvalidateTargetsLocked(bool bump_target_generation);
  [[nodiscard]] auto SlotUnavailableLocked(int index) const -> bool;
  [[nodiscard]] auto SelectWriteSlotLocked(int width, int height) -> PrepareResult;
  [[nodiscard]] auto MatchesSizeRequestLocked(const SizeRequest& request, int slot_index) const
      -> bool;

  mutable std::mutex           mutex_;
  std::condition_variable      wake_;
  EditorBackend                backend_;
  // Starts unavailable: producers must not block on WaitForWritableSlot until
  // a visible exposed window publishes consumer availability.
  bool                         consumer_available_                 = false;
  bool                         shutdown_                           = false;
  std::uint64_t                target_generation_                  = 0;
  std::uint64_t                image_generation_                   = 0;
  std::uint64_t                image_identity_                     = 0;
  std::uint64_t                sequence_                           = 0;
  std::uint64_t                last_composed_image_generation_     = 0;
  std::uint64_t                last_composed_request_id_           = 0;
  std::uint64_t                composed_frame_count_               = 0;
  std::uint64_t                dropped_stale_frame_count_          = 0;
  std::uint64_t                first_composition_image_generation_ = 0;
  bool                         first_composition_emitted_          = false;
  int                          write_idx_                          = 1;
  int                          active_idx_                         = 0;
  int                          mapped_slot_idx_                    = -1;
  std::array<Slot, kSlotCount> slots_{};
  std::deque<SizeRequest>      pending_requests_;
  std::deque<SizeRequest>      failed_requests_;
  std::deque<SlotNative>       release_queue_;
};

// Platform-agnostic slot selection (also used by OpenCL). Mirrors the helpers
// historically gated behind CUDA-only headers.
struct DirectPresentSlotInfo {
  int  width        = 0;
  int  height       = 0;
  bool has_resource = false;
  bool unavailable  = false;
};

struct DirectPresentWriteSelection {
  int  slot_index  = 0;
  bool need_create = false;
};

[[nodiscard]] auto SelectDirectPresentWriteSlotGeneric(const DirectPresentSlotInfo* slot_infos,
                                                       std::size_t slot_count, int preferred_slot,
                                                       int width, int height)
    -> DirectPresentWriteSelection;

}  // namespace alcedo::editor_rhi
