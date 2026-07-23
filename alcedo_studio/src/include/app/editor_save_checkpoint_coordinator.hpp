//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <mutex>

#include "type/type.hpp"  // sl_element_id_t

namespace alcedo {

/// Project-wide editor save checkpoint coordinator for Phase 6C-5.
///
/// Owns the single global save lock that serializes editor materialization so
/// only one image save checkpoint runs at a time. The coordinator performs no
/// DuckDB I/O, journal folding, or pipeline work; it only grants or withholds
/// ownership. Callers acquire a SaveCheckpointLock before capturing a Mini-Git
/// journal prefix and hold it until DuckDB write, journal truncation, thumbnail
/// invalidation scheduling, and the terminal callback have all finished.
///
/// Thread context: TryAcquire, Release, active_element_id, and is_saving may be
/// called from any thread; an internal mutex serializes them. Intended to be
/// used as one shared instance per project.
class EditorSaveCheckpointCoordinator final {
 public:
  /// RAII ownership of the global save lock. Move-only. Releases the coordinator
  /// on destruction, explicit Release(), move-from, or any early-return/exception
  /// path, so the lock cannot be leaked. A default-constructed or moved-from lock
  /// owns nothing. This object is the sole ownership authority: do not infer
  /// ownership from a separate boolean.
  class SaveCheckpointLock {
   public:
    SaveCheckpointLock() = default;
    SaveCheckpointLock(EditorSaveCheckpointCoordinator* owner, sl_element_id_t element_id,
                       bool acquired);
    SaveCheckpointLock(const SaveCheckpointLock&)            = delete;
    SaveCheckpointLock& operator=(const SaveCheckpointLock&) = delete;
    SaveCheckpointLock(SaveCheckpointLock&& other) noexcept;
    SaveCheckpointLock& operator=(SaveCheckpointLock&& other) noexcept;
    ~SaveCheckpointLock();

    /// True when this handle currently owns the global save lock.
    [[nodiscard]] auto owns_lock() const -> bool { return owns_; }
    /// Relinquish ownership immediately. Idempotent; the destructor also calls
    /// it, so explicit Release() is only needed for early release before scope
    /// exit.
    void Release();

   private:
    EditorSaveCheckpointCoordinator* owner_      = nullptr;
    sl_element_id_t                  element_id_ = 0;
    bool                             owns_       = false;
  };

  /// Attempt to take the global save lock for element_id without blocking.
  /// Returns a lock whose owns_lock() is true iff no other save checkpoint is
  /// running. Safe to call from any thread.
  [[nodiscard]] auto TryAcquire(sl_element_id_t element_id) -> SaveCheckpointLock;
  /// Element id currently holding the save lock, or 0 when idle. Diagnostics
  /// only; do not use as an ownership test (a held SaveCheckpointLock is).
  [[nodiscard]] auto active_element_id() const -> sl_element_id_t;
  /// True while a save checkpoint owns the lock. Diagnostics only.
  [[nodiscard]] auto is_saving() const -> bool;

 private:
  friend class SaveCheckpointLock;
  void Release(sl_element_id_t element_id);

  mutable std::mutex mutex_;
  bool               saving_         = false;
  sl_element_id_t    active_element_ = 0;
};

/// Block the calling thread until the global save lock is acquired for
/// element_id, then return ownership. Returns a lock whose owns_lock() is true.
///
/// Caller context: the save worker thread (or any thread permitted to block);
/// never the GUI thread. Side effects: yields to the scheduler while another
/// checkpoint owns the lock. Failure result: cannot fail; it only returns once
/// ownership is obtained.
auto AcquireGlobalSaveLock(EditorSaveCheckpointCoordinator& coordinator,
                           sl_element_id_t                  element_id)
    -> EditorSaveCheckpointCoordinator::SaveCheckpointLock;

}  // namespace alcedo