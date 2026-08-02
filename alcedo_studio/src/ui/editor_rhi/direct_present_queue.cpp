//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/direct_present_queue.hpp"

#include <algorithm>
#include <iostream>
#include <syncstream>

namespace alcedo::editor_rhi {
namespace {

auto SameSizeRequest(const DirectPresentQueue::SizeRequest& lhs,
                     const DirectPresentQueue::SizeRequest& rhs) -> bool {
  return lhs.width == rhs.width && lhs.height == rhs.height &&
         lhs.image_generation == rhs.image_generation && lhs.image_identity == rhs.image_identity &&
         lhs.frame_role == rhs.frame_role;
}

}  // namespace

auto SelectDirectPresentWriteSlotGeneric(const DirectPresentSlotInfo* slot_infos,
                                         std::size_t slot_count, int preferred_slot, int width,
                                         int height) -> DirectPresentWriteSelection {
  if (!slot_infos || slot_count == 0) {
    return {};
  }

  auto is_valid_index = [slot_count](int slot_index) {
    return slot_index >= 0 && static_cast<std::size_t>(slot_index) < slot_count;
  };
  auto reusable = [width, height](const DirectPresentSlotInfo& slot) {
    return !slot.unavailable && slot.has_resource && slot.width == width && slot.height == height;
  };

  int selected_slot = is_valid_index(preferred_slot) ? preferred_slot : 0;
  if (reusable(slot_infos[selected_slot])) {
    return {selected_slot, false};
  }

  for (std::size_t i = 0; i < slot_count; ++i) {
    if (reusable(slot_infos[i])) {
      return {static_cast<int>(i), false};
    }
  }

  if (!slot_infos[selected_slot].unavailable) {
    return {selected_slot, true};
  }

  for (std::size_t i = 0; i < slot_count; ++i) {
    if (!slot_infos[i].unavailable) {
      return {static_cast<int>(i), true};
    }
  }

  return {selected_slot, !reusable(slot_infos[selected_slot])};
}

DirectPresentQueue::DirectPresentQueue(EditorBackend backend) : backend_(backend) {}

auto DirectPresentQueue::IsValidSlot(int index) const -> bool {
  return index >= 0 && index < kSlotCount;
}

auto DirectPresentQueue::SnapshotLocked(int index) const -> SlotSnapshot {
  SlotSnapshot snap;
  if (!IsValidSlot(index)) {
    return snap;
  }
  const auto& slot       = slots_[static_cast<std::size_t>(index)];
  snap.index             = index;
  snap.state             = slot.state;
  snap.width             = slot.width;
  snap.height            = slot.height;
  snap.native            = slot.native;
  snap.presentation_mode = slot.presentation_mode;
  snap.preview_metadata  = slot.preview_metadata;
  snap.image_generation  = slot.image_generation;
  snap.image_identity    = slot.image_identity;
  snap.sequence          = slot.sequence;
  return snap;
}

auto DirectPresentQueue::SlotUnavailableLocked(int index) const -> bool {
  if (!IsValidSlot(index)) {
    return true;
  }
  const auto& slot = slots_[static_cast<std::size_t>(index)];
  // Match the legacy surface: active display, ready queue, and mapped write
  // slots are not recyclable for a new write.
  if (index == active_idx_ && slot.state == SlotState::RendererReading) {
    return true;
  }
  return slot.state != SlotState::Available;
}

void DirectPresentQueue::RecycleSlotLocked(int index, bool queue_native_release) {
  if (!IsValidSlot(index)) {
    return;
  }
  auto& slot = slots_[static_cast<std::size_t>(index)];
  if (queue_native_release && slot.native.valid()) {
    release_queue_.push_back(slot.native);
  }
  slot.state             = SlotState::Available;
  slot.presentation_mode = FramePresentationMode::FullFrame;
  slot.preview_metadata  = {};
  slot.sequence          = 0;
  if (queue_native_release) {
    slot.native           = {};
    slot.width            = 0;
    slot.height           = 0;
    slot.image_generation = 0;
    slot.image_identity   = 0;
  }
  if (mapped_slot_idx_ == index) {
    mapped_slot_idx_ = -1;
  }
}

auto DirectPresentQueue::SelectWriteSlotLocked(int width, int height) -> PrepareResult {
  std::array<DirectPresentSlotInfo, kSlotCount> infos{};
  for (int i = 0; i < kSlotCount; ++i) {
    const auto& slot                   = slots_[static_cast<std::size_t>(i)];
    infos[static_cast<std::size_t>(i)] = DirectPresentSlotInfo{
        slot.width, slot.height, slot.native.valid(), SlotUnavailableLocked(i)};
  }
  const auto selection =
      SelectDirectPresentWriteSlotGeneric(infos.data(), infos.size(), write_idx_, width, height);
  PrepareResult result;
  result.slot_index  = selection.slot_index;
  result.need_create = selection.need_create;
  result.ok          = IsValidSlot(selection.slot_index) &&
              (!infos[static_cast<std::size_t>(selection.slot_index)].unavailable ||
               !selection.need_create);
  if (!result.ok && IsValidSlot(selection.slot_index) &&
      infos[static_cast<std::size_t>(selection.slot_index)].unavailable && selection.need_create) {
    // All slots busy and none reusable — producer must wait for recycle.
    result.ok          = false;
    result.need_create = true;
  } else if (IsValidSlot(selection.slot_index)) {
    result.ok = true;
  }
  return result;
}

auto DirectPresentQueue::MatchesSizeRequestLocked(const SizeRequest& request, int slot_index) const
    -> bool {
  if (!IsValidSlot(slot_index) || !request.valid()) {
    return false;
  }
  const auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  return slot.state == SlotState::Available && slot.native.valid() && slot.width == request.width &&
         slot.height == request.height &&
         (request.image_generation == 0 || slot.image_generation == request.image_generation) &&
         (request.image_identity == 0 || slot.image_identity == request.image_identity);
}

auto DirectPresentQueue::PrepareWrite(int width, int height, std::uint64_t image_generation,
                                      std::uint64_t image_identity) -> PrepareResult {
  std::lock_guard lock(mutex_);
  if (shutdown_ || width <= 0 || height <= 0) {
    return {};
  }
  auto result = SelectWriteSlotLocked(width, height);
  if (!result.ok) {
    return result;
  }
  write_idx_ = result.slot_index;
  auto& slot = slots_[static_cast<std::size_t>(result.slot_index)];
  if (!result.need_create && slot.native.valid() && slot.width == width && slot.height == height) {
    // Reusable exact-size target: stamp session identity for stale rejection.
    slot.image_generation = image_generation;
    slot.image_identity   = image_identity;
  } else {
    result.need_create = true;
  }
  return result;
}

void DirectPresentQueue::NoteSizeRequest(const SizeRequest& request) {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !request.valid()) {
    return;
  }
  // Replace an equivalent pending request so resize storms stay bounded.
  pending_requests_.erase(std::remove_if(pending_requests_.begin(), pending_requests_.end(),
                                         [&](const SizeRequest& existing) {
                                           return SameSizeRequest(existing, request);
                                         }),
                          pending_requests_.end());
  pending_requests_.push_back(request);
  wake_.notify_all();
}

