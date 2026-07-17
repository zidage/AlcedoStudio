//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/frame_presentation_broker.hpp"

#include <algorithm>

namespace alcedo::editor_rhi {
namespace {

auto LayerIndex(LeaseFrameLayer layer) -> std::size_t {
  return static_cast<std::size_t>(layer);
}

}  // namespace

FramePresentationBroker::FramePresentationBroker(EditorBackend backend) : backend_(backend) {}

auto FramePresentationBroker::SameTarget(const WritableTargetLease& lhs,
                                         const WritableTargetLease& rhs) -> bool {
  return lhs.native_handle == rhs.native_handle && lhs.native_handle != 0 &&
         lhs.generation.target_generation == rhs.generation.target_generation &&
         lhs.generation.image_generation == rhs.generation.image_generation &&
         lhs.generation.image_identity == rhs.generation.image_identity &&
         lhs.lifetime_token == rhs.lifetime_token;
}

auto FramePresentationBroker::GenerationMatches(const TargetGeneration& actual,
                                                const TargetGeneration& expected) -> bool {
  return (expected.target_generation == 0 ||
          actual.target_generation == expected.target_generation) &&
         (expected.image_generation == 0 || actual.image_generation == expected.image_generation) &&
         (expected.layer_generation == 0 || actual.layer_generation == expected.layer_generation) &&
         (expected.image_identity == 0 || actual.image_identity == expected.image_identity);
}

auto FramePresentationBroker::IsNewerFrame(const CompletedFrameLease& candidate,
                                           const CompletedFrameLease& current,
                                           std::uint64_t candidate_sequence,
                                           std::uint64_t current_sequence) -> bool {
  if (candidate.preview_generation != current.preview_generation) {
    return candidate.preview_generation > current.preview_generation;
  }
  if (candidate.detail_serial != current.detail_serial) {
    return candidate.detail_serial > current.detail_serial;
  }
  return candidate_sequence > current_sequence;
}

void FramePresentationBroker::QueueTargetForReleaseLocked(WritableTargetLease lease) {
  if (lease.lifetime_token) {
    lease.lifetime_token->RequestCancel();
    // Targets that never left Available still need producer_complete so the
    // adapter can destroy after DrainReleasedTargets.
    if (!lease.lifetime_token->producer_complete.load(std::memory_order_acquire)) {
      lease.lifetime_token->MarkProducerComplete();
    }
  }
  release_queue_.push_back(std::move(lease));
}

void FramePresentationBroker::DropCompletedForTargetLocked(const WritableTargetLease& lease,
                                                           bool recycle_to_available) {
  for (auto it = completed_.begin(); it != completed_.end();) {
    if (!SameTarget(it->frame.target, lease)) {
      ++it;
      continue;
    }
    ++dropped_stale_frame_count_;
    if (recycle_to_available) {
      if (auto* record = FindTargetLocked(it->frame.target)) {
        if (record->state == TargetState::RendererConsuming) {
          record->state = TargetState::Available;
        }
      }
    }
    it = completed_.erase(it);
  }
}

auto FramePresentationBroker::FindTargetLocked(const WritableTargetLease& lease)
    -> TargetRecord* {
  for (auto& target : targets_) {
    if (SameTarget(target.lease, lease)) {
      return &target;
    }
  }
  return nullptr;
}

void FramePresentationBroker::RecycleOrReleaseTargetLocked(TargetRecord& record) {
  if (record.release_when_idle || shutdown_ || !consumer_available_) {
    record.state = TargetState::PendingRelease;
    QueueTargetForReleaseLocked(record.lease);
    record.state = TargetState::PendingRelease;
  } else {
    record.state = TargetState::Available;
  }
}

auto FramePresentationBroker::AcceptsFrameLocked(const CompletedFrameLease& frame) const -> bool {
  if (shutdown_ || !consumer_available_) {
    return false;
  }
  if (frame.generation.target_generation != current_target_generation_ ||
      frame.generation.image_generation != current_image_generation_) {
    return false;
  }
  if (current_image_identity_ != 0 &&
      frame.generation.image_identity != current_image_identity_) {
    return false;
  }
  const auto layer_i = LayerIndex(frame.layer);
  if (layer_i >= last_accepted_preview_generation_.size()) {
    return false;
  }
  if (frame.preview_generation < last_accepted_preview_generation_[layer_i]) {
    return false;
  }
  if (frame.preview_generation == last_accepted_preview_generation_[layer_i] &&
      frame.detail_serial < last_accepted_detail_serial_[layer_i]) {
    return false;
  }
  return true;
}

auto FramePresentationBroker::PublishWritableTarget(WritableTargetLease lease) -> bool {
  if (!lease.valid()) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_ || lease.backend != backend_) {
    QueueTargetForReleaseLocked(std::move(lease));
    return false;
  }
  if (current_target_generation_ != 0 &&
      lease.generation.target_generation < current_target_generation_) {
    ++dropped_stale_frame_count_;
    QueueTargetForReleaseLocked(std::move(lease));
    return false;
  }
  if (current_image_generation_ != 0 &&
      lease.generation.image_generation != current_image_generation_) {
    ++dropped_stale_frame_count_;
    QueueTargetForReleaseLocked(std::move(lease));
    return false;
  }
  if (current_image_identity_ != 0 &&
      lease.generation.image_identity != current_image_identity_) {
    ++dropped_stale_frame_count_;
    QueueTargetForReleaseLocked(std::move(lease));
    return false;
  }

