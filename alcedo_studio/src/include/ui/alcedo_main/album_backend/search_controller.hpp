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
#include "storage/store/semantic/semantic_records.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/search_request_worker.hpp"
#include "ui/alcedo_main/album_backend/search_session.hpp"

namespace alcedo::ui {

class FolderController;
class LibraryModule;
class ProjectModule;
class StatsEngine;

/**
 * @brief Search part of one library refresh, read on the UI thread by LibraryModule.
 *
 * @details The accepted search, or the pending applied search that the refresh must install
 * when it succeeds: field-search text that the worker parses with the field mask
 * (pending_text_), natural-language text that the worker embeds (semantic_text_), or the
 * filter of one exact file (filter_). These three are mutually exclusive. semantic_query_ is
 * the embedding of the accepted natural-language search: reads of an accepted search build its
 * relevance select from it and call no provider. Values only; the worker never reads
 * SearchController.
 */
struct LibrarySearchInput {
  QString                               display_query_{};
  std::optional<std::wstring>           pending_text_{};
  std::optional<std::wstring>           semantic_text_{};
  std::optional<FilterNode>             filter_{};
  std::optional<SemanticQueryEmbedding> semantic_query_{};
  bool                                  pending_    = false;
  SearchFieldMask                       field_mask_ = kAllSearchFields;
};

class SearchController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString activeSearchQuery READ active_search_query NOTIFY SearchStateChanged)
  /// True when the active search is a natural-language search (the Inspector search card).
  Q_PROPERTY(bool activeSearchIsSemantic READ active_search_is_semantic NOTIFY SearchStateChanged)
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
  Q_PROPERTY(SearchSession* session READ session CONSTANT)
  /// Recent searches that the library accepted, newest first: maps {query, route}, route
  /// `fuzzy` or `semantic`. At most kMaxRecentSearches entries, kept in QSettings.
  Q_PROPERTY(QVariantList recentSearches READ RecentSearches NOTIFY RecentSearchesChanged)
  /// Open state of the settings side panel of the search palette, kept in QSettings.
  /// Default: closed.
  Q_PROPERTY(bool searchSettingsPanelOpen READ SearchSettingsPanelOpen WRITE
                 SetSearchSettingsPanelOpen NOTIFY SearchSettingsPanelOpenChanged)

 public:
  /// Largest number of recent searches.
  static constexpr int kMaxRecentSearches = 5;

  SearchController(ProjectModule* project, LibraryModule* library, FolderController* folders,
                   StatsEngine* stats, QObject* parent = nullptr);
  ~SearchController() override;

  [[nodiscard]] const QString& active_search_query() const { return active_search_query_; }
  [[nodiscard]] bool           active_search_is_semantic() const {
    return accepted_semantic_query_.has_value();
  }
  [[nodiscard]] bool           natural_language_search_enabled() const {
    return natural_language_search_enabled_;
  }
  /// Routing, response matching, and the preview rows of the search palette and search.query.
  [[nodiscard]] auto session() -> SearchSession* { return session_; }
  [[nodiscard]] bool HasActiveSearchFilter() const;
  /// Active search filter tree. The tree owns compiler output only; consumers
  /// merge it with other filters and compile once before a scoped query.
  [[nodiscard]] auto ActiveSearchFilterNode() const -> const std::optional<FilterNode>&;

  [[nodiscard]] bool SearchFieldFilenameEnabled() const;
  [[nodiscard]] bool SearchFieldExifEnabled() const;
  [[nodiscard]] bool SearchFieldAiDescriptionEnabled() const;
  [[nodiscard]] bool SearchFieldAiTagsEnabled() const;
  [[nodiscard]] bool SearchSettingsPanelOpen() const;
  void               SetSearchSettingsPanelOpen(bool open);

  Q_INVOKABLE QVariantList SearchRecommendations(int limit = 12);
  /// The SearchRecommendations rows whose label contains @p query (case-insensitive), in the
  /// same order, at most @p limit rows. An empty query matches no row.
  Q_INVOKABLE QVariantList MatchingRecommendations(const QString& query, int limit = 4);
  /// Set the Inspector statistics filter of one recommendation row (StatsEngine::
  /// ToggleStatsFilter). A filter that already has this value stays set; the text search stays.
  Q_INVOKABLE void         ApplyRecommendation(const QString& category, const QString& label);
  [[nodiscard]] auto       RecentSearches() const -> QVariantList;
  /**
   * @brief Apply the recent search at @p index with its stored route.
   *
   * @details A `fuzzy` entry calls ApplyFuzzySearch and returns "". A `semantic` entry applies a
   * natural-language search with the checks of ApplySemanticSearch except the route check: the
   * natural-language switch does not change a stored entry. Returns the rejection reason, or
   * "This recent search does not exist." for an index out of range; a rejection changes nothing.
   */
  Q_INVOKABLE QString      ApplyRecentSearch(int index);
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
  /**
   * @brief Starts the semantic sidecar and embeds the neutral text in the background, so the
   *        first natural-language search after it needs no model load.
   *
   * Does nothing when natural-language search is off, when the project has no semantic
   * provider, or while a prewarm is pending. The worker job (kind kPrewarm, so it replaces no
   * preview or library request) returns at once when the neutral text is already cached. A
   * failure is written to the log only; the search reports the real error.
   */
  Q_INVOKABLE void         PrewarmNaturalLanguageSearch();
  Q_INVOKABLE void         SetSearchFieldFilenameEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldExifEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldAiDescriptionEnabled(bool enabled);
  Q_INVOKABLE void         SetSearchFieldAiTagsEnabled(bool enabled);
  /// Request a library refresh with this search as a pending change. The library worker
  /// parses the query, combines it with the Inspector filters and the presentation options,
  /// and the search becomes active only when that refresh is accepted. Inspector filters stay.
  /// An empty query clears the search.
  Q_INVOKABLE void         ApplyFuzzySearch(const QString& query);
  /// Same as ApplyFuzzySearch with a filter that matches one file.
  Q_INVOKABLE void         ApplyExactSearch(uint elementId);
  /**
   * @brief Apply a natural-language search to the library: the library worker embeds the
   *        query, and the library shows the kept files of the current folder in relevance
   *        order (without a selected sort). Inspector filters stay.
   *
   * @details Checks, in this order, and returns the reason without any change: an empty query
   * returns "" and does nothing; a query whose route is not `semantic` (SearchSession::RouteFor)
   * is rejected; a query that is too long is rejected; a project without a semantic search
   * provider or without an active semantic model is rejected. Otherwise the search becomes the pending search, a library refresh is
   * requested, and "" is returned. An embedding or ranking failure of that refresh shows the
   * library query error and keeps the accepted content and search. GUI thread only.
   */
  Q_INVOKABLE QString      ApplySemanticSearch(const QString& query);
  /// Remove the active search (and drop an apply that is still on the worker).
  Q_INVOKABLE void         ClearFuzzySearch();
  Q_INVOKABLE void SetSearchPreviewThumbnailVisible(uint elementId, uint imageId, bool visible,
                                                    uint maxEdge = 192);
  Q_INVOKABLE void CancelSearchPreviewThumbnails();

  /// Clear the active search without querying, and drop an apply that is still on the worker.
  void ClearSearchState(bool emitSignal = true);
  /// Drop every pending search request and every result still on the way (shutdown).
  void                     CancelSearchRequests();
  /// Search part of the next library refresh: the pending applied search when one exists,
  /// else the accepted search filter.
  [[nodiscard]] auto       LibrarySearch() const -> LibrarySearchInput;
  /// Install the search of an accepted library refresh without emitting SearchStateChanged.
  /// @p used is the input that refresh read; @p filter is the filter it built (empty: the query
  /// has no terms, so no search is active); @p semantic_query is the embedding of a
  /// natural-language search, kept for the later reads of that search. An accepted field or
  /// natural-language query becomes the newest recent search (RecentSearchesChanged is emitted
  /// here: the list does not depend on the library result). Returns true when the search state
  /// changed; the caller emits SearchStateChanged after every owner of the result is installed.
  auto AcceptLibrarySearch(const LibrarySearchInput& used, const std::optional<FilterNode>& filter,
                           const std::optional<SemanticQueryEmbedding>& semantic_query) -> bool;

 signals:
  void SearchStateChanged();
  void RecentSearchesChanged();
  void SearchSettingsPanelOpenChanged();
  void SearchPreviewThumbnailUpdated(uint elementId, const QString& thumbUrl, bool loading,
                                     bool missingSource, const QString& errorText);
  void searchPreviewThumbnailUpdated(uint elementId, const QString& thumbUrl, bool loading,
                                     bool missingSource, const QString& errorText);
  void SearchResponseReady(qulonglong requestId, const QString& mode, const QVariantMap& response);
  void searchResponseReady(qulonglong requestId, const QString& mode, const QVariantMap& response);

 private:
  /// One recent search: the query text and its route.
  struct RecentSearch {
    QString query_;
    bool    semantic_ = false;
  };

  /// Checks of ApplySemanticSearch after the route check, then the pending search and the
  /// library refresh. @p trimmed is not empty.
  auto ApplySemanticText(const QString& trimmed) -> QString;
  /// Put @p query at the top of the recent searches (no duplicate, at most
  /// kMaxRecentSearches), write them to QSettings, and emit RecentSearchesChanged.
  void RecordRecentSearch(const QString& query, bool semantic);
  void RequestSearchPreviewThumbnail(uint elementId, uint imageId, uint maxEdge = 192);
  /// Result rows for QML: display columns from the query row plus the thumbnail state that
  /// LibraryModule already holds. Runs no SQL and no image pool read.
  QVariantList BuildResultRows(const std::vector<SearchResultRow>& result_rows);
  /// @p allow_semantic false classifies the query as with natural language search off, so a
  /// semantic query uses the SQL path.
  qulonglong   RequestSearchPage(const QString& query, int offset, int limit, const QString& mode,
                                 bool submit, bool allow_semantic = true);
  SearchFieldMask BuildSearchFieldMask() const;
  void            ApplySearchFieldEnabled(const char* key, bool enabled);

  ProjectModule*    project_ = nullptr;
  LibraryModule*    library_ = nullptr;
  FolderController* folders_ = nullptr;
  StatsEngine*      stats_   = nullptr;

  QString                   active_search_query_{};
  std::optional<FilterNode> active_search_filter_node_{};
  /// Embedding of the active natural-language search; empty for a field or exact search. An
  /// operation result (global_search_redesign_plan.md, Section 5.2), released when the search
  /// is cleared or replaced.
  std::optional<SemanticQueryEmbedding>                accepted_semantic_query_{};
  /// Applied search that waits for its library refresh; the change description only.
  std::optional<LibrarySearchInput> pending_search_{};
  /// Newest first; read from QSettings once at construction, written after each change.
  std::vector<RecentSearch>         recent_searches_{};
  bool                      natural_language_search_enabled_  = false;
  /// A prewarm job is queued or runs on the worker (PrewarmNaturalLanguageSearch).
  bool                                                 prewarm_pending_                  = false;
  /// Read from QSettings once at construction, written after each change.
  bool                      search_settings_panel_open_       = false;
  std::uint64_t             search_preview_generation_        = 0;
  std::uint64_t             search_preview_request_sequence_  = 0;
  std::unordered_map<ThumbnailCacheKey, image_id_t>    search_preview_visible_thumbnails_{};
  std::unordered_map<ThumbnailCacheKey, std::uint64_t> search_preview_thumbnail_requests_{};
  /// Previews whose ThumbnailService pin this controller holds, with their image URL. Only
  /// these pins are released: the Library grid can pin the same key.
  std::unordered_map<ThumbnailCacheKey, QString>       search_preview_pinned_thumbnails_{};
  /// The library query worker owned by LibraryModule. Preview requests run there; the
  /// destructor waits until no preview job runs.
  SearchRequestWorker*                                 worker_ = nullptr;
  /// Child object; reads responses through SearchResponseReady.
  SearchSession*                                       session_ = nullptr;
};

}  // namespace alcedo::ui
