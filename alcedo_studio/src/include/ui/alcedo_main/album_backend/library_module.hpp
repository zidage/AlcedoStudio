//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "sleeve/album_query.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/album_catalog.hpp"
#include "ui/alcedo_main/album_backend/album_section_model.hpp"
#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/search_request_worker.hpp"
#include "ui/alcedo_main/album_backend/thumbnail_manager.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class FolderController;
class ProjectModule;
class StatsEngine;

/**
 * @brief Immutable input of one library read on the query worker.
 *
 * @details Captured on the UI thread when the read is submitted: the scope, the Inspector
 * filter tree, the search part, the presentation options, the active model key, the bounds,
 * and the services that keep the project alive. It is the message that crosses the thread
 * boundary, not a copy of the library state: it holds no photo, no QObject, and no reference
 * into a UI owner. LibraryModule keeps the accepted refresh input without its services, so
 * later pages, positions, and ids read the same query interpretation without keeping a closed
 * project open.
 */
struct LibraryQueryInput {
  sl_element_id_t                      folder_id_ = 0;
  std::optional<FilterNode>            stats_filter_{};
  LibrarySearchInput                   search_{};
  AlbumQueryOptions                    options_{};
  std::string                          active_model_key_{};
  AlbumQueryRead                       read_{};
  int64_t                              page_size_ = 0;
  std::shared_ptr<AlbumBrowseService>  browse_{};
  std::shared_ptr<SleeveFilterService> filter_service_{};
};

/// Result of one library read, moved once into the queued UI completion.
struct LibraryQueryOutput {
  AlbumQueryResult          result_{};
  /// Search filter the read used (built on the worker for a pending query text).
  std::optional<FilterNode> search_filter_{};
  QString                   error_{};
};

/// IANA id of the system time zone. Import-day filters, groups, and statistics use it.
[[nodiscard]] auto  CurrentImportDayTimeZone() -> std::string;

/// Inspector field name ("date", "import", "camera", "lens", "rating", "label") of a sort or
/// group field; empty for kNone.
[[nodiscard]] auto  InspectorFieldName(AlbumSortField field) -> QString;
[[nodiscard]] auto  InspectorFieldName(AlbumGroupField field) -> QString;

