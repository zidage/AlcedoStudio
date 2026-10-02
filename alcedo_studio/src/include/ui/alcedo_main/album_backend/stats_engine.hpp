//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <optional>
#include <string>

#include "app/sleeve_filter_service.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"

namespace alcedo::ui {

class FolderController;
class LibraryModule;
class ProjectModule;
class SearchController;
class SemanticGenerationController;

/// Runs SQL aggregate queries against DuckDB after import / folder change,
/// and manages the thumbnail view rebuild.
class StatsEngine final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList dateStats READ DateStats NOTIFY StatsChanged)
  Q_PROPERTY(QVariantList cameraStats READ CameraStats NOTIFY StatsChanged)
  Q_PROPERTY(QVariantList lensStats READ LensStats NOTIFY StatsChanged)
  Q_PROPERTY(QVariantList labelStats READ LabelStats NOTIFY StatsChanged)
  Q_PROPERTY(int totalPhotoCount READ TotalPhotoCount NOTIFY StatsChanged)
  Q_PROPERTY(QString statsFilterDate READ StatsFilterDate NOTIFY StatsFilterChanged)
  Q_PROPERTY(QString statsFilterCamera READ StatsFilterCamera NOTIFY StatsFilterChanged)
  Q_PROPERTY(QString statsFilterLens READ StatsFilterLens NOTIFY StatsFilterChanged)
  Q_PROPERTY(QString statsFilterLabel READ StatsFilterLabel NOTIFY StatsFilterChanged)
  Q_PROPERTY(QVariantList ratingStats READ RatingStats NOTIFY StatsChanged)
  Q_PROPERTY(QString statsFilterRating READ StatsFilterRating NOTIFY StatsFilterChanged)
  Q_PROPERTY(QVariantList importDateStats READ ImportDateStats NOTIFY StatsChanged)
  Q_PROPERTY(QString statsFilterImportDate READ StatsFilterImportDate NOTIFY StatsFilterChanged)

 public:
  StatsEngine(ProjectModule* project, LibraryModule* library, FolderController* folders,
              QObject* parent = nullptr);

  void BindCollaborators(SearchController* search, SemanticGenerationController* semantic);

  /// Request one library refresh: the grid, the groups, and these statistics are replaced
  /// together by the accepted result of LibraryModule's query worker.
  void RebuildThumbnailView();
  bool LoadMoreThumbnailView();
  /// Same request as RebuildThumbnailView: statistics come from the one library query.
  void RefreshStats();
  /// Replace the stats properties with @p stats (queried on the library worker). Emits
  /// StatsChanged only when @p emit_changed; LibraryModule emits after it installed every
  /// owner of the same result. Runs no SQL. UI thread only.
  void ApplyFolderStats(const AlbumStatsView& stats, bool emit_changed = true);
  /// Remove every statistic (no project or no folder) and emit StatsChanged.
  void ClearStats();
  /// Active semantic model key read once for one library request.
  [[nodiscard]] auto ActiveSemanticModelKey() const -> std::string;

  [[nodiscard]] auto FormatPhotoInfo(int shown, int total) const -> QString;
  [[nodiscard]] auto MakeThumbMap(const AlbumItem& image, int index) const -> QVariantMap;
  [[nodiscard]] auto BuildSearchRecommendations(int limit) const -> QVariantList;

  [[nodiscard]] auto DateStats() const -> QVariantList { return date_stats_; }
  [[nodiscard]] auto CameraStats() const -> QVariantList { return camera_stats_; }
  [[nodiscard]] auto LensStats() const -> QVariantList { return lens_stats_; }
  [[nodiscard]] auto LabelStats() const -> QVariantList { return label_stats_; }
  [[nodiscard]] auto RatingStats() const -> QVariantList { return rating_stats_; }
  [[nodiscard]] auto ImportDateStats() const -> QVariantList { return import_date_stats_; }
  [[nodiscard]] auto StatsFilterImportDate() const -> QString { return filter_import_date_; }
  [[nodiscard]] int  TotalPhotoCount() const { return total_photo_count_; }
  [[nodiscard]] auto StatsFilterDate() const -> QString { return filter_date_; }
  [[nodiscard]] auto StatsFilterCamera() const -> QString { return filter_camera_; }
  [[nodiscard]] auto StatsFilterLens() const -> QString { return filter_lens_; }
  [[nodiscard]] auto StatsFilterLabel() const -> QString { return filter_label_; }
  [[nodiscard]] auto StatsFilterRating() const -> QString { return filter_rating_; }

  [[nodiscard]] auto date_stats() const -> const QVariantList& { return date_stats_; }
  [[nodiscard]] auto camera_stats() const -> const QVariantList& { return camera_stats_; }
  [[nodiscard]] auto lens_stats() const -> const QVariantList& { return lens_stats_; }
  [[nodiscard]] auto label_stats() const -> const QVariantList& { return label_stats_; }
  [[nodiscard]] auto rating_stats() const -> const QVariantList& { return rating_stats_; }
  [[nodiscard]] int  total_photo_count() const { return total_photo_count_; }
  [[nodiscard]] const QString& filter_date() const { return filter_date_; }
  [[nodiscard]] const QString& filter_camera() const { return filter_camera_; }
  [[nodiscard]] const QString& filter_lens() const { return filter_lens_; }
  [[nodiscard]] const QString& filter_label() const { return filter_label_; }
  [[nodiscard]] const QString& filter_rating() const { return filter_rating_; }

  Q_INVOKABLE void ToggleStatsFilter(const QString& category, const QString& label);
  Q_INVOKABLE void ClearStatsFilter();

  void ToggleFilter(const QString& category, const QString& label);
  void ClearFilters();

  [[nodiscard]] bool HasActiveFilter() const;

  /// Build a FilterNode tree for the active stats-bar filters. Returns
  /// std::nullopt when no stats filter is active. Never contains SQL text.
  [[nodiscard]] auto BuildStatsFilterNode() const -> std::optional<FilterNode>;

 signals:
  void StatsChanged();
  void StatsFilterChanged();

 private:
  [[nodiscard]] bool MatchesActiveFilters(const AlbumItem& image) const;

  ProjectModule*                project_  = nullptr;
  LibraryModule*                library_  = nullptr;
  FolderController*             folders_  = nullptr;
  SearchController*             search_   = nullptr;
  SemanticGenerationController* semantic_ = nullptr;

  QVariantList date_stats_{};
  QVariantList camera_stats_{};
  QVariantList lens_stats_{};
  QVariantList label_stats_{};
  QVariantList rating_stats_{};
  QVariantList                  import_date_stats_{};
  int          total_photo_count_ = 0;

  QString filter_date_{};
  QString filter_camera_{};
  QString filter_lens_{};
  QString filter_label_{};
  QString filter_rating_{};
  QString                       filter_import_date_{};
};

}  // namespace alcedo::ui
