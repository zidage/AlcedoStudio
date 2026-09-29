//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace alcedo {

/**
 * @brief Get write access to a copy-on-write part of a pipeline document.
 *
 * Document parts (graph nodes, adjustment Models, the Mask list of a Color Grade) are held as
 * `shared_ptr<const T>`. @ref PipelineDocument::Freeze shares them with an immutable frozen
 * document. Before the working document writes a part, it calls this function: when another
 * holder shares the part, @p clone makes a private copy and @p part points to that copy; when
 * this holder is the only one, the part is written in place.
 *
 * Every part is created as a non-const object (never `make_shared<const T>`), so the returned
 * reference may be written.
 *
 * @pre Every holder that can write @p part does so on one thread or under one lock. Frozen
 *      documents never write. Under this rule a use count of one cannot increase while the
 *      caller writes, because only the writing holder can share the part again (by freezing).
 * @param part Holder of the part; replaced by the private copy when it was shared.
 * @param clone Callable `(const T&) -> shared_ptr<T>` (or a type convertible to it).
 * @return The part that only @p part holds.
 */
template <class T, class Clone>
auto UnshareForWrite(std::shared_ptr<const T>& part, Clone&& clone) -> T& {
  if (part.use_count() > 1) {
    std::shared_ptr<T> copy = std::forward<Clone>(clone)(*part);
    part                    = copy;
    return *copy;
  }
  // The last other holder may have released the part on another thread just now. Its reads
  // happen before its reference count decrement (release); this fence orders them before our
  // writes.
  std::atomic_thread_fence(std::memory_order_acquire);
  return const_cast<T&>(*part);
}

}  // namespace alcedo
