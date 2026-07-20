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
  // Shader bindings retain the imported QRhiTexture pointer. Invalidate them
  // before destroying a layer texture so a view-driven stale-detail release is
  // safe even when no replacement frame is available yet.
  if (layer.texture &&
      (bound_primary_texture_ == layer.texture || bound_detail_texture_ == layer.texture)) {
    destroyResource(shader_resource_bindings_);
    bound_primary_texture_ = nullptr;
    bound_detail_texture_  = nullptr;
  }
  destroyResource(layer.texture);
  if (layer.imported) {
    NativeResourceCounters::Instance().OnDestroyImportedQRhiTexture();
  }
  if (layer.slot_index >= 0 && present_queue_) {
    present_queue_->CompleteRendererRead(layer.slot_index);
  }
  // Drop producer-owned Metal (etc.) texture retain after QRhi wrapper is gone.
  layer.imported_owner.reset();
  layer.imported_native_handle = 0;
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
  bound_detail_texture_  = nullptr;
  static_upload_pending_ = false;
  target_generation_     = 0;
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
    rhi_     = next_rhi;
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
    present_queue_     = item_->present_queue();
    image_generation_  = 0;
    image_identity_    = 0;
    target_generation_ = 0;
    content_dirty_     = true;
  }

  present_queue_->SetConsumerAvailable(item_->presentationRequested());

  const auto next_image_generation = item_->imageGeneration();
  const auto next_image_identity   = item_->imageIdentity();
  if (next_image_generation != image_generation_ || next_image_identity != image_identity_) {
    for (auto& layer : layers_) {
      releaseLayer(layer);
    }
    releaseQueuedNatives();
    if (item_->frameSink()) {
      item_->frameSink()->ClearPendingImportedFrames();
    }
    image_generation_ = next_image_generation;
    image_identity_   = next_image_identity;
    content_dirty_    = true;
  }

  if (item_->takeAdjustmentFrameRequest()) {
    // The old QRhiWidget presenter kept one active full-frame slot and made the
    // other direct-present slots writable. The first QML port retained one slot
    // for every layer, so Interactive + Quality + Detail could consume all
    // three slots and force the next FAST_PREVIEW to wait for a scene-graph
    // pass. Keep the currently visible primary but release stale auxiliary
    // layers before the producer starts its next frame.
    const LayerState* visible_primary = selectedPrimaryLayer();
    auto&             interactive =
        layers_[layerIndex(LayerId::InteractivePrimary)];
    auto& quality = layers_[layerIndex(LayerId::QualityBase)];
    auto& detail  = layers_[layerIndex(LayerId::DetailPatch)];
    if (visible_primary != &interactive) {
      releaseLayer(interactive);
    }
    if (visible_primary != &quality) {
      releaseLayer(quality);
    }
    releaseLayer(detail);
    content_dirty_ = true;
  }

  const auto next_view = item_->viewStateSnapshot();
  if (next_image_generation == image_generation_ && next_image_identity == image_identity_) {
    auto&      detail   = layers_[layerIndex(LayerId::DetailPatch)];
    const auto next_roi = BuildNormalizedRoi(next_view.snapshot.viewport_render_region_cache);
    // A consumed detail frame owns one of the queue's three slots until its
    // layer is released. Once the view ROI changes it can no longer contribute
    // to the image, so release it immediately. Waiting to release it when the
    // replacement DetailPatch is consumed deadlocks: primary + quality + old
    // detail already occupy all three slots, leaving no writable target for
    // that replacement.
    if (detail.valid && (!next_view.allow_detail_patch || !next_roi.has_value() ||
                         !SameRoi(detail.preview_metadata.source_roi_norm, *next_roi))) {
      releaseLayer(detail);
    }
  }
  view_state_    = next_view;
  content_dirty_ = true;
}

void EditorViewportRenderer::ensureStaticResources(QRhiRenderTarget*  render_target,
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
    layer.imported_owner.reset();
    layer.imported_native_handle = frame->slot.native.native_handle;
    content_dirty_ = true;
    if (item_) {
      item_->setStatusText(QStringLiteral("imported frame role=%1 gen=%2")
                               .arg(static_cast<int>(role))
                               .arg(frame->slot.preview_metadata.preview_generation));
    }
  }
}

