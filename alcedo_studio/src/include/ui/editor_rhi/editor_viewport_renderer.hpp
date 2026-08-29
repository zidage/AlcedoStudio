//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include <QQuickRhiItem>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/editor_rhi/direct_present_queue.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
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
    DirectPresentQueue::ReadyFrame ready_frame{};
  };

  struct UniformData {
    float scale_zoom[4]   = {1.0f, 1.0f, 1.0f, 0.0f};
    float pan_mode[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
    float detail_roi[4]   = {0.0f, 0.0f, 1.0f, 1.0f};
    float detail_flags[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  };

  struct VertexData {
    float position[2];
    float uv[2];
  };

 private:
  [[nodiscard]] auto layerIndex(LayerId layer) const -> std::size_t {
    return static_cast<std::size_t>(layer);
  }
  [[nodiscard]] auto layerForRole(FrameRole role) const -> LayerId;
  [[nodiscard]] auto loadShader(const char* path) const -> QShader;
  void               destroyResource(QRhiTexture*& resource);
  void               destroyResource(QRhiBuffer*& resource);
  void               destroyResource(QRhiSampler*& resource);
  void               destroyResource(QRhiShaderResourceBindings*& resource);
  void               destroyResource(QRhiGraphicsPipeline*& resource);
  void               releaseLayer(LayerState& layer);
  void               releaseResources();
  void               releaseQueuedNatives();
  void ensureStaticResources(QRhiRenderTarget* render_target, QRhiCommandBuffer* command_buffer);
  void fulfillTargetRequests();
  void consumeDirectFrames();
  void consumeHostFrames(QRhiResourceUpdateBatch* updates);
  void consumeImportedGpuFrames();
  [[nodiscard]] auto selectedPrimaryLayer() const -> const LayerState*;
  [[nodiscard]] auto selectedDetailLayer() const -> const LayerState*;
  [[nodiscard]] auto hasVisibleDetailPatch() const -> bool;
  // base: QualityBase preferred, else full-frame InteractivePrimary.
  [[nodiscard]] auto detailPatchAspectOk(const LayerState& detail, const LayerState& base) const
      -> bool;
  void traceDetailDecision(const char* decision, const LayerState* detail, const LayerState* base,
                           const std::optional<FrameRoiRect>& current_roi) const;
  void recreateShaderResources(QRhiTexture* primary, QRhiTexture* detail);
  void publishDiagnosticsIfChanged();

  EditorViewportItem*                  item_ = nullptr;
  std::shared_ptr<DirectPresentQueue>  present_queue_;
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
  QRhiRenderTarget*                    render_target_            = nullptr;

  QRhiTexture*                         placeholder_texture_      = nullptr;
  QRhiSampler*                         primary_sampler_          = nullptr;
  QRhiSampler*                         detail_sampler_           = nullptr;
  QRhiBuffer*                          uniform_buffer_           = nullptr;
  QRhiBuffer*                          vertex_buffer_            = nullptr;
  QRhiShaderResourceBindings*          shader_resource_bindings_ = nullptr;
  QRhiGraphicsPipeline*                pipeline_                 = nullptr;
  QRhiTexture*                         bound_primary_texture_    = nullptr;
  QRhiTexture*                         bound_detail_texture_     = nullptr;
  bool                                 static_upload_pending_    = false;
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
