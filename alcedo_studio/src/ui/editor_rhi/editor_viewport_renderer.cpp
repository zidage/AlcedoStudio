//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_viewport_renderer.hpp"

#include "ui/editor_rhi/editor_viewport_item.hpp"

#include <QFile>
#include <QSize>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "ui/editor_rhi/frame_presentation_broker.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"
#include "ui/edit_viewer/viewport_mapper.hpp"

namespace alcedo::editor_rhi {
namespace {

constexpr const char* kVertexShaderResource = ":/shaders/editor_rhi/editor_viewport.vert.qsb";
constexpr const char* kFragmentShaderResource = ":/shaders/editor_rhi/editor_viewport.frag.qsb";
constexpr int kTargetPoolSize = 3;

auto BackendForRhi(QRhi* rhi) -> EditorBackend {
  if (!rhi) {
    return EditorBackend::Cuda;
  }
  switch (rhi->backend()) {
    case QRhi::OpenGLES2:
      return EditorBackend::OpenCl;
    case QRhi::Metal:
      return EditorBackend::Metal;
    case QRhi::D3D11:
    default:
      return EditorBackend::Cuda;
  }
}

auto LayerForFrameRole(FrameRole role) -> EditorViewportRenderer::LayerId {
  switch (role) {
    case FrameRole::InteractivePrimary:
      return EditorViewportRenderer::LayerId::InteractivePrimary;
    case FrameRole::QualityBase:
      return EditorViewportRenderer::LayerId::QualityBase;
    case FrameRole::DetailPatch:
      return EditorViewportRenderer::LayerId::DetailPatch;
  }
  return EditorViewportRenderer::LayerId::InteractivePrimary;
}

auto LeaseLayerForFrameRole(FrameRole role) -> LeaseFrameLayer {
  switch (role) {
    case FrameRole::InteractivePrimary:
      return LeaseFrameLayer::InteractivePrimary;
    case FrameRole::QualityBase:
      return LeaseFrameLayer::QualityBase;
    case FrameRole::DetailPatch:
      return LeaseFrameLayer::DetailPatch;
  }
  return LeaseFrameLayer::InteractivePrimary;
}

auto FrameMetadata(const CompletedFrameLease& frame) -> FramePreviewMetadata {
  FramePreviewMetadata metadata{};
  switch (frame.layer) {
    case LeaseFrameLayer::InteractivePrimary:
      metadata.frame_role = FrameRole::InteractivePrimary;
      break;
    case LeaseFrameLayer::QualityBase:
      metadata.frame_role = FrameRole::QualityBase;
      break;
    case LeaseFrameLayer::DetailPatch:
      metadata.frame_role = FrameRole::DetailPatch;
      break;
  }
  metadata.preview_generation = frame.preview_generation;
  metadata.detail_serial = frame.detail_serial;
  metadata.source_roi_norm = {frame.roi_x, frame.roi_y, frame.roi_width, frame.roi_height};
  return metadata;
}

auto BuildNormalizedRoi(const std::optional<ViewportRenderRegion>& region)
    -> std::optional<FrameRoiRect> {
  if (!region.has_value() || region->reference_width_ <= 0 ||
      region->reference_height_ <= 0) {
    return std::nullopt;
  }
  return FrameRoiRect{
      std::clamp(static_cast<float>(region->x_) /
                     static_cast<float>(region->reference_width_),
                 0.0f, 1.0f),
      std::clamp(static_cast<float>(region->y_) /
                     static_cast<float>(region->reference_height_),
                 0.0f, 1.0f),
      std::clamp(region->scale_x_, 1.0e-4f, 1.0f),
      std::clamp(region->scale_y_, 1.0e-4f, 1.0f),
  };
}

auto SameRoi(const FrameRoiRect& lhs, const FrameRoiRect& rhs) -> bool {
  constexpr float kEpsilon = 1.0e-4f;
  return std::abs(lhs.x - rhs.x) <= kEpsilon && std::abs(lhs.y - rhs.y) <= kEpsilon &&
         std::abs(lhs.width - rhs.width) <= kEpsilon &&
         std::abs(lhs.height - rhs.height) <= kEpsilon;
}

template <typename T>
void DestroyRhi(T*& resource) {
  if (!resource) {
    return;
  }
  resource->destroy();
  delete resource;
  resource = nullptr;
}

}  // namespace

EditorViewportRenderer::EditorViewportRenderer() = default;

EditorViewportRenderer::~EditorViewportRenderer() {
  releaseResources();
  if (broker_) {
    broker_->Shutdown();
  }
  releaseBrokerTargets();
  adapter_.reset();
}

auto EditorViewportRenderer::layerForLease(LeaseFrameLayer layer) const -> LayerId {
  switch (layer) {
    case LeaseFrameLayer::InteractivePrimary:
      return LayerId::InteractivePrimary;
    case LeaseFrameLayer::QualityBase:
      return LayerId::QualityBase;
    case LeaseFrameLayer::DetailPatch:
      return LayerId::DetailPatch;
  }
  return LayerId::InteractivePrimary;
}

auto EditorViewportRenderer::loadShader(const char* path) const -> QShader {
  QFile file(QString::fromUtf8(path));
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

void EditorViewportRenderer::destroyResource(QRhiTexture*& resource) {
  DestroyRhi(resource);
}

void EditorViewportRenderer::destroyResource(QRhiBuffer*& resource) {
  DestroyRhi(resource);
}

void EditorViewportRenderer::destroyResource(QRhiSampler*& resource) {
  DestroyRhi(resource);
}

void EditorViewportRenderer::destroyResource(QRhiShaderResourceBindings*& resource) {
  DestroyRhi(resource);
}

void EditorViewportRenderer::destroyResource(QRhiGraphicsPipeline*& resource) {
  DestroyRhi(resource);
}

void EditorViewportRenderer::releaseLayer(LayerState& layer) {
  // QRhi must drop its imported wrapper before the adapter can release the
  // underlying native object. This ordering is the central lifetime rule of
  // the phase-2 broker boundary.
  destroyResource(layer.texture);
  if (layer.imported) {
    NativeResourceCounters::Instance().OnDestroyImportedQRhiTexture();
  }
  if (layer.direct_frame.valid() && broker_) {
    broker_->CompleteRendererConsumption(layer.direct_frame);
  }
  layer = {};
}

void EditorViewportRenderer::releaseBrokerTargets() {
  if (!broker_) {
    return;
  }
  const auto released = broker_->DrainReleasedTargets();
  if (!adapter_) {
    return;
  }
  for (const auto& lease : released) {
    adapter_->DestroyTarget(lease);
  }
}

void EditorViewportRenderer::releaseResources() {
  for (auto& layer : layers_) {
    releaseLayer(layer);
  }
  destroyResource(pipeline_);
  destroyResource(shader_resource_bindings_);
  destroyResource(vertex_buffer_);
  destroyResource(uniform_buffer_);
  destroyResource(primary_sampler_);
  destroyResource(detail_sampler_);
  destroyResource(placeholder_texture_);
  bound_primary_texture_ = nullptr;
  bound_detail_texture_ = nullptr;
  static_upload_pending_ = false;
  target_size_ = {};
  target_generation_ = 0;
  image_generation_ = 0;
  if (broker_) {
    broker_->InvalidateTargetGeneration();
  }
  releaseBrokerTargets();
  render_target_ = nullptr;
}

void EditorViewportRenderer::initialize(QRhiCommandBuffer* /*command_buffer*/) {
  QRhi* next_rhi = rhi();
  if (!next_rhi) {
    return;
  }
  releaseResources();
  rhi_ = next_rhi;
  backend_ = BackendForRhi(rhi_);
  adapter_ = MakeLeaseTargetAdapter(backend_);
  if (item_) {
    item_->setBackendName(QString::fromUtf8(ToString(backend_)));
    item_->setStatusText(QStringLiteral("render thread initialized (%1)")
                             .arg(QString::fromUtf8(QtGraphicsApiName(backend_))));
  }
}

void EditorViewportRenderer::synchronize(QQuickRhiItem* item) {
  item_ = qobject_cast<EditorViewportItem*>(item);
  if (!item_) {
    return;
  }
  if (broker_ != item_->broker()) {
    broker_ = item_->broker();
    image_generation_ = 0;
    target_generation_ = 0;
    target_size_ = {};
  }

  view_state_ = item_->viewStateSnapshot();
  auto pending = item_->takePendingFrames();
  for (auto& frame : pending) {
    host_frames_.push_back(std::move(frame));
  }
  while (host_frames_.size() > 8) {
    host_frames_.pop_front();
  }

  const auto next_image_generation = item_->imageGeneration();
  if (next_image_generation != image_generation_) {
    for (auto& layer : layers_) {
      releaseLayer(layer);
    }
    if (broker_) {
      broker_->InvalidateImageGeneration(next_image_generation);
    }
    releaseBrokerTargets();
    image_generation_ = next_image_generation;
    target_size_ = {};
  }
}

void EditorViewportRenderer::ensureStaticResources(QRhiRenderTarget* render_target,
                                                   QRhiCommandBuffer* command_buffer) {
  if (!rhi_ || !render_target) {
    return;
  }
  if (render_target_ != render_target) {
    destroyResource(pipeline_);
    render_target_ = render_target;
  }

  if (!placeholder_texture_) {
    placeholder_texture_ = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(1, 1), 1);
    if (!placeholder_texture_->create()) {
      destroyResource(placeholder_texture_);
    } else {
      static_upload_pending_ = true;
    }
  }
  if (!primary_sampler_) {
    primary_sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                        QRhiSampler::None, QRhiSampler::ClampToEdge,
                                        QRhiSampler::ClampToEdge);
    primary_sampler_->create();
  }
  if (!detail_sampler_) {
    detail_sampler_ = rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                        QRhiSampler::None, QRhiSampler::ClampToEdge,
                                        QRhiSampler::ClampToEdge);
    detail_sampler_->create();
  }
  if (!uniform_buffer_) {
    uniform_buffer_ =
        rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformData));
    uniform_buffer_->create();
  }
  if (!vertex_buffer_) {
    vertex_buffer_ =
        rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 4 * sizeof(VertexData));
    vertex_buffer_->create();
    static_upload_pending_ = true;
  }

  if (command_buffer && static_upload_pending_ && vertex_buffer_ && placeholder_texture_) {
    static constexpr std::array<VertexData, 4> vertices = {
        VertexData{{-1.0f, -1.0f}, {0.0f, 1.0f}},
        VertexData{{1.0f, -1.0f}, {1.0f, 1.0f}},
        VertexData{{-1.0f, 1.0f}, {0.0f, 0.0f}},
        VertexData{{1.0f, 1.0f}, {1.0f, 0.0f}},
    };
    static constexpr std::array<float, 4> black = {0.0f, 0.0f, 0.0f, 1.0f};
    auto* updates = rhi_->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertex_buffer_, vertices.data());
    QByteArray data(reinterpret_cast<const char*>(black.data()), static_cast<int>(sizeof(black)));
    QRhiTextureSubresourceUploadDescription description(data);
    description.setDataStride(static_cast<quint32>(sizeof(black)));
    description.setSourceSize(QSize(1, 1));
    updates->uploadTexture(
        placeholder_texture_, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, description)));
    command_buffer->resourceUpdate(updates);
    static_upload_pending_ = false;
  }
}

