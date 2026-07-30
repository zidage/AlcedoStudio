//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "ui/alcedo_main/album_backend/album_types.hpp"

namespace alcedo::ui {

/// Album grid catalog: item lookup, windowed loads, and HDR flags.
/// Owned by LibraryModule; injected into modules that need synchronous catalog access.
class IAlbumCatalog {
 public:
  virtual ~IAlbumCatalog() = default;

  virtual auto FindAlbumItem(sl_element_id_t elementId) -> AlbumItem*             = 0;
  virtual auto FindAlbumItem(sl_element_id_t elementId) const -> const AlbumItem* = 0;
  virtual void AddOrUpdateAlbumItem(sl_element_id_t elementId, image_id_t imageId,
                                    sl_element_id_t folderId, const QString& scopeType,
                                    const file_name_t&           fallbackName,
                                    const std::filesystem::path& filePath)        = 0;
  virtual void SetAlbumItemHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) = 0;
  virtual void PersistImageHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) = 0;

  virtual auto view_state() -> AlbumViewState&             = 0;
  virtual auto view_state() const -> const AlbumViewState& = 0;

  virtual void ReloadFolderTree(const std::filesystem::path& preferredFolderPath = {}) = 0;
  virtual void ReloadCurrentFolder()                                                   = 0;
  virtual bool LoadThumbnailWindow(const std::optional<std::wstring>& filterWhere,
                                   bool                               reset)           = 0;
  virtual auto EffectiveFilterWhere(const std::optional<std::wstring>& filterWhere) const
      -> std::optional<std::wstring> = 0;
};

}  // namespace alcedo::ui
