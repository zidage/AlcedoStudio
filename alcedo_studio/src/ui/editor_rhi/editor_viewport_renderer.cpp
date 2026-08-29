//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/editor_viewport_renderer.hpp"

#include <QDebug>
#include <QFile>
#include <QSize>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "ui/edit_viewer/viewport_mapper.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/diagnostics/render_e2e_timing.hpp"

using alcedo::diag::editorPresentLog;
namespace alcedo::editor_rhi {
namespace {

constexpr const char* kVertexShaderResource   = ":/shaders/editor_rhi/editor_viewport.vert.qsb";
constexpr const char* kFragmentShaderResource = ":/shaders/editor_rhi/editor_viewport.frag.qsb";

auto                  BackendForRhi(QRhi* rhi) -> EditorBackend {
  if (!rhi) {
    return ActiveEditorBackend();
  }
  switch (rhi->backend()) {
    case QRhi::OpenGLES2:
      return ActiveEditorBackend() == EditorBackend::Cpu ? EditorBackend::Cpu
                                                         : EditorBackend::OpenCl;
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
  layer                        = {};
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
    rhi_                = next_rhi;
    // Process-wide backend selected in main() is authoritative. Deriving only
    // from QRhi can silently flip CUDA↔OpenCL when setGraphicsApi did not take
    // effect, which produces host_upload frames and a black viewport.
    const auto active   = ActiveEditorBackend();
    const auto from_rhi = BackendForRhi(rhi_);
    backend_            = active;
    adapter_            = MakeLeaseTargetAdapter(backend_);
    if (active != from_rhi) {
      qCWarning(editorPresentLog,
                "[EditorPresent] RHI/backend mismatch: active=%s rhi_inferred=%s rhi_api=%d. "
                "Present will use active=%s; OpenCL requires OpenGL QRhi.",
                ToString(active), ToString(from_rhi), static_cast<int>(rhi_->backend()),
                ToString(active));
      target_error_ = std::string("RHI/backend mismatch: active=") + ToString(active) +
                      " rhi=" + ToString(from_rhi);
    }
    if (backend_ == EditorBackend::OpenCl && rhi_->backend() != QRhi::OpenGLES2) {
      qCWarning(editorPresentLog,
                "[EditorPresent] OpenCL present selected but Qt RHI is not OpenGL (api=%d). "
                "Native targets cannot be created; frames fall back to empty host_upload.",
                static_cast<int>(rhi_->backend()));
      target_error_ = "OpenCL present requires OpenGL QRhi; graphics API selection failed";
    }
    qCWarning(editorPresentLog, "[EditorPresent] render thread initialized backend=%s rhi_api=%d",
              ToString(backend_), static_cast<int>(rhi_->backend()));
    if (item_) {
      item_->setBackendName(QString::fromUtf8(ToString(backend_)));
      item_->setStatusText(target_error_.empty()
                               ? QStringLiteral("render thread initialized (%1)")
                                     .arg(QString::fromUtf8(QtGraphicsApiName(backend_)))
                               : QString::fromStdString(target_error_));
    }
  }
  content_dirty_ = true;
}

void EditorViewportRenderer::synchronize(QQuickRhiItem* item) {
  item_ = qobject_cast<EditorViewportItem*>(item);
  if (!item_) {
    qCWarning(editorPresentLog, "[EditorPresent] synchronize received incompatible item");
    return;
  }
  if (present_queue_ != item_->present_queue()) {
    present_queue_     = item_->present_queue();
    session_epoch_     = 0;
    image_identity_    = 0;
    target_generation_ = 0;
    content_dirty_     = true;
  }

  present_queue_->SetConsumerAvailable(item_->presentationRequested());

  const auto next_session_epoch  = item_->sessionEpoch();
  const auto next_image_identity = item_->imageIdentity();
  if (next_session_epoch != session_epoch_ || next_image_identity != image_identity_) {
    for (auto& layer : layers_) {
      releaseLayer(layer);
    }
    releaseQueuedNatives();
    if (item_->frameSink()) {
      item_->frameSink()->ClearPendingImportedFrames();
      item_->frameSink()->ClearPendingHostFrames();
    }
    session_epoch_  = next_session_epoch;
    image_identity_ = next_image_identity;
    content_dirty_  = true;
  }

  if (item_->takeAdjustmentFrameRequest()) {
    // The old QRhiWidget presenter kept one active full-frame slot and made the
    // other direct-present slots writable. The first QML port retained one slot
    // for every layer, so Interactive + Quality + Detail could consume all
    // three slots and force the next FAST_PREVIEW to wait for a scene-graph
    // pass. Keep the currently visible primary but release stale auxiliary
    // layers before the producer starts its next frame.
    const LayerState* visible_primary = selectedPrimaryLayer();
    auto&             interactive     = layers_[layerIndex(LayerId::InteractivePrimary)];
    auto&             quality         = layers_[layerIndex(LayerId::QualityBase)];
    auto&             detail          = layers_[layerIndex(LayerId::DetailPatch)];
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
  if (next_session_epoch == session_epoch_ && next_image_identity == image_identity_) {
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
      if (editorPresentLog().isDebugEnabled()) {
        QString msg =
            QStringLiteral(
                "[ROI_TRACE][renderer-release] request=%1 reason=view-state-changed allow=%2 "
                "current_roi=")
                .arg(detail.preview_metadata.presentation_request_id)
                .arg(next_view.allow_detail_patch ? 1 : 0);
        if (next_roi) {
          msg += QStringLiteral("%1,%2,%3,%4")
                     .arg(next_roi->x)
                     .arg(next_roi->y)
                     .arg(next_roi->width)
                     .arg(next_roi->height);
        } else {
          msg += QStringLiteral("none");
        }
        msg += QStringLiteral(" patch_roi=%1,%2,%3,%4")
                   .arg(detail.preview_metadata.source_roi_norm.x)
                   .arg(detail.preview_metadata.source_roi_norm.y)
                   .arg(detail.preview_metadata.source_roi_norm.width)
                   .arg(detail.preview_metadata.source_roi_norm.height);
        qCDebug(editorPresentLog).noquote() << msg;
      }
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
    primary_sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    primary_sampler_->create();
  }
  if (!detail_sampler_) {
    detail_sampler_ =
        rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
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
    static constexpr std::array<float, 4> black   = {0.0f, 0.0f, 0.0f, 1.0f};
    auto*                                 updates = rhi_->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertex_buffer_, vertices.data());
    QByteArray data(reinterpret_cast<const char*>(black.data()), static_cast<int>(sizeof(black)));
    QRhiTextureSubresourceUploadDescription description(data);
    description.setDataStride(static_cast<quint32>(sizeof(black)));
    description.setSourceSize(QSize(1, 1));
    updates->uploadTexture(placeholder_texture_,
                           QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, description)));
    command_buffer->resourceUpdate(updates);
    static_upload_pending_ = false;
  }
}

