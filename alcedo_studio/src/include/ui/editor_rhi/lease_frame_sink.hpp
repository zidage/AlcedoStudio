//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

class EditorViewportItem;

// Production IFrameSink that maps pipeline Map/Unmap/Notify onto the lease
// protocol. No host-upload fallback: MapResourceForWrite returns empty when no
// native target is available yet.
class LeaseFrameSink final : public alcedo::IFrameSink {
 public:
  using PresentationAcknowledgement =
      std::function<void(std::uint64_t request_id, std::uint64_t image_generation,
                         std::uint64_t image_identity)>;
  explicit LeaseFrameSink(EditorViewportItem* item);
  ~LeaseFrameSink() override;

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
  void SetPresentationAcknowledgementCallback(PresentationAcknowledgement callback);
  void AcknowledgePresentedFrame(const CompletedFrameLease& frame);
  [[nodiscard]] auto HasWritableTargetForNextFrame() const -> bool;
  [[nodiscard]] auto submitted_frame_count() const -> std::uint64_t;
  // Test/production diagnostics: last view state accepted from the controller.
  [[nodiscard]] auto ViewState() const -> ViewerViewState;
  void ClearMappedLease();

 private:
  [[nodiscard]] auto CurrentRequest(LeaseFrameLayer layer) const -> WritableTargetRequest;
  [[nodiscard]] static auto LayerForMetadata(const FramePreviewMetadata& metadata)
      -> LeaseFrameLayer;
  [[nodiscard]] static auto ToLeasePresentationMode(FramePresentationMode mode)
      -> LeasePresentationMode;

  EditorViewportItem* item_ = nullptr;
  mutable std::mutex mutex_;
  int width_ = 0;
  int height_ = 0;
  // Last image session identity for which EnsureSize published a target request.
  // Re-sync on every new image/session generation even when width/height match the
  // previous image (Phase 5B equal-output-size geometry).
  std::uint64_t last_sized_image_generation_ = 0;
  std::uint64_t last_sized_image_identity_   = 0;
  bool has_mapped_lease_ = false;
  std::optional<WritableTargetLease> prepared_lease_;
  bool unmapped_pending_submit_ = false;
  WritableTargetLease mapped_lease_{};
  FramePresentationMode pending_presentation_mode_ = FramePresentationMode::FullFrame;
  bool pending_presentation_mode_valid_ = false;
  FramePreviewMetadata pending_preview_metadata_{};
  bool pending_preview_metadata_valid_ = false;
  ViewerViewState view_state_{};
  PresentationAcknowledgement presentation_acknowledgement_{};
  std::uint64_t last_acknowledged_request_id_ = 0;
  std::uint64_t submitted_frame_count_ = 0;
};

}  // namespace alcedo::editor_rhi