void EditorViewportRenderer::ensureTargetPool(const QSize& size) {
  if (!broker_ || !adapter_ || image_generation_ == 0 || size.width() <= 0 ||
      size.height() <= 0) {
    return;
  }
  const auto diagnostics = broker_->DiagnosticsSnapshot();
  if (target_size_ == size && diagnostics.live_target_count > 0 &&
      diagnostics.target_generation == target_generation_ &&
      diagnostics.consumer_available) {
    return;
  }

  for (auto& layer : layers_) {
    releaseLayer(layer);
  }
  broker_->InvalidateTargetGeneration();
  releaseBrokerTargets();
  target_generation_ = broker_->CurrentTargetGeneration();
  if (target_generation_ == 0) {
    broker_->InvalidateTargetGeneration();
    target_generation_ = broker_->CurrentTargetGeneration();
  }

  for (int i = 0; i < kTargetPoolSize; ++i) {
    auto lease = adapter_->CreateTarget(
        rhi_, size, TargetGeneration{target_generation_, image_generation_, 0});
    if (!lease.has_value()) {
      if (item_) {
        item_->setStatusText(QString::fromStdString(adapter_->lastError()));
      }
      break;
    }
    if (!broker_->PublishWritableTarget(*lease)) {
      adapter_->DestroyTarget(*lease);
    }
  }
  target_size_ = size;
  if (item_) {
    item_->notifyDiagnosticsChanged();
  }
}

