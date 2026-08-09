//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/direct_frame_sink.hpp"

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <chrono>
#include <utility>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/diagnostics/render_e2e_timing.hpp"

using alcedo::diag::editorPresentLog;

namespace alcedo::editor_rhi {
namespace {

auto IsRenderReferenceFrame(FramePresentationMode presentation_mode, FrameRole role) -> bool {
  return presentation_mode != FramePresentationMode::RoiFrame && role != FrameRole::DetailPatch;
}

}  // namespace

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

auto DirectFrameSink::AcceptSubmissionRequestId(std::uint64_t request_id) -> bool {
  if (request_id == 0) {
    return true;
  }
  if (request_id < latest_accepted_request_id_) {
    return false;
  }
  latest_accepted_request_id_ = std::max(latest_accepted_request_id_, request_id);
  return true;
}

auto DirectFrameSink::MakeSizeRequestLocked() const -> DirectPresentQueue::SizeRequest {
  DirectPresentQueue::SizeRequest request;
  request.width          = width_;
  request.height         = height_;
  request.preferred_slot = prepared_slot_index_;
  request.session_epoch  = item_ ? item_->sessionEpoch() : 0;
  request.image_identity = item_ ? item_->imageIdentity() : 0;
  if (bound_submission_valid_) {
    request.layer_generation = bound_submission_.metadata.presentation_request_id;
    request.frame_role       = bound_submission_.metadata.frame_role;
  }
  return request;
}

auto DirectFrameSink::ReserveWritableSlot(int width, int height) -> std::optional<int> {
  if (!item_ || !item_->present_queue() || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  auto*      queue          = item_->present_queue().get();
  const auto session_epoch  = item_->sessionEpoch();
  const auto image_identity = item_->imageIdentity();

  auto       prepare        = queue->PrepareWrite(width, height, session_epoch, image_identity);

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

  // Keep render_lock held for the whole frame (configure + GPU + present).
  // Live-pipeline ownership is that single lock: history waits for it (and
  // pumps owner events while waiting so this slot handshake can complete).
  // Do not drop ownership during WaitForWritableSlot — that reopens rebuild
  // races without a second occupancy layer.
  if (!queue->DiagnosticsSnapshot().consumer_available) {
    qCDebug(editorPresentLog,
            "[EditorPresent] producer waiting for consumer %dx%d image=%llu epoch=%llu", width,
            height, static_cast<unsigned long long>(image_identity),
            static_cast<unsigned long long>(session_epoch));
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
          "[EditorPresent] producer waiting for native target %dx%d image=%llu epoch=%llu", width,
          height, static_cast<unsigned long long>(image_identity),
          static_cast<unsigned long long>(session_epoch));
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
  const std::uint64_t session_epoch    = item_->sessionEpoch();
  const std::uint64_t image_identity   = item_->imageIdentity();
  const bool          metal_present    = IsMetalPresentPath();
  bool                emit_target_size = false;
  std::uint64_t       request_id       = 0;
  FrameRole           frame_role       = FrameRole::InteractivePrimary;
  {
    std::lock_guard lock(mutex_);
    if (has_mapped_slot_) {
      return;
    }
    const bool geometry_changed = width_ != width || height_ != height ||
                                  last_sized_session_epoch_ != session_epoch ||
                                  last_sized_image_identity_ != image_identity;
    // DetailPatch / RoiFrame sizes
    // reserve write slots but must not rewrite interaction render-reference
    // geometry. Otherwise a zoomed ROI EnsureSize (e.g. 1600x900) overwrites the
    // full-frame reference (QualityBase / InteractivePrimary) used for zoom,
    // pan, and SameRoi matching — the high-res detail patch then fails to cover
    // the view.
    bool is_render_reference = true;
    if (bound_submission_valid_) {
      is_render_reference =
          IsRenderReferenceFrame(bound_submission_.mode, bound_submission_.metadata.frame_role);
      request_id = bound_submission_.metadata.presentation_request_id;
      frame_role = bound_submission_.metadata.frame_role;
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
    last_sized_session_epoch_  = session_epoch;
    last_sized_image_identity_ = image_identity;
  }

  if (emit_target_size) {
    qCDebug(editorPresentLog) << "[ROI_TRACE][render-reference-size] request=" << request_id
                              << " role=" << static_cast<int>(frame_role) << " size=" << width
                              << 'x' << height << " image=" << image_identity
                              << " session_epoch=" << session_epoch;
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

void DirectFrameSink::BindFrameSubmission(const FrameCompletionSubmission& submission) {
  std::lock_guard lock(mutex_);
  bound_submission_       = submission;
  bound_submission_valid_ = true;
}

void DirectFrameSink::NotifyFrameReady(const FrameCompletionSubmission& submission) {
  int                   slot_index = -1;
  FramePresentationMode mode       = submission.mode;
  FramePreviewMetadata  metadata   = submission.metadata;
  {
    std::lock_guard lock(mutex_);
    if (!has_mapped_slot_ || !unmapped_pending_submit_) {
      if (metadata.frame_role == FrameRole::DetailPatch) {
        qCDebug(editorPresentLog) << "[ROI_TRACE][sink-drop] request="
                                  << metadata.presentation_request_id
                                  << " reason=not-mapped-or-not-unmapped mapped="
                                  << has_mapped_slot_
                                  << " unmap_pending=" << unmapped_pending_submit_;
      }
      diag::NoteRenderE2eTerminal(metadata.presentation_request_id, "sink-not-mapped");
      return;
    }
    if (!AcceptSubmissionRequestId(metadata.presentation_request_id)) {
      if (metadata.frame_role == FrameRole::DetailPatch) {
        qCDebug(editorPresentLog) << "[ROI_TRACE][sink-drop] request="
                                  << metadata.presentation_request_id << " reason=stale-request-id";
      }
      has_mapped_slot_         = false;
      unmapped_pending_submit_ = false;
      mapped_slot_index_       = -1;
      diag::NoteRenderE2eTerminal(metadata.presentation_request_id, "stale-request-id");
      return;
    }
    slot_index               = mapped_slot_index_;
    has_mapped_slot_         = false;
    unmapped_pending_submit_ = false;
    mapped_slot_index_       = -1;
    bound_submission_valid_  = false;
    ++submitted_frame_count_;
  }
  if (!item_ || !item_->present_queue() || slot_index < 0) {
    if (metadata.frame_role == FrameRole::DetailPatch) {
      qCDebug(editorPresentLog) << "[ROI_TRACE][sink-drop] request="
                                << metadata.presentation_request_id
                                << " reason=no-item-queue-or-slot slot=" << slot_index;
    }
    diag::NoteRenderE2eTerminal(metadata.presentation_request_id, "sink-no-queue");
    return;
  }
  if (metadata.frame_role == FrameRole::DetailPatch) {
    qCDebug(editorPresentLog) << "[ROI_TRACE][sink-notify-ready] request="
                              << metadata.presentation_request_id
                              << " image=" << item_->imageIdentity()
                              << " session_epoch=" << item_->sessionEpoch()
                              << " slot=" << slot_index
                              << " roi_norm=" << metadata.source_roi_norm.x << ','
                              << metadata.source_roi_norm.y << ',' << metadata.source_roi_norm.width
                              << ',' << metadata.source_roi_norm.height;
  }
  item_->present_queue()->NotifyReady(slot_index, mode, metadata);
  diag::NoteRenderE2eProducerReady(metadata.presentation_request_id);
  qCDebug(editorPresentLog,
          "[EditorPresent] submitted frame request=%llu image=%llu epoch=%llu slot=%d",
          static_cast<unsigned long long>(metadata.presentation_request_id),
          static_cast<unsigned long long>(item_->imageIdentity()),
          static_cast<unsigned long long>(item_->sessionEpoch()), slot_index);
  item_->requestPresentUpdate();
  diag::NoteRenderE2ePresentWake(metadata.presentation_request_id);
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
  imported.session_epoch              = item_->sessionEpoch();
  imported.image_identity             = item_->imageIdentity();

  std::uint64_t request_id            = 0;
  bool          emit_render_reference = false;
  int           ref_w                 = 0;
  int           ref_h                 = 0;
  {
    std::lock_guard lock(mutex_);
    if (!AcceptSubmissionRequestId(imported.preview_metadata.presentation_request_id)) {
      diag::NoteRenderE2eTerminal(imported.preview_metadata.presentation_request_id,
                                  "stale-metal-request");
      return;
    }
    request_id        = imported.preview_metadata.presentation_request_id;
    imported.sequence = ++imported_sequence_;
    const auto layer  = LayerIndexForRole(imported.preview_metadata.frame_role);
    if (pending_imported_[layer].has_value()) {
      const auto superseded = pending_imported_[layer]->preview_metadata.presentation_request_id;
      qCDebug(editorPresentLog,
              "[EditorPresent] superseding pending Metal import role=%d request=%llu",
              static_cast<int>(imported.preview_metadata.frame_role),
              static_cast<unsigned long long>(superseded));
      diag::NoteRenderE2eTerminal(superseded, "superseded-metal-import");
    }
    pending_imported_[layer] = imported;
    ++submitted_frame_count_;

    // Only full-frame textures
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

  diag::NoteRenderE2eProducerReady(request_id);
  qCDebug(editorPresentLog,
          "[EditorPresent] queued Metal import request=%llu image=%llu epoch=%llu size=%dx%d "
          "handle=%llu (zero-copy)",
          static_cast<unsigned long long>(request_id),
          static_cast<unsigned long long>(item_->imageIdentity()),
          static_cast<unsigned long long>(item_->sessionEpoch()), frame.width, frame.height,
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
  diag::NoteRenderE2ePresentWake(request_id);
}
#endif

void DirectFrameSink::SubmitFinalDisplayFrame(const FinalDisplayFrameView&) {
  // Scope taps remain responsible for analyzer paths; presentation is direct GPU only.
}

auto DirectFrameSink::DrainPendingImportedFrames(std::uint64_t session_epoch,
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
    if (session_epoch != 0 && slot->session_epoch != 0 && slot->session_epoch != session_epoch) {
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

auto DirectFrameSink::ViewState() const -> ViewerViewState {
  std::lock_guard lock(mutex_);
  return view_state_;
}

void DirectFrameSink::SetViewState(const ViewerViewState& state) {
  std::lock_guard lock(mutex_);
  view_state_ = state;
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
  return item_->present_queue()->HasWritableSlot(width_, height_, item_->sessionEpoch(),
                                                 item_->imageIdentity());
}

auto DirectFrameSink::submitted_frame_count() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return submitted_frame_count_;
}

auto DirectFrameSink::latest_accepted_request_id() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return latest_accepted_request_id_;
}

}  // namespace alcedo::editor_rhi