  current_target_generation_ =
      std::max(current_target_generation_, lease.generation.target_generation);
  current_image_generation_ = lease.generation.image_generation;
  if (lease.generation.image_identity != 0) {
    current_image_identity_ = lease.generation.image_identity;
  }

  const auto duplicate = std::find_if(
      targets_.begin(), targets_.end(), [&lease](const TargetRecord& record) {
        return SameTarget(record.lease, lease);
      });
  if (duplicate != targets_.end()) {
    return true;
  }
  targets_.push_back(TargetRecord{std::move(lease), TargetState::Available, false});
  return true;
}

auto FramePresentationBroker::TryAcquireWritableTarget(const WritableTargetRequest& request)
    -> std::optional<WritableTargetLease> {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_ || !request.valid()) {
    if (request.valid() && !shutdown_) {
      pending_requests_.push_back(request);
    }
    return std::nullopt;
  }
  if (current_image_generation_ != 0 &&
      request.image_generation != current_image_generation_) {
    return std::nullopt;
  }
  if (current_image_identity_ != 0 && request.image_identity != 0 &&
      request.image_identity != current_image_identity_) {
    return std::nullopt;
  }

  for (auto& target : targets_) {
    if (target.state != TargetState::Available) {
      continue;
    }
    if (target.lease.generation.target_generation != current_target_generation_) {
      continue;
    }
    if (target.lease.generation.image_generation != current_image_generation_ &&
        current_image_generation_ != 0) {
      continue;
    }
    if (target.lease.dimensions != request.dimensions) {
      continue;
    }
    // Layer on the lease is a preferred pool tag; any Available target of the
    // right size may be claimed and retagged for the requested layer.
    target.state = TargetState::ProducerWriting;
    target.lease.layer = request.layer;
    target.lease.generation.layer_generation = request.layer_generation;
    if (target.lease.lifetime_token) {
      target.lease.lifetime_token->cancel_requested.store(false, std::memory_order_release);
      target.lease.lifetime_token->producer_complete.store(false, std::memory_order_release);
    }
    return target.lease;
  }

  pending_requests_.push_back(request);
  return std::nullopt;
}