void EditorViewportRenderer::consumeDirectFrames() {
  if (!broker_ || !rhi_) {
    return;
  }
  const auto diagnostics = broker_->DiagnosticsSnapshot();
  const TargetGeneration expected{diagnostics.target_generation, image_generation_, 0};
  constexpr LeaseFrameLayer layers[] = {
      LeaseFrameLayer::InteractivePrimary, LeaseFrameLayer::QualityBase,
      LeaseFrameLayer::DetailPatch};
  for (const auto layer_id : layers) {
    const auto frame = broker_->ConsumeNewestCompletedFrame(expected, layer_id);
    if (!frame.has_value()) {
      continue;
    }
    auto& layer = layers_[layerIndex(layerForLease(layer_id))];
    releaseLayer(layer);
    auto* texture = rhi_->newTexture(
        QRhiTexture::RGBA32F,
        QSize(frame->target.dimensions.width, frame->target.dimensions.height), 1);
    if (!texture || !texture->createFrom({static_cast<quint64>(frame->target.native_handle), 0})) {
      destroyResource(texture);
      broker_->CompleteRendererConsumption(*frame);
      if (item_) {
        item_->setStatusText(QStringLiteral("failed to import a completed native frame"));
      }
      continue;
    }
    layer.texture = texture;
    layer.width = frame->target.dimensions.width;
    layer.height = frame->target.dimensions.height;
    layer.imported = true;
    layer.valid = true;
    NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
    layer.presentation_mode = FramePresentationMode::FullFrame;
    layer.preview_metadata = FrameMetadata(*frame);
    layer.direct_frame = *frame;
  }
}

