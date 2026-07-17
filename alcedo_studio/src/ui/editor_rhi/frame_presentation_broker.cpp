//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/editor_rhi/frame_presentation_broker.hpp"

#include <algorithm>

namespace alcedo::editor_rhi {

FramePresentationBroker::FramePresentationBroker(EditorBackend backend) : backend_(backend) {}

auto FramePresentationBroker::SameTarget(const WritableTargetLease& lhs,
                                         const WritableTargetLease& rhs) -> bool {
  return lhs.native_handle == rhs.native_handle && lhs.native_handle != 0 &&
         lhs.generation.target_generation == rhs.generation.target_generation &&
         lhs.generation.image_generation == rhs.generation.image_generation &&
         lhs.lifetime_token == rhs.lifetime_token;
}

auto FramePresentationBroker::GenerationMatches(const TargetGeneration& actual,
                                                const TargetGeneration& expected) -> bool {
  return (expected.target_generation == 0 ||
          actual.target_generation == expected.target_generation) &&
         (expected.image_generation == 0 || actual.image_generation == expected.image_generation) &&
         (expected.layer_generation == 0 || actual.layer_generation == expected.layer_generation);
}

void FramePresentationBroker::QueueTargetForReleaseLocked(const WritableTargetLease& lease) {
  release_queue_.push_back(lease);
}

void FramePresentationBroker::DropCompletedForTargetLocked(const WritableTargetLease& lease) {
  for (auto it = completed_.begin(); it != completed_.end();) {
    if (SameTarget(it->frame.target, lease)) {
      ++dropped_stale_frame_count_;
      it = completed_.erase(it);
    } else {
      ++it;
    }
  }
}

auto FramePresentationBroker::PublishWritableTarget(WritableTargetLease lease) -> bool {
  if (!lease.valid()) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_ || lease.backend != backend_) {
    QueueTargetForReleaseLocked(lease);
    return false;
  }
  if (current_target_generation_ != 0 &&
      lease.generation.target_generation < current_target_generation_) {
    ++dropped_stale_frame_count_;
    QueueTargetForReleaseLocked(lease);
    return false;
  }
  if (current_image_generation_ != 0 &&
      lease.generation.image_generation < current_image_generation_) {
    ++dropped_stale_frame_count_;
    QueueTargetForReleaseLocked(lease);
    return false;
  }

  current_target_generation_ =
      std::max(current_target_generation_, lease.generation.target_generation);
  current_image_generation_ = lease.generation.image_generation;

  const auto duplicate = std::find_if(
      targets_.begin(), targets_.end(), [&lease](const TargetRecord& record) {
        return SameTarget(record.lease, lease);
      });
  if (duplicate != targets_.end()) {
    return true;
  }
  targets_.push_back(TargetRecord{std::move(lease), TargetState::Available});
  return true;
}

auto FramePresentationBroker::TryAcquireWritableTarget()
    -> std::optional<WritableTargetLease> {
  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_) {
    return std::nullopt;
  }
  for (auto& target : targets_) {
    if (target.state == TargetState::Available &&
        target.lease.generation.target_generation == current_target_generation_ &&
        target.lease.generation.image_generation == current_image_generation_) {
      target.state = TargetState::ProducerWriting;
      return target.lease;
    }
  }
  return std::nullopt;
}

auto FramePresentationBroker::SubmitCompletedFrame(CompletedFrameLease frame) -> bool {
  if (!frame.valid()) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (shutdown_ || !consumer_available_ ||
      frame.generation.target_generation != current_target_generation_ ||
      frame.generation.image_generation != current_image_generation_) {
    ++dropped_stale_frame_count_;
    return false;
  }

  const auto target = std::find_if(
      targets_.begin(), targets_.end(), [&frame](const TargetRecord& record) {
        return SameTarget(record.lease, frame.target);
      });
  if (target == targets_.end() || target->state != TargetState::ProducerWriting) {
    ++dropped_stale_frame_count_;
    return false;
  }

  target->state = TargetState::RendererConsuming;
  for (auto it = completed_.begin(); it != completed_.end();) {
    if (SameTarget(it->frame.target, frame.target)) {
      ++dropped_stale_frame_count_;
      it = completed_.erase(it);
    } else {
      ++it;
    }
  }
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
    if (it->frame.layer != layer ||
        !GenerationMatches(it->frame.generation, expected) ||
        it->frame.generation.target_generation != current_target_generation_ ||
        it->frame.generation.image_generation != current_image_generation_) {
      continue;
    }
    if (newest == completed_.end() || it->sequence > newest->sequence) {
      newest = it;
    }
  }
  if (newest == completed_.end()) {
    return std::nullopt;
  }

  CompletedFrameLease result = newest->frame;
  for (auto it = completed_.begin(); it != completed_.end();) {
    if (it->frame.layer == layer) {
      if (&*it != &*newest) {
        ++dropped_stale_frame_count_;
      }
      it = completed_.erase(it);
    } else {
      ++it;
    }
  }
  return result;
}

void FramePresentationBroker::CompleteRendererConsumption(const CompletedFrameLease& frame) {
  if (!frame.valid()) {
    return;
  }
  std::lock_guard lock(mutex_);
  const auto target = std::find_if(
      targets_.begin(), targets_.end(), [&frame](const TargetRecord& record) {
        return SameTarget(record.lease, frame.target);
      });
  if (target != targets_.end() && target->state == TargetState::RendererConsuming) {
    target->state = TargetState::Available;
    last_presented_image_generation_ = frame.generation.image_generation;
  }
}

void FramePresentationBroker::InvalidateLocked(std::optional<std::uint64_t> image_generation) {
  for (auto it = targets_.begin(); it != targets_.end();) {
    if (!image_generation.has_value() ||
        it->lease.generation.image_generation != *image_generation) {
      DropCompletedForTargetLocked(it->lease);
      QueueTargetForReleaseLocked(it->lease);
      it = targets_.erase(it);
    } else {
      ++it;
    }
  }
  completed_.clear();
}

void FramePresentationBroker::SetConsumerAvailable(bool available) {
  std::lock_guard lock(mutex_);
  if (consumer_available_ == available) {
    return;
  }
  consumer_available_ = available;
  if (!available) {
    InvalidateLocked(std::nullopt);
  }
}

void FramePresentationBroker::InvalidateTargetGeneration() {
  std::lock_guard lock(mutex_);
  InvalidateLocked(std::nullopt);
  ++current_target_generation_;
  current_image_generation_ = 0;
}

void FramePresentationBroker::InvalidateImageGeneration(std::uint64_t image_generation) {
  std::lock_guard lock(mutex_);
  InvalidateLocked(std::nullopt);
  current_image_generation_ = image_generation;
}

void FramePresentationBroker::Shutdown() {
  std::lock_guard lock(mutex_);
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  consumer_available_ = false;
  InvalidateLocked(std::nullopt);
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

auto FramePresentationBroker::CurrentTargetGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return current_target_generation_;
}

auto FramePresentationBroker::CurrentImageGeneration() const -> std::uint64_t {
  std::lock_guard lock(mutex_);
  return current_image_generation_;
}

auto FramePresentationBroker::DiagnosticsSnapshot() const -> Diagnostics {
  std::lock_guard lock(mutex_);
  return Diagnostics{backend_, current_target_generation_, last_presented_image_generation_,
                      dropped_stale_frame_count_, targets_.size(),
                      consumer_available_ && !shutdown_};
}

}  // namespace alcedo::editor_rhi
