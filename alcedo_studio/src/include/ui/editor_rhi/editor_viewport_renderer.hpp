//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QtGui/rhi/qrhi.h>

#include <QQuickRhiItem>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "ui/editor_rhi/viewport_composite_pass.hpp"
#include "ui/editor_rhi/viewport_mask_coverage_layer.hpp"
#include "ui/viewer/viewer_view_state.hpp"

namespace alcedo::editor_rhi {

class EditorViewportItem;

// Scene-graph render-thread half of the production viewport. Owns QRhi wrappers
// and native target adapters. The fixed three-slot DirectPresentQueue is shared
// with the producer (DirectFrameSink) and holds ownership state only.
class EditorViewportRenderer final : public QQuickRhiItemRenderer {
 public:
  EditorViewportRenderer();
  ~EditorViewportRenderer() override;

  /// Shared slot published by EditorViewportItem so a render-thread
  /// presentation opportunity can reach this renderer without the GUI thread.
  /// The destructor clears the slot with CAS when it still points at `this`.
  void SetConsumeArmSlot(std::shared_ptr<std::atomic<EditorViewportRenderer*>> slot);
  /// Arm the owning node's render-pending flag so the current render pass
  /// consumes a Ready frame. Render thread only (inside beforeRendering or
  /// the node's render path).
  void ArmForPresent();

 protected:
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void synchronize(QQuickRhiItem* item) override;
  void render(QRhiCommandBuffer* command_buffer) override;

 public:
  enum class LayerId : std::uint8_t {
    InteractivePrimary = 0,
    QualityBase,
    DetailPatch,
  };

  struct LayerState {
    QRhiTexture*                   texture    = nullptr;
    int                            width      = 0;
    int                            height     = 0;
    bool                           imported   = false;
    bool                           valid      = false;
    int                            slot_index = -1;
    // Keeps Metal (or other producer-owned) textures alive until QRhi release.
    std::shared_ptr<const void>    imported_owner{};
    std::uintptr_t                 imported_native_handle = 0;
    FramePresentationMode          presentation_mode      = FramePresentationMode::FullFrame;
    FramePreviewMetadata           preview_metadata{};
    // ResolveRenderGeometry result of the frame; maps coverage to this layer (SM8.6).
    ResolvedRenderGeometry         geometry{};
    DirectPresentQueue::ReadyFrame ready_frame{};
  };

 private:
  [[nodiscard]] auto layerIndex(LayerId layer) const -> std::size_t {
    return static_cast<std::size_t>(layer);
  }
  [[nodiscard]] auto layerForRole(FrameRole role) const -> LayerId;
  void               destroyResource(QRhiTexture*& resource);
  void               releaseLayer(LayerState& layer);
  void               releaseResources();
  void               releaseQueuedNatives();
  void fulfillTargetRequests();
  void consumeDirectFrames();
  void consumeImportedGpuFrames();
  [[nodiscard]] auto selectedPrimaryLayer() const -> const LayerState*;
  [[nodiscard]] auto selectedDetailLayer() const -> const LayerState*;
  [[nodiscard]] auto hasVisibleDetailPatch() const -> bool;
  // base: QualityBase preferred, else full-frame InteractivePrimary.
  [[nodiscard]] auto detailPatchAspectOk(const LayerState& detail, const LayerState& base) const
      -> bool;
  void traceDetailDecision(const char* decision, const LayerState* detail, const LayerState* base,
                           const std::optional<FrameRoiRect>& current_roi) const;
  void publishDiagnosticsIfChanged();

  EditorViewportItem*                  item_ = nullptr;
  std::shared_ptr<DirectPresentQueue>  present_queue_;
  // Coverage presentation channel (SM8.5): the viewport coverage input of the composite. Its
  // frames never enter the photo layers.
  std::shared_ptr<DirectPresentQueue>                   coverage_queue_;
  ViewportMaskCoverageLayer                             coverage_layer_;
  // Coverage of the detail ROI render (Mask ranges D10), composited inside the DetailPatch ROI.
  std::shared_ptr<DirectPresentQueue>                   detail_coverage_queue_;
  ViewportMaskCoverageLayer                             detail_coverage_layer_;
  // The one display composite (SM8.6): photo base/detail selection and the coverage tint.
  ViewportCompositePass                                 composite_;
  // Copied from EditorViewportItem::maskCoverageStyle in synchronize().
  MaskCoverageDisplayStyle                              coverage_style_{};
  std::shared_ptr<std::atomic<EditorViewportRenderer*>> consume_arm_slot_;
  std::unique_ptr<ILeaseTargetAdapter> adapter_;
  // Maps adapter_cookie (native_handle) -> last WritableTargetLease for destroy.
  std::vector<WritableTargetLease>     owned_natives_;
  ViewerViewState                      view_state_{};
  std::array<LayerState, 3>            layers_{};
  EditorBackend                        backend_                  = EditorBackend::Cuda;
  std::uint64_t                        target_generation_        = 0;
  std::uint64_t                        session_epoch_            = 0;
  std::uint64_t                        image_identity_           = 0;
  QRhi*                                rhi_                      = nullptr;
  bool                                 content_dirty_            = false;
  std::string                          target_error_;
  // ROI tracing is transition-based so a rejected patch does not print
  // once per scene-graph frame and create a second performance problem.
  mutable std::string                  last_detail_trace_decision_;
  mutable std::uint64_t                last_detail_trace_request_id_ = 0;
  mutable bool                         last_detail_trace_has_roi_    = false;
  mutable FrameRoiRect                 last_detail_trace_roi_{};
  mutable float                        last_detail_trace_zoom_  = 0.0f;
  mutable float                        last_detail_trace_pan_x_ = 0.0f;
  mutable float                        last_detail_trace_pan_y_ = 0.0f;
};

}  // namespace alcedo::editor_rhi
