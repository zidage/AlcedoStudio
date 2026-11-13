//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "app/mask_thumbnail_spec.hpp"

namespace alcedo {

/**
 * @brief Project-level best-effort LRU of Mask thumbnails.
 *
 * The service maps a pixel @ref MaskThumbnailSpec to a QImage. It does not know
 * commit, undo, delete, sessions, or QML rows. Old jobs may finish and fill the
 * cache. Whether those pixels are shown is the GUI receiver's decision.
 *
 * Thread: @ref Request is safe from the GUI thread. Pixel work runs on one
 * worker unless a test replaces the job runner. Results are posted through the
 * result dispatcher (queued GUI invoke by default).
 */
class MaskThumbnailService {
 public:
  using Callback = std::function<void(std::uint64_t request_id, MaskThumbnailSpec spec, QImage image,
                                      QString error)>;
  using JobRunner =
      std::function<void(std::function<void()>)>;
  using ResultDispatcher = std::function<void(QObject* receiver, std::function<void()>)>;

  explicit MaskThumbnailService(std::size_t capacity = kMaskThumbnailCacheCapacity);
  ~MaskThumbnailService();

  MaskThumbnailService(const MaskThumbnailService&)            = delete;
  auto operator=(const MaskThumbnailService&) -> MaskThumbnailService& = delete;

  /**
   * @brief Look up or render @p spec and post @p callback to the GUI thread.
   *
   * A cache hit posts @p callback without enqueueing a worker job. Duplicate
   * misses may render more than once. A destroyed @p receiver drops the queued
   * callback; the image may still enter the LRU. Does not cancel in-flight work.
   */
  void Request(MaskThumbnailSpec spec, QObject* receiver, std::uint64_t request_id, Callback callback);

  /// Replace the worker enqueue function. Tests use this to run jobs inline or hold a latch.
  void SetJobRunner(JobRunner runner);
  /// Replace GUI posting. Tests use this to drain callbacks without a Qt event loop.
  void SetResultDispatcher(ResultDispatcher dispatcher);

  [[nodiscard]] auto size() const -> std::size_t;
  [[nodiscard]] auto capacity() const -> std::size_t;
  [[nodiscard]] auto generate_count() const -> std::uint64_t;
  [[nodiscard]] auto Cached(const MaskThumbnailSpec& spec) const -> std::optional<QImage>;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace alcedo