void DirectPresentQueue::FailSizeRequest(const SizeRequest& request) {
  std::lock_guard lock(mutex_);
  failed_requests_.push_back(request);
  pending_requests_.erase(std::remove_if(pending_requests_.begin(), pending_requests_.end(),
                                         [&](const SizeRequest& existing) {
                                           return SameSizeRequest(existing, request);
                                         }),
                          pending_requests_.end());
  wake_.notify_all();
}

auto DirectPresentQueue::DrainSizeRequests() -> std::vector<SizeRequest> {
  std::lock_guard          lock(mutex_);
  std::vector<SizeRequest> out(pending_requests_.begin(), pending_requests_.end());
  pending_requests_.clear();
  return out;
}

auto DirectPresentQueue::WaitUntilConsumerAvailable(std::chrono::milliseconds timeout) -> bool {
  std::unique_lock lock(mutex_);
  if (!wake_.wait_for(lock, timeout, [&] { return shutdown_ || consumer_available_; })) {
    return false;
  }
  return consumer_available_ && !shutdown_;
}

auto DirectPresentQueue::WaitForWritableSlot(const SizeRequest& request) -> std::optional<int> {
  std::unique_lock lock(mutex_);
  // Keep waiting while the consumer is offline: the first editor frame often
  // lands before synchronize() has flipped consumer_available. Treat suspend /
  // shutdown as terminal, not a silent success that returns nullopt without a
  // size request having been fulfilled.
  wake_.wait(lock, [&] {
    if (shutdown_) {
      return true;
    }
    if (!consumer_available_) {
      return false;
    }
    if (image_generation_ != 0 && request.image_generation != 0 &&
        image_generation_ != request.image_generation) {
      return true;
    }
    if (image_identity_ != 0 && request.image_identity != 0 &&
        image_identity_ != request.image_identity) {
      return true;
    }
    for (const auto& failed : failed_requests_) {
      if (SameSizeRequest(failed, request)) {
        return true;
      }
    }
    for (int i = 0; i < kSlotCount; ++i) {
      if (MatchesSizeRequestLocked(request, i)) {
        return true;
      }
    }
    return false;
  });

  if (shutdown_ || !consumer_available_) {
    return std::nullopt;
  }
  if (image_generation_ != 0 && request.image_generation != 0 &&
      image_generation_ != request.image_generation) {
    return std::nullopt;
  }
  if (image_identity_ != 0 && request.image_identity != 0 &&
      image_identity_ != request.image_identity) {
    return std::nullopt;
  }
  failed_requests_.erase(
      std::remove_if(failed_requests_.begin(), failed_requests_.end(),
                     [&](const SizeRequest& failed) { return SameSizeRequest(failed, request); }),
      failed_requests_.end());
  for (int i = 0; i < kSlotCount; ++i) {
    if (MatchesSizeRequestLocked(request, i)) {
      write_idx_ = i;
      return i;
    }
  }
  return std::nullopt;
}

