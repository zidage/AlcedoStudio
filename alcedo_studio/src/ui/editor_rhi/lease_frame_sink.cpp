//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/lease_frame_sink.hpp"

#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"

#include <QDebug>
#include <QMetaObject>

namespace alcedo::editor_rhi {

LeaseFrameSink::LeaseFrameSink(EditorViewportItem* item) : item_(item) {}

LeaseFrameSink::~LeaseFrameSink() {
  ClearMappedLease();
}

void LeaseFrameSink::ClearMappedLease() {
  std::lock_guard lock(mutex_);
  if (has_mapped_lease_) {
    if (item_) {
      item_->abandonProducerWrite(mapped_lease_);
    }
    has_mapped_lease_ = false;
    unmapped_pending_submit_ = false;
    mapped_lease_ = {};
  }
  if (prepared_lease_) {
    if (item_) {
      item_->abandonProducerWrite(*prepared_lease_);
    }
    prepared_lease_.reset();
  }
}

auto LeaseFrameSink::LayerForMetadata(const FramePreviewMetadata& metadata) -> LeaseFrameLayer {
  switch (metadata.frame_role) {
    case FrameRole::InteractivePrimary:
      return LeaseFrameLayer::InteractivePrimary;
    case FrameRole::QualityBase:
      return LeaseFrameLayer::QualityBase;
    case FrameRole::DetailPatch:
      return LeaseFrameLayer::DetailPatch;
  }
  return LeaseFrameLayer::InteractivePrimary;
}

auto LeaseFrameSink::ToLeasePresentationMode(FramePresentationMode mode)
    -> LeasePresentationMode {
  return mode == FramePresentationMode::RoiFrame ? LeasePresentationMode::RoiFrame
                                                 : LeasePresentationMode::FullFrame;
}

auto LeaseFrameSink::CurrentRequest(LeaseFrameLayer layer) const -> WritableTargetRequest {
  WritableTargetRequest request;
  request.layer = layer;
  request.dimensions = {width_, height_};
  request.image_generation = item_ ? item_->imageGeneration() : 0;
  request.image_identity = item_ ? item_->imageIdentity() : 0;
  request.layer_generation = pending_preview_metadata_valid_
                                 ? pending_preview_metadata_.preview_generation
                                 : 0;
  return request;
}

void LeaseFrameSink::EnsureSize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const std::uint64_t image_generation = item_ ? item_->imageGeneration() : 0;
  const std::uint64_t image_identity   = item_ ? item_->imageIdentity() : 0;
  bool geometry_changed = false;
  WritableTargetRequest request;
  {
    std::lock_guard lock(mutex_);
    if (has_mapped_lease_ || prepared_lease_) {
      return;
    }
    // Same pixel size is not enough for geometry notification when the image
    // session changes, but every frame still reserves its own write slot.
    geometry_changed = width_ != width || height_ != height ||
                       last_sized_image_generation_ != image_generation ||
                       last_sized_image_identity_ != image_identity;
    width_                       = width;
    height_                      = height;
    last_sized_image_generation_ = image_generation;
    last_sized_image_identity_   = image_identity;
    const LeaseFrameLayer layer = pending_preview_metadata_valid_
                                      ? LayerForMetadata(pending_preview_metadata_)
                                      : LeaseFrameLayer::InteractivePrimary;
    request = CurrentRequest(layer);
  }
  if (item_) {
    // This is the QQuick render-thread form of the pre-refactor viewer's
    // BlockingQueuedConnection resize: reserve the exact native slot before
    // CUDA/OpenCL dispatch begins, so target creation never races GPU work.
    if (item_->broker()) {
      item_->broker()->NoteTargetRequest(request);
    }
    if (geometry_changed) {
      // EnsureSize runs on the pipeline worker. QML handlers must remain on the
      // item's GUI thread.
      QMetaObject::invokeMethod(
          item_, [item = item_, width, height] { emit item->targetSizeRequested(width, height); },
          Qt::QueuedConnection);
    }
    item_->requestPresentUpdate();
    auto lease = item_->broker()->WaitAcquireWritableTarget(request);
    std::lock_guard lock(mutex_);
    prepared_lease_ = std::move(lease);
  }
}