/// Library/catalog module: thumbnail grid model, disk cache, windowed loads,
/// and IAlbumCatalog for sibling modules.
///
/// It owns the accepted library query: the presentation options (one optional group field and
/// one optional photo sort field), the accepted result's photos and sections, and the query
/// worker that runs every library read (refresh, page, position, and id reads) and the search
/// dialog previews. StatsEngine owns the Inspector filter values and SearchController owns the
/// search term; both request a refresh here instead of reading pages themselves.
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
  Q_PROPERTY(QObject* sectionModel READ SectionModel CONSTANT)
  Q_PROPERTY(QString sortField READ SortFieldName NOTIFY PresentationChanged)
  Q_PROPERTY(bool sortDescending READ SortDescending NOTIFY PresentationChanged)
  Q_PROPERTY(QString groupField READ GroupFieldName NOTIFY PresentationChanged)
  Q_PROPERTY(bool grouped READ Grouped NOTIFY PresentationChanged)
  Q_PROPERTY(bool queryUpdating READ QueryUpdating NOTIFY QueryStateChanged)
  Q_PROPERTY(QString queryError READ QueryError NOTIFY QueryStateChanged)

 public:
  explicit LibraryModule(ProjectModule* project, QObject* parent = nullptr);
  /// Joins the query worker first: a running job posts its result to this object.
  ~LibraryModule() override;

  void BindCollaborators(FolderController* folders, SearchController* search, StatsEngine* stats);
  void SetSemanticLabelProvider(std::function<QString(sl_element_id_t)> provider);

  [[nodiscard]] auto thumbs() -> ThumbnailManager& { return thumbs_; }
  [[nodiscard]] auto thumbs() const -> const ThumbnailManager& { return thumbs_; }
  [[nodiscard]] auto model() -> AlbumThumbnailModel& { return thumbnail_model_; }
  [[nodiscard]] auto model() const -> const AlbumThumbnailModel& { return thumbnail_model_; }
  [[nodiscard]] auto project() -> ProjectModule* { return project_; }
  [[nodiscard]] auto project() const -> const ProjectModule* { return project_; }
  /// The one worker of library reads and search previews.
  [[nodiscard]] auto query_worker() -> SearchRequestWorker& { return *query_worker_; }
  [[nodiscard]] auto section_model() -> AlbumSectionModel& { return section_model_; }
  [[nodiscard]] auto section_model() const -> const AlbumSectionModel& { return section_model_; }
  [[nodiscard]] auto accepted_options() const -> const AlbumQueryOptions& {
    return accepted_options_;
  }

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
  QObject*     SectionModel() { return &section_model_; }
  QString      SortFieldName() const { return InspectorFieldName(accepted_options_.sort_field_); }
  bool         SortDescending() const {
    return accepted_options_.sort_field_ != AlbumSortField::kNone &&
           accepted_options_.sort_direction_ == SortDirection::kDescending;
  }
  QString GroupFieldName() const { return InspectorFieldName(accepted_options_.group_field_); }
  bool    Grouped() const { return accepted_options_.group_field_ != AlbumGroupField::kNone; }
  bool    QueryUpdating() const { return refresh_in_flight_; }
  QString QueryError() const { return query_error_; }

  // ── Q_INVOKABLE ────────────────────────────────────────────────────────
  Q_INVOKABLE void SetThumbnailVisible(uint elementId, uint imageId, bool visible,
                                       uint maxEdge = 1024);
  Q_INVOKABLE void SetThumbnailCacheHint(uint visibleCells, uint maxEdge = 1024);
  Q_INVOKABLE bool LoadMoreThumbnails();
  Q_INVOKABLE bool LoadThumbnailsThroughIndex(int index);
  /// Row in the current collection + filter listing, not just the loaded page.
  /// Returns -1 when the element is outside that listing.
  Q_INVOKABLE int IndexOfElementInCurrentView(uint elementId);
  Q_INVOKABLE void SetThumbnailDiskCacheEnabled(bool enabled);
  Q_INVOKABLE void SetThumbnailDiskCacheRoot(const QString& rootPath);
  Q_INVOKABLE void SetThumbnailDiskCacheMaxEntries(int maxEntries);
  Q_INVOKABLE void SetThumbnailDiskCacheJpegQuality(int quality);
  Q_INVOKABLE void ClearAllThumbnailDiskCache();
  Q_INVOKABLE void ClearProjectThumbnailDiskCache();
  Q_INVOKABLE int  PromptForInt(const QString& title, const QString& label, int defaultValue,
                                int minValue, int maxValue);

  // ── Library query: presentation options, sections, ordered reads ───────
  /**
   * @brief Inspector sort action of @p field. Selects that field and direction as the one
   *        photo sort, or clears the explicit sort when that exact action is already selected.
   *        The group field and every filter stay. Unknown field names report a query error.
   */
  Q_INVOKABLE void       ToggleInspectorSort(const QString& field, bool descending);
  /**
   * @brief Inspector Group checkbox of @p field. Checking selects @p field as the one group
   *        field (the previous one is unchecked); unchecking the current field returns to the
   *        flat mode. The photo sort and every filter stay.
   */
  Q_INVOKABLE void       SetInspectorGrouping(const QString& field, bool enabled);
  /// Submit the failed refresh again with the same pending changes.
  Q_INVOKABLE void       RetryLibraryQuery();
  /// Load the occurrence pages that the section rows [@p firstRow, @p lastRow] show, with a
  /// margin; pages far from them are released (bounded retention).
  Q_INVOKABLE void       RequestSectionRows(int firstRow, int lastRow);
  /// Read the ordered unique files of the occurrence ranges (`[{begin, end}]`) on the worker.
  /// They arrive with OrderedFileIdsReady as selection items `{elementId, fileId, imageId,
  /// fileName}`. Reads ids and typed columns only, no thumbnail metadata.
  Q_INVOKABLE qulonglong RequestOrderedFileIds(const QVariantList& ranges);
  /// Same as RequestOrderedFileIds for every occurrence of the accepted result (Ctrl+A: all
  /// unique files, also those of collapsed groups).
  Q_INVOKABLE qulonglong RequestAllFileIds();
  /// Find @p fileId in the accepted result with SQL ranking on the worker and load its page.
  /// The occurrence arrives with FocusPositionReady (-1 when the file no longer matches).
  Q_INVOKABLE void       RequestFocusPosition(uint fileId, const QString& preferredGroupTitle = {},
                                              bool preferredGroupUnknown = false);
  /// Grouped-view visibility of one occurrence (see ThumbnailManager).
  Q_INVOKABLE void       SetOccurrenceThumbnailVisible(const QString& groupTitle, bool groupUnknown,
                                                       uint elementId, uint imageId, bool visible,
                                                       uint maxEdge = 1024);

  /// Request one refresh of the accepted library query. Requests of one event loop turn are
  /// combined: the refresh reads the filters, the search, and the presentation options when it
  /// is submitted to the worker. UI thread only.
  void                   RequestLibraryRefresh();

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
  bool LoadThumbnailWindow(const std::optional<FilterNode>& statsFilter, bool reset) override;

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
  void PresentationChanged();
  void QueryStateChanged();
  void OrderedFileIdsReady(qulonglong requestId, const QVariantList& fileIds);
  void orderedFileIdsReady(qulonglong requestId, const QVariantList& fileIds);
  void FocusPositionReady(uint fileId, qint64 occurrence, int sectionRow);
  void focusPositionReady(uint fileId, qint64 occurrence, int sectionRow);

 private:
  void SaveThumbnailDiskCacheSettings();
  /// Release the pins and empty the grid model, the sections, and the counts.
  void               ResetThumbnailWindow();
  /// Options of the next request: the pending change, else the accepted options.
  [[nodiscard]] auto RequestedOptions() const -> AlbumQueryOptions;
  void               SubmitPendingRefresh();
  /// Inputs of a read of the accepted result (page, position, ids), or empty without a result.
  [[nodiscard]] auto AcceptedReadInput(AlbumQueryRead read) const
      -> std::shared_ptr<const LibraryQueryInput>;
  void               PublishRefresh(const LibraryQueryInput& input, LibraryQueryOutput output);
  void               PublishPage(const LibraryQueryInput& input, LibraryQueryOutput output);
  void               SubmitPage(int64_t offset);
  /// Load the next missing page of the requested section rows or flat range, if any.
  void               ContinuePaging();
  /// Album item of one query row: SQL display values, plus the image pool fields SQL lacks.
  auto               ItemFromRow(const SearchResultRow& row, sl_element_id_t folderId) -> AlbumItem;
  void               SetQueryError(const QString& message);
  void               RunAfterRefresh(std::function<void()> action);

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

  // Accepted library query. The pending options are the change description only.
  AlbumSectionModel                        section_model_{};
  AlbumQueryOptions                        accepted_options_{};
  std::optional<AlbumQueryOptions>         pending_options_{};
  std::shared_ptr<const LibraryQueryInput> accepted_input_{};
  int64_t                                  occurrence_count_  = 0;
  int64_t                              loaded_occurrence_end_ = 0;  ///< Flat mode: loaded prefix.
  int64_t                              load_through_          = -1;
  int64_t                              requested_begin_       = 0;  ///< Grouped viewport range.
  int64_t                              requested_end_         = 0;
  bool                                 refresh_scheduled_     = false;
  bool                                 refresh_in_flight_     = false;
  bool                                 page_in_flight_        = false;
  QString                              query_error_{};
  std::vector<std::function<void()>>   after_refresh_{};
  /// Last member: destroyed first, so no job outlives the state it posts to.
  std::unique_ptr<SearchRequestWorker> query_worker_;
};

}  // namespace alcedo::ui
