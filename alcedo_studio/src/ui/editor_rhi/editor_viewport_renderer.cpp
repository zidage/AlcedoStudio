//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_viewport_renderer.hpp"

#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

#include <QDebug>
#include <QFile>
#include <QSize>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "ui/edit_viewer/viewport_mapper.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"

namespace alcedo::editor_rhi {
namespace {

constexpr const char* kVertexShaderResource = ":/shaders/editor_rhi/editor_viewport.vert.qsb";
constexpr const char* kFragmentShaderResource = ":/shaders/editor_rhi/editor_viewport.frag.qsb";

auto BackendForRhi(QRhi* rhi) -> EditorBackend {
  if (!rhi) {
    return ActiveEditorBackend();
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

auto BuildNormalizedRoi(const std::optional<ViewportRenderRegion>& region)
    -> std::optional<FrameRoiRect> {
  if (!region.has_value() || region->reference_width_ <= 0 || region->reference_height_ <= 0) {
    return std::nullopt;
  }
  return FrameRoiRect{
      std::clamp(static_cast<float>(region->x_) / static_cast<float>(region->reference_width_),
                 0.0f, 1.0f),
      std::clamp(static_cast<float>(region->y_) / static_cast<float>(region->reference_height_),
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

auto RoleToLeaseLayer(FrameRole role) -> LeaseFrameLayer {
  switch (role) {
    case FrameRole::QualityBase:
      return LeaseFrameLayer::QualityBase;
    case FrameRole::DetailPatch:
      return LeaseFrameLayer::DetailPatch;
    case FrameRole::InteractivePrimary:
    default:
      return LeaseFrameLayer::InteractivePrimary;
  }
}

}  // namespace

EditorViewportRenderer::EditorViewportRenderer() = default;

EditorViewportRenderer::~EditorViewportRenderer() {
  if (present_queue_) {
    // Renderer lifetime, not QWindow exposure, defines whether native target
    // requests can be serviced.
    present_queue_->SetConsumerAvailable(false);
  }
  releaseResources();
  releaseQueuedNatives();
  adapter_.reset();
}

auto EditorViewportRenderer::layerForRole(FrameRole role) const -> LayerId {
  switch (role) {
    case FrameRole::InteractivePrimary:
      return LayerId::InteractivePrimary;
    case FrameRole::QualityBase:
      return LayerId::QualityBase;
    case FrameRole::DetailPatch:
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

void EditorViewportRenderer::destroyResource(QRhiTexture*& resource) { DestroyRhi(resource); }
void EditorViewportRenderer::destroyResource(QRhiBuffer*& resource) { DestroyRhi(resource); }
void EditorViewportRenderer::destroyResource(QRhiSampler*& resource) { DestroyRhi(resource); }
void EditorViewportRenderer::destroyResource(QRhiShaderResourceBindings*& resource) {
  DestroyRhi(resource);
}
void EditorViewportRenderer::destroyResource(QRhiGraphicsPipeline*& resource) {
  DestroyRhi(resource);
}

void EditorViewportRenderer::releaseLayer(LayerState& layer) {
  destroyResource(layer.texture);
  if (layer.imported) {
    NativeResourceCounters::Instance().OnDestroyImportedQRhiTexture();
  }
  if (layer.slot_index >= 0 && present_queue_) {
    present_queue_->CompleteRendererRead(layer.slot_index);
  }
  layer = {};
}

void EditorViewportRenderer::releaseQueuedNatives() {
  if (!present_queue_ || !adapter_) {
    return;
  }
  const auto released = present_queue_->DrainReleasedNatives();
  for (const auto& native : released) {
    auto it = std::find_if(owned_natives_.begin(), owned_natives_.end(),
                           [&](const WritableTargetLease& lease) {
                             return lease.native_handle == native.native_handle;
                           });
    if (it != owned_natives_.end()) {
      adapter_->DestroyTarget(*it);
      owned_natives_.erase(it);
    }
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
  target_generation_ = 0;
  if (present_queue_) {
    present_queue_->InvalidateTargetGeneration();
  }
  releaseQueuedNatives();
  render_target_ = nullptr;
}

void EditorViewportRenderer::initialize(QRhiCommandBuffer* /*command_buffer*/) {
  QRhi* next_rhi = rhi();
  if (!next_rhi) {
    return;
  }
  // QQuickRhiItem calls initialize() before rendering and may call it again
  // after geometry or render-target changes. It is not a one-shot constructor.
  // Releasing on every call used to invalidate the native target request just
  // before render() could fulfill it, leaving the viewport permanently black.
  if (rhi_ != next_rhi) {
    releaseResources();
    rhi_ = next_rhi;
    backend_ = BackendForRhi(rhi_);
    adapter_ = MakeLeaseTargetAdapter(backend_);
    qInfo("[EditorPresent] render thread initialized backend=%s", ToString(backend_));
    if (item_) {
      item_->setBackendName(QString::fromUtf8(ToString(backend_)));
      item_->setStatusText(QStringLiteral("render thread initialized (%1)")
                               .arg(QString::fromUtf8(QtGraphicsApiName(backend_))));
    }
  }
  content_dirty_ = true;
}

void EditorViewportRenderer::synchronize(QQuickRhiItem* item) {
  item_ = qobject_cast<EditorViewportItem*>(item);
  if (!item_) {
    qWarning("[EditorPresent] synchronize received incompatible item");
    return;
  }
  if (present_queue_ != item_->present_queue()) {
    present_queue_ = item_->present_queue();
    image_generation_ = 0;
    image_identity_ = 0;
    target_generation_ = 0;
    content_dirty_ = true;
  }

  present_queue_->SetConsumerAvailable(item_->presentationRequested());

  const auto next_view = item_->viewStateSnapshot();
  view_state_ = next_view;
  content_dirty_ = true;

  const auto next_image_generation = item_->imageGeneration();
  const auto next_image_identity = item_->imageIdentity();
  if (next_image_generation != image_generation_ || next_image_identity != image_identity_) {
    for (auto& layer : layers_) {
      releaseLayer(layer);
    }
    releaseQueuedNatives();
    image_generation_ = next_image_generation;
    image_identity_ = next_image_identity;
    content_dirty_ = true;
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
    content_dirty_ = true;
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

void EditorViewportRenderer::fulfillTargetRequests() {
  if (!present_queue_ || !adapter_ || !rhi_ || image_generation_ == 0) {
    return;
  }
  auto requests = present_queue_->DrainSizeRequests();
  if (requests.empty()) {
    return;
  }
  qInfo("[EditorPresent] fulfilling %zu target request(s), image=%llu generation=%llu",
        requests.size(), static_cast<unsigned long long>(image_identity_),
        static_cast<unsigned long long>(image_generation_));

  if (target_generation_ == 0) {
    present_queue_->InvalidateTargetGeneration();
    target_generation_ = present_queue_->CurrentTargetGeneration();
  }

  // Deduplicate exact sizes for this pass (one lazy allocation per size).
  std::vector<DirectPresentQueue::SizeRequest> unique;
  for (const auto& request : requests) {
    if (!request.valid() || request.image_generation != image_generation_) {
      continue;
    }
    if (request.image_identity != 0 && request.image_identity != image_identity_) {
      continue;
    }
    const auto exists = std::any_of(unique.begin(), unique.end(), [&](const auto& other) {
      return other.width == request.width && other.height == request.height;
    });
    if (!exists) {
      unique.push_back(request);
    }
  }

  for (const auto& request : unique) {
    // Match the proven QRhiWidget path: allocate the selected slot lazily.
    auto prepare = present_queue_->PrepareWrite(request.width, request.height, image_generation_,
                                                image_identity_);
    int slot_index = prepare.slot_index;
    if (slot_index < 0) {
      slot_index = request.preferred_slot >= 0 ? request.preferred_slot : 0;
    }

    // Reuse existing exact-size Available slot without reallocating.
    if (!prepare.need_create && prepare.ok) {
      continue;
    }

    TargetGeneration generation{target_generation_, image_generation_, request.layer_generation,
                                image_identity_};
    auto lease = adapter_->CreateTarget(rhi_, QSize(request.width, request.height), generation,
                                        RoleToLeaseLayer(request.frame_role));
    if (!lease.has_value()) {
      target_error_ = adapter_->lastError();
      qWarning("[EditorPresent] target allocation failed %dx%d: %s", request.width, request.height,
               target_error_.c_str());
      present_queue_->FailSizeRequest(request);
      if (item_) {
        item_->setStatusText(QString::fromStdString(target_error_));
      }
      continue;
    }

    DirectPresentQueue::SlotNative native;
    native.backend = lease->backend;
    native.handle_kind = lease->handle_kind;
    native.writable_kind = lease->writable_kind;
    native.native_handle = lease->native_handle;
    native.writable_resource = lease->writable_resource;
    native.sync_object = lease->sync_object;
    native.sync_value = lease->sync_value;
    native.adapter_cookie = lease->native_handle;

    if (!present_queue_->PublishCreatedSlot(slot_index, request.width, request.height, native,
                                            image_generation_, image_identity_)) {
      // Try another free slot index.
      bool published = false;
      for (int i = 0; i < DirectPresentQueue::kSlotCount; ++i) {
        if (present_queue_->PublishCreatedSlot(i, request.width, request.height, native,
                                               image_generation_, image_identity_)) {
          published = true;
          break;
        }
      }
      if (!published) {
        qWarning("[EditorPresent] queue rejected target %dx%d", request.width, request.height);
        adapter_->DestroyTarget(*lease);
        present_queue_->FailSizeRequest(request);
        continue;
      }
    }
    owned_natives_.push_back(*lease);
    qInfo("[EditorPresent] published native target %dx%d slot=%d", request.width, request.height,
          slot_index);
    target_error_.clear();
    content_dirty_ = true;
  }
  publishDiagnosticsIfChanged();
}

void EditorViewportRenderer::consumeDirectFrames() {
  if (!present_queue_ || !rhi_) {
    return;
  }
  constexpr FrameRole roles[] = {FrameRole::InteractivePrimary, FrameRole::QualityBase,
                                 FrameRole::DetailPatch};
  for (const auto role : roles) {
    auto frame = present_queue_->ConsumeNewestReady(role, image_generation_, image_identity_);
    if (!frame.has_value()) {
      continue;
    }
    qInfo("[EditorPresent] consuming frame request=%llu image=%llu generation=%llu size=%dx%d",
          static_cast<unsigned long long>(frame->slot.preview_metadata.presentation_request_id),
          static_cast<unsigned long long>(frame->slot.image_identity),
          static_cast<unsigned long long>(frame->slot.image_generation), frame->slot.width,
          frame->slot.height);

    auto& layer = layers_[layerIndex(layerForRole(role))];
    releaseLayer(layer);
    auto* texture =
        rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame->slot.width, frame->slot.height), 1);
    if (!texture ||
        !texture->createFrom({static_cast<quint64>(frame->slot.native.native_handle), 0})) {
      qWarning("[EditorPresent] QRhi import failed for request=%llu handle=%llu",
               static_cast<unsigned long long>(
                   frame->slot.preview_metadata.presentation_request_id),
               static_cast<unsigned long long>(frame->slot.native.native_handle));
      destroyResource(texture);
      present_queue_->CompleteRendererRead(frame->slot.index);
      if (item_) {
        item_->setStatusText(QStringLiteral("failed to import a completed native frame"));
      }
      continue;
    }
    // Keep imported-resource state explicit, matching the proven
    // RhiEditViewerSurface path. D3D11/OpenGL use layout 0.
    texture->setNativeLayout(0);
    layer.texture = texture;
    layer.width = frame->slot.width;
    layer.height = frame->slot.height;
    layer.imported = true;
    layer.valid = true;
    layer.slot_index = frame->slot.index;
    NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
    layer.presentation_mode = frame->slot.presentation_mode;
    layer.preview_metadata = frame->slot.preview_metadata;
    layer.ready_frame = *frame;
    content_dirty_ = true;
    if (item_) {
      item_->setStatusText(QStringLiteral("imported frame role=%1 gen=%2")
                               .arg(static_cast<int>(role))
                               .arg(frame->slot.preview_metadata.preview_generation));
    }
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
    const auto current_roi = BuildNormalizedRoi(view_state_.snapshot.viewport_render_region_cache);
    if (!current_roi.has_value() ||
        SameRoi(interactive.preview_metadata.source_roi_norm, *current_roi) ||
        interactive.presentation_mode == FramePresentationMode::FullFrame) {
      return &interactive;
    }
  }
  return &quality;
}

auto EditorViewportRenderer::detailPatchAspectOk(const LayerState& detail,
                                                 const LayerState& quality) const -> bool {
  if (quality.width <= 0 || quality.height <= 0 || detail.width <= 0 || detail.height <= 0) {
    return false;
  }
  const float quality_aspect =
      static_cast<float>(quality.width) / static_cast<float>(quality.height);
  const float detail_aspect =
      static_cast<float>(detail.width) / static_cast<float>(detail.height);
  const float roi_w = std::max(detail.preview_metadata.source_roi_norm.width, 1.0e-4f);
  const float roi_h = std::max(detail.preview_metadata.source_roi_norm.height, 1.0e-4f);
  const float expected_aspect = quality_aspect * (roi_w / roi_h);
  return std::abs(detail_aspect - expected_aspect) <= 0.15f * std::max(expected_aspect, 1.0e-3f);
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
  if (!detailPatchAspectOk(detail, quality)) {
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

void EditorViewportRenderer::recreateShaderResources(QRhiTexture* primary, QRhiTexture* detail) {
  destroyResource(shader_resource_bindings_);
  destroyResource(pipeline_);
  if (!rhi_ || !uniform_buffer_ || !primary_sampler_ || !detail_sampler_ || !primary || !detail) {
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

void EditorViewportRenderer::publishDiagnosticsIfChanged() {
  if (item_) {
    item_->notifyDiagnosticsChanged();
  }
}

void EditorViewportRenderer::render(QRhiCommandBuffer* command_buffer) {
  QRhiRenderTarget* render_target = renderTarget();
  if (!command_buffer || !render_target || !rhi_ || !item_) {
    return;
  }

  // Targets are producer-driven. Matches the pre-refactor surface: create the
  // exact output slot requested by EnsureSize, with no speculative pool.
  fulfillTargetRequests();
  consumeDirectFrames();
  ensureStaticResources(render_target, command_buffer);
  auto* updates = rhi_->nextResourceUpdateBatch();

  const auto* primary_layer = selectedPrimaryLayer();
  const auto* detail_layer = selectedDetailLayer();
  QRhiTexture* primary = primary_layer ? primary_layer->texture : placeholder_texture_;
  QRhiTexture* detail = detail_layer ? detail_layer->texture : placeholder_texture_;
  if (primary != bound_primary_texture_ || detail != bound_detail_texture_ ||
      !shader_resource_bindings_) {
    recreateShaderResources(primary, detail);
    content_dirty_ = true;
  }

  if (!pipeline_ && shader_resource_bindings_) {
    pipeline_ = rhi_->newGraphicsPipeline();
    QRhiVertexInputLayout layout;
    layout.setBindings({QRhiVertexInputBinding(sizeof(VertexData))});
    layout.setAttributes(
        {QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
         QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))});
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
  const bool drew_primary =
      primary_layer && pipeline_ && shader_resource_bindings_ && vertex_buffer_;
  if (drew_primary) {
    const QRhiCommandBuffer::VertexInput vertex_input[] = {{vertex_buffer_, 0}};
    const QSize size = render_target->pixelSize();
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
    command_buffer->setShaderResources(shader_resource_bindings_);
    command_buffer->setVertexInput(0, 1, vertex_input);
    command_buffer->draw(4);
  }
  command_buffer->endPass();

  // Composition confirmation only after the selected primary slot was encoded
  // into this Qt Quick window frame. Intermediate frames are not application
  // presentation events; first-frame service uses a one-shot session event.
  if (drew_primary && primary_layer->slot_index >= 0 && item_->frameSink()) {
    item_->frameSink()->NotifyPrimaryFrameComposed(primary_layer->ready_frame);
  }

  releaseQueuedNatives();

  if (drew_primary) {
    item_->setStatusText(QStringLiteral("presented"));
  } else if (!target_error_.empty()) {
    item_->setStatusText(QString::fromStdString(target_error_));
  } else {
    item_->setStatusText(QStringLiteral("waiting for a compatible frame"));
  }
  publishDiagnosticsIfChanged();
  (void)content_dirty_;
}

}  // namespace alcedo::editor_rhi
