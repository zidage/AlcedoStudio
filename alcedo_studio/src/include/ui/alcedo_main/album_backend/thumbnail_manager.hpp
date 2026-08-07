//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
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
  void RequestThumbnail(sl_element_id_t elementId, image_id_t imageId, uint32_t maxEdge = 1024,
                        int retryAttempt = 0);
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
  void ReleasePinnedThumbnailRequest(const ThumbnailCacheKey& key, bool update_state_if_unpinned);
  void ReleaseOtherPinnedRequestsForElement(sl_element_id_t         elementId,
                                            const ThumbnailCacheKey& keep_key);

  LibraryModule& library_;
  std::shared_ptr<ThumbnailImageStore> image_store_{std::make_shared<ThumbnailImageStore>()};
  // TODO: Move pin ref-count tracking into ThumbnailService.
  std::unordered_map<ThumbnailCacheKey, PinnedThumbnailState> thumbnail_pins_{};
  std::unordered_map<sl_element_id_t, ThumbnailCacheKey>      current_visible_thumbnail_keys_{};
  // Strategy B: active flags for in-flight thumbnail requests.
  std::unordered_map<ThumbnailCacheKey, std::shared_ptr<std::atomic<bool>>>
      thumbnail_active_flags_{};
};

}  // namespace alcedo::ui
