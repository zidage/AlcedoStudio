//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>

#include "image/image.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {

class ImagePoolManager {
 public:
  static constexpr uint32_t kDefaultPoolCapacity = 1024;

  class PinnedImageHandle {
   public:
    PinnedImageHandle() = default;
    PinnedImageHandle(ImagePoolManager* manager, image_id_t image_id,
                      std::shared_ptr<Image> image)
        : manager_(manager), image_id_(image_id), image_(std::move(image)) {}

    PinnedImageHandle(const PinnedImageHandle&)            = delete;
    PinnedImageHandle& operator=(const PinnedImageHandle&) = delete;

    PinnedImageHandle(PinnedImageHandle&& other) noexcept
        : manager_(other.manager_), image_id_(other.image_id_), image_(std::move(other.image_)) {
      other.manager_ = nullptr;
      other.image_id_ = 0;
    }

    PinnedImageHandle& operator=(PinnedImageHandle&& other) noexcept {
      if (this != &other) {
        Release();
        manager_  = other.manager_;
        image_id_ = other.image_id_;
        image_    = std::move(other.image_);
        other.manager_ = nullptr;
        other.image_id_ = 0;
      }
      return *this;
    }

    ~PinnedImageHandle() { Release(); }

    auto Get() const -> const std::shared_ptr<Image>& { return image_; }
    auto operator->() const -> Image* { return image_.get(); }
    auto operator*() const -> Image& { return *image_; }
    explicit operator bool() const { return image_ != nullptr; }

   private:
    void Release();

    ImagePoolManager*     manager_  = nullptr;
    image_id_t            image_id_ = 0;
    std::shared_ptr<Image> image_{};
  };

 private:
  IncrID::IDGenerator<image_id_t>                        id_generator_{0};
  std::unordered_map<image_id_t, std::shared_ptr<Image>> image_pool_;
  std::unordered_map<image_id_t, uint32_t>               pin_counts_;
  LRUCache<image_id_t, image_id_t>                        lru_pool_{
      std::numeric_limits<size_t>::max()};
  uint32_t                                                capacity_ = kDefaultPoolCapacity;

  /// Make room for one insert by evicting least recently used entries that IsEvictable
  /// allows. The pool grows past its capacity while no entry is evictable.
  void EnsureCapacityForInsert();
  /// True when @p id is unpinned and its Image has no pending write (sync state SYNCED).
  auto IsEvictable(image_id_t id) -> bool;
  void EvictByKey(image_id_t id);
  void Pin(image_id_t id);
  void Unpin(image_id_t id);

 public:

  explicit ImagePoolManager();
  explicit ImagePoolManager(uint32_t start_id);
  explicit ImagePoolManager(uint32_t capacity_thumb, uint32_t start_id);

  auto GetPool() -> std::unordered_map<image_id_t, std::shared_ptr<Image>>&;
  void Insert(const std::shared_ptr<Image> img);
  auto CreateAndReturnPinnedEmpty() -> PinnedImageHandle;
  auto PoolContains(const image_id_t& id) -> bool;

  auto GetImage(const image_id_t& id) -> std::shared_ptr<Image>;
  auto GetImagePinned(const image_id_t& id) -> std::optional<PinnedImageHandle>;

  auto Capacity() -> uint32_t;

  void ResizeCache(const uint32_t new_capacity);
  void ResizePool(const uint32_t new_capacity);

  auto GetCurrentID() -> image_id_t { return id_generator_.GetCurrentID(); }

  void Flush();
  void Clear();
};
};  // namespace alcedo