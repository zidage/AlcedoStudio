//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <array>
#include <memory>

#include <QtGui/rhi/qshader.h>
#include <QtWidgets/qrhiwidget.h>

#include "ui/edit_viewer/edit_viewer_surface.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiResourceUpdateBatch;
class QRhiRenderTarget;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace alcedo {

struct ImportedTextureFrame {
  int                   width              = 0;
  int                   height             = 0;
  std::uintptr_t        texture_handle     = 0;
  int                   native_layout      = 0;
  FramePresentationMode presentation_mode  = FramePresentationMode::FullFrame;
  FramePreviewMetadata  preview_metadata   = {};

  explicit operator bool() const {
    return width > 0 && height > 0 && texture_handle != 0;
  }
};

class RhiImageRenderer {
 public:
  RhiImageRenderer() = default;
  ~RhiImageRenderer();

  void initialize(QRhi* rhi, QRhiRenderTarget* render_target, QRhiCommandBuffer* command_buffer);
  void releaseResources();
  void queueFrame(const ViewerFrame& frame);
  void queueImportedFrame(const ImportedTextureFrame& frame,
                          std::shared_ptr<const void> owner = {});
  void releaseImportedTexture(std::uintptr_t texture_handle);
  auto currentRenderState(const ViewerViewState& view_state) const -> EditViewerRenderTargetState;
  void render(QRhiCommandBuffer* command_buffer, QRhiRenderTarget* render_target,
              const ViewerViewState& view_state, const ViewportWidgetInfo& widget_info);

 private:
  enum class LayerId {
    InteractivePrimary,
    QualityBase,
    DetailPatch,
  };

  struct UniformData;
  struct VertexData;
  struct PendingLayerFrame {
    ViewerFrame                       host_frame{};
    std::unique_ptr<ViewerGpuFrameUpload> pending_upload{};
    ImportedTextureFrame             imported_frame{};
    std::shared_ptr<const void>      imported_owner{};
    bool                             has_update = false;
  };
  struct LayerTextureState {
    int                   width              = 0;
    int                   height             = 0;
    FramePresentationMode presentation_mode  = FramePresentationMode::FullFrame;
    FramePreviewMetadata  preview_metadata   = {};
    std::shared_ptr<const void> imported_owner{};
    bool                  valid              = false;
    bool                  source_is_imported = false;
  };

  auto loadShader(const char* resource_path) const -> QShader;
  void destroyResource(QRhiTexture*& resource);
  void destroyResource(QRhiSampler*& resource);
  void destroyResource(QRhiBuffer*& resource);
  void destroyResource(QRhiShaderResourceBindings*& resource);
  void destroyResource(QRhiGraphicsPipeline*& resource);
  auto layerIdForRole(FrameRole role) const -> LayerId;
  auto pendingLayer(LayerId layer) -> PendingLayerFrame&;
  auto pendingLayer(LayerId layer) const -> const PendingLayerFrame&;
  auto layerState(LayerId layer) -> LayerTextureState&;
  auto layerState(LayerId layer) const -> const LayerTextureState&;
  void ensureStaticResources(QRhiRenderTarget* render_target, QRhiCommandBuffer* command_buffer);
  void ensureTexture(QRhiTexture*& texture, int& width, int& height, const QSize& size);
  void ensureImportedTexture(QRhiTexture*& texture, int& width, int& height,
                             quint64& native_object, const ImportedTextureFrame& frame);
  void uploadPendingLayer(LayerId layer, QRhiResourceUpdateBatch* resource_updates,
                          QRhiCommandBuffer* command_buffer);
  auto selectedPrimaryTexture(const ViewerViewState& view_state) const -> QRhiTexture*;
  auto selectedDetailTexture(const ViewerViewState& view_state) const -> QRhiTexture*;
  auto selectedRenderState(const ViewerViewState& view_state) const -> EditViewerRenderTargetState;
  auto hasVisibleDetailPatch(const ViewerViewState& view_state) const -> bool;
  void recreateShaderResources();