void EditorViewportRenderer::consumeHostFrames(QRhiResourceUpdateBatch* updates) {
  if (!updates || !rhi_) {
    return;
  }
  while (!host_frames_.empty()) {
    ViewerFrame frame = std::move(host_frames_.front());
    host_frames_.pop_front();
    if (!frame) {
      continue;
    }
    auto& layer = layers_[layerIndex(LayerForFrameRole(frame.preview_metadata.frame_role))];
    if (layer.valid &&
        layer.preview_metadata.preview_generation > frame.preview_metadata.preview_generation) {
      continue;
    }
    releaseLayer(layer);
    const QSize size(frame.width, frame.height);
    layer.texture = rhi_->newTexture(QRhiTexture::RGBA32F, size, 1);
    if (!layer.texture || !layer.texture->create()) {
      destroyResource(layer.texture);
      continue;
    }
    const auto bytes = frame.row_bytes * static_cast<size_t>(frame.height);
    if (bytes > static_cast<size_t>((std::numeric_limits<int>::max)())) {
      releaseLayer(layer);
      continue;
    }
    const QByteArray data =
        QByteArray::fromRawData(static_cast<const char*>(frame.pixels.get()),
                                static_cast<int>(bytes));
    QRhiTextureSubresourceUploadDescription description(data);
    description.setDataStride(static_cast<quint32>(frame.row_bytes));
    description.setSourceSize(size);
    updates->uploadTexture(
        layer.texture, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, description)));
    layer.width = frame.width;
    layer.height = frame.height;
    layer.imported = false;
    layer.valid = true;
    layer.presentation_mode = frame.presentation_mode;
    layer.preview_metadata = frame.preview_metadata;
  }
}

