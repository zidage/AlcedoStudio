//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/direct_frame_sink.hpp"

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <chrono>
#include <utility>

#include "ui/edit_viewer/edit_viewer_surface.hpp"
#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "utils/diagnostics/app_logging.hpp"

using alcedo::diag::editorPresentLog;

namespace alcedo::editor_rhi {

DirectFrameSink::DirectFrameSink(EditorViewportItem* item) : item_(item) {}

DirectFrameSink::~DirectFrameSink() {
  ClearMappedSlot();
  ClearPendingImportedFrames();
}

auto DirectFrameSink::LayerIndexForRole(FrameRole role) -> std::size_t {
  switch (role) {
    case FrameRole::QualityBase:
      return 1;
    case FrameRole::DetailPatch:
      return 2;
    case FrameRole::InteractivePrimary:
    default:
      return 0;
  }
}

auto DirectFrameSink::IsMetalPresentPath() const -> bool {
  if (!item_ || !item_->present_queue()) {
    return ActiveEditorBackend() == EditorBackend::Metal;
  }
  return item_->present_queue()->backend() == EditorBackend::Metal;
}

void DirectFrameSink::ClearMappedSlot() {
  std::lock_guard lock(mutex_);
  if (has_mapped_slot_ && item_ && item_->present_queue()) {
    item_->present_queue()->AbandonWrite(mapped_slot_index_);
  }
  has_mapped_slot_         = false;
  unmapped_pending_submit_ = false;
  mapped_slot_index_       = -1;
  prepared_slot_index_     = -1;
}

auto DirectFrameSink::MakeSizeRequestLocked() const -> DirectPresentQueue::SizeRequest {
  DirectPresentQueue::SizeRequest request;
  request.width            = width_;
  request.height           = height_;
  request.preferred_slot   = prepared_slot_index_;
  request.image_generation = item_ ? item_->imageGeneration() : 0;
  request.image_identity   = item_ ? item_->imageIdentity() : 0;
  if (pending_preview_metadata_valid_) {
    request.layer_generation = pending_preview_metadata_.preview_generation;
    request.frame_role       = pending_preview_metadata_.frame_role;
  }
  return request;
}

auto DirectFrameSink::ReserveWritableSlot(int width, int height) -> std::optional<int> {
  if (!item_ || !item_->present_queue() || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  auto*      queue            = item_->present_queue().get();
  const auto image_generation = item_->imageGeneration();
  const auto image_identity   = item_->imageIdentity();

  auto       prepare = queue->PrepareWrite(width, height, image_generation, image_identity);

  DirectPresentQueue::SizeRequest request;
  {
    std::lock_guard lock(mutex_);
    width_  = width;
    height_ = height;
    request = MakeSizeRequestLocked();
    if (prepare.ok && prepare.slot_index >= 0) {
      request.preferred_slot = prepare.slot_index;
    }
  }

  if (prepare.ok && !prepare.need_create) {
    return prepare.slot_index;
  }

  // Legacy BlockingQueuedConnection resize equivalent for QQuickRhiItem:
  // note the exact size, kick the scene-graph thread, wait for explicit result.
  // GUI-thread producers must not block — Qt Quick only advances the scene
  // graph after the GUI event handler returns.
  const bool gui_thread_caller = item_->thread() == QThread::currentThread();
  if (gui_thread_caller) {
    queue->NoteSizeRequest(request);
    item_->requestPresentUpdate();
    return std::nullopt;
  }

  queue->NoteSizeRequest(request);
  item_->requestPresentUpdate();

  if (!queue->DiagnosticsSnapshot().consumer_available) {
    qCDebug(editorPresentLog,
            "[EditorPresent] producer waiting for consumer %dx%d image=%llu generation=%llu", width,
            height, static_cast<unsigned long long>(image_identity),
            static_cast<unsigned long long>(image_generation));
    constexpr auto kSceneGraphStartupTimeout = std::chrono::seconds(5);
    if (!queue->WaitUntilConsumerAvailable(kSceneGraphStartupTimeout)) {
      qCWarning(editorPresentLog,
                "[EditorPresent] native target handshake timed out %dx%d waiting for the "
                "scene-graph consumer",
                width, height);
      return std::nullopt;
    }
  }

  qCDebug(editorPresentLog,
          "[EditorPresent] producer waiting for native target %dx%d image=%llu generation=%llu",
          width, height, static_cast<unsigned long long>(image_identity),
          static_cast<unsigned long long>(image_generation));
  auto slot = queue->WaitForWritableSlot(request);
  if (!slot.has_value()) {
    qCWarning(editorPresentLog, "[EditorPresent] native target handshake failed %dx%d", width,
              height);
    return std::nullopt;
  }
  qCInfo(editorPresentLog,
         "[EditorPresent] producer acquired native target %dx%d slot=%d backend=%s", width, height,
         *slot, ToString(queue->backend()));
  return slot;
}

void DirectFrameSink::EnsureSize(int width, int height) {
  if (width <= 0 || height <= 0 || !item_) {
    return;
  }
  const std::uint64_t image_generation = item_->imageGeneration();
  const std::uint64_t image_identity   = item_->imageIdentity();
  const bool          metal_present    = IsMetalPresentPath();
  bool                emit_target_size = false;
  {
    std::lock_guard lock(mutex_);
    if (has_mapped_slot_) {
      return;
    }
    const bool geometry_changed = width_ != width || height_ != height ||
                                  last_sized_image_generation_ != image_generation ||
                                  last_sized_image_identity_ != image_identity;
    // Match QtEditViewer::IsRenderReferenceFrame: DetailPatch / RoiFrame sizes
    // reserve write slots but must not rewrite interaction render-reference
    // geometry. Otherwise a zoomed ROI EnsureSize (e.g. 1600x900) overwrites the
    // full-frame reference (QualityBase / InteractivePrimary) used for zoom,
    // pan, and SameRoi matching — the high-res detail patch then fails to cover
    // the view.
    bool is_render_reference = true;
    if (pending_preview_metadata_valid_) {
      const FramePresentationMode mode = pending_presentation_mode_valid_
                                             ? pending_presentation_mode_
                                             : FramePresentationMode::FullFrame;
      is_render_reference = IsRenderReferenceFrame(mode, pending_preview_metadata_.frame_role);
    }
    // Metal zero-copy: production EnsureSize uses the presentation *viewport*
    // size, not the pipeline MTLTexture size. Emitting that as the render
    // reference rewrites zoom/pan math to the viewport aspect and causes FIT
    // snaps / ROI thrash when a real frame arrives. Publish render-reference
    // geometry from SubmitMetalFrame with the real texture size instead.
    emit_target_size = geometry_changed && is_render_reference && !metal_present;
    // CUDA/OpenCL always track the requested write size. On Metal, only track
    // full-frame requests so Detail/Roi sizes cannot poison later change
    // detection (actual ref size is set when the MTLTexture is submitted).
    if (!metal_present || is_render_reference) {
      width_  = width;
      height_ = height;
    }
    last_sized_image_generation_ = image_generation;
    last_sized_image_identity_   = image_identity;
  }

  if (emit_target_size) {
    // Pipeline workers are off-thread; unit tests and GUI-thread callers can
    // receive the geometry signal synchronously.
    const auto connection =
        (item_->thread() == QThread::currentThread()) ? Qt::DirectConnection : Qt::QueuedConnection;
    QMetaObject::invokeMethod(
        item_, [item = item_, width, height] { emit item->targetSizeRequested(width, height); },
        connection);
  }

  // Metal presents by importing the pipeline's own MTLTexture (SubmitMetalFrame).
  // Shared-slot allocation is CUDA/OpenCL only — skip the failed handshake.
  if (metal_present) {
    std::lock_guard lock(mutex_);
    prepared_slot_index_ = -1;
    return;
  }

  auto            slot = ReserveWritableSlot(width, height);
  std::lock_guard lock(mutex_);
  prepared_slot_index_ = slot.value_or(-1);
}

auto DirectFrameSink::MapResourceForWrite(FrameMemoryDomain preferred_domain) -> FrameWriteMapping {
  if (!item_ || !item_->present_queue()) {
    qCWarning(editorPresentLog, "[EditorPresent] MapResourceForWrite: no item/present_queue");
    return {};
  }

  int width      = 0;
  int height     = 0;
  int slot_index = -1;
  {
    std::lock_guard lock(mutex_);
    if (has_mapped_slot_ || width_ <= 0 || height_ <= 0) {
      qCWarning(editorPresentLog,
                "[EditorPresent] MapResourceForWrite: invalid state mapped=%d size=%dx%d",
                has_mapped_slot_ ? 1 : 0, width_, height_);
      return {};
    }
    width                = width_;
    height               = height_;
    slot_index           = prepared_slot_index_;
    prepared_slot_index_ = -1;
  }

  if (slot_index < 0) {
    auto reserved = ReserveWritableSlot(width, height);
    if (!reserved.has_value()) {
      qCWarning(editorPresentLog,
                "[EditorPresent] MapResourceForWrite: no writable slot %dx%d domain=%d "
                "active_backend=%s queue_backend=%s",
                width, height, static_cast<int>(preferred_domain), ToString(ActiveEditorBackend()),
                ToString(item_->present_queue()->backend()));
      return {};
    }
    slot_index = *reserved;
  }

  auto begun = item_->present_queue()->BeginWrite(slot_index);
  if (!begun.has_value() || !begun->native.valid()) {
    auto reserved = ReserveWritableSlot(width, height);
    if (!reserved.has_value()) {
      qCWarning(editorPresentLog,
                "[EditorPresent] MapResourceForWrite: BeginWrite/reserve failed slot=%d %dx%d",
                slot_index, width, height);
      return {};
    }
    slot_index = *reserved;
    begun      = item_->present_queue()->BeginWrite(slot_index);
    if (!begun.has_value() || !begun->native.valid()) {
      qCWarning(editorPresentLog,
                "[EditorPresent] MapResourceForWrite: BeginWrite failed after reserve slot=%d",
                slot_index);
      return {};
    }
  }

  // Synthetic lease for shared OpenCL acquire helpers.
  WritableTargetLease lease;
  lease.backend           = begun->native.backend;
  lease.handle_kind       = begun->native.handle_kind;
  lease.writable_kind     = begun->native.writable_kind;
  lease.dimensions        = {begun->width, begun->height};
  lease.native_handle     = begun->native.native_handle;
  lease.writable_resource = begun->native.writable_resource;
  lease.sync_object       = begun->native.sync_object;
  lease.sync_value        = begun->native.sync_value;
  lease.lifetime_token    = std::make_shared<LeaseLifetimeToken>();

  if (!ProducerAcquireWritable(lease)) {
    qCWarning(editorPresentLog,
              "[EditorPresent] MapResourceForWrite: ProducerAcquireWritable failed kind=%d",
              static_cast<int>(lease.writable_kind));
    item_->present_queue()->AbandonWrite(slot_index);
    return {};
  }

  FrameWriteMapping mapping{};
  mapping.row_bytes     = static_cast<size_t>(begun->width) * sizeof(float) * 4ULL;
  mapping.pixel_format  = FramePixelFormat::RGBA32F;
  mapping.native_object = begun->native.native_handle;

  if (begun->native.writable_kind == LeaseWritableResourceKind::CudaArray) {
    if (preferred_domain != FrameMemoryDomain::CudaDevice &&
        preferred_domain != FrameMemoryDomain::HostVisible) {
      qCWarning(editorPresentLog,
                "[EditorPresent] MapResourceForWrite: domain mismatch (slot=CudaArray, "
                "preferred=%d) — pipeline backend and Qt RHI/present backend disagree",
                static_cast<int>(preferred_domain));
      (void)ProducerReleaseWritable(lease);
      item_->present_queue()->AbandonWrite(slot_index);
      return {};
    }
    mapping.image_array   = reinterpret_cast<void*>(begun->native.writable_resource);
    mapping.memory_domain = FrameMemoryDomain::CudaDevice;
    mapping.target_type   = FrameWriteTargetType::CudaArray;
  } else if (begun->native.writable_kind == LeaseWritableResourceKind::OpenClImage) {
    if (preferred_domain != FrameMemoryDomain::OpenClDevice &&
        preferred_domain != FrameMemoryDomain::HostVisible) {
      qCWarning(editorPresentLog,
                "[EditorPresent] MapResourceForWrite: domain mismatch (slot=OpenClImage, "
                "preferred=%d)",
                static_cast<int>(preferred_domain));
      (void)ProducerReleaseWritable(lease);
      item_->present_queue()->AbandonWrite(slot_index);
      return {};
    }
    mapping.data          = reinterpret_cast<void*>(begun->native.writable_resource);
    mapping.memory_domain = FrameMemoryDomain::OpenClDevice;
    mapping.target_type   = FrameWriteTargetType::OpenClImage;
  } else {
    qCWarning(editorPresentLog, "[EditorPresent] MapResourceForWrite: unsupported writable_kind=%d",
              static_cast<int>(begun->native.writable_kind));
    (void)ProducerReleaseWritable(lease);
    item_->present_queue()->AbandonWrite(slot_index);
    return {};
  }

  std::lock_guard lock(mutex_);
  mapped_slot_index_       = slot_index;
  has_mapped_slot_         = true;
  unmapped_pending_submit_ = false;
  return mapping;
}

void DirectFrameSink::UnmapResource() {
  int slot_index = -1;
  {
    std::lock_guard lock(mutex_);
    if (!has_mapped_slot_ || !item_ || !item_->present_queue()) {
      return;
    }
    slot_index = mapped_slot_index_;
  }

  auto snap = item_->present_queue()->SlotAt(slot_index);
  if (snap.has_value()) {
    WritableTargetLease lease;
    lease.backend           = snap->native.backend;
    lease.handle_kind       = snap->native.handle_kind;
    lease.writable_kind     = snap->native.writable_kind;
    lease.dimensions        = {snap->width, snap->height};
    lease.native_handle     = snap->native.native_handle;
    lease.writable_resource = snap->native.writable_resource;
    lease.sync_object       = snap->native.sync_object;
    lease.sync_value        = snap->native.sync_value;
    lease.lifetime_token    = std::make_shared<LeaseLifetimeToken>();
    (void)ProducerReleaseWritable(lease);
    // CUDA's producer already synchronizes its dedicated render stream before
    // UnmapResource. A second cudaDeviceSynchronize here stalls on unrelated
    // thumbnail/import streams and made every slider frame pay a global GPU
    // barrier. OpenCL release remains asynchronous and still needs completion
    // before Qt samples the shared GL texture.
    if (lease.writable_kind == LeaseWritableResourceKind::OpenClImage) {
      (void)ProducerWaitWritableComplete(lease);
    }
  }
  item_->present_queue()->EndWrite(slot_index);

  std::lock_guard lock(mutex_);
  unmapped_pending_submit_ = true;
}

void DirectFrameSink::NotifyFrameReady() {
  int                   slot_index = -1;
  FramePresentationMode mode       = FramePresentationMode::FullFrame;
  FramePreviewMetadata  metadata{};
  {
    std::lock_guard lock(mutex_);
    if (!has_mapped_slot_ || !unmapped_pending_submit_) {
      return;
    }
    slot_index = mapped_slot_index_;
    if (pending_presentation_mode_valid_) {
      mode                             = pending_presentation_mode_;
      pending_presentation_mode_valid_ = false;
    }
    if (pending_preview_metadata_valid_) {
      metadata                        = pending_preview_metadata_;
      pending_preview_metadata_valid_ = false;
    }
    has_mapped_slot_         = false;
    unmapped_pending_submit_ = false;
    mapped_slot_index_       = -1;
    ++submitted_frame_count_;
  }
  if (!item_ || !item_->present_queue() || slot_index < 0) {
    return;
  }
  item_->present_queue()->NotifyReady(slot_index, mode, metadata);
  qCDebug(editorPresentLog,
          "[EditorPresent] submitted frame request=%llu image=%llu generation=%llu slot=%d",
          static_cast<unsigned long long>(metadata.presentation_request_id),
          static_cast<unsigned long long>(item_->imageIdentity()),
          static_cast<unsigned long long>(item_->imageGeneration()), slot_index);
  item_->requestPresentUpdate();
}

void DirectFrameSink::SubmitHostFrame(const ViewerFrame&) {
  // Intentionally empty: production presentation has no CPU-upload fallback.
}

#ifdef HAVE_METAL
void DirectFrameSink::SubmitMetalFrame(const ViewerMetalFrame& frame) {
  if (!frame || !item_) {
    qCWarning(editorPresentLog, "[EditorPresent] SubmitMetalFrame rejected: invalid frame or item");
    return;
  }
  if (!IsMetalPresentPath()) {
    qCWarning(editorPresentLog,
              "[EditorPresent] SubmitMetalFrame ignored: active backend is not Metal");
    return;
  }
  // The CAMetalLayer belongs to the complete QML window, not to this item.
  // Forward the producer's exact output encoding to the GUI-thread owner
  // before requesting composition.
  item_->setDisplayConfig(frame.display_config);

  ImportedGpuFrame imported;
  imported.width                      = frame.width;
  imported.height                     = frame.height;
  imported.texture_handle             = frame.texture_handle;
  imported.native_layout              = 0;
  imported.owner                      = frame.owner;
  imported.presentation_mode          = frame.presentation_mode;
  imported.preview_metadata           = frame.preview_metadata;
  imported.image_generation           = item_->imageGeneration();
  imported.image_identity             = item_->imageIdentity();

  std::uint64_t request_id            = 0;
  bool          emit_render_reference = false;
  int           ref_w                 = 0;
  int           ref_h                 = 0;
  {
    std::lock_guard lock(mutex_);
    // Prefer the session-stamped role/ROI/request id over pipeline defaults.
    if (pending_presentation_mode_valid_) {
      imported.presentation_mode       = pending_presentation_mode_;
      pending_presentation_mode_valid_ = false;
    }
    if (pending_preview_metadata_valid_) {
      imported.preview_metadata       = pending_preview_metadata_;
      pending_preview_metadata_valid_ = false;
    }
    request_id        = imported.preview_metadata.presentation_request_id;
    imported.sequence = ++imported_sequence_;
    const auto layer  = LayerIndexForRole(imported.preview_metadata.frame_role);
    if (pending_imported_[layer].has_value()) {
      qCDebug(editorPresentLog,
              "[EditorPresent] superseding pending Metal import role=%d request=%llu",
              static_cast<int>(imported.preview_metadata.frame_role),
              static_cast<unsigned long long>(
                  pending_imported_[layer]->preview_metadata.presentation_request_id));
    }
    pending_imported_[layer] = imported;
    ++submitted_frame_count_;

    // Match QtEditViewer::RefreshFrameDerivedState: only full-frame textures
    // redefine crop/zoom render-reference geometry. Detail/Roi sizes must not.
    if (IsRenderReferenceFrame(imported.presentation_mode, imported.preview_metadata.frame_role)) {
      const bool geometry_changed = width_ != frame.width || height_ != frame.height;
      width_                      = frame.width;
      height_                     = frame.height;
      if (geometry_changed) {
        emit_render_reference = true;
        ref_w                 = frame.width;
        ref_h                 = frame.height;
      }
    }
  }

  qCDebug(editorPresentLog,
          "[EditorPresent] queued Metal import request=%llu image=%llu generation=%llu size=%dx%d "
          "handle=%llu (zero-copy)",
          static_cast<unsigned long long>(request_id),
          static_cast<unsigned long long>(item_->imageIdentity()),
          static_cast<unsigned long long>(item_->imageGeneration()), frame.width, frame.height,
          static_cast<unsigned long long>(frame.texture_handle));

  if (emit_render_reference) {
    // Publish the real MTLTexture size as the interaction render reference
    // (not the presentation viewport size from EnsureSize).
    const auto connection =
        (item_->thread() == QThread::currentThread()) ? Qt::DirectConnection : Qt::QueuedConnection;
    QMetaObject::invokeMethod(
        item_, [item = item_, ref_w, ref_h] { emit item->targetSizeRequested(ref_w, ref_h); },
        connection);
  }
  item_->requestPresentUpdate();
}
#endif

void DirectFrameSink::SubmitFinalDisplayFrame(const FinalDisplayFrameView&) {
  // Scope taps remain responsible for analyzer paths; presentation is direct GPU only.
}

auto DirectFrameSink::DrainPendingImportedFrames(std::uint64_t image_generation,
                                                 std::uint64_t image_identity)
    -> std::vector<ImportedGpuFrame> {
  std::lock_guard               lock(mutex_);
  std::vector<ImportedGpuFrame> out;
  out.reserve(pending_imported_.size());
  for (auto& slot : pending_imported_) {
    if (!slot.has_value() || !slot->valid()) {
      slot.reset();
      continue;
    }
    if (image_generation != 0 && slot->image_generation != 0 &&
        slot->image_generation != image_generation) {
      slot.reset();
      continue;
    }
    if (image_identity != 0 && slot->image_identity != 0 &&
        slot->image_identity != image_identity) {
      slot.reset();
      continue;
    }
    out.push_back(std::move(*slot));
    slot.reset();
  }
  return out;
}

void DirectFrameSink::ClearPendingImportedFrames() {
  std::lock_guard lock(mutex_);
  for (auto& slot : pending_imported_) {
    slot.reset();
  }
}

auto DirectFrameSink::GetWidth() const -> int {
  std::lock_guard lock(mutex_);
  return width_;
}

auto DirectFrameSink::GetHeight() const -> int {
  std::lock_guard lock(mutex_);
  return height_;
}

auto DirectFrameSink::GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> {
  std::lock_guard lock(mutex_);
  return view_state_.snapshot.viewport_render_region_cache;
}

void DirectFrameSink::SetNextFramePresentationMode(FramePresentationMode mode) {
  std::lock_guard lock(mutex_);
  pending_presentation_mode_       = mode;
  pending_presentation_mode_valid_ = true;
}

void DirectFrameSink::SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) {
  std::lock_guard lock(mutex_);
  pending_preview_metadata_       = metadata;
  pending_preview_metadata_valid_ = true;
}

auto DirectFrameSink::ViewState() const -> ViewerViewState {
  std::lock_guard lock(mutex_);
  return view_state_;
}

void DirectFrameSink::SetViewState(const ViewerViewState& state) {
  std::lock_guard lock(mutex_);
  view_state_ = state;
}

void DirectFrameSink::SetFirstFrameCompositionCallback(FirstFrameCompositionCallback callback) {
  std::lock_guard lock(mutex_);
  first_frame_composition_ = std::move(callback);
}

void DirectFrameSink::NotifyPrimaryFrameComposed(const DirectPresentQueue::ReadyFrame& frame) {
  FirstFrameCompositionCallback callback;
  const auto                    request_id = frame.slot.preview_metadata.presentation_request_id;
  const auto                    image_generation = frame.slot.image_generation;
  const auto                    image_identity   = frame.slot.image_identity;
  {
    std::lock_guard lock(mutex_);
    callback = first_frame_composition_;
  }
  if (!item_ || !item_->present_queue()) {
    return;
  }
  // Diagnostics: every composed primary frame is counted.
  item_->present_queue()->NoteFrameComposed(request_id, image_generation, image_identity);
  // Session first-frame service: exactly one composition confirmation.
  if (item_->present_queue()->AcknowledgeFirstComposition(request_id, image_generation,
                                                          image_identity)) {
    qCInfo(editorPresentLog,
           "[EditorPresent] first frame composed request=%llu image=%llu generation=%llu "
           "backend=%s",
           static_cast<unsigned long long>(request_id),
           static_cast<unsigned long long>(image_identity),
           static_cast<unsigned long long>(image_generation),
           ToString(item_->present_queue()->backend()));
    if (callback) {
      callback(request_id, image_generation, image_identity);
    }
  }
  item_->notifyDiagnosticsChanged();
}

auto DirectFrameSink::HasWritableTargetForNextFrame() const -> bool {
  if (!item_) {
    return false;
  }
  // Metal does not pre-allocate shared write targets; a submit is always possible
  // once geometry is known and the consumer is live.
  if (IsMetalPresentPath()) {
    std::lock_guard lock(mutex_);
    return width_ > 0 && height_ > 0;
  }
  if (!item_->present_queue()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return item_->present_queue()->HasWritableSlot(width_, height_, item_->imageGeneration(),
                                                 item_->imageIdentity());
}

auto DirectFrameSink::submitted_frame_count() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return submitted_frame_count_;
}

}  // namespace alcedo::editor_rhi