  QRhi*                       rhi_                      = nullptr;
  QRhiTexture*                interactive_texture_      = nullptr;
  QRhiTexture*                quality_base_texture_     = nullptr;
  QRhiTexture*                detail_patch_texture_     = nullptr;
  QRhiTexture*                interactive_imported_texture_ = nullptr;
  QRhiTexture*                quality_base_imported_texture_ = nullptr;
  QRhiTexture*                detail_patch_imported_texture_ = nullptr;
  QRhiTexture*                placeholder_texture_      = nullptr;
  QRhiTexture*                bound_primary_texture_    = nullptr;
  QRhiTexture*                bound_detail_texture_     = nullptr;
  QRhiSampler*                primary_sampler_          = nullptr;
  QRhiSampler*                detail_sampler_           = nullptr;
  QRhiBuffer*                 uniform_buffer_           = nullptr;
  QRhiBuffer*                 vertex_buffer_            = nullptr;
  QRhiShaderResourceBindings* shader_resource_bindings_ = nullptr;
  QRhiGraphicsPipeline*       pipeline_                 = nullptr;
  QRhiRenderTarget*           bound_render_target_      = nullptr;
  int                         interactive_texture_width_ = 0;
  int                         interactive_texture_height_ = 0;
  int                         quality_base_texture_width_ = 0;
  int                         quality_base_texture_height_ = 0;
  int                         detail_patch_texture_width_ = 0;
  int                         detail_patch_texture_height_ = 0;
  int                         interactive_imported_width_ = 0;
  int                         interactive_imported_height_ = 0;
  int                         quality_base_imported_width_ = 0;
  int                         quality_base_imported_height_ = 0;
  int                         detail_patch_imported_width_ = 0;
  int                         detail_patch_imported_height_ = 0;
  quint64                     interactive_imported_native_object_ = 0;
  quint64                     quality_base_imported_native_object_ = 0;
  quint64                     detail_patch_imported_native_object_ = 0;
  bool                        static_upload_pending_     = false;
  std::array<PendingLayerFrame, 3> pending_layers_{};
  std::array<LayerTextureState, 3> layer_states_{};
};

class RhiEditViewerSurface final : public QRhiWidget,
                                   public IEditViewerSurface,
                                   public IEditViewerRenderTargetSurface {
 public:
  explicit RhiEditViewerSurface(QWidget* parent = nullptr);
  ~RhiEditViewerSurface() override;

  auto widget() -> QWidget* override;
  void submitFrame(const ViewerFrame& frame) override;
#ifdef HAVE_METAL
  void submitMetalFrame(const ViewerMetalFrame& frame) override;
#endif
  void setDisplayConfig(const ViewerDisplayConfig& config) override;
  void setViewState(const ViewerViewState& state) override;
  void requestRedraw() override;

  auto supportsDirectGpuPresent() const -> bool override;
  auto supportsDirectCudaPresent() const -> bool override;
  auto prepareRenderTarget(int width, int height) -> EditViewerRenderTargetResizeDecision override;
  void commitRenderTargetResize(int slot_index, int width, int height) override;
  auto mapResourceForWrite(
      FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice) -> FrameWriteMapping override;
  void unmapResource() override;
  void notifyFrameReady() override;
  void setNextFramePresentationMode(FramePresentationMode mode) override;
  void setNextFramePreviewMetadata(const FramePreviewMetadata& metadata) override;
  auto activeRenderTargetState() const -> EditViewerRenderTargetState override;
  auto hasRenderTarget(int slot_index, int width, int height) const -> bool override;
  auto ensureRenderTarget(int slot_index, int width, int height) -> bool override;

 protected:
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void render(QRhiCommandBuffer* command_buffer) override;
  void releaseResources() override;

 private:
 public:
  struct PlatformState;

 private:
  void applyDisplayConfig();
  void releasePlatformTargets();

  RhiImageRenderer               renderer_{};
  ViewerViewState                view_state_{};
  ViewerDisplayConfig            display_config_{};
  bool                           display_config_dirty_ = true;
  std::unique_ptr<PlatformState> platform_state_{};
};

}  // namespace alcedo