void EditorViewportRenderer::consumeImportedGpuFrames() {
  if (!rhi_ || !item_ || !item_->frameSink()) {
    return;
  }
  auto pending =
      item_->frameSink()->DrainPendingImportedFrames(image_generation_, image_identity_);
  for (auto& frame : pending) {
    if (!frame.valid()) {
      continue;
    }
    const FrameRole role = frame.preview_metadata.frame_role;
    qInfo("[EditorPresent] consuming Metal import request=%llu image=%llu generation=%llu "
          "size=%dx%d handle=%llu",
          static_cast<unsigned long long>(frame.preview_metadata.presentation_request_id),
          static_cast<unsigned long long>(frame.image_identity),
          static_cast<unsigned long long>(frame.image_generation), frame.width, frame.height,
          static_cast<unsigned long long>(frame.texture_handle));

    auto& layer = layers_[layerIndex(layerForRole(role))];
    // Same native object already bound for this layer: keep QRhi wrapper, refresh
    // metadata/owner only (zero-copy path can re-submit the same MTLTexture).
    if (layer.valid && layer.texture && layer.imported_native_handle == frame.texture_handle &&
        layer.width == frame.width && layer.height == frame.height) {
      layer.imported_owner = std::move(frame.owner);
      layer.presentation_mode = frame.presentation_mode;
      layer.preview_metadata = frame.preview_metadata;
      layer.ready_frame = {};
      layer.ready_frame.slot.width = frame.width;
      layer.ready_frame.slot.height = frame.height;
      layer.ready_frame.slot.presentation_mode = frame.presentation_mode;
      layer.ready_frame.slot.preview_metadata = frame.preview_metadata;
      layer.ready_frame.slot.image_generation = frame.image_generation;
      layer.ready_frame.slot.image_identity = frame.image_identity;
      layer.ready_frame.slot.sequence = frame.sequence;
      layer.texture->setNativeLayout(frame.native_layout);
      content_dirty_ = true;
      continue;
    }

    releaseLayer(layer);
    auto* texture =
        rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame.width, frame.height), 1);
    if (!texture ||
        !texture->createFrom(
            {static_cast<quint64>(frame.texture_handle), frame.native_layout})) {
      qWarning("[EditorPresent] QRhi Metal import failed for request=%llu handle=%llu",
               static_cast<unsigned long long>(frame.preview_metadata.presentation_request_id),
               static_cast<unsigned long long>(frame.texture_handle));
      destroyResource(texture);
      if (item_) {
        item_->setStatusText(QStringLiteral("failed to import Metal frame (zero-copy)"));
      }
      continue;
    }
    // Match RhiEditViewerSurface::ensureImportedTexture: keep layout explicit.
    texture->setNativeLayout(frame.native_layout);
    layer.texture = texture;
    layer.width = frame.width;
    layer.height = frame.height;
    layer.imported = true;
    layer.valid = true;
    layer.slot_index = -1;  // producer-owned; not a DirectPresentQueue slot
    layer.imported_owner = std::move(frame.owner);
    layer.imported_native_handle = frame.texture_handle;
    layer.presentation_mode = frame.presentation_mode;
    layer.preview_metadata = frame.preview_metadata;
    layer.ready_frame = {};
    layer.ready_frame.slot.width = frame.width;
    layer.ready_frame.slot.height = frame.height;
    layer.ready_frame.slot.presentation_mode = frame.presentation_mode;
    layer.ready_frame.slot.preview_metadata = frame.preview_metadata;
    layer.ready_frame.slot.image_generation = frame.image_generation;
    layer.ready_frame.slot.image_identity = frame.image_identity;
    layer.ready_frame.slot.sequence = frame.sequence;
    NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
    content_dirty_ = true;
    if (item_) {
      item_->setStatusText(QStringLiteral("imported Metal frame role=%1 gen=%2")
                               .arg(static_cast<int>(role))
                               .arg(frame.preview_metadata.preview_generation));
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
                                                 const LayerState& base) const -> bool {
  if (base.width <= 0 || base.height <= 0 || detail.width <= 0 || detail.height <= 0) {
    return false;
  }
  const float base_aspect = static_cast<float>(base.width) / static_cast<float>(base.height);
  const float detail_aspect =
      static_cast<float>(detail.width) / static_cast<float>(detail.height);
  const float roi_w = std::max(detail.preview_metadata.source_roi_norm.width, 1.0e-4f);
  const float roi_h = std::max(detail.preview_metadata.source_roi_norm.height, 1.0e-4f);
  const float expected_aspect = base_aspect * (roi_w / roi_h);
  return std::abs(detail_aspect - expected_aspect) <= 0.15f * std::max(expected_aspect, 1.0e-3f);
}

auto EditorViewportRenderer::hasVisibleDetailPatch() const -> bool {
  // Phase 5D: the QML editor route does not use the legacy expected-detail
  // token handshake (EditorRenderCoordinator never calls a SetExpectedDetail
  // equivalent), so has_expected_detail_token / expected_detail_generation /
  // expected_detail_serial stay at their defaults and would gate the patch
  // off forever. The new route guarantees a detail patch is valid purely from
  // generation + ROI + aspect matching: the coordinator cancels obsolete
  // DetailPatch work on every view- and render-generation advance, the
  // generation match below rejects a detail from a prior content generation,
  // and SameRoi rejects a detail whose source ROI no longer matches the
  // current viewport. That is sufficient — the token fields remain on
  // ViewerViewState only for the legacy QWidget viewer's own check.
  if (!view_state_.allow_detail_patch) {
    return false;
  }
  const auto& quality = layers_[layerIndex(LayerId::QualityBase)];
  const auto& interactive = layers_[layerIndex(LayerId::InteractivePrimary)];
  const auto& detail = layers_[layerIndex(LayerId::DetailPatch)];
  if (!detail.valid) {
    return false;
  }
  // Prefer QualityBase as the full-frame aspect/generation reference. Fall back
  // to a full-frame InteractivePrimary so a DetailPatch can land before the
  // settled quality pass finishes (common after open + immediate zoom).
  const LayerState* base = nullptr;
  if (quality.valid && quality.preview_metadata.preview_generation ==
                           detail.preview_metadata.preview_generation) {
    base = &quality;
  } else if (interactive.valid &&
             interactive.preview_metadata.preview_generation ==
                 detail.preview_metadata.preview_generation &&
             interactive.presentation_mode != FramePresentationMode::RoiFrame) {
    base = &interactive;
  }
  if (!base || !detailPatchAspectOk(detail, *base)) {
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
  // Every frame uses the same binding layout (one UBO + two sampled textures),
  // so an existing graphics pipeline remains compatible with the replacement
  // bindings. Rebuilding it for every triple-buffer slot rotation was pure
  // render-thread overhead during slider drags.
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
  // Metal skips shared-slot allocation and consumes zero-copy imports instead.
  fulfillTargetRequests();
  consumeDirectFrames();
  consumeImportedGpuFrames();
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

  // Composition confirmation only after the selected primary was encoded into
  // this Qt Quick window frame. Covers both DirectPresentQueue slots (CUDA /
  // OpenCL) and zero-copy Metal imports (slot_index == -1).
  if (drew_primary && item_->frameSink()) {
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