auto FramePresentationBroker::TryAcquireWritableTarget() -> std::optional<WritableTargetLease> {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_) {
    return std::nullopt;
  }
  for (auto& target : targets_) {
    if (target.state == TargetState::Available &&
        target.lease.generation.target_generation == current_target_generation_ &&
        (current_image_generation_ == 0 ||
         target.lease.generation.image_generation == current_image_generation_)) {
      target.state = TargetState::ProducerWriting;
      if (target.lease.lifetime_token) {
        target.lease.lifetime_token->cancel_requested.store(false, std::memory_order_release);
        target.lease.lifetime_token->producer_complete.store(false, std::memory_order_release);
      }
      return target.lease;
    }
  }
  return std::nullopt;
}

void FramePresentationBroker::AbandonProducerWrite(const WritableTargetLease& lease) {
  std::lock_guard lock(mutex_);
  auto* record = FindTargetLocked(lease);
  if (!record || record->state != TargetState::ProducerWriting) {
    return;
  }
  if (record->lease.lifetime_token) {
    record->lease.lifetime_token->MarkProducerComplete();
  }
  if (record->release_when_idle || shutdown_ || !consumer_available_ ||
      record->lease.generation.target_generation != current_target_generation_ ||
      record->lease.generation.image_generation != current_image_generation_) {
    QueueTargetForReleaseLocked(record->lease);
    targets_.erase(std::find_if(targets_.begin(), targets_.end(),
                                [&lease](const TargetRecord& r) {
                                  return SameTarget(r.lease, lease);
                                }));
  } else {
    record->state = TargetState::Available;
  }
}

auto FramePresentationBroker::SubmitCompletedFrame(CompletedFrameLease frame) -> bool {
  if (!frame.valid()) {
    return false;
  }

  std::lock_guard lock(mutex_);
  auto* target = FindTargetLocked(frame.target);
  if (!target || target->state != TargetState::ProducerWriting) {
    ++dropped_stale_frame_count_;
    return false;
  }

  if (target->lease.lifetime_token) {
    target->lease.lifetime_token->MarkProducerComplete();
  }

  if (!AcceptsFrameLocked(frame) || target->release_when_idle) {
    ++dropped_stale_frame_count_;
    QueueTargetForReleaseLocked(target->lease);
    targets_.erase(std::find_if(targets_.begin(), targets_.end(),
                                [&frame](const TargetRecord& r) {
                                  return SameTarget(r.lease, frame.target);
                                }));
    return false;
  }

  // Drop older completed frames for the same layer and recycle their targets.
  for (auto it = completed_.begin(); it != completed_.end();) {
    if (it->frame.layer != frame.layer) {
      ++it;
      continue;
    }
    const bool older =
        it->frame.preview_generation < frame.preview_generation ||
        (it->frame.preview_generation == frame.preview_generation &&
         it->frame.detail_serial < frame.detail_serial);
    if (older || SameTarget(it->frame.target, frame.target)) {
      ++dropped_stale_frame_count_;
      if (auto* older_target = FindTargetLocked(it->frame.target)) {
        if (older_target->state == TargetState::RendererConsuming) {
          older_target->state = TargetState::Available;
        }
      }
      it = completed_.erase(it);
    } else {
      ++it;
    }
  }

  const auto layer_i = LayerIndex(frame.layer);
  last_accepted_preview_generation_[layer_i] =
      std::max(last_accepted_preview_generation_[layer_i], frame.preview_generation);
  last_accepted_detail_serial_[layer_i] =
      std::max(last_accepted_detail_serial_[layer_i], frame.detail_serial);

  target->state = TargetState::RendererConsuming;
  // Carry the producer's retagged layer onto the completed record target.
  frame.target.layer = frame.layer;
  frame.target.generation.layer_generation = frame.generation.layer_generation;
  completed_.push_back(CompletedRecord{std::move(frame), ++sequence_});
  return true;
}

