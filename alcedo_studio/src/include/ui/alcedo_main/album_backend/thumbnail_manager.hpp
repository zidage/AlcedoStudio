//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "app/thumbnail_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/thumbnail_image_provider.hpp"

namespace alcedo::ui {

class LibraryModule;

/// Manages thumbnail pin reference counts and async image:// provider delivery.
class ThumbnailManager {
 public:
  explicit ThumbnailManager(LibraryModule& library);

  void SetThumbnailVisible(sl_element_id_t elementId, image_id_t imageId, bool visible,
                           uint32_t maxEdge = 1024);
  /// Visibility of one occurrence of a photo in a grouped view, identified by
  /// `(group_key, elementId, tier)`. Repeated notifications for the same occurrence change
  /// nothing; the photo stays pinned while at least one of its occurrences is visible.
  void SetOccurrenceThumbnailVisible(const std::string& group_key, sl_element_id_t elementId,
                                     image_id_t imageId, bool visible, uint32_t maxEdge = 1024);
  /// Number of visible occurrences registered for @p elementId (all tiers).
  [[nodiscard]] auto VisibleOccurrenceCount(sl_element_id_t elementId) const -> size_t;
  void RequestThumbnail(sl_element_id_t elementId, image_id_t imageId, uint32_t maxEdge = 1024,
                        int retryAttempt = 0);
  /// Request every pinned tier of @p elementId again. Returns false when no tier is pinned.
  [[nodiscard]] bool RefreshCurrentThumbnail(sl_element_id_t elementId, image_id_t imageId);
  void               UpdateThumbnailState(sl_element_id_t elementId, const QString& dataUrl,
                                          bool loading, bool missingSource,
                                          const QString& errorText = {});
  [[nodiscard]] bool IsThumbnailPinned(sl_element_id_t elementId) const;
  void               RemoveThumbnailState(sl_element_id_t elementId, image_id_t imageId);
  void               ReleaseVisibleThumbnailPins();

  /// Drop a store entry only when the library grid/filmstrip is not pinning that key.
  void ReleaseStoreImageIfUnpinned(const ThumbnailCacheKey& key);

  [[nodiscard]] auto image_store() -> std::shared_ptr<ThumbnailImageStore> { return image_store_; }
  [[nodiscard]] auto image_store() const -> std::shared_ptr<ThumbnailImageStore> {
    return image_store_;
  }

 private:
  struct PinnedThumbnailState {
    uint32_t            ref_count_  = 0;
    image_id_t          image_id_   = 0;
    ThumbnailResolution resolution_ = ThumbnailResolution::k1024;
  };

  [[nodiscard]] auto ResolveThumbnailSourcePath(sl_element_id_t elementId,
                                                image_id_t      imageId) const
      -> std::filesystem::path;
  [[nodiscard]] static auto PathExists(const std::filesystem::path& path) -> bool;
  [[nodiscard]] bool        IsThumbnailPinned(const ThumbnailCacheKey& key) const;
  void                      DeactivateThumbnailRequest(const ThumbnailCacheKey& key);
  void ReleasePinnedThumbnailRequest(const ThumbnailCacheKey& key);
  /// After one tier of @p elementId was released while another stays pinned, show the remaining
  /// tier (the largest one): its stored image, or a new request when none is in flight.
  void ShowRemainingPinnedTier(sl_element_id_t elementId);
  /// A request for @p key answered. Forget its active flag unless a newer request replaced it.
  void FinishThumbnailRequest(const ThumbnailCacheKey&                  key,
                              const std::shared_ptr<std::atomic<bool>>& is_active);

  LibraryModule& library_;
  std::shared_ptr<ThumbnailImageStore> image_store_{std::make_shared<ThumbnailImageStore>()};
  // TODO: Move pin ref-count tracking into ThumbnailService.
  std::unordered_map<ThumbnailCacheKey, PinnedThumbnailState> thumbnail_pins_{};
  /// Visible grouped-view occurrences: (group key, element id, tier edge).
  std::set<std::tuple<std::string, sl_element_id_t, uint32_t>> visible_occurrences_{};
  // Strategy B: active flags for in-flight thumbnail requests. A key leaves the map when its
  // request is released or answers.
  std::unordered_map<ThumbnailCacheKey, std::shared_ptr<std::atomic<bool>>>
      thumbnail_active_flags_{};
};

}  // namespace alcedo::ui