void EditorViewportRenderer::fulfillTargetRequests() {
  if (!present_queue_ || !adapter_ || !rhi_ || session_epoch_ == 0) {
    return;
  }
  auto requests = present_queue_->DrainSizeRequests();
  if (requests.empty()) {
    return;
  }
  qCDebug(editorPresentLog,
          "[EditorPresent] fulfilling %zu target request(s), image=%llu epoch=%llu",
          requests.size(), static_cast<unsigned long long>(image_identity_),
          static_cast<unsigned long long>(session_epoch_));

  if (target_generation_ == 0) {
    present_queue_->InvalidateTargetGeneration();
    target_generation_ = present_queue_->CurrentTargetGeneration();
  }

  // Deduplicate exact sizes for this pass (one lazy allocation per size).
  std::vector<DirectPresentQueue::SizeRequest> unique;
  for (const auto& request : requests) {
    if (!request.valid() || request.session_epoch != session_epoch_) {
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
    auto prepare    = present_queue_->PrepareWrite(request.width, request.height, session_epoch_,
                                                   image_identity_);
    int  slot_index = prepare.slot_index;
    if (slot_index < 0) {
      slot_index = request.preferred_slot >= 0 ? request.preferred_slot : 0;
    }

    // Reuse existing exact-size Available slot without reallocating.
    if (!prepare.need_create && prepare.ok) {
      continue;
    }

    TargetGeneration generation{target_generation_, session_epoch_, request.layer_generation,
                                image_identity_};
    auto lease = adapter_->CreateTarget(rhi_, QSize(request.width, request.height), generation,
                                        RoleToLeaseLayer(request.frame_role));
    if (!lease.has_value()) {
      target_error_ = adapter_->lastError();
      qCWarning(editorPresentLog, "[EditorPresent] target allocation failed %dx%d: %s",
                request.width, request.height, target_error_.c_str());
      present_queue_->FailSizeRequest(request);
      if (item_) {
        item_->setStatusText(QString::fromStdString(target_error_));
      }
      continue;
    }

    DirectPresentQueue::SlotNative native;
    native.backend           = lease->backend;
    native.handle_kind       = lease->handle_kind;
    native.writable_kind     = lease->writable_kind;
    native.native_handle     = lease->native_handle;
    native.writable_resource = lease->writable_resource;
    native.sync_object       = lease->sync_object;
    native.sync_value        = lease->sync_value;
    native.adapter_cookie    = lease->native_handle;

    if (!present_queue_->PublishCreatedSlot(slot_index, request.width, request.height, native,
                                            session_epoch_, image_identity_)) {
      // Try another free slot index.
      bool published = false;
      for (int i = 0; i < DirectPresentQueue::kSlotCount; ++i) {
        if (present_queue_->PublishCreatedSlot(i, request.width, request.height, native,
                                               session_epoch_, image_identity_)) {
          published = true;
          break;
        }
      }
      if (!published) {
        qCWarning(editorPresentLog, "[EditorPresent] queue rejected target %dx%d", request.width,
                  request.height);
        adapter_->DestroyTarget(*lease);
        present_queue_->FailSizeRequest(request);
        continue;
      }
    }
    owned_natives_.push_back(*lease);
    qCDebug(editorPresentLog, "[EditorPresent] published native target %dx%d slot=%d",
            request.width, request.height, slot_index);
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
    auto frame = present_queue_->ConsumeNewestReady(role, session_epoch_, image_identity_);
    if (!frame.has_value()) {
      continue;
    }
    const std::uint64_t request_id = frame->slot.preview_metadata.presentation_request_id;
    diag::NoteRenderE2eConsumeBegin(request_id);
    qCDebug(editorPresentLog,
            "[EditorPresent] consuming frame request=%llu image=%llu epoch=%llu size=%dx%d",
            static_cast<unsigned long long>(request_id),
            static_cast<unsigned long long>(frame->slot.image_identity),
            static_cast<unsigned long long>(frame->slot.session_epoch), frame->slot.width,
            frame->slot.height);
    if (role == FrameRole::DetailPatch) {
      qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-import-begin] request="
                                << frame->slot.preview_metadata.presentation_request_id
                                << " slot=" << frame->slot.index << " size=" << frame->slot.width
                                << 'x' << frame->slot.height
                                << " native=" << frame->slot.native.native_handle;
    }

    auto& layer = layers_[layerIndex(layerForRole(role))];
    releaseLayer(layer);
    auto* texture =
        rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame->slot.width, frame->slot.height), 1);
    if (!texture ||
        !texture->createFrom({static_cast<quint64>(frame->slot.native.native_handle), 0})) {
      qCWarning(
          editorPresentLog, "[EditorPresent] QRhi import failed for request=%llu handle=%llu",
          static_cast<unsigned long long>(frame->slot.preview_metadata.presentation_request_id),
          static_cast<unsigned long long>(frame->slot.native.native_handle));
      diag::NoteRenderE2eTerminal(frame->slot.preview_metadata.presentation_request_id,
                                  "qrhi-import-failed");
      destroyResource(texture);
      present_queue_->CompleteRendererRead(frame->slot.index);
      if (role == FrameRole::DetailPatch) {
        qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-drop] request="
                                  << frame->slot.preview_metadata.presentation_request_id
                                  << " reason=qrhi-import-failed native="
                                  << frame->slot.native.native_handle;
      }
      if (item_) {
        item_->setStatusText(QStringLiteral("failed to import a completed native frame"));
      }
      continue;
    }
    // Keep imported-resource state explicit, matching the proven
    // D3D11/OpenGL use layout 0.
    texture->setNativeLayout(0);
    layer.texture    = texture;
    layer.width      = frame->slot.width;
    layer.height     = frame->slot.height;
    layer.imported   = true;
    layer.valid      = true;
    layer.slot_index = frame->slot.index;
    NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
    layer.presentation_mode = frame->slot.presentation_mode;
    layer.preview_metadata  = frame->slot.preview_metadata;
    layer.ready_frame       = *frame;
    layer.imported_owner.reset();
    layer.imported_native_handle = frame->slot.native.native_handle;
    content_dirty_               = true;
    diag::NoteRenderE2eDisplayed(layer.preview_metadata.presentation_request_id);
    if (role == FrameRole::DetailPatch) {
      qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-imported] request="
                                << layer.preview_metadata.presentation_request_id
                                << " slot=" << layer.slot_index << " size=" << layer.width << 'x'
                                << layer.height;
    }
    if (item_) {
      item_->setStatusText(QStringLiteral("imported frame role=%1 gen=%2")
                               .arg(static_cast<int>(role))
                               .arg(frame->slot.preview_metadata.preview_generation));
    }
  }
}