auto FramePresentationBroker::ConsumeNewestCompletedFrame(TargetGeneration expected,
                                                          LeaseFrameLayer layer)
    -> std::optional<CompletedFrameLease> {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_) {
    return std::nullopt;
  }

  auto newest = completed_.end();
  for (auto it = completed_.begin(); it != completed_.end(); ++it) {
    if (it->frame.layer != layer) {
      continue;
    }
    if (!GenerationMatches(it->frame.generation, expected)) {
      continue;
    }
    // Always reject frames that are no longer the active image/target session.
    if (current_target_generation_ != 0 &&
        it->frame.generation.target_generation != current_target_generation_) {
      continue;
    }
    if (current_image_generation_ != 0 &&
        it->frame.generation.image_generation != current_image_generation_) {
      continue;
    }
    if (current_image_identity_ != 0 &&
        it->frame.generation.image_identity != current_image_identity_) {
      continue;
    }
    if (newest == completed_.end() ||
        IsNewerFrame(it->frame, newest->frame, it->sequence, newest->sequence)) {
      newest = it;
    }
  }
  if (newest == completed_.end()) {
    return std::nullopt;
  }

  CompletedFrameLease result = newest->frame;
  std::vector<WritableTargetLease> recycle;
  std::vector<WritableTargetLease> release;
  recycle.reserve(completed_.size());
  for (auto it = completed_.begin(); it != completed_.end();) {
    if (it->frame.layer != layer) {
      ++it;
      continue;
    }
    if (&*it != &*newest) {
      ++dropped_stale_frame_count_;
      if (auto* older_target = FindTargetLocked(it->frame.target)) {
        if (older_target->state == TargetState::RendererConsuming) {
          if (older_target->release_when_idle) {
            release.push_back(older_target->lease);
          } else {
            recycle.push_back(older_target->lease);
          }
        }
      }
    }
    it = completed_.erase(it);
  }
  for (const auto& lease : recycle) {
    if (auto* record = FindTargetLocked(lease)) {
      record->state = TargetState::Available;
      if (record->lease.lifetime_token) {
        record->lease.lifetime_token->renderer_complete.store(false, std::memory_order_release);
        record->lease.lifetime_token->producer_complete.store(false, std::memory_order_release);
      }
    }
  }
  for (auto& lease : release) {
    QueueTargetForReleaseLocked(lease);
    targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                                  [&lease](const TargetRecord& r) {
                                    return SameTarget(r.lease, lease);
                                  }),
                   targets_.end());
  }
  return result;
}

void FramePresentationBroker::CompleteRendererConsumption(const CompletedFrameLease& frame) {
  if (!frame.valid() && !frame.target.valid()) {
    return;
  }
  std::lock_guard lock(mutex_);
  auto* target = FindTargetLocked(frame.target);
  if (!target) {
    return;
  }
  if (target->lease.lifetime_token) {
    target->lease.lifetime_token->MarkRendererComplete();
  }
  if (target->state == TargetState::RendererConsuming ||
      target->state == TargetState::PendingRelease) {
    if (target->release_when_idle || shutdown_ || !consumer_available_ ||
        target->lease.generation.target_generation != current_target_generation_ ||
        target->lease.generation.image_generation != current_image_generation_) {
      QueueTargetForReleaseLocked(target->lease);
      targets_.erase(std::find_if(targets_.begin(), targets_.end(),
                                  [&frame](const TargetRecord& r) {
                                    return SameTarget(r.lease, frame.target);
                                  }));
    } else {
      target->state = TargetState::Available;
      // Ready for another write cycle; clear renderer-complete for reuse.
      if (target->lease.lifetime_token) {
        target->lease.lifetime_token->renderer_complete.store(false, std::memory_order_release);
        target->lease.lifetime_token->producer_complete.store(false, std::memory_order_release);
      }
      last_presented_image_generation_ = frame.generation.image_generation;
    }
  }
}

