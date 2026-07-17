//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QQuickRhiItem>
#include <QtGui/rhi/qrhi.h>
#include <QtGui/rhi/qshader.h>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>

#include "ui/editor_rhi/frame_presentation_broker.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/viewer/viewer_view_state.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo::editor_rhi {

class EditorViewportItem;

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
    QRhiTexture* texture = nullptr;
    int width = 0;
    int height = 0;
    bool imported = false;
    bool valid = false;
    FramePresentationMode presentation_mode = FramePresentationMode::FullFrame;
    FramePreviewMetadata preview_metadata{};
    CompletedFrameLease direct_frame{};
  };

  struct UniformData {
    float scale_zoom[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    float pan_mode[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float detail_roi[4] = {0.0f, 0.0f, 1.0f, 1.0f};
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
  [[nodiscard]] auto layerForLease(LeaseFrameLayer layer) const -> LayerId;
  [[nodiscard]] auto loadShader(const char* path) const -> QShader;
  void destroyResource(QRhiTexture*& resource);
  void destroyResource(QRhiBuffer*& resource);
  void destroyResource(QRhiSampler*& resource);
  void destroyResource(QRhiShaderResourceBindings*& resource);
  void destroyResource(QRhiGraphicsPipeline*& resource);
  void releaseLayer(LayerState& layer);
  void releaseResources();
  void releaseBrokerTargets();
  void ensureStaticResources(QRhiRenderTarget* render_target,
                             QRhiCommandBuffer* command_buffer);
  void ensureTargetPool(const QSize& size);
  void consumeDirectFrames();
  void consumeHostFrames(QRhiResourceUpdateBatch* updates);
  [[nodiscard]] auto selectedPrimaryLayer() const -> const LayerState*;
  [[nodiscard]] auto selectedDetailLayer() const -> const LayerState*;
  [[nodiscard]] auto hasVisibleDetailPatch() const -> bool;
  void recreateShaderResources(QRhiTexture* primary, QRhiTexture* detail);

  EditorViewportItem* item_ = nullptr;
  std::shared_ptr<FramePresentationBroker> broker_;
  std::unique_ptr<ILeaseTargetAdapter> adapter_;
  ViewerViewState view_state_{};
  std::array<LayerState, 3> layers_{};
  std::deque<ViewerFrame> host_frames_;
  EditorBackend backend_ = EditorBackend::Cuda;
  std::uint64_t target_generation_ = 0;
  std::uint64_t image_generation_ = 0;
  QSize target_size_{};
  QRhi* rhi_ = nullptr;
  QRhiRenderTarget* render_target_ = nullptr;

  QRhiTexture* placeholder_texture_ = nullptr;
  QRhiSampler* primary_sampler_ = nullptr;
  QRhiSampler* detail_sampler_ = nullptr;
  QRhiBuffer* uniform_buffer_ = nullptr;
  QRhiBuffer* vertex_buffer_ = nullptr;
  QRhiShaderResourceBindings* shader_resource_bindings_ = nullptr;
  QRhiGraphicsPipeline* pipeline_ = nullptr;
  QRhiTexture* bound_primary_texture_ = nullptr;
  QRhiTexture* bound_detail_texture_ = nullptr;
  bool static_upload_pending_ = false;
};

}  // namespace alcedo::editor_rhi