auto DirectPresentQueue::PublishCreatedSlot(int slot_index, int width, int height,
                                            SlotNative native, std::uint64_t image_generation,
                                            std::uint64_t image_identity) -> bool {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !IsValidSlot(slot_index) || !native.valid() || width <= 0 || height <= 0) {
    return false;
  }
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (slot.state != SlotState::Available && slot.state != SlotState::Ready) {
    // Do not clobber a live write/read.
    return false;
  }
  if (slot.native.valid() && (slot.native.native_handle != native.native_handle ||
                              slot.width != width || slot.height != height)) {
    release_queue_.push_back(slot.native);
  }
  slot.state             = SlotState::Available;
  slot.width             = width;
  slot.height            = height;
  slot.native            = native;
  slot.image_generation  = image_generation;
  slot.image_identity    = image_identity;
  slot.presentation_mode = FramePresentationMode::FullFrame;
  slot.preview_metadata  = {};
  slot.sequence          = 0;
  write_idx_             = slot_index;
  wake_.notify_all();
  return true;
}

auto DirectPresentQueue::BeginWrite(int slot_index) -> std::optional<SlotSnapshot> {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_ || !IsValidSlot(slot_index)) {
    return std::nullopt;
  }
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (slot.state != SlotState::Available || !slot.native.valid()) {
    return std::nullopt;
  }
  slot.state       = SlotState::ProducerWriting;
  mapped_slot_idx_ = slot_index;
  return SnapshotLocked(slot_index);
}

void DirectPresentQueue::EndWrite(int slot_index) {
  std::lock_guard lock(mutex_);
  if (!IsValidSlot(slot_index)) {
    return;
  }
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (slot.state != SlotState::ProducerWriting) {
    return;
  }
  // Keep ProducerWriting until NotifyReady or AbandonWrite. EndWrite only marks
  // GPU ownership handoff complete (OpenCL release / CUDA fence already done).
  if (mapped_slot_idx_ == slot_index) {
    mapped_slot_idx_ = -1;
  }
}

