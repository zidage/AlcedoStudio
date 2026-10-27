//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "ui/edit_viewer/mask_coverage_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"

namespace alcedo::editor_rhi {

class EditorViewportItem;

/**
 * @brief Producer side of the coverage presentation channel of one editor viewport (SM8.5).
 *
 * Writes into its own DirectPresentQueue (owned by EditorViewportItem, separate from the photo
 * queue), so a coverage frame never occupies a photo slot and never reaches the photo IFrameSink
 * or scope analysis. CUDA and OpenCL use renderer-created native RGBA32F targets with the same
 * creation handshake and the same producer acquire/release as photo frames. Metal hands its own
 * completed texture to the render thread.
 *
 * A frame carries the image session it was rendered for (MaskCoverageFrameIdentity). The sink
 * drops it at map and at publication when the viewport changed image session; the render thread
 * checks the same identity again before it imports the frame.
 *
 * Owner: EditorViewportItem. Thread: the producing render thread for the IMaskCoverageSink calls;
 * the scene-graph render thread for @ref PresentationOfSlot and @ref TakePendingMetalCoverage.
 */
class ViewportMaskCoverageSink final : public IMaskCoverageSink {
 public:
#ifdef HAVE_METAL
  /// A submitted Metal coverage frame that waits for the render thread.
  struct PendingMetalCoverage {
    MaskCoverageMetalFrame   frame{};
    MaskCoveragePresentation presentation{};
    /// Publication sequence of the frame (@ref PublishedCount after its publication).
    std::uint64_t            sequence = 0;
  };
#endif

  /// @p layer labels the native targets of @p queue: MaskCoverage for the base channel,
  /// DetailMaskCoverage for the detail ROI channel.
  ViewportMaskCoverageSink(EditorViewportItem* item, std::shared_ptr<DirectPresentQueue> queue,
                           LeaseFrameLayer layer = LeaseFrameLayer::MaskCoverage);
  ~ViewportMaskCoverageSink() override;

  auto MapCoverageTarget(const MaskCoveragePresentation& presentation, int width, int height,
                         FrameMemoryDomain preferred_domain) -> FrameWriteMapping override;
  void UnmapCoverageTarget() override;
  auto PublishCoverage() -> MaskCoveragePublication override;
  void AbandonCoverageTarget() override;
  /**
   * @brief Hide the displayed coverage and every frame published so far (IMaskCoverageSink).
   *
   * Records the current publication sequence as @ref ClearedThrough and drops a submitted Metal
   * frame; the render thread releases the displayed frame and skips the queued frames whose
   * sequence is not above it. Thread: any; it does not touch a mapped target.
   */
  void ClearCoverage() override;
#ifdef HAVE_METAL
  auto               SubmitMetalCoverage(const MaskCoveragePresentation& presentation,
                                         const MaskCoverageMetalFrame& frame) -> MaskCoveragePublication override;
  /// Take the newest submitted Metal frame of the given image session; frames of another session
  /// are dropped.
  [[nodiscard]] auto TakePendingMetalCoverage(std::uint64_t session_epoch,
                                              std::uint64_t image_identity)
      -> std::optional<PendingMetalCoverage>;
#endif
  /// Drop a submitted Metal frame that the render thread has not taken (image change, close).
  void               ClearPendingMetalCoverage();
  /// True while a submitted Metal frame waits for the render thread.
  [[nodiscard]] auto HasPendingMetalCoverage() const -> bool;

  /// Metadata of the frame published into queue slot @p slot_index. Valid while the slot is Ready
  /// or RendererReading: the producer does not write the slot again before the renderer recycles
  /// it.
  [[nodiscard]] auto PresentationOfSlot(int slot_index) const -> MaskCoveragePresentation;
  /// Publication sequence of the frame published into queue slot @p slot_index; same validity as
  /// @ref PresentationOfSlot.
  [[nodiscard]] auto SequenceOfSlot(int slot_index) const -> std::uint64_t;
  /// Number of published frames (both paths) since construction. The n-th publication has
  /// sequence n.
  [[nodiscard]] auto PublishedCount() const -> std::uint64_t;
  /// Sequence of the last frame that @ref ClearCoverage hid; 0 before the first clear.
  [[nodiscard]] auto ClearedThrough() const -> std::uint64_t;
  /// Lease layer of the native targets of this channel.
  [[nodiscard]] auto LeaseLayer() const -> LeaseFrameLayer { return layer_; }

 private:
  [[nodiscard]] auto  MatchesViewportImage(const MaskCoverageFrameIdentity& identity) const -> bool;
  [[nodiscard]] auto  ReserveSlot(const MaskCoveragePresentation& presentation, int width,
                                  int height) -> std::optional<int>;
  void                ResetMappedLocked();

  EditorViewportItem* item_ = nullptr;
  std::shared_ptr<DirectPresentQueue>                                  queue_;
  LeaseFrameLayer                                                      layer_;
  mutable std::mutex                                                   mutex_;
  int                                                                  mapped_slot_ = -1;
  bool                                                                 unmapped_    = false;
  MaskCoveragePresentation                                             mapped_presentation_{};
  std::array<MaskCoveragePresentation, DirectPresentQueue::kSlotCount> slot_presentations_{};
  std::array<std::uint64_t, DirectPresentQueue::kSlotCount>            slot_sequences_{};
#ifdef HAVE_METAL
  std::optional<PendingMetalCoverage> pending_metal_;
#endif
  std::uint64_t published_count_ = 0;
  std::uint64_t cleared_through_ = 0;
};

}  // namespace alcedo::editor_rhi
