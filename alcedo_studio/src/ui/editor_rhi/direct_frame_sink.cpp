//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/direct_frame_sink.hpp"

#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"

#include <QDebug>
#include <QMetaObject>
#include <QThread>

namespace alcedo::editor_rhi {

DirectFrameSink::DirectFrameSink(EditorViewportItem* item) : item_(item) {}

DirectFrameSink::~DirectFrameSink() {
  ClearMappedSlot();
}

void DirectFrameSink::ClearMappedSlot() {
  std::lock_guard lock(mutex_);
  if (has_mapped_slot_ && item_ && item_->present_queue()) {
    item_->present_queue()->AbandonWrite(mapped_slot_index_);
  }
  has_mapped_slot_ = false;
  unmapped_pending_submit_ = false;
  mapped_slot_index_ = -1;
  prepared_slot_index_ = -1;
}

auto DirectFrameSink::MakeSizeRequestLocked() const -> DirectPresentQueue::SizeRequest {
  DirectPresentQueue::SizeRequest request;
  request.width = width_;
  request.height = height_;
  request.preferred_slot = prepared_slot_index_;
  request.image_generation = item_ ? item_->imageGeneration() : 0;
  request.image_identity = item_ ? item_->imageIdentity() : 0;
  if (pending_preview_metadata_valid_) {
    request.layer_generation = pending_preview_metadata_.preview_generation;
    request.frame_role = pending_preview_metadata_.frame_role;
  }
  return request;
}

auto DirectFrameSink::ReserveWritableSlot(int width, int height) -> std::optional<int> {
  if (!item_ || !item_->present_queue() || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  auto* queue = item_->present_queue().get();
  const auto image_generation = item_->imageGeneration();
  const auto image_identity = item_->imageIdentity();

  auto prepare = queue->PrepareWrite(width, height, image_generation, image_identity);

  DirectPresentQueue::SizeRequest request;
  {
    std::lock_guard lock(mutex_);
    width_ = width;
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
  // When the consumer is not available (no exposed window / tests without a
  // scene graph), do not block — geometry notification has already been queued.
  // Qt Quick can only advance synchronization/render after the GUI event
  // handler returns. A GUI-thread producer must therefore request allocation
  // and retry later; waiting here would block the very frame that creates it.
  const bool gui_thread_caller = item_->thread() == QThread::currentThread();
  if (gui_thread_caller || !queue->DiagnosticsSnapshot().consumer_available) {
    queue->NoteSizeRequest(request);
    item_->requestPresentUpdate();
    return std::nullopt;
  }
  queue->NoteSizeRequest(request);
  item_->requestPresentUpdate();
  qInfo("[EditorPresent] producer waiting for native target %dx%d image=%llu generation=%llu",
        width, height, static_cast<unsigned long long>(image_identity),
        static_cast<unsigned long long>(image_generation));
  auto slot = queue->WaitForWritableSlot(request);
  if (!slot.has_value()) {
    qWarning("[EditorPresent] native target handshake failed %dx%d", width, height);
    return std::nullopt;
  }
  qInfo("[EditorPresent] producer acquired native target %dx%d slot=%d", width, height, *slot);
  return slot;
}

void DirectFrameSink::EnsureSize(int width, int height) {
  if (width <= 0 || height <= 0 || !item_) {
    return;
  }
  const std::uint64_t image_generation = item_->imageGeneration();
  const std::uint64_t image_identity = item_->imageIdentity();
  bool geometry_changed = false;
  {
    std::lock_guard lock(mutex_);
    if (has_mapped_slot_) {
      return;
    }
    geometry_changed = width_ != width || height_ != height ||
                       last_sized_image_generation_ != image_generation ||
                       last_sized_image_identity_ != image_identity;
    width_ = width;
    height_ = height;
    last_sized_image_generation_ = image_generation;
    last_sized_image_identity_ = image_identity;
  }

  if (geometry_changed) {
    // Pipeline workers are off-thread; unit tests and GUI-thread callers can
    // receive the geometry signal synchronously.
    const auto connection = (item_->thread() == QThread::currentThread())
                                ? Qt::DirectConnection
                                : Qt::QueuedConnection;
    QMetaObject::invokeMethod(
        item_, [item = item_, width, height] { emit item->targetSizeRequested(width, height); },
        connection);
  }

  auto slot = ReserveWritableSlot(width, height);
  std::lock_guard lock(mutex_);
  prepared_slot_index_ = slot.value_or(-1);
}

auto DirectFrameSink::MapResourceForWrite(FrameMemoryDomain preferred_domain)
    -> FrameWriteMapping {
  if (!item_ || !item_->present_queue()) {
    return {};
  }

  int width = 0;
  int height = 0;
  int slot_index = -1;
  {
    std::lock_guard lock(mutex_);
    if (has_mapped_slot_ || width_ <= 0 || height_ <= 0) {
      return {};
    }
    width = width_;
    height = height_;
    slot_index = prepared_slot_index_;
    prepared_slot_index_ = -1;
  }

  if (slot_index < 0) {
    auto reserved = ReserveWritableSlot(width, height);
    if (!reserved.has_value()) {
      return {};
    }
    slot_index = *reserved;
  }

  auto begun = item_->present_queue()->BeginWrite(slot_index);
  if (!begun.has_value() || !begun->native.valid()) {
    auto reserved = ReserveWritableSlot(width, height);
    if (!reserved.has_value()) {
      return {};
    }
    slot_index = *reserved;
    begun = item_->present_queue()->BeginWrite(slot_index);
    if (!begun.has_value() || !begun->native.valid()) {
      return {};
    }
  }

  // Synthetic lease for shared OpenCL acquire helpers.
  WritableTargetLease lease;
  lease.backend = begun->native.backend;
  lease.handle_kind = begun->native.handle_kind;
  lease.writable_kind = begun->native.writable_kind;
  lease.dimensions = {begun->width, begun->height};
  lease.native_handle = begun->native.native_handle;
  lease.writable_resource = begun->native.writable_resource;
  lease.sync_object = begun->native.sync_object;
  lease.sync_value = begun->native.sync_value;
  lease.lifetime_token = std::make_shared<LeaseLifetimeToken>();

  if (!ProducerAcquireWritable(lease)) {
    item_->present_queue()->AbandonWrite(slot_index);
    return {};
  }

  FrameWriteMapping mapping{};
  mapping.row_bytes = static_cast<size_t>(begun->width) * sizeof(float) * 4ULL;
  mapping.pixel_format = FramePixelFormat::RGBA32F;
  mapping.native_object = begun->native.native_handle;

  if (begun->native.writable_kind == LeaseWritableResourceKind::CudaArray) {
    if (preferred_domain != FrameMemoryDomain::CudaDevice &&
        preferred_domain != FrameMemoryDomain::HostVisible) {
      (void)ProducerReleaseWritable(lease);
      item_->present_queue()->AbandonWrite(slot_index);
      return {};
    }
    mapping.image_array = reinterpret_cast<void*>(begun->native.writable_resource);
    mapping.memory_domain = FrameMemoryDomain::CudaDevice;
    mapping.target_type = FrameWriteTargetType::CudaArray;
  } else if (begun->native.writable_kind == LeaseWritableResourceKind::OpenClImage) {
    if (preferred_domain != FrameMemoryDomain::OpenClDevice &&
        preferred_domain != FrameMemoryDomain::HostVisible) {
      (void)ProducerReleaseWritable(lease);
      item_->present_queue()->AbandonWrite(slot_index);
      return {};
    }
    mapping.data = reinterpret_cast<void*>(begun->native.writable_resource);
    mapping.memory_domain = FrameMemoryDomain::OpenClDevice;
    mapping.target_type = FrameWriteTargetType::OpenClImage;
  } else {
    (void)ProducerReleaseWritable(lease);
    item_->present_queue()->AbandonWrite(slot_index);
    return {};
  }

  std::lock_guard lock(mutex_);
  mapped_slot_index_ = slot_index;
  has_mapped_slot_ = true;
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
    lease.backend = snap->native.backend;
    lease.handle_kind = snap->native.handle_kind;
    lease.writable_kind = snap->native.writable_kind;
    lease.dimensions = {snap->width, snap->height};
    lease.native_handle = snap->native.native_handle;
    lease.writable_resource = snap->native.writable_resource;
    lease.sync_object = snap->native.sync_object;
    lease.sync_value = snap->native.sync_value;
    lease.lifetime_token = std::make_shared<LeaseLifetimeToken>();
    (void)ProducerReleaseWritable(lease);
    (void)ProducerWaitWritableComplete(lease);
  }
  item_->present_queue()->EndWrite(slot_index);

  std::lock_guard lock(mutex_);
  unmapped_pending_submit_ = true;
}

void DirectFrameSink::NotifyFrameReady() {
  int slot_index = -1;
  FramePresentationMode mode = FramePresentationMode::FullFrame;
  FramePreviewMetadata metadata{};
  {
    std::lock_guard lock(mutex_);
    if (!has_mapped_slot_ || !unmapped_pending_submit_) {
      return;
    }
    slot_index = mapped_slot_index_;
    if (pending_presentation_mode_valid_) {
      mode = pending_presentation_mode_;
      pending_presentation_mode_valid_ = false;
    }
    if (pending_preview_metadata_valid_) {
      metadata = pending_preview_metadata_;
      pending_preview_metadata_valid_ = false;
    }
    has_mapped_slot_ = false;
    unmapped_pending_submit_ = false;
    mapped_slot_index_ = -1;
    ++submitted_frame_count_;
  }
  if (!item_ || !item_->present_queue() || slot_index < 0) {
    return;
  }
  item_->present_queue()->NotifyReady(slot_index, mode, metadata);
  qInfo("[EditorPresent] submitted frame request=%llu image=%llu generation=%llu slot=%d",
        static_cast<unsigned long long>(metadata.presentation_request_id),
        static_cast<unsigned long long>(item_->imageIdentity()),
        static_cast<unsigned long long>(item_->imageGeneration()), slot_index);
  item_->requestPresentUpdate();
}

void DirectFrameSink::SubmitHostFrame(const ViewerFrame&) {
  // Intentionally empty: production presentation has no CPU-upload fallback.
}

void DirectFrameSink::SubmitFinalDisplayFrame(const FinalDisplayFrameView&) {
  // Scope taps remain responsible for analyzer paths; presentation is direct GPU only.
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
  pending_presentation_mode_ = mode;
  pending_presentation_mode_valid_ = true;
}

void DirectFrameSink::SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) {
  std::lock_guard lock(mutex_);
  pending_preview_metadata_ = metadata;
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
  const auto request_id = frame.slot.preview_metadata.presentation_request_id;
  const auto image_generation = frame.slot.image_generation;
  const auto image_identity = frame.slot.image_identity;
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
    if (callback) {
      callback(request_id, image_generation, image_identity);
    }
  }
  item_->notifyDiagnosticsChanged();
}

auto DirectFrameSink::HasWritableTargetForNextFrame() const -> bool {
  if (!item_ || !item_->present_queue()) {
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