auto EditorViewportRenderer::selectedPrimaryLayer() const -> const LayerState* {
  const auto& interactive = layers_[layerIndex(LayerId::InteractivePrimary)];
  const auto& quality = layers_[layerIndex(LayerId::QualityBase)];
  if (!interactive.valid && !quality.valid) {
    return nullptr;
  }
  if (!quality.valid) {
    return &interactive;
  }
  if (!interactive.valid) {
    return &quality;
  }
  if (interactive.preview_metadata.preview_generation >
      quality.preview_metadata.preview_generation) {
    return &interactive;
  }
  if (interactive.preview_metadata.preview_generation ==
          quality.preview_metadata.preview_generation &&
      view_state_.prefer_interactive_primary) {
    return &interactive;
  }
  return &quality;
}

auto EditorViewportRenderer::hasVisibleDetailPatch() const -> bool {
  if (!view_state_.allow_detail_patch || !view_state_.has_expected_detail_token) {
    return false;
  }
  const auto& quality = layers_[layerIndex(LayerId::QualityBase)];
  const auto& detail = layers_[layerIndex(LayerId::DetailPatch)];
  if (!quality.valid || !detail.valid ||
      quality.preview_metadata.preview_generation !=
          detail.preview_metadata.preview_generation ||
      detail.preview_metadata.preview_generation != view_state_.expected_detail_generation ||
      detail.preview_metadata.detail_serial != view_state_.expected_detail_serial) {
    return false;
  }
  const auto current_roi = BuildNormalizedRoi(view_state_.snapshot.viewport_render_region_cache);
  return current_roi.has_value() &&
         SameRoi(detail.preview_metadata.source_roi_norm, *current_roi);
}

auto EditorViewportRenderer::selectedDetailLayer() const -> const LayerState* {
  if (!hasVisibleDetailPatch()) {
    return nullptr;
  }
  return &layers_[layerIndex(LayerId::DetailPatch)];
}

void EditorViewportRenderer::recreateShaderResources(QRhiTexture* primary,
                                                     QRhiTexture* detail) {
  destroyResource(shader_resource_bindings_);
  destroyResource(pipeline_);
  if (!rhi_ || !uniform_buffer_ || !primary_sampler_ || !detail_sampler_ || !primary ||
      !detail) {
    bound_primary_texture_ = nullptr;
    bound_detail_texture_ = nullptr;
    return;
  }
  shader_resource_bindings_ = rhi_->newShaderResourceBindings();
  shader_resource_bindings_->setBindings(
      {QRhiShaderResourceBinding::uniformBuffer(
           0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
           uniform_buffer_),
       QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                 primary, primary_sampler_),
       QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                 detail, detail_sampler_)});
  if (!shader_resource_bindings_->create()) {
    destroyResource(shader_resource_bindings_);
    return;
  }
  bound_primary_texture_ = primary;
  bound_detail_texture_ = detail;
}

