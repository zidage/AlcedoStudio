//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

class EditorViewportItem;

// Production IFrameSink for the unified QML editor. Restores the pre-branch
// EnsureSize → MapResourceForWrite → GPU write → UnmapResource → NotifyFrameReady
// sequence with a fixed three-slot direct-present queue. No host upload, no
// lease acquisition, and no per-request presentation bookkeeping.
class DirectFrameSink final : public alcedo::IFrameSink {
 public:
  // One-shot first-frame Qt Quick composition confirmation for the active
  // image session. Not emitted for superseded interactive frames, QualityBase,
  // or DetailPatch.
  using FirstFrameCompositionCallback =
      std::function<void(std::uint64_t request_id, std::uint64_t image_generation,
                         std::uint64_t image_identity)>;

  explicit DirectFrameSink(EditorViewportItem* item);
  ~DirectFrameSink() override;

  void EnsureSize(int width, int height) override;
  auto MapResourceForWrite(FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice)
      -> FrameWriteMapping override;
  void UnmapResource() override;
  void NotifyFrameReady() override;
  void SubmitHostFrame(const ViewerFrame&) override;
  void SubmitFinalDisplayFrame(const FinalDisplayFrameView&) override;

  [[nodiscard]] int GetWidth() const override;
  [[nodiscard]] int GetHeight() const override;
  [[nodiscard]] auto GetViewportRenderRegion() const
      -> std::optional<ViewportRenderRegion> override;
  void SetNextFramePresentationMode(FramePresentationMode mode) override;
  void SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) override;

  void SetViewState(const ViewerViewState& state);
  void SetFirstFrameCompositionCallback(FirstFrameCompositionCallback callback);
  // Called by the renderer after a compatible primary frame is drawn into a
  // Qt Quick window frame. Emits the session first-composition event at most once.
  void NotifyPrimaryFrameComposed(const DirectPresentQueue::ReadyFrame& frame);
  [[nodiscard]] auto HasWritableTargetForNextFrame() const -> bool;
  [[nodiscard]] auto submitted_frame_count() const -> std::uint64_t;
  [[nodiscard]] auto ViewState() const -> ViewerViewState;
  void ClearMappedSlot();

 private:
  [[nodiscard]] auto MakeSizeRequestLocked() const -> DirectPresentQueue::SizeRequest;
  auto ReserveWritableSlot(int width, int height) -> std::optional<int>;

  EditorViewportItem* item_ = nullptr;
  mutable std::mutex mutex_;
  int width_ = 0;
  int height_ = 0;
  std::uint64_t last_sized_image_generation_ = 0;
  std::uint64_t last_sized_image_identity_ = 0;
  bool has_mapped_slot_ = false;
  bool unmapped_pending_submit_ = false;
  int mapped_slot_index_ = -1;
  int prepared_slot_index_ = -1;
  FramePresentationMode pending_presentation_mode_ = FramePresentationMode::FullFrame;
  bool pending_presentation_mode_valid_ = false;
  FramePreviewMetadata pending_preview_metadata_{};
  bool pending_preview_metadata_valid_ = false;
  ViewerViewState view_state_{};
  FirstFrameCompositionCallback first_frame_composition_;
  std::uint64_t submitted_frame_count_ = 0;
};

}  // namespace alcedo::editor_rhi