void EditorViewportRenderer::consumeHostFrames(QRhiResourceUpdateBatch* updates) {
  if (!rhi_ || !updates || !item_ || !item_->frameSink()) {
    return;
  }

  auto pending = item_->frameSink()->DrainPendingHostFrames(session_epoch_, image_identity_);
  for (auto& frame : pending) {
    if (!frame || frame.width <= 0 || frame.height <= 0 || frame.row_bytes == 0) {
      continue;
    }
    const auto role       = frame.preview_metadata.frame_role;
    const auto request_id = frame.preview_metadata.presentation_request_id;
    const auto minimum_stride = static_cast<std::size_t>(frame.width) * sizeof(float) * 4U;
    if (frame.row_bytes < minimum_stride ||
        frame.row_bytes > static_cast<std::size_t>(std::numeric_limits<quint32>::max())) {
      qCWarning(editorPresentLog,
                "[EditorPresent] host frame has unsupported stride request=%llu stride=%zu",
                static_cast<unsigned long long>(request_id), frame.row_bytes);
      diag::NoteRenderE2eTerminal(request_id, "host-frame-invalid-stride");
      continue;
    }
    const auto height = static_cast<std::size_t>(frame.height);
    if (height > std::numeric_limits<std::size_t>::max() / frame.row_bytes) {
      diag::NoteRenderE2eTerminal(request_id, "host-frame-size-overflow");
      continue;
    }
    const auto byte_count = frame.row_bytes * height;
    if (!frame.pixels || byte_count > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
      diag::NoteRenderE2eTerminal(request_id, "host-frame-invalid-pixels");
      continue;
    }

    diag::NoteRenderE2eConsumeBegin(request_id);
    auto& layer = layers_[layerIndex(layerForRole(role))];
    releaseLayer(layer);

    auto* texture = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame.width, frame.height), 1);
    if (!texture || !texture->create()) {
      qCWarning(editorPresentLog,
                "[EditorPresent] host QRhi texture creation failed request=%llu size=%dx%d",
                static_cast<unsigned long long>(request_id), frame.width, frame.height);
      diag::NoteRenderE2eTerminal(request_id, "host-qrhi-create-failed");
      destroyResource(texture);
      continue;
    }

    // QByteArray owns a copy of the worker-produced pixels until the resource
    // update batch is consumed by QRhi. This avoids a render-thread race with
    // a short-lived cv::Mat or shared vector held by the pipeline worker.
    const QByteArray bytes(reinterpret_cast<const char*>(frame.pixels.get()),
                           static_cast<qsizetype>(byte_count));
    QRhiTextureSubresourceUploadDescription description(bytes);
    description.setDataStride(static_cast<quint32>(frame.row_bytes));
    description.setSourceSize(QSize(frame.width, frame.height));
    updates->uploadTexture(
        texture,
        QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, description)));

    layer.texture                    = texture;
    layer.width                      = frame.width;
    layer.height                     = frame.height;
    layer.imported                   = false;
    layer.valid                      = true;
    layer.slot_index                 = -1;
    layer.imported_owner.reset();
    layer.imported_native_handle     = 0;
    layer.presentation_mode          = frame.presentation_mode;
    layer.preview_metadata           = frame.preview_metadata;
    layer.ready_frame                = {};
    layer.ready_frame.slot.width     = frame.width;
    layer.ready_frame.slot.height    = frame.height;
    layer.ready_frame.slot.presentation_mode = frame.presentation_mode;
    layer.ready_frame.slot.preview_metadata  = frame.preview_metadata;
    layer.ready_frame.slot.session_epoch     = frame.preview_metadata.session_epoch;
    layer.ready_frame.slot.image_identity    = frame.preview_metadata.image_identity;
    layer.ready_frame.slot.sequence          = request_id;
    content_dirty_                   = true;
    diag::NoteRenderE2eDisplayed(request_id);
    if (item_) {
      item_->setStatusText(QStringLiteral("uploaded host frame role=%1 gen=%2")
                               .arg(static_cast<int>(role))
                               .arg(frame.preview_metadata.preview_generation));
    }
  }
}

