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


namespace alcedo::ui {

class AlbumBackend;

/// Manages thumbnail pin reference counts and async data-URL generation.
class ThumbnailManager {
 public:
  explicit ThumbnailManager(AlbumBackend& backend);

  void SetThumbnailVisible(sl_element_id_t elementId, image_id_t imageId, bool visible,
                           uint32_t maxEdge = 1024);
  void RequestThumbnail(sl_element_id_t elementId, image_id_t imageId,
                         uint32_t maxEdge = 1024);
  void UpdateThumbnailState(sl_element_id_t elementId, const QString& dataUrl, bool loading,
                            bool missingSource);
  [[nodiscard]] bool IsThumbnailPinned(sl_element_id_t elementId) const;
  [[nodiscard]] uint32_t GetPinnedMaxEdge(sl_element_id_t elementId) const;
  void               RemoveThumbnailState(sl_element_id_t elementId, image_id_t imageId);
  void               ReleaseVisibleThumbnailPins();

 private:
  struct PinnedThumbnailState {
    uint32_t            ref_count_  = 0;
    image_id_t          image_id_    = 0;
    ThumbnailResolution resolution_  = ThumbnailResolution::k1024;
  };

  [[nodiscard]] auto ResolveThumbnailSourcePath(sl_element_id_t elementId,
                                                image_id_t imageId) const
      -> std::filesystem::path;
  [[nodiscard]] static auto PathExists(const std::filesystem::path& path) -> bool;

  AlbumBackend&                                 backend_;
  // TODO: Move pin ref-count tracking into ThumbnailService.
  std::unordered_map<sl_element_id_t, PinnedThumbnailState> thumbnail_pins_{};
  // Strategy B: active flags for in-flight thumbnail requests.
  std::unordered_map<sl_element_id_t, std::shared_ptr<std::atomic<bool>>> thumbnail_active_flags_{};
};

}  // namespace alcedo::ui
