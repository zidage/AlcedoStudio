//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>

#include "ui/alcedo_main/album_backend/album_catalog.hpp"
#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/thumbnail_manager.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class FolderController;
class ProjectModule;
class SearchController;
class StatsEngine;

/// Library/catalog module: thumbnail grid model, disk cache, windowed loads,
/// and IAlbumCatalog for sibling modules.
class LibraryModule final : public QObject, public IAlbumCatalog {
  Q_OBJECT
  Q_PROPERTY(QVariantList thumbnails READ Thumbnails NOTIFY ThumbnailsChanged)
  Q_PROPERTY(QObject* thumbnailModel READ ThumbnailModel CONSTANT)
  Q_PROPERTY(int shownCount READ ShownCount NOTIFY CountsChanged)
  Q_PROPERTY(int totalCount READ TotalCount NOTIFY CountsChanged)
  Q_PROPERTY(bool hasMoreThumbnails READ HasMoreThumbnails NOTIFY CountsChanged)
  Q_PROPERTY(QString filterInfo READ FilterInfo NOTIFY CountsChanged)
  Q_PROPERTY(bool thumbnailDiskCacheEnabled READ ThumbnailDiskCacheEnabled NOTIFY
                 ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(QString thumbnailDiskCacheRoot READ ThumbnailDiskCacheRoot NOTIFY
                 ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(int thumbnailDiskCacheMaxEntries READ ThumbnailDiskCacheMaxEntries NOTIFY
                 ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(int thumbnailDiskCacheJpegQuality READ ThumbnailDiskCacheJpegQuality NOTIFY
                 ThumbnailDiskCacheStateChanged)
  Q_PROPERTY(QString thumbnailDiskCacheStats READ ThumbnailDiskCacheStats NOTIFY
                 ThumbnailDiskCacheStateChanged)

 public:
  explicit LibraryModule(ProjectModule* project, QObject* parent = nullptr);
  ~LibraryModule() override = default;

  void BindCollaborators(FolderController* folders, SearchController* search, StatsEngine* stats);
  void SetSemanticLabelProvider(std::function<QString(sl_element_id_t)> provider);

  [[nodiscard]] auto thumbs() -> ThumbnailManager& { return thumbs_; }
  [[nodiscard]] auto thumbs() const -> const ThumbnailManager& { return thumbs_; }
  [[nodiscard]] auto model() -> AlbumThumbnailModel& { return thumbnail_model_; }
  [[nodiscard]] auto model() const -> const AlbumThumbnailModel& { return thumbnail_model_; }
  [[nodiscard]] auto project() -> ProjectModule* { return project_; }
  [[nodiscard]] auto project() const -> const ProjectModule* { return project_; }

  // ── Q_PROPERTY getters ─────────────────────────────────────────────────
  QVariantList Thumbnails() const;
  QObject*     ThumbnailModel() { return &thumbnail_model_; }
  int          ShownCount() const { return thumbnail_model_.count(); }
  int          TotalCount() const;
  bool         HasMoreThumbnails() const { return thumbnail_model_.hasMore(); }
  QString      FilterInfo() const;
  bool         ThumbnailDiskCacheEnabled() const;
  QString      ThumbnailDiskCacheRoot() const;
  int          ThumbnailDiskCacheMaxEntries() const;
  int          ThumbnailDiskCacheJpegQuality() const;
  QString      ThumbnailDiskCacheStats() const;

  // ── Q_INVOKABLE ────────────────────────────────────────────────────────
  Q_INVOKABLE void SetThumbnailVisible(uint elementId, uint imageId, bool visible,
                                       uint maxEdge = 1024);
  Q_INVOKABLE void SetThumbnailCacheHint(uint visibleCells, uint maxEdge = 1024);
  Q_INVOKABLE bool LoadMoreThumbnails();
  Q_INVOKABLE bool LoadThumbnailsThroughIndex(int index);
  Q_INVOKABLE void SetThumbnailDiskCacheEnabled(bool enabled);
  Q_INVOKABLE void SetThumbnailDiskCacheRoot(const QString& rootPath);
  Q_INVOKABLE void SetThumbnailDiskCacheMaxEntries(int maxEntries);
  Q_INVOKABLE void SetThumbnailDiskCacheJpegQuality(int quality);
  Q_INVOKABLE void ClearAllThumbnailDiskCache();
  Q_INVOKABLE void ClearProjectThumbnailDiskCache();
  Q_INVOKABLE int  PromptForInt(const QString& title, const QString& label, int defaultValue,
                                int minValue, int maxValue);

  // ── IAlbumCatalog ──────────────────────────────────────────────────────
  auto FindAlbumItem(sl_element_id_t elementId) -> AlbumItem* override;
  auto FindAlbumItem(sl_element_id_t elementId) const -> const AlbumItem* override;
  void AddOrUpdateAlbumItem(sl_element_id_t elementId, image_id_t imageId, sl_element_id_t folderId,
                            const QString& scopeType, const file_name_t& fallbackName,
                            const std::filesystem::path& filePath) override;
  void SetAlbumItemHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) override;
  void PersistImageHdrFlag(sl_element_id_t elementId, image_id_t imageId, bool isHdr) override;
  auto view_state() -> AlbumViewState& override { return view_state_; }
  auto view_state() const -> const AlbumViewState& override { return view_state_; }
  void ReloadFolderTree(const std::filesystem::path& preferredFolderPath = {}) override;
  void ReloadCurrentFolder() override;
  bool LoadThumbnailWindow(const std::optional<std::wstring>& filterWhere, bool reset) override;
  auto EffectiveFilterWhere(const std::optional<std::wstring>& filterWhere) const
      -> std::optional<std::wstring> override;

  void LoadThumbnailDiskCacheSettings();
  void ApplyThumbnailDiskCacheSettingsToService();
  void NotifyThumbnailsChanged();
  void NotifyCountsChanged();
  void EmitThumbnailUpdated(uint elementId, const QString& dataUrl, bool loading,
                            bool missingSource, const QString& errorText);

 signals:
  void ThumbnailsChanged();
  void thumbnailsChanged();
  void ThumbnailUpdated(uint elementId, const QString& dataUrl, bool loading, bool missingSource,
                        const QString& errorText);
  void thumbnailUpdated(uint elementId, const QString& dataUrl, bool loading, bool missingSource,
                        const QString& errorText);
  void CountsChanged();
  void ThumbnailDiskCacheStateChanged();

 private:
  void SaveThumbnailDiskCacheSettings();

  ProjectModule*     project_ = nullptr;
  FolderController*  folders_ = nullptr;
  SearchController*  search_  = nullptr;
  StatsEngine*       stats_   = nullptr;

  ThumbnailManager    thumbs_;
  AlbumThumbnailModel thumbnail_model_{};
  AlbumViewState      view_state_{};

  bool    thumbnail_disk_cache_enabled_      = true;
  QString thumbnail_disk_cache_root_;
  int     thumbnail_disk_cache_max_entries_  = 10000;
  int     thumbnail_disk_cache_jpeg_quality_ = 85;
  std::function<QString(sl_element_id_t)> semantic_label_provider_{};
};

}  // namespace alcedo::ui
