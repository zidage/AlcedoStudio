//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

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
  bool has_mapped_lease_ = false;
  bool unmapped_pending_submit_ = false;
  WritableTargetLease mapped_lease_{};
  FramePresentationMode pending_presentation_mode_ = FramePresentationMode::FullFrame;
  bool pending_presentation_mode_valid_ = false;
  FramePreviewMetadata pending_preview_metadata_{};
  bool pending_preview_metadata_valid_ = false;
  ViewerViewState view_state_{};
};

}  // namespace alcedo::editor_rhi