void EditorViewportRenderer::render(QRhiCommandBuffer* command_buffer) {
  QRhiRenderTarget* render_target = renderTarget();
  if (!command_buffer || !render_target || !rhi_ || !item_) {
    return;
  }

  ensureTargetPool(render_target->pixelSize());
  consumeDirectFrames();
  ensureStaticResources(render_target, command_buffer);
  auto* updates = rhi_->nextResourceUpdateBatch();
  consumeHostFrames(updates);

  const auto* primary_layer = selectedPrimaryLayer();
  const auto* detail_layer = selectedDetailLayer();
  QRhiTexture* primary = primary_layer ? primary_layer->texture : placeholder_texture_;
  QRhiTexture* detail = detail_layer ? detail_layer->texture : placeholder_texture_;
  if (primary != bound_primary_texture_ || detail != bound_detail_texture_ ||
      !shader_resource_bindings_) {
    recreateShaderResources(primary, detail);
  }

  if (!pipeline_ && shader_resource_bindings_) {
    pipeline_ = rhi_->newGraphicsPipeline();
    QRhiVertexInputLayout layout;
    layout.setBindings({QRhiVertexInputBinding(sizeof(VertexData))});
    layout.setAttributes(
        {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
         QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2,
                                  2 * sizeof(float))});
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setCullMode(QRhiGraphicsPipeline::None);
    pipeline_->setSampleCount(render_target->sampleCount());
    pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, loadShader(kVertexShaderResource)),
         QRhiShaderStage(QRhiShaderStage::Fragment, loadShader(kFragmentShaderResource))});
    pipeline_->setVertexInputLayout(layout);
    pipeline_->setShaderResourceBindings(shader_resource_bindings_);
    pipeline_->setRenderPassDescriptor(render_target->renderPassDescriptor());
    if (!pipeline_->create()) {
      destroyResource(pipeline_);
    }
  }

  UniformData uniform{};
  if (primary_layer) {
    const auto scale = ViewportMapper::ComputeLetterboxScale(
        ViewportWidgetInfo{render_target->pixelSize().width(), render_target->pixelSize().height(),
                           1.0f},
        ViewportImageInfo{primary_layer->width, primary_layer->height});
    uniform.scale_zoom[0] = scale.x;
    uniform.scale_zoom[1] = scale.y;
    uniform.scale_zoom[2] = view_state_.snapshot.view_transform.zoom;
    uniform.pan_mode[0] = view_state_.snapshot.view_transform.pan.x();
    uniform.pan_mode[1] = view_state_.snapshot.view_transform.pan.y();
    if (primary_layer->presentation_mode == FramePresentationMode::RoiFrame) {
      uniform.scale_zoom[2] = 1.0f;
      uniform.pan_mode[0] = 0.0f;
      uniform.pan_mode[1] = 0.0f;
      uniform.pan_mode[2] = 1.0f;
    }
  }
  if (detail_layer) {
    const auto& roi = detail_layer->preview_metadata.source_roi_norm;
    uniform.detail_roi[0] = roi.x;
    uniform.detail_roi[1] = roi.y;
    uniform.detail_roi[2] = roi.width;
    uniform.detail_roi[3] = roi.height;
    uniform.detail_flags[0] = 1.0f;
  }
  if (uniform_buffer_) {
    updates->updateDynamicBuffer(uniform_buffer_, 0, sizeof(uniform), &uniform);
  }

  command_buffer->beginPass(render_target, Qt::black, {1.0f, 0}, updates);
  if (primary_layer && pipeline_ && shader_resource_bindings_ && vertex_buffer_) {
    const QRhiCommandBuffer::VertexInput vertex_input[] = {{vertex_buffer_, 0}};
    const QSize size = render_target->pixelSize();
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
    command_buffer->setShaderResources(shader_resource_bindings_);
    command_buffer->setVertexInput(0, 1, vertex_input);
    command_buffer->draw(4);
  }
  command_buffer->endPass();

  if (primary_layer) {
    item_->setStatusText(QStringLiteral("presented"));
  } else {
    item_->setStatusText(QStringLiteral("waiting for a compatible frame"));
  }
  item_->notifyDiagnosticsChanged();
  update();
}

}  // namespace alcedo::editor_rhi