void EditorViewportRenderer::consumeImportedGpuFrames() {
  if (!rhi_ || !item_ || !item_->frameSink()) {
    return;
  }
  auto pending = item_->frameSink()->DrainPendingImportedFrames(session_epoch_, image_identity_);
  for (auto& frame : pending) {
    if (!frame.valid()) {
      continue;
    }
    const FrameRole     role       = frame.preview_metadata.frame_role;
    const std::uint64_t request_id = frame.preview_metadata.presentation_request_id;
    diag::NoteRenderE2eConsumeBegin(request_id);
    qCDebug(editorPresentLog,
            "[EditorPresent] consuming Metal import request=%llu image=%llu epoch=%llu "
            "role=%d mode=%d size=%dx%d roi=%.6f,%.6f,%.6f,%.6f handle=%llu",
            static_cast<unsigned long long>(request_id),
            static_cast<unsigned long long>(frame.image_identity),
            static_cast<unsigned long long>(frame.session_epoch), static_cast<int>(role),
            static_cast<int>(frame.presentation_mode), frame.width, frame.height,
            frame.preview_metadata.source_roi_norm.x, frame.preview_metadata.source_roi_norm.y,
            frame.preview_metadata.source_roi_norm.width,
            frame.preview_metadata.source_roi_norm.height,
            static_cast<unsigned long long>(frame.texture_handle));
    if (role == FrameRole::DetailPatch) {
      qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-metal-import-begin] request="
                                << frame.preview_metadata.presentation_request_id
                                << " size=" << frame.width << 'x' << frame.height
                                << " native=" << frame.texture_handle;
    }

    auto& layer = layers_[layerIndex(layerForRole(role))];
    // Same native object already bound for this layer: keep QRhi wrapper, refresh
    // metadata/owner only (zero-copy path can re-submit the same MTLTexture).
    if (layer.valid && layer.texture && layer.imported_native_handle == frame.texture_handle &&
        layer.width == frame.width && layer.height == frame.height) {
      layer.imported_owner                     = std::move(frame.owner);
      layer.presentation_mode                  = frame.presentation_mode;
      layer.preview_metadata                   = frame.preview_metadata;
      layer.ready_frame                        = {};
      layer.ready_frame.slot.width             = frame.width;
      layer.ready_frame.slot.height            = frame.height;
      layer.ready_frame.slot.presentation_mode = frame.presentation_mode;
      layer.ready_frame.slot.preview_metadata  = frame.preview_metadata;
      layer.ready_frame.slot.session_epoch     = frame.session_epoch;
      layer.ready_frame.slot.image_identity    = frame.image_identity;
      layer.ready_frame.slot.sequence          = frame.sequence;
      layer.texture->setNativeLayout(frame.native_layout);
      content_dirty_ = true;
      diag::NoteRenderE2eDisplayed(layer.preview_metadata.presentation_request_id);
      if (role == FrameRole::DetailPatch) {
        qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-metal-reused] request="
                                  << layer.preview_metadata.presentation_request_id
                                  << " size=" << layer.width << 'x' << layer.height;
      }
      continue;
    }

    releaseLayer(layer);
    auto* texture = rhi_->newTexture(QRhiTexture::RGBA32F, QSize(frame.width, frame.height), 1);
    if (!texture ||
        !texture->createFrom({static_cast<quint64>(frame.texture_handle), frame.native_layout})) {
      qCWarning(editorPresentLog,
                "[EditorPresent] QRhi Metal import failed for request=%llu handle=%llu",
                static_cast<unsigned long long>(frame.preview_metadata.presentation_request_id),
                static_cast<unsigned long long>(frame.texture_handle));
      diag::NoteRenderE2eTerminal(frame.preview_metadata.presentation_request_id,
                                  "metal-qrhi-import-failed");
      destroyResource(texture);
      if (role == FrameRole::DetailPatch) {
        qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-drop] request="
                                  << frame.preview_metadata.presentation_request_id
                                  << " reason=metal-qrhi-import-failed native="
                                  << frame.texture_handle;
      }
      if (item_) {
        item_->setStatusText(QStringLiteral("failed to import Metal frame (zero-copy)"));
      }
      continue;
    }
    // Keep the imported texture layout explicit.
    texture->setNativeLayout(frame.native_layout);
    layer.texture                            = texture;
    layer.width                              = frame.width;
    layer.height                             = frame.height;
    layer.imported                           = true;
    layer.valid                              = true;
    layer.slot_index                         = -1;  // producer-owned; not a DirectPresentQueue slot
    layer.imported_owner                     = std::move(frame.owner);
    layer.imported_native_handle             = frame.texture_handle;
    layer.presentation_mode                  = frame.presentation_mode;
    layer.preview_metadata                   = frame.preview_metadata;
    layer.ready_frame                        = {};
    layer.ready_frame.slot.width             = frame.width;
    layer.ready_frame.slot.height            = frame.height;
    layer.ready_frame.slot.presentation_mode = frame.presentation_mode;
    layer.ready_frame.slot.preview_metadata  = frame.preview_metadata;
    layer.ready_frame.slot.session_epoch     = frame.session_epoch;
    layer.ready_frame.slot.image_identity    = frame.image_identity;
    layer.ready_frame.slot.sequence          = frame.sequence;
    NativeResourceCounters::Instance().OnCreateImportedQRhiTexture();
    content_dirty_ = true;
    diag::NoteRenderE2eDisplayed(layer.preview_metadata.presentation_request_id);
    if (role == FrameRole::DetailPatch) {
      qCDebug(editorPresentLog) << "[ROI_TRACE][renderer-metal-imported] request="
                                << layer.preview_metadata.presentation_request_id
                                << " size=" << layer.width << 'x' << layer.height;
    }
    if (item_) {
      item_->setStatusText(QStringLiteral("imported Metal frame role=%1 gen=%2")
                               .arg(static_cast<int>(role))
                               .arg(frame.preview_metadata.preview_generation));
    }
  }
}

