//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "app/sleeve_filter_service.hpp"
#include "app/thumbnail_types.hpp"

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
  [[nodiscard]] auto ActiveSearchFilterWhere() const -> const std::optional<std::wstring>&;

  [[nodiscard]] bool SearchFieldFilenameEnabled() const;
  [[nodiscard]] bool SearchFieldExifEnabled() const;
  [[nodiscard]] bool SearchFieldAiDescriptionEnabled() const;
  [[nodiscard]] bool SearchFieldAiTagsEnabled() const;

  Q_INVOKABLE QVariantList SearchRecommendations(int limit = 12);
  Q_INVOKABLE QVariantMap  SearchPreview(const QString& query, int offset = 0, int limit = 24);
  Q_INVOKABLE QVariantMap  SubmitSearch(const QString& query, int offset = 0, int limit = 24);
  Q_INVOKABLE qulonglong   RequestSubmitSearch(const QString& query, int offset = 0, int limit = 24,
                                               const QString& mode = QStringLiteral("replace"));
  Q_INVOKABLE QString      ClassifyQuery(const QString& query) const;
  Q_INVOKABLE void         SetNaturalLanguageSearchEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldFilenameEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldExifEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldAiDescriptionEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldAiTagsEnabled(bool enabled);
  Q_INVOKABLE void         ApplyFuzzySearch(const QString& query);
  Q_INVOKABLE void         ApplyExactSearch(uint elementId);
  Q_INVOKABLE void         ClearFuzzySearch();
  Q_INVOKABLE void SetSearchPreviewThumbnailVisible(uint elementId, uint imageId, bool visible,
                                                    uint maxEdge = 192);
  Q_INVOKABLE void CancelSearchPreviewThumbnails();

  void ClearSearchState(bool emitSignal = true);

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
  QVariantMap RunTraditionalPreview(const QString& query, int offset, int limit,
                                    const std::string& route_name);
  QVariantList BuildResultRows(const std::vector<alcedo::FuzzySearchMatch>& matches);
  qulonglong   RequestSearch(const QString& query, int offset, int limit, const QString& mode,
                             bool submit);
  SearchFieldMask BuildSearchFieldMask() const;
  void            ApplySearchFieldEnabled(const char* key, bool enabled);

  ProjectModule*    project_ = nullptr;
  LibraryModule*    library_ = nullptr;
  FolderController* folders_ = nullptr;
  StatsEngine*      stats_   = nullptr;

  QString                     active_search_query_{};
  std::optional<std::wstring> active_search_filter_where_{};
  bool                        natural_language_search_enabled_ = false;
  std::uint64_t               search_response_request_sequence_ = 0;
  std::uint64_t               search_preview_generation_        = 0;
  std::uint64_t               search_preview_request_sequence_  = 0;
  std::unordered_map<ThumbnailCacheKey, image_id_t>    search_preview_visible_thumbnails_{};
  std::unordered_map<ThumbnailCacheKey, std::uint64_t> search_preview_thumbnail_requests_{};
};

}  // namespace alcedo::ui