auto LeaseFrameSink::MapResourceForWrite(FrameMemoryDomain preferred_domain) -> FrameWriteMapping {
  if (!item_ || !item_->broker()) {
    return {};
  }
  std::lock_guard lock(mutex_);
  if (has_mapped_lease_ || width_ <= 0 || height_ <= 0) {
    return {};
  }

  const LeaseFrameLayer layer = pending_preview_metadata_valid_
                                    ? LayerForMetadata(pending_preview_metadata_)
                                    : LeaseFrameLayer::InteractivePrimary;
  const auto request = CurrentRequest(layer);
  auto lease = std::move(prepared_lease_);
  prepared_lease_.reset();
  if (lease.has_value() &&
      (lease->dimensions != request.dimensions ||
       lease->generation.image_generation != request.image_generation ||
       lease->generation.image_identity != request.image_identity)) {
    item_->abandonProducerWrite(*lease);
    lease.reset();
  }
  if (!lease.has_value()) {
    lease = item_->tryAcquireWritableTarget(request);
  }
  if (!lease.has_value()) {
    // Pipeline output geometry may only become known at the final GPU stage.
    // Restore the legacy deterministic resize handshake: the scene-graph thread
    // publishes a matching native target (or reports failure/lifecycle exit),
    // and this producer resumes from that explicit result without polling.
    item_->broker()->NoteTargetRequest(request);
    item_->requestPresentUpdate();
    qInfo("[EditorPresent] producer waiting for native target %dx%d image=%llu generation=%llu",
          request.dimensions.width, request.dimensions.height,
          static_cast<unsigned long long>(request.image_identity),
          static_cast<unsigned long long>(request.image_generation));
    lease = item_->broker()->WaitAcquireWritableTarget(request);
    if (!lease.has_value()) {
      qWarning("[EditorPresent] native target handshake failed %dx%d", request.dimensions.width,
               request.dimensions.height);
      return {};
    }
    qInfo("[EditorPresent] producer acquired native target %dx%d", request.dimensions.width,
          request.dimensions.height);
  }

  if (lease->lifetime_token && lease->lifetime_token->cancelled()) {
    item_->abandonProducerWrite(*lease);
    return {};
  }

  if (!ProducerAcquireWritable(*lease)) {
    item_->abandonProducerWrite(*lease);
    return {};
  }

  FrameWriteMapping mapping{};
  mapping.row_bytes = static_cast<size_t>(lease->dimensions.width) * sizeof(float) * 4ULL;
  mapping.pixel_format = FramePixelFormat::RGBA32F;
  mapping.native_object = lease->native_handle;

  if (lease->writable_kind == LeaseWritableResourceKind::CudaArray) {
    if (preferred_domain != FrameMemoryDomain::CudaDevice &&
        preferred_domain != FrameMemoryDomain::HostVisible) {
      (void)ProducerReleaseWritable(*lease);
      item_->abandonProducerWrite(*lease);
      return {};
    }
    mapping.image_array = reinterpret_cast<void*>(lease->writable_resource);
    mapping.memory_domain = FrameMemoryDomain::CudaDevice;
    mapping.target_type = FrameWriteTargetType::CudaArray;
  } else if (lease->writable_kind == LeaseWritableResourceKind::OpenClImage) {
    if (preferred_domain != FrameMemoryDomain::OpenClDevice &&
        preferred_domain != FrameMemoryDomain::HostVisible) {
      (void)ProducerReleaseWritable(*lease);
      item_->abandonProducerWrite(*lease);
      return {};
    }
    mapping.data = reinterpret_cast<void*>(lease->writable_resource);
    mapping.memory_domain = FrameMemoryDomain::OpenClDevice;
    mapping.target_type = FrameWriteTargetType::OpenClImage;
  } else {
    (void)ProducerReleaseWritable(*lease);
    item_->abandonProducerWrite(*lease);
    return {};
  }

  mapped_lease_ = *lease;
  has_mapped_lease_ = true;
  unmapped_pending_submit_ = false;
  return mapping;
}

void LeaseFrameSink::UnmapResource() {
  std::lock_guard lock(mutex_);
  if (!has_mapped_lease_) {
    return;
  }
  (void)ProducerReleaseWritable(mapped_lease_);
  (void)ProducerWaitWritableComplete(mapped_lease_);
  unmapped_pending_submit_ = true;
}