void FramePresentationBroker::InvalidateLocked(bool bump_target_generation) {
  for (auto it = targets_.begin(); it != targets_.end();) {
    DropCompletedForTargetLocked(it->lease, false);
    if (it->state == TargetState::ProducerWriting) {
      // Do not destroy while the producer still holds the writable resource.
      it->release_when_idle = true;
      if (it->lease.lifetime_token) {
        it->lease.lifetime_token->RequestCancel();
      }
      ++it;
      continue;
    }
    if (it->state == TargetState::RendererConsuming) {
      it->release_when_idle = true;
      if (it->lease.lifetime_token) {
        it->lease.lifetime_token->RequestCancel();
      }
      // Keep until CompleteRendererConsumption.
      ++it;
      continue;
    }
    QueueTargetForReleaseLocked(it->lease);
    it = targets_.erase(it);
  }
  completed_.clear();
  if (bump_target_generation) {
    ++current_target_generation_;
  }
  last_accepted_preview_generation_.fill(0);
  last_accepted_detail_serial_.fill(0);
  pending_requests_.clear();
}

void FramePresentationBroker::SetConsumerAvailable(bool available) {
  std::lock_guard lock(mutex_);
  if (consumer_available_ == available) {
    return;
  }
  consumer_available_ = available;
  if (!available) {
    InvalidateLocked(false);
  }
}

void FramePresentationBroker::InvalidateTargetGeneration() {
  std::lock_guard lock(mutex_);
  // Resize / scene-graph rebuild advances target generation only. Image
  // session identity stays so the same open continues after the pool rebuild.
  InvalidateLocked(true);
}

void FramePresentationBroker::InvalidateImageGeneration(std::uint64_t image_generation,
                                                        std::uint64_t image_identity) {
  std::lock_guard lock(mutex_);
  InvalidateLocked(false);
  current_image_generation_ = image_generation;
  current_image_identity_ = image_identity;
}

void FramePresentationBroker::Shutdown() {
  std::lock_guard lock(mutex_);
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  consumer_available_ = false;
  InvalidateLocked(false);
}

auto FramePresentationBroker::DrainReleasedTargets() -> std::vector<WritableTargetLease> {
  std::lock_guard lock(mutex_);
  std::vector<WritableTargetLease> result;
  result.reserve(release_queue_.size());
  while (!release_queue_.empty()) {
    result.push_back(std::move(release_queue_.front()));
    release_queue_.pop_front();
  }
  return result;
}

void FramePresentationBroker::NoteTargetRequest(const WritableTargetRequest& request) {
  if (!request.valid()) {
    return;
  }
  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_) {
    return;
  }
  pending_requests_.push_back(request);
}

auto FramePresentationBroker::DrainTargetRequests() -> std::vector<WritableTargetRequest> {
  std::lock_guard lock(mutex_);
  std::vector<WritableTargetRequest> result;
  result.reserve(pending_requests_.size());
  while (!pending_requests_.empty()) {
    result.push_back(pending_requests_.front());
    pending_requests_.pop_front();
  }
  return result;
}

auto FramePresentationBroker::CurrentTargetGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return current_target_generation_;
}

auto FramePresentationBroker::CurrentImageGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return current_image_generation_;
}

auto FramePresentationBroker::CurrentImageIdentity() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return current_image_identity_;
}

auto FramePresentationBroker::DiagnosticsSnapshot() const -> Diagnostics {
  std::lock_guard lock(mutex_);
  std::size_t writing = 0;
  std::size_t available = 0;
  for (const auto& target : targets_) {
    if (target.state == TargetState::ProducerWriting) {
      ++writing;
    } else if (target.state == TargetState::Available) {
      ++available;
    }
  }
  return Diagnostics{backend_,
                     current_target_generation_,
                     current_image_generation_,
                     current_image_identity_,
                     last_presented_image_generation_,
                     dropped_stale_frame_count_,
                     targets_.size(),
                     available,
                     writing,
                     consumer_available_ && !shutdown_};
}

}  // namespace alcedo::editor_rhi
