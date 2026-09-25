//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_types.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/search_request_worker.hpp"

namespace alcedo::ui {

class FolderController;
class LibraryModule;
class ProjectModule;
class StatsEngine;

class SearchController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString activeSearchQuery READ active_search_query NOTIFY SearchStateChanged)
  Q_PROPERTY(bool naturalLanguageSearchEnabled READ natural_language_search_enabled
                 NOTIFY SearchStateChanged)
  Q_PROPERTY(bool searchFieldFilenameEnabled READ SearchFieldFilenameEnabled WRITE
                 SetSearchFieldFilenameEnabled NOTIFY SearchStateChanged)
  Q_PROPERTY(bool searchFieldExifEnabled READ SearchFieldExifEnabled WRITE SetSearchFieldExifEnabled
                 NOTIFY SearchStateChanged)
  Q_PROPERTY(bool searchFieldAiDescriptionEnabled READ SearchFieldAiDescriptionEnabled WRITE
                 SetSearchFieldAiDescriptionEnabled NOTIFY SearchStateChanged)
  Q_PROPERTY(bool searchFieldAiTagsEnabled READ SearchFieldAiTagsEnabled WRITE
                 SetSearchFieldAiTagsEnabled NOTIFY SearchStateChanged)

 public:
  SearchController(ProjectModule* project, LibraryModule* library, FolderController* folders,
                   StatsEngine* stats, QObject* parent = nullptr);
  ~SearchController() override;

  [[nodiscard]] const QString& active_search_query() const { return active_search_query_; }
  [[nodiscard]] bool           natural_language_search_enabled() const {
    return natural_language_search_enabled_;
  }
  [[nodiscard]] bool HasActiveSearchFilter() const;
  /// Active search filter tree. The tree owns compiler output only; consumers
  /// merge it with other filters and compile once before a scoped query.
  [[nodiscard]] auto ActiveSearchFilterNode() const -> const std::optional<FilterNode>&;

  [[nodiscard]] bool SearchFieldFilenameEnabled() const;
  [[nodiscard]] bool SearchFieldExifEnabled() const;
  [[nodiscard]] bool SearchFieldAiDescriptionEnabled() const;
  [[nodiscard]] bool SearchFieldAiTagsEnabled() const;

  Q_INVOKABLE QVariantList SearchRecommendations(int limit = 12);
  /// Queue a typing preview page on the search worker and return its request id. The result
  /// arrives as SearchResponseReady on the UI thread, only while no newer preview or submit
  /// request exists. A semantic query returns `awaitingSubmit` without searching.
  Q_INVOKABLE qulonglong   RequestSearch(const QString& query, int offset = 0, int limit = 24,
                                         const QString& mode = QStringLiteral("replace"));
  /// Queue an explicit submit (Enter / Search button) page. Same delivery as RequestSearch;
  /// this is the only request that reaches the semantic provider.
  Q_INVOKABLE qulonglong   RequestSubmitSearch(const QString& query, int offset = 0, int limit = 24,
                                               const QString& mode = QStringLiteral("replace"));
  Q_INVOKABLE QString      ClassifyQuery(const QString& query) const;
  Q_INVOKABLE void         SetNaturalLanguageSearchEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldFilenameEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldExifEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldAiDescriptionEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldAiTagsEnabled(bool enabled);
  /// Queue the search for the thumbnail grid and stats panel. The worker builds the filter
  /// and queries the first grid page and the stats; the UI thread then installs the filter,
  /// clears the stats filters, and replaces the grid and stats. An empty query clears.
  Q_INVOKABLE void         ApplyFuzzySearch(const QString& query);
  /// Same as ApplyFuzzySearch with a filter that matches one file.
  Q_INVOKABLE void         ApplyExactSearch(uint elementId);
  /// Remove the active search (and drop an apply that is still on the worker).
  Q_INVOKABLE void         ClearFuzzySearch();
  Q_INVOKABLE void SetSearchPreviewThumbnailVisible(uint elementId, uint imageId, bool visible,
                                                    uint maxEdge = 192);
  Q_INVOKABLE void CancelSearchPreviewThumbnails();

  /// Clear the active search without querying, and drop an apply that is still on the worker.
  void ClearSearchState(bool emitSignal = true);
  /// Drop every pending search request and every result still on the way (shutdown).
  void                     CancelSearchRequests();

 signals:
  void SearchStateChanged();
  void SearchPreviewThumbnailUpdated(uint elementId, const QString& dataUrl, bool loading,
                                     bool missingSource, const QString& errorText);
  void searchPreviewThumbnailUpdated(uint elementId, const QString& dataUrl, bool loading,
                                     bool missingSource, const QString& errorText);
  void SearchResponseReady(qulonglong requestId, const QString& mode, const QVariantMap& response);
  void searchResponseReady(qulonglong requestId, const QString& mode, const QVariantMap& response);

 private:
  void RequestSearchPreviewThumbnail(uint elementId, uint imageId, uint maxEdge = 192);
  /// Result rows for QML: display columns from the query row plus the thumbnail state that
  /// LibraryModule already holds. Runs no SQL and no image pool read.
  QVariantList BuildResultRows(const std::vector<SearchResultRow>& result_rows);
  qulonglong   RequestSearchPage(const QString& query, int offset, int limit, const QString& mode,
                                 bool submit);
  void         SubmitApplyRequest(const QString& display_query, std::optional<std::wstring> query_w,
                                  std::optional<FilterNode> filter_node);
  void         CommitAppliedSearch(const QString& display_query, sl_element_id_t folder_id,
                                   std::optional<FilterNode> filter_node, const SearchResultPage& page,
                                   const AlbumStatsView& stats, const QString& error_text);
  SearchFieldMask BuildSearchFieldMask() const;
  void            ApplySearchFieldEnabled(const char* key, bool enabled);

  ProjectModule*    project_ = nullptr;
  LibraryModule*    library_ = nullptr;
  FolderController* folders_ = nullptr;
  StatsEngine*      stats_   = nullptr;

  QString                   active_search_query_{};
  std::optional<FilterNode> active_search_filter_node_{};
  bool                      natural_language_search_enabled_  = false;
  std::uint64_t             search_preview_generation_        = 0;
  std::uint64_t             search_preview_request_sequence_  = 0;
  std::unordered_map<ThumbnailCacheKey, image_id_t>    search_preview_visible_thumbnails_{};
  std::unordered_map<ThumbnailCacheKey, std::uint64_t> search_preview_thumbnail_requests_{};
  /// Runs every search SQL statement of this controller. Destroyed first in the destructor.
  std::unique_ptr<SearchRequestWorker>                 worker_;
};

}  // namespace alcedo::ui