void LeaseFrameSink::NotifyFrameReady() {
  CompletedFrameLease frame;
  {
    std::lock_guard lock(mutex_);
    if (!has_mapped_lease_ || !unmapped_pending_submit_) {
      return;
    }
    frame.target = mapped_lease_;
    frame.generation = mapped_lease_.generation;
    frame.layer = mapped_lease_.layer;
    if (pending_preview_metadata_valid_) {
      frame.layer = LayerForMetadata(pending_preview_metadata_);
      frame.preview_generation = pending_preview_metadata_.preview_generation;
      frame.detail_serial = pending_preview_metadata_.detail_serial;
      frame.presentation_request_id = pending_preview_metadata_.presentation_request_id;
      frame.roi_x = pending_preview_metadata_.source_roi_norm.x;
      frame.roi_y = pending_preview_metadata_.source_roi_norm.y;
      frame.roi_width = pending_preview_metadata_.source_roi_norm.width;
      frame.roi_height = pending_preview_metadata_.source_roi_norm.height;
      frame.generation.layer_generation = pending_preview_metadata_.preview_generation;
      pending_preview_metadata_valid_ = false;
    }
    if (pending_presentation_mode_valid_) {
      frame.presentation_mode = ToLeasePresentationMode(pending_presentation_mode_);
      pending_presentation_mode_valid_ = false;
    }
    frame.producer_complete = true;
    has_mapped_lease_ = false;
    unmapped_pending_submit_ = false;
    mapped_lease_ = {};
  }
  if (item_) {
    if (item_->submitCompletedFrame(frame)) {
      qWarning("[EditorPresent] submitted frame request=%llu image=%llu generation=%llu",
               static_cast<unsigned long long>(frame.presentation_request_id),
               static_cast<unsigned long long>(frame.generation.image_identity),
               static_cast<unsigned long long>(frame.generation.image_generation));
      std::lock_guard lock(mutex_);
      ++submitted_frame_count_;
    } else {
      // Stale or cancelled; nothing else to do (broker abandoned the target).
    }
  }
}

void LeaseFrameSink::SubmitHostFrame(const ViewerFrame&) {
  // Intentionally empty: production presentation has no CPU-upload fallback.
}

void LeaseFrameSink::SubmitFinalDisplayFrame(const FinalDisplayFrameView&) {
  // Scope taps remain responsible for analyzer paths; presentation is lease-only.
}

auto LeaseFrameSink::GetWidth() const -> int {
  std::lock_guard lock(mutex_);
  return width_;
}

auto LeaseFrameSink::GetHeight() const -> int {
  std::lock_guard lock(mutex_);
  return height_;
}

auto LeaseFrameSink::GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> {
  std::lock_guard lock(mutex_);
  return view_state_.snapshot.viewport_render_region_cache;
}

void LeaseFrameSink::SetNextFramePresentationMode(FramePresentationMode mode) {
  std::lock_guard lock(mutex_);
  pending_presentation_mode_ = mode;
  pending_presentation_mode_valid_ = true;
}

void LeaseFrameSink::SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) {
  std::lock_guard lock(mutex_);
  pending_preview_metadata_ = metadata;
  pending_preview_metadata_valid_ = true;
}

auto LeaseFrameSink::ViewState() const -> ViewerViewState {
  std::lock_guard lock(mutex_);
  return view_state_;
}

void LeaseFrameSink::SetViewState(const ViewerViewState& state) {
  std::lock_guard lock(mutex_);
  view_state_ = state;
}

void LeaseFrameSink::SetPresentationAcknowledgementCallback(
    PresentationAcknowledgement callback) {
  std::lock_guard lock(mutex_);
  presentation_acknowledgement_ = std::move(callback);
}

void LeaseFrameSink::AcknowledgePresentedFrame(const CompletedFrameLease& frame) {
  PresentationAcknowledgement callback;
  {
    std::lock_guard lock(mutex_);
    if (frame.presentation_request_id == 0 ||
        frame.presentation_request_id == last_acknowledged_request_id_) {
      return;
    }
    last_acknowledged_request_id_ = frame.presentation_request_id;
    callback = presentation_acknowledgement_;
  }
  if (callback) {
    callback(frame.presentation_request_id, frame.generation.image_generation,
             frame.generation.image_identity);
  }
}

auto LeaseFrameSink::HasWritableTargetForNextFrame() const -> bool {
  if (!item_ || !item_->broker()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const LeaseFrameLayer layer = pending_preview_metadata_valid_
                                    ? LayerForMetadata(pending_preview_metadata_)
                                    : LeaseFrameLayer::InteractivePrimary;
  return item_->broker()->HasWritableTarget(CurrentRequest(layer));
}

auto LeaseFrameSink::submitted_frame_count() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return submitted_frame_count_;
}

}  // namespace alcedo::editor_rhi
