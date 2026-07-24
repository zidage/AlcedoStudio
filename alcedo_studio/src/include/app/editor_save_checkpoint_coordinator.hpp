//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <condition_variable>
#include <mutex>

#include "type/type.hpp"  // sl_element_id_t

namespace alcedo {

/// Project-wide editor save checkpoint coordinator.
///
/// Owns the single global save lock that serializes editor materialization so
/// only one image save checkpoint runs at a time. The coordinator performs no
/// DuckDB I/O, journal folding, or pipeline work; it only grants or withholds
/// ownership. Callers acquire a SaveCheckpointLock before capturing a Mini-Git
/// journal prefix and hold it until DuckDB write, journal truncation, thumbnail
/// invalidation scheduling, and durable save work have all finished. Terminal
/// callbacks run after release so image navigation may acquire the same lock
/// for Mini-Git recovery.
///
/// Ownership model: the move-only SaveCheckpointLock is the sole ownership
/// authority. active_element_id() and is_saving() are diagnostics only and must
/// not be treated as ownership tests.
///
/// Lifetime: construct exactly one instance per open editor/project composition
/// root (ApplicationModuleHost / EditorSessionRuntime) and inject that shared
/// instance into EditorSaveCheckpointService and EditorMiniGitMaterializer.
/// Call Shutdown() during project teardown so blocked waiters exit cleanly.
///
/// Thread context: TryAcquire, AcquireBlocking, Release, active_element_id,
/// is_saving, and Shutdown may be called from any thread; an internal mutex
/// serializes them. AcquireBlocking must not run on the GUI thread.
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
    /// Element id stamped when ownership was granted; 0 when not owning.
    [[nodiscard]] auto element_id() const -> sl_element_id_t { return element_id_; }
    /// Relinquish ownership immediately. Idempotent; the destructor also calls
    /// it, so explicit Release() is only needed for early release before scope
    /// exit.
    void               Release();

   private:
    EditorSaveCheckpointCoordinator* owner_      = nullptr;
    sl_element_id_t                  element_id_ = 0;
    bool                             owns_       = false;
  };

  /// Attempt to take the global save lock for element_id without blocking.
  /// Returns a lock whose owns_lock() is true iff no other save checkpoint is
  /// running and Shutdown() has not been called. Safe to call from any thread,
  /// including the GUI thread.
  [[nodiscard]] auto TryAcquire(sl_element_id_t element_id) -> SaveCheckpointLock;

  /// Block the calling thread until the global save lock is acquired, or until
  /// Shutdown() wakes waiters. Uses a condition variable (no busy-wait). Must
  /// not be called from the GUI thread. On shutdown, returns a lock whose
  /// owns_lock() is false so the waiter can exit and join cleanly.
  [[nodiscard]] auto AcquireBlocking(sl_element_id_t element_id) -> SaveCheckpointLock;

  /// Wake every blocked AcquireBlocking waiter and refuse new acquisitions.
  /// Idempotent. Does not force-release a currently held SaveCheckpointLock;
  /// that lock still releases ownership when its owner destroys or Releases it.
  /// Call during project/editor composition teardown before joining workers.
  void Shutdown();

  /// True after Shutdown() has been invoked. Diagnostics only.
  [[nodiscard]] auto is_shutdown() const -> bool;

  /// Element id currently holding the save lock, or 0 when idle. Diagnostics
  /// only; do not use as an ownership test (a held SaveCheckpointLock is).
  [[nodiscard]] auto active_element_id() const -> sl_element_id_t;

  /// True while a save checkpoint owns the lock. Diagnostics only; do not use
  /// as ownership authority in place of SaveCheckpointLock.
  [[nodiscard]] auto is_saving() const -> bool;

 private:
  friend class SaveCheckpointLock;
  void                    Release(sl_element_id_t element_id);

  mutable std::mutex      mutex_;
  std::condition_variable condition_;
  bool                    saving_         = false;
  bool                    shutting_down_  = false;
  sl_element_id_t         active_element_ = 0;
};

/// Block until the global save lock is acquired for element_id, then return
/// ownership. Equivalent to coordinator.AcquireBlocking(element_id).
///
/// Caller context: the save worker thread (or any thread permitted to block);
/// never the GUI thread. Side effects: waits on the coordinator condition
/// variable while another checkpoint owns the lock, or returns a non-owning
/// lock if Shutdown() has been called. Failure result: owns_lock() is false
/// only after project shutdown; otherwise ownership is obtained.
auto AcquireGlobalSaveLock(EditorSaveCheckpointCoordinator& coordinator, sl_element_id_t element_id)
    -> EditorSaveCheckpointCoordinator::SaveCheckpointLock;

}  // namespace alcedo
