//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/lease_frame_sink.hpp"

#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"

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
  {
    std::lock_guard lock(mutex_);
    if (width_ == width && height_ == height) {
      return;
    }
    width_ = width;
    height_ = height;
  }
  if (item_) {
    // Queue a target request so the render thread can publish matching leases.
    WritableTargetRequest request;
    request.layer = LeaseFrameLayer::InteractivePrimary;
    request.dimensions = {width, height};
    request.image_generation = item_->imageGeneration();
    request.image_identity = item_->imageIdentity();
    if (item_->broker()) {
      item_->broker()->NoteTargetRequest(request);
    }
    emit item_->TargetSizeRequested(width, height);
    item_->requestPresentUpdate();
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
  auto lease = item_->tryAcquireWritableTarget(CurrentRequest(layer));
  if (!lease.has_value()) {
    // Ask the render thread to create matching targets; producer retries later.
    item_->broker()->NoteTargetRequest(CurrentRequest(layer));
    item_->requestPresentUpdate();
    return {};
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
    if (!item_->submitCompletedFrame(frame)) {
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

void LeaseFrameSink::SetViewState(const ViewerViewState& state) {
  std::lock_guard lock(mutex_);
  view_state_ = state;
}

}  // namespace alcedo::editor_rhi
