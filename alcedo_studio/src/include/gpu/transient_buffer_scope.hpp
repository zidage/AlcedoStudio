//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <utility>
#include <vector>

#include "gpu/transient_buffer_arena.hpp"

namespace alcedo {

/**
 * @brief RAII nested bump scope.
 *
 * SessionPacked rewinds bump offsets. ExactRelease also destroys slabs allocated
 * after the captured mark. Caller must satisfy GPU last-use before Release.
 * LIFO only. Not thread-safe.
 */
template <class Backend>
class TransientBufferScope {
 public:
  explicit TransientBufferScope(TransientBufferArena<Backend>& arena)
      : arena_(&arena), mark_(arena.CaptureMark()) {}

  TransientBufferScope(const TransientBufferScope&)            = delete;
  auto operator=(const TransientBufferScope&) -> TransientBufferScope& = delete;

  TransientBufferScope(TransientBufferScope&& other) noexcept
      : arena_(other.arena_), mark_(std::move(other.mark_)) {
    other.arena_ = nullptr;
  }

  auto operator=(TransientBufferScope&& other) noexcept -> TransientBufferScope& {
    if (this != &other) {
      Release();
      arena_       = other.arena_;
      mark_        = std::move(other.mark_);
      other.arena_ = nullptr;
    }
    return *this;
  }

  ~TransientBufferScope() { Release(); }

  void Release() noexcept {
    if (arena_ != nullptr) {
      if (arena_->allocation_policy() == TransientAllocationPolicy::ExactRelease) {
        arena_->ReleaseSlabsAfterMark(mark_);
      } else {
        arena_->RewindToMark(mark_);
      }
      arena_ = nullptr;
    }
  }

  [[nodiscard]] auto mark() const noexcept -> const typename TransientBufferArena<Backend>::Mark& {
    return mark_;
  }

 private:
  TransientBufferArena<Backend>*                    arena_ = nullptr;
  typename TransientBufferArena<Backend>::Mark mark_;
};

/**
 * @brief Restore the previous TransientAllocationPolicy when destroyed.
 *
 * Develop uses ExactRelease so finished full-frame planes can be freed
 * independently. Mask and other later stages may still use SessionPacked.
 */
template <class Backend>
class TransientAllocationPolicyScope {
 public:
  TransientAllocationPolicyScope(TransientBufferArena<Backend>& arena,
                                 TransientAllocationPolicy policy)
      : arena_(&arena), previous_(arena.allocation_policy()) {
    arena.SetAllocationPolicy(policy);
  }

  TransientAllocationPolicyScope(const TransientAllocationPolicyScope&) = delete;
  auto operator=(const TransientAllocationPolicyScope&)
      -> TransientAllocationPolicyScope& = delete;

  TransientAllocationPolicyScope(TransientAllocationPolicyScope&& other) noexcept
      : arena_(other.arena_), previous_(other.previous_) {
    other.arena_ = nullptr;
  }

  auto operator=(TransientAllocationPolicyScope&& other) noexcept
      -> TransientAllocationPolicyScope& {
    if (this != &other) {
      Restore();
      arena_         = other.arena_;
      previous_      = other.previous_;
      other.arena_   = nullptr;
    }
    return *this;
  }

  ~TransientAllocationPolicyScope() { Restore(); }

 private:
  void Restore() noexcept {
    if (arena_ != nullptr) {
      arena_->SetAllocationPolicy(previous_);
      arena_ = nullptr;
    }
  }

  TransientBufferArena<Backend>* arena_    = nullptr;
  TransientAllocationPolicy      previous_ = TransientAllocationPolicy::SessionPacked;
};

}  // namespace alcedo
