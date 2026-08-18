//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

class EditorViewportItem;

// Production IFrameSink for the unified QML editor.
//
// CUDA / OpenCL: EnsureSize → MapResourceForWrite → GPU write → UnmapResource →
// NotifyFrameReady against the fixed three-slot DirectPresentQueue.
// CPU and OpenCL readback: SubmitHostFrame queues a shared host-memory frame for
// QRhi texture upload on the scene-graph thread.
//
// Metal: zero-copy SubmitMetalFrame. The pipeline retains an MTLTexture* on the
// Metal device; the sink queues it with its owner, and EditorViewportRenderer
// imports via QRhiTexture::createFrom (no host staging).
class DirectFrameSink final : public alcedo::IFrameSink {
 public:
  // Zero-copy GPU present payload for backends that publish their own texture
  // (Metal). Lifetime is held by `owner` until the renderer releases the layer.
  struct ImportedGpuFrame {
    int width = 0;
    int height = 0;
    std::uintptr_t texture_handle = 0;
    // Passed to QRhiTexture::createFrom / setNativeLayout. Metal uses 0 today.
    int native_layout = 0;
    FramePresentationMode presentation_mode = FramePresentationMode::FullFrame;
    FramePreviewMetadata preview_metadata{};
    std::shared_ptr<const void> owner{};
    std::uint64_t session_epoch = 0;
    std::uint64_t image_identity = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] auto valid() const -> bool {
      return width > 0 && height > 0 && texture_handle != 0 && owner != nullptr;
    }
  };

  explicit DirectFrameSink(EditorViewportItem* item);
  ~DirectFrameSink() override;

  void EnsureSize(int width, int height) override;
  auto MapResourceForWrite(FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice)
      -> FrameWriteMapping override;
  void UnmapResource() override;
  void NotifyFrameReady(const FrameCompletionSubmission& submission) override;
  void BindFrameSubmission(const FrameCompletionSubmission& submission) override;
  void SubmitHostFrame(const ViewerFrame&) override;
#ifdef HAVE_METAL
  void SubmitMetalFrame(const ViewerMetalFrame& frame) override;
#endif
  void SubmitFinalDisplayFrame(const FinalDisplayFrameView&) override;

  [[nodiscard]] int GetWidth() const override;
  [[nodiscard]] int GetHeight() const override;
  [[nodiscard]] auto GetViewportRenderRegion() const
      -> std::optional<ViewportRenderRegion> override;

  void SetViewState(const ViewerViewState& state);
  // Drain newest pending zero-copy imports for the active image session.
  // One entry per frame role; older undisplayed frames for that role are dropped.
  [[nodiscard]] auto DrainPendingImportedFrames(std::uint64_t session_epoch,
                                                std::uint64_t image_identity)
      -> std::vector<ImportedGpuFrame>;
  // Drain newest host frames for the active image session. The renderer uploads
  // these RGBA32F frames to a QRhi texture on the scene-graph thread. This is
  // the CPU presentation path and the OpenCL readback fallback.
  [[nodiscard]] auto DrainPendingHostFrames(std::uint64_t session_epoch,
                                            std::uint64_t image_identity)
      -> std::vector<ViewerFrame>;
  // Drop all pending imports (image switch / renderer teardown).
  void ClearPendingImportedFrames();
  void ClearPendingHostFrames();
  [[nodiscard]] auto HasWritableTargetForNextFrame() const -> bool;
  [[nodiscard]] auto submitted_frame_count() const -> std::uint64_t;
  [[nodiscard]] auto latest_accepted_request_id() const -> std::uint64_t;
  [[nodiscard]] auto ViewState() const -> ViewerViewState;
  void ClearMappedSlot();

 private:
  [[nodiscard]] auto MakeSizeRequestLocked() const -> DirectPresentQueue::SizeRequest;
  auto ReserveWritableSlot(int width, int height) -> std::optional<int>;
  [[nodiscard]] auto IsMetalPresentPath() const -> bool;
  static auto LayerIndexForRole(FrameRole role) -> std::size_t;
  [[nodiscard]] auto AcceptSubmissionRequestId(std::uint64_t request_id) -> bool;

  EditorViewportItem* item_ = nullptr;
  mutable std::mutex mutex_;
  int width_ = 0;
  int height_ = 0;
  std::uint64_t last_sized_session_epoch_ = 0;
  std::uint64_t last_sized_image_identity_ = 0;
  bool has_mapped_slot_ = false;
  bool unmapped_pending_submit_ = false;
  int mapped_slot_index_ = -1;
  int prepared_slot_index_ = -1;
  FrameCompletionSubmission bound_submission_{};
  bool bound_submission_valid_ = false;
  std::uint64_t latest_accepted_request_id_ = 0;
  // Latest pending import per FrameRole (Interactive / Quality / Detail).
  std::array<std::optional<ImportedGpuFrame>, 3> pending_imported_{};
  // Latest pending host upload per FrameRole. The shared pixel owner keeps the
  // worker-produced copy alive until QRhi has copied it into its upload batch.
  std::array<std::optional<ViewerFrame>, 3> pending_host_{};
  std::uint64_t imported_sequence_ = 0;
  ViewerViewState view_state_{};
  std::uint64_t submitted_frame_count_ = 0;
};

}  // namespace alcedo::editor_rhi
