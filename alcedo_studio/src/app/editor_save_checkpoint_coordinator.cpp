//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_save_checkpoint_coordinator.hpp"

namespace alcedo {

// ── SaveCheckpointLock ──────────────────────────────────────────────────────

EditorSaveCheckpointCoordinator::SaveCheckpointLock::SaveCheckpointLock(
    EditorSaveCheckpointCoordinator* owner, sl_element_id_t element_id, bool acquired)
    : owner_(owner), element_id_(element_id), owns_(acquired) {}

EditorSaveCheckpointCoordinator::SaveCheckpointLock::SaveCheckpointLock(
    SaveCheckpointLock&& other) noexcept
    : owner_(other.owner_), element_id_(other.element_id_), owns_(other.owns_) {
  other.owner_      = nullptr;
  other.element_id_ = 0;
  other.owns_       = false;
}

auto EditorSaveCheckpointCoordinator::SaveCheckpointLock::operator=(
    SaveCheckpointLock&& other) noexcept -> SaveCheckpointLock& {
  if (this != &other) {
    Release();
    owner_            = other.owner_;
    element_id_       = other.element_id_;
    owns_             = other.owns_;
    other.owner_      = nullptr;
    other.element_id_ = 0;
    other.owns_       = false;
  }
  return *this;
}

EditorSaveCheckpointCoordinator::SaveCheckpointLock::~SaveCheckpointLock() { Release(); }

void EditorSaveCheckpointCoordinator::SaveCheckpointLock::Release() {
  if (owns_ && owner_ != nullptr) {
    owner_->Release(element_id_);
    owns_ = false;
  }
  owner_      = nullptr;
  element_id_ = 0;
}

// ── EditorSaveCheckpointCoordinator ──────────────────────────────────────────

auto EditorSaveCheckpointCoordinator::TryAcquire(sl_element_id_t element_id) -> SaveCheckpointLock {
  std::scoped_lock lock(mutex_);
  if (saving_) {
    return SaveCheckpointLock(this, element_id, false);
  }
  saving_         = true;
  active_element_ = element_id;
  return SaveCheckpointLock(this, element_id, true);
}

void EditorSaveCheckpointCoordinator::Release(sl_element_id_t element_id) {
  {
    std::scoped_lock lock(mutex_);
    if (!saving_ || active_element_ != element_id) {
      return;
    }
    saving_         = false;
    active_element_ = 0;
  }
  condition_.notify_one();
}

auto EditorSaveCheckpointCoordinator::active_element_id() const -> sl_element_id_t {
  std::scoped_lock lock(mutex_);
  return active_element_;
}

auto EditorSaveCheckpointCoordinator::is_saving() const -> bool {
  std::scoped_lock lock(mutex_);
  return saving_;
}

// ── Blocking acquire ────────────────────────────────────────────────────────

auto EditorSaveCheckpointCoordinator::AcquireBlocking(sl_element_id_t element_id)
    -> SaveCheckpointLock {
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [this] { return !saving_; });
  saving_         = true;
  active_element_ = element_id;
  return SaveCheckpointLock(this, element_id, true);
}

// ── Free-function wrapper (kept for backward compatibility) ──────────────────

auto AcquireGlobalSaveLock(EditorSaveCheckpointCoordinator& coordinator, sl_element_id_t element_id)
    -> EditorSaveCheckpointCoordinator::SaveCheckpointLock {
  return coordinator.AcquireBlocking(element_id);
}

}  // namespace alcedo
