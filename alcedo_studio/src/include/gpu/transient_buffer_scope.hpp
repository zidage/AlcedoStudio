//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <utility>

#include "gpu/transient_buffer_arena.hpp"

namespace alcedo {

/**
 * @brief RAII nested bump scope. Rewinds @ref TransientBufferArena to the mark
 * taken at construction. Allocations inside the scope are invalid after Release.
 *
 * LIFO only. Not thread-safe.
 */
template <class Backend>
class TransientBufferScope {
 public:
  explicit TransientBufferScope(TransientBufferArena<Backend>& arena)
      : arena_(&arena), mark_(arena.used_bytes()) {}

  TransientBufferScope(const TransientBufferScope&)            = delete;
  auto operator=(const TransientBufferScope&) -> TransientBufferScope& = delete;

  TransientBufferScope(TransientBufferScope&& other) noexcept
      : arena_(other.arena_), mark_(other.mark_) {
    other.arena_ = nullptr;
  }

  auto operator=(TransientBufferScope&& other) noexcept -> TransientBufferScope& {
    if (this != &other) {
      Release();
      arena_       = other.arena_;
      mark_        = other.mark_;
      other.arena_ = nullptr;
    }
    return *this;
  }

  ~TransientBufferScope() { Release(); }

  void Release() noexcept {
    if (arena_ != nullptr) {
      arena_->RewindUnchecked(mark_);
      arena_ = nullptr;
    }
  }

  [[nodiscard]] auto mark() const noexcept -> std::size_t { return mark_; }

 private:
  TransientBufferArena<Backend>* arena_ = nullptr;
  std::size_t                    mark_  = 0;
};

}  // namespace alcedo