auto EditorViewportRenderer::selectedPrimaryLayer() const -> const LayerState* {
  const auto& interactive = layers_[layerIndex(LayerId::InteractivePrimary)];
  const auto& quality     = layers_[layerIndex(LayerId::QualityBase)];
  if (!interactive.valid && !quality.valid) {
    return nullptr;
  }
  if (!quality.valid) {
    return &interactive;
  }
  if (!interactive.valid) {
    return &quality;
  }
  if (interactive.preview_metadata.presentation_request_id >
      quality.preview_metadata.presentation_request_id) {
    return &interactive;
  }
  if (interactive.preview_metadata.presentation_request_id ==
          quality.preview_metadata.presentation_request_id &&
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
  const float base_aspect   = static_cast<float>(base.width) / static_cast<float>(base.height);
  const float detail_aspect = static_cast<float>(detail.width) / static_cast<float>(detail.height);
  const float roi_w         = std::max(detail.preview_metadata.source_roi_norm.width, 1.0e-4f);
  const float roi_h         = std::max(detail.preview_metadata.source_roi_norm.height, 1.0e-4f);
  const float expected_aspect = base_aspect * (roi_w / roi_h);
  return std::abs(detail_aspect - expected_aspect) <= 0.15f * std::max(expected_aspect, 1.0e-3f);
}

void EditorViewportRenderer::traceDetailDecision(
    const char* decision, const LayerState* detail, const LayerState* base,
    const std::optional<FrameRoiRect>& current_roi) const {
  const std::uint64_t request_id  = detail ? detail->preview_metadata.presentation_request_id : 0;
  const bool          has_roi     = detail && current_roi.has_value();
  const bool          roi_changed = has_roi != last_detail_trace_has_roi_ ||
                           (has_roi && !SameRoi(*current_roi, last_detail_trace_roi_));
  const auto& transform    = view_state_.snapshot.view_transform;
  const bool  view_changed = std::abs(transform.zoom - last_detail_trace_zoom_) > 1.0e-5f ||
                            std::abs(transform.pan.x() - last_detail_trace_pan_x_) > 1.0e-5f ||
                            std::abs(transform.pan.y() - last_detail_trace_pan_y_) > 1.0e-5f;
  if (last_detail_trace_decision_ == decision && last_detail_trace_request_id_ == request_id &&
      !roi_changed && !view_changed) {
    return;
  }
  last_detail_trace_decision_   = decision;
  last_detail_trace_request_id_ = request_id;
  last_detail_trace_has_roi_    = has_roi;
  last_detail_trace_zoom_       = transform.zoom;
  last_detail_trace_pan_x_      = transform.pan.x();
  last_detail_trace_pan_y_      = transform.pan.y();
  if (has_roi) last_detail_trace_roi_ = *current_roi;

  if (!editorPresentLog().isDebugEnabled()) {
    return;
  }
  const auto& quality     = layers_[layerIndex(LayerId::QualityBase)];
  const auto& interactive = layers_[layerIndex(LayerId::InteractivePrimary)];
  QString     msg =
      QStringLiteral(
          "[ROI_TRACE][renderer-decision] decision=%1 detail_request=%2 detail_valid=%3 "
          "quality_request=%4 interactive_request=%5 base_request=%6 image=%7 session_epoch=%8 "
          "view_zoom=%9 view_pan=%10,%11 ref=%12x%13")
          .arg(QLatin1String(decision))
          .arg(request_id)
          .arg((detail && detail->valid) ? 1 : 0)
          .arg(quality.valid ? quality.preview_metadata.presentation_request_id : 0)
          .arg(interactive.valid ? interactive.preview_metadata.presentation_request_id : 0)
          .arg(base ? base->preview_metadata.presentation_request_id : 0)
          .arg(image_identity_)
          .arg(session_epoch_)
          .arg(transform.zoom)
          .arg(transform.pan.x())
          .arg(transform.pan.y())
          .arg(view_state_.snapshot.render_reference_width)
          .arg(view_state_.snapshot.render_reference_height);
  const auto* selected_primary = selectedPrimaryLayer();
  if (selected_primary) {
    msg += QStringLiteral(" selected_primary_size=%1x%2 selected_primary_mode=%3")
               .arg(selected_primary->width)
               .arg(selected_primary->height)
               .arg(static_cast<int>(selected_primary->presentation_mode));
  } else {
    msg += QStringLiteral(" selected_primary_size=none");
  }
  if (base) {
    msg += QStringLiteral(" base_size=%1x%2").arg(base->width).arg(base->height);
  } else {
    msg += QStringLiteral(" base_size=none");
  }
  if (detail) {
    msg += QStringLiteral(" patch_size=%1x%2 patch_roi=%3,%4,%5,%6")
               .arg(detail->width)
               .arg(detail->height)
               .arg(detail->preview_metadata.source_roi_norm.x)
               .arg(detail->preview_metadata.source_roi_norm.y)
               .arg(detail->preview_metadata.source_roi_norm.width)
               .arg(detail->preview_metadata.source_roi_norm.height);
  }
  if (current_roi) {
    msg += QStringLiteral(" current_roi=%1,%2,%3,%4")
               .arg(current_roi->x)
               .arg(current_roi->y)
               .arg(current_roi->width)
               .arg(current_roi->height);
  } else {
    msg += QStringLiteral(" current_roi=none");
  }
  if (const auto& region = view_state_.snapshot.viewport_render_region_cache; region.has_value()) {
    msg += QStringLiteral(" viewport_target=%1x%2 viewport_ref=%3x%4")
               .arg(region->target_width_)
               .arg(region->target_height_)
               .arg(region->reference_width_)
               .arg(region->reference_height_);
  } else {
    msg += QStringLiteral(" viewport_target=none");
  }
  qCDebug(editorPresentLog).noquote() << msg;
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
    traceDetailDecision("detail-disabled", nullptr, nullptr, std::nullopt);
    return false;
  }
  const auto& quality     = layers_[layerIndex(LayerId::QualityBase)];
  const auto& interactive = layers_[layerIndex(LayerId::InteractivePrimary)];
  const auto& detail      = layers_[layerIndex(LayerId::DetailPatch)];
  if (!detail.valid) {
    traceDetailDecision("no-detail-layer", nullptr, nullptr, std::nullopt);
    return false;
  }
  // Base and detail are separate render requests, so their request IDs cannot
  // be equal. Reject only a detail older than the currently selected primary;
  // otherwise composite it over the same-image full-frame quality/interactive
  // layer already filtered by ConsumeNewestReady.
  const auto* newest_primary = selectedPrimaryLayer();
  if (newest_primary && newest_primary->preview_metadata.presentation_request_id >
                            detail.preview_metadata.presentation_request_id) {
    const auto current_roi = BuildNormalizedRoi(view_state_.snapshot.viewport_render_region_cache);
    traceDetailDecision("detail-older-than-primary", &detail, newest_primary, current_roi);
    return false;
  }

  // Prefer QualityBase as the full-frame reference. Fall back to a full-frame
  // InteractivePrimary while the settled quality pass is not available.
  const LayerState* base = nullptr;
  if (quality.valid) {
    base = &quality;
  } else if (interactive.valid &&
             interactive.presentation_mode != FramePresentationMode::RoiFrame) {
    base = &interactive;
  }
  if (!base /*|| !detailPatchAspectOk(detail, *base)*/) {
    const auto current_roi = BuildNormalizedRoi(view_state_.snapshot.viewport_render_region_cache);
    traceDetailDecision("no-full-frame-base", &detail, nullptr, current_roi);
    return false;
  }
  const auto current_roi = BuildNormalizedRoi(view_state_.snapshot.viewport_render_region_cache);
  if (!current_roi) {
    traceDetailDecision("current-roi-missing", &detail, base, current_roi);
    return false;
  }
  if (!SameRoi(detail.preview_metadata.source_roi_norm, *current_roi)) {
    traceDetailDecision("roi-mismatch", &detail, base, current_roi);
    return false;
  }
  traceDetailDecision("visible", &detail, base, current_roi);
  return true;
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
    bound_detail_texture_  = nullptr;
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
  bound_detail_texture_  = detail;
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

  // P0 present split: scene-graph / vsync wait ends when this render pass starts.
  diag::NoteRenderE2eRenderEnter();

  // Targets are producer-driven. Matches the pre-refactor surface: create the
  // exact output slot requested by EnsureSize, with no speculative pool.
  // Metal skips shared-slot allocation and consumes zero-copy imports instead.
  fulfillTargetRequests();
  consumeDirectFrames();
  consumeImportedGpuFrames();
  ensureStaticResources(render_target, command_buffer);
  auto*        updates       = rhi_->nextResourceUpdateBatch();
  consumeHostFrames(updates);

  const auto*  primary_layer = selectedPrimaryLayer();
  const auto*  detail_layer  = selectedDetailLayer();
  QRhiTexture* primary       = primary_layer ? primary_layer->texture : placeholder_texture_;
  QRhiTexture* detail        = detail_layer ? detail_layer->texture : placeholder_texture_;
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
    uniform.pan_mode[0]   = view_state_.snapshot.view_transform.pan.x();
    uniform.pan_mode[1]   = view_state_.snapshot.view_transform.pan.y();
    if (primary_layer->presentation_mode == FramePresentationMode::RoiFrame) {
      uniform.scale_zoom[2] = 1.0f;
      uniform.pan_mode[0]   = 0.0f;
      uniform.pan_mode[1]   = 0.0f;
      uniform.pan_mode[2]   = 1.0f;
    }
  }
  if (detail_layer) {
    const auto& roi         = detail_layer->preview_metadata.source_roi_norm;
    uniform.detail_roi[0]   = roi.x;
    uniform.detail_roi[1]   = roi.y;
    uniform.detail_roi[2]   = roi.width;
    uniform.detail_roi[3]   = roi.height;
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
    const QSize                          size           = render_target->pixelSize();
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
    command_buffer->setShaderResources(shader_resource_bindings_);
    command_buffer->setVertexInput(0, 1, vertex_input);
    command_buffer->draw(4);
  }
  command_buffer->endPass();

  // Composition metrics are renderer-owned diagnostics. They do not feed back
  // into request scheduling; FrameReady already completed the blocking job.
  if (drew_primary && item_->present_queue()) {
    const auto& slot = primary_layer->ready_frame.slot;
    item_->present_queue()->NoteFrameComposed(slot.preview_metadata.presentation_request_id,
                                              slot.session_epoch, slot.image_identity);
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
