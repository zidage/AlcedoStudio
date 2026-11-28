//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <iterator>
#include <stdexcept>

namespace alcedo {

/**
 * @brief The link of one object in one IntrusiveList.
 *
 * The object holds the hook as a member, so a list operation allocates no memory. A hook is in
 * at most one list. A copy of a hook is unlinked: copying the object that holds it does not put
 * the copy into the list.
 */
template <typename T>
class IntrusiveListHook {
 public:
  IntrusiveListHook() = default;
  IntrusiveListHook(const IntrusiveListHook&) noexcept {}
  auto operator=(const IntrusiveListHook&) noexcept -> IntrusiveListHook& { return *this; }

  /// True while the hook is in a list.
  [[nodiscard]] auto IsLinked() const -> bool { return next_ != nullptr; }

 private:
  template <typename U, IntrusiveListHook<U> U::*>
  friend class IntrusiveList;

  IntrusiveListHook* prev_  = nullptr;
  IntrusiveListHook* next_  = nullptr;
  T*                 owner_ = nullptr;
};

/**
 * @brief A doubly linked list of objects that hold an IntrusiveListHook member.
 *
 * The list does not own the objects. An object must leave the list before it is destroyed.
 * Every operation is O(1) and allocates no memory. Not thread-safe: the owner of the list
 * serializes the calls.
 *
 * @tparam T     The object type.
 * @tparam Hook  The hook member of T that this list uses.
 */
template <typename T, IntrusiveListHook<T> T::* Hook>
class IntrusiveList {
 public:
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = T;
    using difference_type   = std::ptrdiff_t;
    using pointer           = T*;
    using reference         = T&;

    Iterator()              = default;
    explicit Iterator(const IntrusiveListHook<T>* node) : node_(node) {}

    auto operator*() const -> T& { return *node_->owner_; }
    auto operator->() const -> T* { return node_->owner_; }
    auto operator++() -> Iterator& {
      node_ = node_->next_;
      return *this;
    }
    auto operator++(int) -> Iterator {
      auto previous = *this;
      node_         = node_->next_;
      return previous;
    }
    auto operator==(const Iterator& other) const -> bool { return node_ == other.node_; }

   private:
    const IntrusiveListHook<T>* node_ = nullptr;
  };

  IntrusiveList() { head_.prev_ = head_.next_ = &head_; }
  IntrusiveList(const IntrusiveList&)                    = delete;
  auto operator=(const IntrusiveList&) -> IntrusiveList& = delete;
  /// Unlinks every object that is still in the list.
  ~IntrusiveList() { Clear(); }

  [[nodiscard]] auto Empty() const -> bool { return size_ == 0; }
  [[nodiscard]] auto Size() const -> std::size_t { return size_; }

  /// Appends @p object. Throws std::logic_error when the hook of @p object is already linked.
  void               PushBack(T& object) {
    auto& hook = object.*Hook;
    if (hook.IsLinked()) {
      throw std::logic_error("IntrusiveList: the object is already in a list");
    }
    hook.owner_        = &object;
    hook.prev_         = head_.prev_;
    hook.next_         = &head_;
    head_.prev_->next_ = &hook;
    head_.prev_        = &hook;
    ++size_;
  }

  /// Removes @p object, which must be in this list.
  void Erase(T& object) {
    auto& hook = object.*Hook;
    if (!hook.IsLinked()) {
      throw std::logic_error("IntrusiveList: the object is not in a list");
    }
    hook.prev_->next_ = hook.next_;
    hook.next_->prev_ = hook.prev_;
    hook.prev_ = hook.next_ = nullptr;
    hook.owner_             = nullptr;
    --size_;
  }

  /// Moves @p object, which must be in this list, to the back.
  void MoveToBack(T& object) {
    Erase(object);
    PushBack(object);
  }

  /// The first object, or nullptr when the list is empty.
  [[nodiscard]] auto Front() const -> T* { return Empty() ? nullptr : head_.next_->owner_; }

  /// Unlinks every object.
  void               Clear() {
    while (!Empty()) {
      Erase(*head_.next_->owner_);
    }
  }

  [[nodiscard]] auto begin() const -> Iterator { return Iterator(head_.next_); }
  [[nodiscard]] auto end() const -> Iterator { return Iterator(&head_); }

 private:
  IntrusiveListHook<T> head_;
  std::size_t          size_ = 0;
};

}  // namespace alcedo
