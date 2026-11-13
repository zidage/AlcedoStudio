//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>

#pragma once

namespace alcedo {
/**
 * @brief A thread-safe blocking task queue used by the threadpool.
 */
template <typename T>
class ConcurrentBlockingQueue {
 public:
  std::uint32_t           max_size_;
  bool                    has_capacity_limit_ = true;
  std::queue<T>           queue_;
  // Mutex used for non-blocking queue
  std::mutex              mtx_;
  std::condition_variable producer_cv_;
  std::condition_variable consumer_cv_;

  explicit ConcurrentBlockingQueue() { has_capacity_limit_ = false; };

  explicit ConcurrentBlockingQueue(uint32_t max_size) : max_size_(max_size) {}

  /**
   * @brief A thread-safe wrapper for _request_queue push() method
   *
   * @param new_request the request to enqueue
   */
  void push(T new_request) {
    {
      std::unique_lock<std::mutex> lock(mtx_);
      queue_.push(std::move(new_request));
    }
    consumer_cv_.notify_all();
  }

  /**
   * @brief A thread-safe wrapper for _request_queue push() method
   *
   * @param new_request the request to enqueue
   */
  void push_r(T&& new_request) {
    {
      std::unique_lock<std::mutex> lock(mtx_);
      queue_.push(std::move(new_request));
    }
    consumer_cv_.notify_all();
  }

  /**
   * @brief A thread-safe wrapper for pop() method
   *
   * @return the front-most element of the queue
   */
  T pop() {
    std::unique_lock<std::mutex> lock(mtx_);
    // Wait for the queue to be fill with at least one value
    consumer_cv_.wait(lock, [this] { return !queue_.empty(); });

    auto handled_request = queue_.front();
    queue_.pop();

    return handled_request;
  }

  /**
   * @brief A thread-safe wrapper for pop() method
   *
   * @return the front-most element of the queue
   */
  T pop_r() {
    std::unique_lock<std::mutex> lock(mtx_);
    // Wait for the queue to be fill with at least one value
    consumer_cv_.wait(lock, [this] { return !queue_.empty(); });

    auto handled_request = std::move(queue_.front());
    queue_.pop();

    return handled_request;
  }
};

/**
 * @brief A thread-safe non-blocking ring buffer.
 */
template <typename T>
class LockFreeMPMCQueue {
 public:
  explicit LockFreeMPMCQueue(size_t capacity) : capacity_(capacity), buffer_(capacity) {
    for (size_t i = 0; i < capacity; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  bool push(const T& item) {
    size_t pos = tail_.load(std::memory_order_relaxed);
    while (true) {
      Slot&    slot = buffer_[pos % capacity_];
      size_t   seq  = slot.sequence_.load(std::memory_order_acquire);
      intptr_t diff = (intptr_t)seq - (intptr_t)pos;
      if (diff == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          slot.data_ = item;
          slot.sequence_.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // queue is full
      } else {
        pos = tail_.load(std::memory_order_relaxed);  // retry
      }
    }
  }

  std::optional<T> pop() {
    size_t pos = head_.load(std::memory_order_relaxed);
    while (true) {
      Slot&    slot = buffer_[pos % capacity_];
      size_t   seq  = slot.sequence_.load(std::memory_order_acquire);
      intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
      if (diff == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          T result = slot.data_;
          slot.sequence_.store(pos + capacity_, std::memory_order_release);
          return result;
        }
      } else if (diff < 0) {
        return std::nullopt;  // queue is empty
      } else {
        pos = head_.load(std::memory_order_relaxed);  // retry
      }
    }
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

 private:
  struct Slot {
    std::atomic<size_t> sequence_;
    T                   data_;
  };

  size_t              capacity_;
  std::vector<Slot>   buffer_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

template <typename T>
class BlockingMPMCQueue {
 public:
  explicit BlockingMPMCQueue(size_t capacity) : queue_(capacity) {}

  void push(const T& item) {
    while (true) {
      if (queue_.push(item)) {
        {
          std::lock_guard<std::mutex> lock(cv_mutex_);
          not_empty_.notify_one();
        }
        return;
      }

      std::unique_lock<std::mutex> lock(cv_mutex_);
      not_full_.wait(lock);  // wait until space available
    }
  }

  T pop() {
    while (true) {
      auto item = queue_.pop();
      if (item.has_value()) {
        {
          std::lock_guard<std::mutex> lock(cv_mutex_);
          not_full_.notify_one();
        }
        return item.value();
      }

      std::unique_lock<std::mutex> lock(cv_mutex_);
      not_empty_.wait(lock);  // wait until item available
    }
  }

  bool empty() const { return queue_.empty(); }

 private:
  LockFreeMPMCQueue<T>    queue_;

  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::mutex              cv_mutex_;
};
};  // namespace alcedo