void DirectPresentQueue::NotifyReady(int slot_index, FramePresentationMode mode,
                                     const FramePreviewMetadata& metadata) {
  std::lock_guard lock(mutex_);
  if (!IsValidSlot(slot_index)) {
    if (metadata.frame_role == FrameRole::DetailPatch) {
      std::osyncstream(std::cout)
          << "[ROI_TRACE][queue-drop] request=" << metadata.presentation_request_id
          << " reason=invalid-slot slot=" << slot_index << std::endl;
    }
    return;
  }
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  // EndWrite may already have cleared mapped_slot_idx while leaving the slot
  // in ProducerWriting until this handoff.
  if (slot.state != SlotState::ProducerWriting) {
    if (metadata.frame_role == FrameRole::DetailPatch) {
      std::osyncstream(std::cout)
          << "[ROI_TRACE][queue-drop] request=" << metadata.presentation_request_id
          << " reason=slot-not-producer-writing slot=" << slot_index
          << " state=" << static_cast<int>(slot.state) << std::endl;
    }
    return;
  }
  if (!slot.native.valid()) {
    if (metadata.frame_role == FrameRole::DetailPatch) {
      std::osyncstream(std::cout)
          << "[ROI_TRACE][queue-drop] request=" << metadata.presentation_request_id
          << " reason=invalid-native slot=" << slot_index << std::endl;
    }
    RecycleSlotLocked(slot_index, false);
    return;
  }
  // Stale session rejection.
  if (image_generation_ != 0 && slot.image_generation != 0 &&
      slot.image_generation != image_generation_) {
    if (metadata.frame_role == FrameRole::DetailPatch) {
      std::osyncstream(std::cout)
          << "[ROI_TRACE][queue-drop] request=" << metadata.presentation_request_id
          << " reason=image-generation-mismatch slot_generation=" << slot.image_generation
          << " queue_generation=" << image_generation_ << std::endl;
    }
    ++dropped_stale_frame_count_;
    RecycleSlotLocked(slot_index, false);
    wake_.notify_all();
    return;
  }
  if (image_identity_ != 0 && slot.image_identity != 0 && slot.image_identity != image_identity_) {
    if (metadata.frame_role == FrameRole::DetailPatch) {
      std::osyncstream(std::cout)
          << "[ROI_TRACE][queue-drop] request=" << metadata.presentation_request_id
          << " reason=image-identity-mismatch slot_image=" << slot.image_identity
          << " queue_image=" << image_identity_ << std::endl;
    }
    ++dropped_stale_frame_count_;
    RecycleSlotLocked(slot_index, false);
    wake_.notify_all();
    return;
  }

  // Supersede older Ready frames for the same layer (latest-compatible wins).
  for (int i = 0; i < kSlotCount; ++i) {
    if (i == slot_index) {
      continue;
    }
    auto& other = slots_[static_cast<std::size_t>(i)];
    if (other.state == SlotState::Ready &&
        other.preview_metadata.frame_role == metadata.frame_role) {
      if (metadata.frame_role == FrameRole::DetailPatch) {
        std::osyncstream(std::cout)
            << "[ROI_TRACE][queue-supersede] old_request="
            << other.preview_metadata.presentation_request_id
            << " new_request=" << metadata.presentation_request_id << " old_slot=" << i
            << " new_slot=" << slot_index << std::endl;
      }
      ++dropped_stale_frame_count_;
      RecycleSlotLocked(i, false);
    }
  }

  slot.state             = SlotState::Ready;
  slot.presentation_mode = mode;
  slot.preview_metadata  = metadata;
  slot.sequence          = ++sequence_;
  if (metadata.frame_role == FrameRole::DetailPatch) {
    std::osyncstream(std::cout)
        << "[ROI_TRACE][queue-ready] request=" << metadata.presentation_request_id
        << " slot=" << slot_index << " sequence=" << slot.sequence << " size=" << slot.width << 'x'
        << slot.height << " image=" << slot.image_identity
        << " image_generation=" << slot.image_generation << " roi_norm="
        << metadata.source_roi_norm.x << ',' << metadata.source_roi_norm.y << ','
        << metadata.source_roi_norm.width << ',' << metadata.source_roi_norm.height << std::endl;
  }
  if (mapped_slot_idx_ == slot_index) {
    mapped_slot_idx_ = -1;
  }
  wake_.notify_all();
}

void DirectPresentQueue::AbandonWrite(int slot_index) {
  std::lock_guard lock(mutex_);
  if (!IsValidSlot(slot_index)) {
    return;
  }
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (slot.state == SlotState::ProducerWriting) {
    RecycleSlotLocked(slot_index, false);
    wake_.notify_all();
  }
}

