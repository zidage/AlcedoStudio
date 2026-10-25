//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/viewport_mask_coverage_sink.hpp"

#include <QThread>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "ui/editor_rhi/editor_viewport_item.hpp"
#include "ui/editor_rhi/lease_target_adapters.hpp"
#include "utils/diagnostics/app_logging.hpp"

using alcedo::diag::editorPresentLog;

namespace alcedo::editor_rhi {
namespace {

/// Same limit as the photo sink: a viewport that never synchronizes must not hold the producer.
constexpr auto kConsumerStartupTimeout = std::chrono::seconds(5);

auto           SuspendedMapping() -> FrameWriteMapping {
  FrameWriteMapping mapping{};
  mapping.presentation_suspended = true;
  return mapping;
}

auto ImageChangedMapping() -> FrameWriteMapping {
  FrameWriteMapping mapping{};
  mapping.image_changed = true;
  return mapping;
}

/// Producer lease of a queue slot for the shared acquire/release helpers.
auto SlotLease(const DirectPresentQueue::SlotSnapshot& slot) -> WritableTargetLease {
  WritableTargetLease lease;
  lease.backend           = slot.native.backend;
  lease.handle_kind       = slot.native.handle_kind;
  lease.writable_kind     = slot.native.writable_kind;
  lease.layer             = LeaseFrameLayer::MaskCoverage;
  lease.dimensions        = {slot.width, slot.height};
  lease.native_handle     = slot.native.native_handle;
  lease.writable_resource = slot.native.writable_resource;
  lease.sync_object       = slot.native.sync_object;
  lease.sync_value        = slot.native.sync_value;
  lease.lifetime_token    = std::make_shared<LeaseLifetimeToken>();
  return lease;
}

}  // namespace

ViewportMaskCoverageSink::ViewportMaskCoverageSink(EditorViewportItem*                 item,
                                                   std::shared_ptr<DirectPresentQueue> queue)
    : item_(item), queue_(std::move(queue)) {
  if (item_ == nullptr || queue_ == nullptr) {
    throw std::invalid_argument(
        "ViewportMaskCoverageSink: the viewport and its queue are required");
  }
}

ViewportMaskCoverageSink::~ViewportMaskCoverageSink() { AbandonCoverageTarget(); }

auto ViewportMaskCoverageSink::MatchesViewportImage(const MaskCoverageFrameIdentity& identity) const
    -> bool {
  return identity.session_epoch == item_->sessionEpoch() &&
         identity.image_identity == item_->imageIdentity();
}

auto ViewportMaskCoverageSink::ReserveSlot(const MaskCoveragePresentation& presentation, int width,
                                           int height) -> std::optional<int> {
  const auto& identity = presentation.identity;
  const auto  prepare =
      queue_->PrepareWrite(width, height, identity.session_epoch, identity.image_identity);
  if (prepare.ok && !prepare.need_create) {
    return prepare.slot_index;
  }
  // The render thread creates the native target; a GUI-thread producer would block the frame
  // that services the request.
  if (item_->thread() == QThread::currentThread()) {
    qCWarning(editorPresentLog, "[EditorPresent] coverage target %dx%d requested on the GUI thread",
              width, height);
    return std::nullopt;
  }
  DirectPresentQueue::SizeRequest request;
  request.width            = width;
  request.height           = height;
  request.preferred_slot   = prepare.slot_index;
  request.session_epoch    = identity.session_epoch;
  request.image_identity   = identity.image_identity;
  request.layer_generation = identity.request_id;
  queue_->NoteSizeRequest(request);
  item_->requestPresentUpdate();
  if (!queue_->DiagnosticsSnapshot().consumer_available &&
      !queue_->WaitUntilConsumerAvailable(kConsumerStartupTimeout)) {
    return std::nullopt;
  }
  return queue_->WaitForWritableSlot(request);
}

auto ViewportMaskCoverageSink::MapCoverageTarget(const MaskCoveragePresentation& presentation,
                                                 int width, int height,
                                                 FrameMemoryDomain preferred_domain)
    -> FrameWriteMapping {
  {
    std::lock_guard lock(mutex_);
    if (mapped_slot_ >= 0) {
      throw std::logic_error("ViewportMaskCoverageSink: a coverage target is already mapped");
    }
  }
  if (width <= 0 || height <= 0) {
    return {};
  }
  if (!MatchesViewportImage(presentation.identity)) {
    return ImageChangedMapping();
  }
  if (queue_->PresentationSuspended()) {
    return SuspendedMapping();
  }
  const auto slot = ReserveSlot(presentation, width, height);
  if (!slot.has_value()) {
    if (queue_->PresentationSuspended()) {
      return SuspendedMapping();
    }
    if (!MatchesViewportImage(presentation.identity)) {
      return ImageChangedMapping();
    }
    qCWarning(editorPresentLog, "[EditorPresent] coverage target handshake failed %dx%d", width,
              height);
    return {};
  }
  const auto begun = queue_->BeginWrite(*slot);
  if (!begun.has_value() || !begun->native.valid()) {
    return queue_->PresentationSuspended() ? SuspendedMapping() : FrameWriteMapping{};
  }
  const auto lease = SlotLease(*begun);
  if (!ProducerAcquireWritable(lease)) {
    queue_->AbandonWrite(*slot);
    return {};
  }

  FrameWriteMapping mapping{};
  mapping.row_bytes     = static_cast<std::size_t>(begun->width) * sizeof(float) * 4ULL;
  mapping.pixel_format  = FramePixelFormat::RGBA32F;
  mapping.native_object = begun->native.native_handle;
  if (begun->native.writable_kind == LeaseWritableResourceKind::CudaArray &&
      preferred_domain == FrameMemoryDomain::CudaDevice) {
    mapping.image_array   = reinterpret_cast<void*>(begun->native.writable_resource);
    mapping.memory_domain = FrameMemoryDomain::CudaDevice;
    mapping.target_type   = FrameWriteTargetType::CudaArray;
  } else if (begun->native.writable_kind == LeaseWritableResourceKind::OpenClImage &&
             preferred_domain == FrameMemoryDomain::OpenClDevice) {
    mapping.data          = reinterpret_cast<void*>(begun->native.writable_resource);
    mapping.memory_domain = FrameMemoryDomain::OpenClDevice;
    mapping.target_type   = FrameWriteTargetType::OpenClImage;
  } else {
    qCWarning(editorPresentLog,
              "[EditorPresent] coverage target kind %s does not match producer domain %d",
              ToString(begun->native.writable_kind), static_cast<int>(preferred_domain));
    (void)ProducerReleaseWritable(lease);
    queue_->AbandonWrite(*slot);
    return {};
  }

  std::lock_guard lock(mutex_);
  mapped_slot_         = *slot;
  unmapped_            = false;
  mapped_presentation_ = presentation;
  return mapping;
}

void ViewportMaskCoverageSink::UnmapCoverageTarget() {
  int slot_index = -1;
  {
    std::lock_guard lock(mutex_);
    if (mapped_slot_ < 0 || unmapped_) {
      return;
    }
    slot_index = mapped_slot_;
  }
  if (const auto slot = queue_->SlotAt(slot_index); slot.has_value()) {
    const auto lease = SlotLease(*slot);
    (void)ProducerReleaseWritable(lease);
    // OpenCL releases the shared image asynchronously; finish it before OpenGL samples.
    if (lease.writable_kind == LeaseWritableResourceKind::OpenClImage) {
      (void)ProducerWaitWritableComplete(lease);
    }
  }
  queue_->EndWrite(slot_index);
  std::lock_guard lock(mutex_);
  unmapped_ = true;
}

auto ViewportMaskCoverageSink::PublishCoverage() -> MaskCoveragePublication {
  int                      slot_index = -1;
  MaskCoveragePresentation presentation;
  {
    std::lock_guard lock(mutex_);
    if (mapped_slot_ < 0 || !unmapped_) {
      throw std::logic_error("ViewportMaskCoverageSink: publication needs an unmapped target");
    }
    slot_index   = mapped_slot_;
    presentation = mapped_presentation_;
    ResetMappedLocked();
  }
  if (!MatchesViewportImage(presentation.identity)) {
    queue_->AbandonWrite(slot_index);
    return MaskCoveragePublication::DiscardedForImageChange;
  }
  {
    std::lock_guard lock(mutex_);
    slot_presentations_[static_cast<std::size_t>(slot_index)] = presentation;
    ++published_count_;
  }
  FramePreviewMetadata metadata;
  metadata.presentation_request_id = presentation.identity.request_id;
  metadata.session_epoch           = presentation.identity.session_epoch;
  metadata.image_identity          = presentation.identity.image_identity;
  metadata.scope_update_allowed    = false;
  queue_->NotifyReady(slot_index, FramePresentationMode::FullFrame, metadata);
  item_->requestPresentUpdate();
  return MaskCoveragePublication::Published;
}

void ViewportMaskCoverageSink::AbandonCoverageTarget() {
  int  slot_index = -1;
  bool unmapped   = false;
  {
    std::lock_guard lock(mutex_);
    if (mapped_slot_ < 0) {
      return;
    }
    slot_index = mapped_slot_;
    unmapped   = unmapped_;
    ResetMappedLocked();
  }
  if (!unmapped) {
    if (const auto slot = queue_->SlotAt(slot_index); slot.has_value()) {
      (void)ProducerReleaseWritable(SlotLease(*slot));
    }
  }
  queue_->AbandonWrite(slot_index);
}

void ViewportMaskCoverageSink::ResetMappedLocked() {
  mapped_slot_         = -1;
  unmapped_            = false;
  mapped_presentation_ = {};
}

#ifdef HAVE_METAL
auto ViewportMaskCoverageSink::SubmitMetalCoverage(const MaskCoveragePresentation& presentation,
                                                   const MaskCoverageMetalFrame&   frame)
    -> MaskCoveragePublication {
  if (!frame) {
    throw std::invalid_argument("ViewportMaskCoverageSink: the Metal coverage frame is empty");
  }
  if (!MatchesViewportImage(presentation.identity)) {
    return MaskCoveragePublication::DiscardedForImageChange;
  }
  {
    std::lock_guard lock(mutex_);
    pending_metal_ = PendingMetalCoverage{frame, presentation};
    ++published_count_;
  }
  item_->requestPresentUpdate();
  return MaskCoveragePublication::Published;
}

auto ViewportMaskCoverageSink::TakePendingMetalCoverage(std::uint64_t session_epoch,
                                                        std::uint64_t image_identity)
    -> std::optional<PendingMetalCoverage> {
  std::lock_guard lock(mutex_);
  auto            pending = std::move(pending_metal_);
  pending_metal_.reset();
  if (pending.has_value() && (pending->presentation.identity.session_epoch != session_epoch ||
                              pending->presentation.identity.image_identity != image_identity)) {
    return std::nullopt;
  }
  return pending;
}
#endif

void ViewportMaskCoverageSink::ClearPendingMetalCoverage() {
#ifdef HAVE_METAL
  std::lock_guard lock(mutex_);
  pending_metal_.reset();
#endif
}

auto ViewportMaskCoverageSink::HasPendingMetalCoverage() const -> bool {
#ifdef HAVE_METAL
  std::lock_guard lock(mutex_);
  return pending_metal_.has_value();
#else
  return false;
#endif
}

auto ViewportMaskCoverageSink::PresentationOfSlot(int slot_index) const
    -> MaskCoveragePresentation {
  std::lock_guard lock(mutex_);
  if (slot_index < 0 || slot_index >= DirectPresentQueue::kSlotCount) {
    return {};
  }
  return slot_presentations_[static_cast<std::size_t>(slot_index)];
}

auto ViewportMaskCoverageSink::PublishedCount() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return published_count_;
}

}  // namespace alcedo::editor_rhi