auto DirectPresentQueue::ConsumeNewestReady(FrameRole layer, std::uint64_t image_generation,
                                            std::uint64_t image_identity)
    -> std::optional<ReadyFrame> {
  std::lock_guard lock(mutex_);
  int             best          = -1;
  std::uint64_t   best_request  = 0;
  std::uint64_t   best_sequence = 0;
  for (int i = 0; i < kSlotCount; ++i) {
    const auto& slot = slots_[static_cast<std::size_t>(i)];
    if (slot.state != SlotState::Ready || !slot.native.valid()) {
      continue;
    }
    if (slot.preview_metadata.frame_role != layer) {
      continue;
    }
    if (image_generation != 0 && slot.image_generation != 0 &&
        slot.image_generation != image_generation) {
      continue;
    }
    if (image_identity != 0 && slot.image_identity != 0 && slot.image_identity != image_identity) {
      continue;
    }
    const auto request_id = slot.preview_metadata.presentation_request_id;
    const bool better     = best < 0 || request_id > best_request ||
                        (request_id == best_request && slot.sequence > best_sequence);
    if (better) {
      best         = i;
      best_request = request_id;
      best_sequence = slot.sequence;
    }
  }
  if (best < 0) {
    return std::nullopt;
  }

  // Supersede other Ready frames for this layer.
  for (int i = 0; i < kSlotCount; ++i) {
    if (i == best) {
      continue;
    }
    auto& other = slots_[static_cast<std::size_t>(i)];
    if (other.state == SlotState::Ready && other.preview_metadata.frame_role == layer) {
      ++dropped_stale_frame_count_;
      RecycleSlotLocked(i, false);
    }
  }

  auto& chosen = slots_[static_cast<std::size_t>(best)];
  chosen.state = SlotState::RendererReading;
  active_idx_  = best;
  if (layer == FrameRole::DetailPatch) {
    std::osyncstream(std::cout)
        << "[ROI_TRACE][queue-consume] request="
        << chosen.preview_metadata.presentation_request_id << " slot=" << best
        << " sequence=" << chosen.sequence << " size=" << chosen.width << 'x' << chosen.height
        << " image=" << chosen.image_identity << " image_generation=" << chosen.image_generation
        << " roi_norm=" << chosen.preview_metadata.source_roi_norm.x << ','
        << chosen.preview_metadata.source_roi_norm.y << ','
        << chosen.preview_metadata.source_roi_norm.width << ','
        << chosen.preview_metadata.source_roi_norm.height << std::endl;
  }
  ReadyFrame frame;
  frame.slot = SnapshotLocked(best);
  return frame;
}

void DirectPresentQueue::CompleteRendererRead(int slot_index) {
  std::lock_guard lock(mutex_);
  if (!IsValidSlot(slot_index)) {
    return;
  }
  auto& slot = slots_[static_cast<std::size_t>(slot_index)];
  if (slot.state != SlotState::RendererReading) {
    return;
  }
  // Keep the native resource; slot returns to Available for reuse at same size.
  RecycleSlotLocked(slot_index, false);
  wake_.notify_all();
}

void DirectPresentQueue::NoteFrameComposed(std::uint64_t request_id, std::uint64_t image_generation,
                                           std::uint64_t image_identity) {
  std::lock_guard lock(mutex_);
  last_composed_image_generation_ = image_generation;
  if (request_id != 0) {
    last_composed_request_id_ = request_id;
  }
  (void)image_identity;
  ++composed_frame_count_;
}

void DirectPresentQueue::SetConsumerAvailable(bool available) {
  std::lock_guard lock(mutex_);
  consumer_available_ = available;
  if (!available) {
    // Wake producers blocked in WaitForWritableSlot so they observe failure.
    wake_.notify_all();
  } else {
    wake_.notify_all();
  }
}

void DirectPresentQueue::InvalidateTargetsLocked(bool bump_target_generation) {
  if (bump_target_generation) {
    ++target_generation_;
  }
  for (int i = 0; i < kSlotCount; ++i) {
    auto& slot = slots_[static_cast<std::size_t>(i)];
    if (slot.state == SlotState::ProducerWriting) {
      // Leave producer-writing slots; AbandonWrite will recycle.
      if (slot.native.valid()) {
        // Mark identity stale so NotifyReady drops them.
        slot.image_generation = 0;
      }
      continue;
    }
    RecycleSlotLocked(i, true);
  }
  pending_requests_.clear();
  failed_requests_.clear();
  mapped_slot_idx_ = -1;
  active_idx_      = 0;
  write_idx_       = 1;
  wake_.notify_all();
}

void DirectPresentQueue::InvalidateImageGeneration(std::uint64_t image_generation,
                                                   std::uint64_t image_identity) {
  std::lock_guard lock(mutex_);
  image_generation_                   = image_generation;
  image_identity_                     = image_identity;
  // Drop ready/available content from the previous session; keep producer writes
  // until they abandon so Map cannot hang forever without a wake.
  for (int i = 0; i < kSlotCount; ++i) {
    auto& slot = slots_[static_cast<std::size_t>(i)];
    if (slot.state == SlotState::ProducerWriting) {
      slot.image_generation = 0;
      continue;
    }
    if (slot.state == SlotState::Ready || slot.state == SlotState::RendererReading ||
        slot.state == SlotState::Available) {
      // Keep native capacity; clear ready content and session stamp.
      if (slot.state == SlotState::Ready || slot.state == SlotState::RendererReading) {
        slot.state            = SlotState::Available;
        slot.preview_metadata = {};
        slot.sequence         = 0;
      }
      slot.image_generation = image_generation;
      slot.image_identity   = image_identity;
    }
  }
  pending_requests_.clear();
  failed_requests_.clear();
  wake_.notify_all();
}

void DirectPresentQueue::InvalidateTargetGeneration() {
  std::lock_guard lock(mutex_);
  InvalidateTargetsLocked(true);
}

void DirectPresentQueue::Shutdown() {
  std::lock_guard lock(mutex_);
  shutdown_           = true;
  consumer_available_ = false;
  InvalidateTargetsLocked(false);
}

auto DirectPresentQueue::DrainReleasedNatives() -> std::vector<SlotNative> {
  std::lock_guard         lock(mutex_);
  std::vector<SlotNative> out(release_queue_.begin(), release_queue_.end());
  release_queue_.clear();
  return out;
}

auto DirectPresentQueue::CurrentTargetGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return target_generation_;
}

auto DirectPresentQueue::CurrentImageGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return image_generation_;
}

auto DirectPresentQueue::CurrentImageIdentity() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return image_identity_;
}

auto DirectPresentQueue::DiagnosticsSnapshot() const -> Diagnostics {
  std::lock_guard lock(mutex_);
  Diagnostics     diag;
  diag.backend                        = backend_;
  diag.target_generation              = target_generation_;
  diag.image_generation               = image_generation_;
  diag.image_identity                 = image_identity_;
  diag.last_composed_image_generation = last_composed_image_generation_;
  diag.last_composed_request_id       = last_composed_request_id_;
  diag.composed_frame_count           = composed_frame_count_;
  diag.dropped_stale_frame_count      = dropped_stale_frame_count_;
  diag.consumer_available             = consumer_available_ && !shutdown_;
  for (const auto& slot : slots_) {
    if (slot.native.valid()) {
      ++diag.live_target_count;
    }
    switch (slot.state) {
      case SlotState::Available:
        if (slot.native.valid()) {
          ++diag.available_count;
        }
        break;
      case SlotState::ProducerWriting:
        ++diag.producer_writing_count;
        break;
      case SlotState::Ready:
        ++diag.ready_count;
        break;
      case SlotState::RendererReading:
        break;
    }
  }
  return diag;
}

auto DirectPresentQueue::SlotAt(int index) const -> std::optional<SlotSnapshot> {
  std::lock_guard lock(mutex_);
  if (!IsValidSlot(index)) {
    return std::nullopt;
  }
  return SnapshotLocked(index);
}

auto DirectPresentQueue::HasWritableSlot(int width, int height, std::uint64_t image_generation,
                                         std::uint64_t image_identity) const -> bool {
  std::lock_guard lock(mutex_);
  for (int i = 0; i < kSlotCount; ++i) {
    const auto& slot = slots_[static_cast<std::size_t>(i)];
    if (slot.state == SlotState::Available && slot.native.valid() && slot.width == width &&
        slot.height == height &&
        (image_generation == 0 || slot.image_generation == image_generation) &&
        (image_identity == 0 || slot.image_identity == image_identity)) {
      return true;
    }
  }
  return false;
}

}  // namespace alcedo::editor_rhi
