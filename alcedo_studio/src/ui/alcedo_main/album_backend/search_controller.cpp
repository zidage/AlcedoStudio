//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/search_controller.hpp"

#include <QCoreApplication>
#include <QDate>
#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "app/search_query_classifier.hpp"
#include "app/thumbnail_service.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/search_request_worker.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {

using namespace album_util;

namespace {

#define SEARCH_TEXT(text, ...)                 \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

// QSettings key for the natural-language-search toggle (the user-facing name
// for the CLIP semantic route). Default is off for both new and existing users
// (see roadmap 5b). Renamed from "search/semanticEnabled"; the legacy key is
// read once in the ctor for migration so existing users keep their toggle.
constexpr auto kNaturalLanguageSearchEnabledKey = "search/naturalLanguageSearchEnabled";
constexpr auto kLegacySemanticSearchEnabledKey  = "search/semanticEnabled";
// Search-settings drawer field-scope toggles. Each defaults to on (the
// pre-mask behavior searched every field).
constexpr auto kSearchFieldFilenameKey      = "search/fieldFilename";
constexpr auto kSearchFieldExifKey          = "search/fieldExif";
constexpr auto kSearchFieldAiDescriptionKey = "search/fieldAiDescription";
constexpr auto kSearchFieldAiTagsKey        = "search/fieldAiTags";
// Recent searches that the library accepted: a list of maps {query, route}, newest first.
constexpr auto kRecentQueriesKey            = "search/recentQueries";
constexpr auto kRecentQueryKey              = "query";
constexpr auto kRecentRouteKey              = "route";
constexpr auto kFuzzyRouteName              = "fuzzy";
constexpr auto kSemanticRouteName           = "semantic";
// Open state of the settings side panel of the search palette.
constexpr auto kSettingsPanelOpenKey        = "search/settingsPanelOpen";

auto SearchPreviewThumbnailResolution(uint maxEdge) -> ThumbnailResolution {
  return maxEdge <= 256   ? ThumbnailResolution::k256
         : maxEdge <= 512 ? ThumbnailResolution::k512
                          : ThumbnailResolution::k1024;
}

auto CurrentExceptionText(const char* fallback) -> QString {
  try {
    throw;
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  } catch (...) {
    return QString::fromUtf8(fallback);
  }
}

auto MakeEmptyResponse(int offset, int limit, const std::string& route_name) -> QVariantMap {
  return QVariantMap{{"rows", QVariantList{}},
                    {"offset", std::max(0, offset)},
                    {"limit", std::max(0, limit)},
                    {"total", 0},
                    {"hasMore", false},
                    {"route", QString::fromStdString(route_name)}};
}

auto ClampToInt(size_t value) -> int {
  return static_cast<int>(
      std::min<size_t>(value, static_cast<size_t>(std::numeric_limits<int>::max())));
}

/// Input of one search dialog page request, captured on the UI thread (the classification,
/// the field mask from QSettings, and the current folder) and read on the search worker.
struct SearchPageRequest {
  std::wstring                         query_w;
  int                                  offset = 0;
  int                                  limit  = 0;
  bool                                 submit = false;
  SearchQueryClassification            classification;
  std::string                          route_name;
  SearchFieldMask                      field_mask = kAllSearchFields;
  std::optional<sl_element_id_t>       folder_id;
  std::shared_ptr<SleeveFilterService> filter_service;
};

/// Output of one page request: the response fields and the result rows. The UI thread adds
/// the thumbnail state of each row (owned by LibraryModule) when it builds `rows`.
struct SearchPageResult {
  QVariantMap                  response;
  std::vector<SearchResultRow> rows;
};

/// Runs on the search worker: one SQL statement for a traditional or label page, or the
/// semantic provider and one display-column statement for a semantic submit.
auto RunSearchPageRequest(const SearchPageRequest& request) -> SearchPageResult {
  SearchPageResult result{MakeEmptyResponse(request.offset, request.limit, request.route_name), {}};
  const auto       route = request.classification.route_;

  if (route == SearchQueryRoute::Empty) {
    if (request.submit) {
      result.response["recommendations"] = true;
    }
    return result;
  }
  // Semantic preview must not run on every keystroke. Typing only signals that an explicit
  // submit (Enter / Search button) is required.
  if (route == SearchQueryRoute::Semantic && !request.submit) {
    result.response["awaitingSubmit"] = true;
    return result;
  }
  if (request.limit <= 0) {
    return result;
  }

  const auto safe_offset = static_cast<size_t>(std::max(0, request.offset));
  const auto safe_limit  = static_cast<size_t>(request.limit);

  if (route != SearchQueryRoute::Semantic) {
    // Label and Traditional routes use the ordinary SQL path.
    if (!request.filter_service || !request.folder_id.has_value()) {
      return result;
    }
    try {
      auto page = request.filter_service->SearchFolderPage(
          request.folder_id.value(), request.query_w, safe_offset, safe_limit, request.field_mask);
      result.response["total"]   = ClampToInt(page.total_);
      result.response["hasMore"] = safe_offset + page.rows_.size() < page.total_;
      result.rows                = std::move(page.rows_);
    } catch (const std::exception& e) {
      result.response["searchErrorText"] = QString::fromUtf8(e.what());
    } catch (...) {
      result.response["searchErrorText"] = QStringLiteral("Unknown search error.");
    }
    return result;
  }

  // Semantic route: the only path that may reach the semantic provider. Guard the prompt
  // length before embedding, and surface a clean unavailable state instead of falling back
  // to a C++ vector scan.
  if (!request.folder_id.has_value()) {
    result.response["semanticUnavailable"] = true;
    return result;
  }
  if (request.classification.too_long_) {
    result.response["tooLong"] = true;
    return result;
  }
  if (!request.filter_service || !request.filter_service->HasSemanticSearchProvider()) {
    result.response["semanticUnavailable"] = true;
    return result;
  }
  try {
    auto page = request.filter_service->SearchFolderSemanticRows(
        request.folder_id.value(), request.query_w, safe_offset, safe_limit);
    result.response["total"]   = ClampToInt(page.total_);
    result.response["hasMore"] = safe_offset + page.rows_.size() < page.total_;
    result.rows                = std::move(page.rows_);
  } catch (const std::exception& e) {
    result.response["semanticUnavailable"] = true;
    result.response["semanticErrorText"]   = QString::fromUtf8(e.what());
  } catch (...) {
    result.response["semanticUnavailable"] = true;
  }
  return result;
}

}  // namespace

SearchController::SearchController(ProjectModule* project, LibraryModule* library,
                                   FolderController* folders, StatsEngine* stats, QObject* parent)
    : QObject(parent),
      project_(project),
      library_(library),
      folders_(folders),
      stats_(stats),
      // Lifecycle tests construct the controller without a LibraryModule.
      worker_(library != nullptr ? &library->query_worker() : nullptr) {
  QSettings settings;
  if (!settings.contains(QLatin1String(kNaturalLanguageSearchEnabledKey))) {
    // One-time migration: carry over the pre-rename "Semantic" toggle so
    // existing users keep their natural-language-search preference.
    natural_language_search_enabled_ =
        settings.value(QLatin1String(kLegacySemanticSearchEnabledKey), false).toBool();
    settings.setValue(QLatin1String(kNaturalLanguageSearchEnabledKey),
                      natural_language_search_enabled_);
  } else {
    natural_language_search_enabled_ =
        settings.value(QLatin1String(kNaturalLanguageSearchEnabledKey), false).toBool();
  }

  search_settings_panel_open_ =
      settings.value(QLatin1String(kSettingsPanelOpenKey), false).toBool();
  for (const auto& value : settings.value(QLatin1String(kRecentQueriesKey)).toList()) {
    const auto    entry = value.toMap();
    const QString query = entry.value(QLatin1String(kRecentQueryKey)).toString().trimmed();
    const QString route = entry.value(QLatin1String(kRecentRouteKey)).toString();
    // An entry with an unknown route is skipped.
    if (query.isEmpty() ||
        (route != QLatin1String(kFuzzyRouteName) && route != QLatin1String(kSemanticRouteName))) {
      continue;
    }
    const bool duplicate = std::ranges::any_of(
        recent_searches_, [&](const RecentSearch& recent) { return recent.query_ == query; });
    if (!duplicate && static_cast<int>(recent_searches_.size()) < kMaxRecentSearches) {
      recent_searches_.push_back(
          RecentSearch{.query_ = query, .semantic_ = route == QLatin1String(kSemanticRouteName)});
    }
  }

  session_ = new SearchSession(
      SearchSessionPort{
          .classify_query           = [this](const QString& query) { return ClassifyQuery(query); },
          .natural_language_enabled = [this] { return natural_language_search_enabled_; },
          .request_page =
              [this](const QString& query, int offset, int limit, const QString& mode, bool submit,
                     bool allow_semantic) {
                return RequestSearchPage(query, offset, limit, mode, submit, allow_semantic);
              }},
      this);
  connect(this, &SearchController::SearchResponseReady, session_, &SearchSession::ApplyResponse);
}

SearchController::~SearchController() {
  // A running preview or prewarm job posts its result to `this`; wait for it before teardown.
  if (worker_ != nullptr) {
    worker_->InvalidateAndWait(SearchRequestKind::kPreview);
    worker_->InvalidateAndWait(SearchRequestKind::kPrewarm);
  }
  CancelSearchPreviewThumbnails();
}

bool SearchController::HasActiveSearchFilter() const {
  return active_search_filter_node_.has_value();
}

auto SearchController::ActiveSearchFilterNode() const -> const std::optional<FilterNode>& {
  return active_search_filter_node_;
}

auto SearchController::SearchFieldFilenameEnabled() const -> bool {
  return QSettings{}.value(QLatin1String(kSearchFieldFilenameKey), true).toBool();
}
auto SearchController::SearchFieldExifEnabled() const -> bool {
  return QSettings{}.value(QLatin1String(kSearchFieldExifKey), true).toBool();
}
auto SearchController::SearchFieldAiDescriptionEnabled() const -> bool {
  return QSettings{}.value(QLatin1String(kSearchFieldAiDescriptionKey), true).toBool();
}
auto SearchController::SearchFieldAiTagsEnabled() const -> bool {
  return QSettings{}.value(QLatin1String(kSearchFieldAiTagsKey), true).toBool();
}

auto SearchController::SearchSettingsPanelOpen() const -> bool {
  return search_settings_panel_open_;
}

void SearchController::SetSearchSettingsPanelOpen(bool open) {
  if (search_settings_panel_open_ == open) {
    return;
  }
  search_settings_panel_open_ = open;
  QSettings{}.setValue(QLatin1String(kSettingsPanelOpenKey), open);
  emit SearchSettingsPanelOpenChanged();
}

auto SearchController::BuildSearchFieldMask() const -> SearchFieldMask {
  SearchFieldMask mask = 0;
  if (SearchFieldFilenameEnabled()) {
    mask |= SearchField::Filename;
  }
  if (SearchFieldExifEnabled()) {
    mask |= SearchField::Exif;
  }
  if (SearchFieldAiDescriptionEnabled()) {
    mask |= SearchField::AiDescription;
  }
  if (SearchFieldAiTagsEnabled()) {
    mask |= SearchField::AiTags;
  }
  return mask;
}

void SearchController::ApplySearchFieldEnabled(const char* key, bool enabled) {
  QSettings{}.setValue(QLatin1String(key), enabled);
  emit SearchStateChanged();
  // The cached WHERE drives not just the search dialog preview but also the
  // thumbnail grid and the stats panel (via StatsEngine). When a search is
  // active, re-apply it so the new mask regenerates the WHERE for all three
  // surfaces. When no search is active this is a no-op (the next search will
  // pick up the new mask).
  if (HasActiveSearchFilter()) {
    ApplyFuzzySearch(active_search_query_);
  }
}

void SearchController::SetSearchFieldFilenameEnabled(bool enabled) {
  ApplySearchFieldEnabled(kSearchFieldFilenameKey, enabled);
}

void SearchController::SetSearchFieldExifEnabled(bool enabled) {
  ApplySearchFieldEnabled(kSearchFieldExifKey, enabled);
}

void SearchController::SetSearchFieldAiDescriptionEnabled(bool enabled) {
  ApplySearchFieldEnabled(kSearchFieldAiDescriptionKey, enabled);
}

void SearchController::SetSearchFieldAiTagsEnabled(bool enabled) {
  ApplySearchFieldEnabled(kSearchFieldAiTagsKey, enabled);
}

auto SearchController::SearchRecommendations(int limit) -> QVariantList {
  if (limit <= 0) {
    return {};
  }
  return stats_->BuildSearchRecommendations(limit);
}

auto SearchController::MatchingRecommendations(const QString& query, int limit) -> QVariantList {
  const QString needle = query.trimmed();
  if (limit <= 0 || needle.isEmpty()) {
    return {};
  }
  // Every recommendation row: one per bucket of the four categories.
  const auto all_rows = static_cast<int>(
      std::min<qsizetype>(stats_->camera_stats().size() + stats_->date_stats().size() +
                              stats_->lens_stats().size() + stats_->label_stats().size(),
                          std::numeric_limits<int>::max()));
  QVariantList rows;
  for (const auto& row : stats_->BuildSearchRecommendations(all_rows)) {
    if (rows.size() >= limit) {
      break;
    }
    if (row.toMap()
            .value(QStringLiteral("label"))
            .toString()
            .contains(needle, Qt::CaseInsensitive)) {
      rows.push_back(row);
    }
  }
  return rows;
}

void SearchController::ApplyRecommendation(const QString& category, const QString& label) {
  if (category.isEmpty() || label.isEmpty()) {
    return;
  }
  const QString current = category == u"camera"  ? stats_->filter_camera()
                          : category == u"date"  ? stats_->filter_date()
                          : category == u"lens"  ? stats_->filter_lens()
                          : category == u"label" ? stats_->filter_label()
                                                 : QString{};
  // ToggleStatsFilter clears a filter that already has this value; a row click sets it.
  if (current == label) {
    return;
  }
  stats_->ToggleStatsFilter(category, label);
}

auto SearchController::RecentSearches() const -> QVariantList {
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(recent_searches_.size()));
  for (const auto& recent : recent_searches_) {
    rows.push_back(
        QVariantMap{{QLatin1String(kRecentQueryKey), recent.query_},
                    {QLatin1String(kRecentRouteKey),
                     QLatin1String(recent.semantic_ ? kSemanticRouteName : kFuzzyRouteName)}});
  }
  return rows;
}

auto SearchController::ApplyRecentSearch(int index) -> QString {
  if (index < 0 || index >= static_cast<int>(recent_searches_.size())) {
    return SEARCH_TEXT("This recent search does not exist.").Render();
  }
  const auto recent = recent_searches_.at(static_cast<size_t>(index));
  if (recent.semantic_) {
    return ApplySemanticText(recent.query_);
  }
  ApplyFuzzySearch(recent.query_);
  return {};
}

void SearchController::RecordRecentSearch(const QString& query, bool semantic) {
  if (query.isEmpty()) {
    return;
  }
  std::erase_if(recent_searches_,
                [&](const RecentSearch& recent) { return recent.query_ == query; });
  recent_searches_.insert(recent_searches_.begin(),
                          RecentSearch{.query_ = query, .semantic_ = semantic});
  if (static_cast<int>(recent_searches_.size()) > kMaxRecentSearches) {
    recent_searches_.resize(static_cast<size_t>(kMaxRecentSearches));
  }
  QSettings{}.setValue(QLatin1String(kRecentQueriesKey), RecentSearches());
  emit RecentSearchesChanged();
}

auto SearchController::BuildResultRows(const std::vector<SearchResultRow>& result_rows)
    -> QVariantList {
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(result_rows.size()));

  for (const auto& result_row : result_rows) {
    const QDate capture_date =
        QDate::fromString(QString::fromStdString(result_row.capture_date_), Qt::ISODate);
    QVariantMap row{
        {"elementId", static_cast<uint>(result_row.file_id_)},
        {"fileId", static_cast<uint>(result_row.file_id_)},
        {"imageId", static_cast<uint>(result_row.image_id_)},
        {"fileName", QString::fromStdString(result_row.file_name_)},
        {"cameraModel", result_row.camera_model_.empty()
                            ? SEARCH_TEXT("Unknown").Render()
                            : QString::fromStdString(result_row.camera_model_)},
        {"lens", QString::fromStdString(result_row.lens_)},
        {"captureDate", capture_date.isValid() ? capture_date.toString(QStringLiteral("yyyy-MM-dd"))
                                               : QStringLiteral("--")},
        {"rating", result_row.rating_},
        {"thumbUrl", QString{}},
        {"thumbLoading", false},
        {"thumbMissingSource", false},
        {"thumbErrorText", QString{}}};

    if (const auto* item = library_->FindAlbumItem(result_row.file_id_); item != nullptr) {
      row["thumbUrl"]           = item->thumb_url;
      row["thumbLoading"]       = item->thumb_loading;
      row["thumbMissingSource"] = item->thumb_missing_source;
      row["thumbErrorText"]     = item->thumb_error_text;
    }

    rows.push_back(std::move(row));
  }
  return rows;
}

auto SearchController::RequestSearch(const QString& query, int offset, int limit,
                                     const QString& mode) -> qulonglong {
  return RequestSearchPage(query, offset, limit, mode, false);
}

auto SearchController::RequestSubmitSearch(const QString& query, int offset, int limit,
                                           const QString& mode) -> qulonglong {
  return RequestSearchPage(query, offset, limit, mode, true);
}

auto SearchController::RequestSearchPage(const QString& query, int offset, int limit,
                                         const QString& mode, bool submit, bool allow_semantic)
    -> qulonglong {
  const auto        trimmed = query.trimmed();
  SearchPageRequest request{
      .query_w        = trimmed.toStdWString(),
      .offset         = offset,
      .limit          = limit,
      .submit         = submit,
      .classification = ClassifySearchQuery(trimmed.toStdWString(),
                                            natural_language_search_enabled_ && allow_semantic),
      .field_mask     = BuildSearchFieldMask(),
      .folder_id      = folders_->CurrentFolderElementId(),
  };
  request.route_name = std::string(SearchQueryRouteName(request.classification.route_));
  if (auto project = project_->handler().project()) {
    request.filter_service = project->GetSleeveFilterService();
  }

  const auto generation = worker_->Submit(
      SearchRequestKind::kPreview,
      [this, mode, request = std::move(request)](std::uint64_t request_generation) {
        auto result = RunSearchPageRequest(request);
        // The destructor joins the worker before QObject teardown, so `this` is valid here;
        // a result posted during teardown is removed with the object's posted events.
        QMetaObject::invokeMethod(
            this,
            [this, request_generation, mode, result = std::move(result)]() mutable {
              if (!worker_->IsCurrent(SearchRequestKind::kPreview, request_generation)) {
                return;
              }
              result.response["rows"] = BuildResultRows(result.rows);
              const auto request_id   = static_cast<qulonglong>(request_generation);
              emit       SearchResponseReady(request_id, mode, result.response);
              emit       searchResponseReady(request_id, mode, result.response);
            },
            Qt::QueuedConnection);
      });
  return static_cast<qulonglong>(generation);
}

auto SearchController::ClassifyQuery(const QString& query) const -> QString {
  const auto classification =
      ClassifySearchQuery(query.trimmed().toStdWString(), natural_language_search_enabled_);
  return QString::fromUtf8(SearchQueryRouteName(classification.route_).data(),
                           static_cast<int>(SearchQueryRouteName(classification.route_).size()));
}

void SearchController::PrewarmNaturalLanguageSearch() {
  if (!natural_language_search_enabled_ || prewarm_pending_ || worker_ == nullptr) {
    return;
  }
  auto project        = project_->handler().project();
  auto filter_service = project ? project->GetSleeveFilterService() : nullptr;
  if (!filter_service || !filter_service->HasSemanticSearchProvider()) {
    return;
  }
  prewarm_pending_ = true;
  worker_->Submit(SearchRequestKind::kPrewarm, [this, filter_service = std::move(filter_service)](
                                                   std::uint64_t) {
    try {
      if (!filter_service->IsSemanticSearchPrewarmed()) {
        filter_service->PrewarmSemanticSearch();
      }
    } catch (const std::exception& e) {
      qCWarning(diag::semanticLog).noquote()
          << QStringLiteral("semantic.search.prewarm.failed error=%1")
                 .arg(QString::fromUtf8(e.what()));
    }
    // The destructor waits for this job before QObject teardown.
    QMetaObject::invokeMethod(this, [this]() { prewarm_pending_ = false; }, Qt::QueuedConnection);
  });
}

void SearchController::SetNaturalLanguageSearchEnabled(bool enabled) {
  if (natural_language_search_enabled_ == enabled) {
    return;
  }
  natural_language_search_enabled_ = enabled;
  QSettings{}.setValue(QLatin1String(kNaturalLanguageSearchEnabledKey), enabled);
  emit SearchStateChanged();
}

void SearchController::ApplyFuzzySearch(const QString& query) {
  const QString trimmed = query.trimmed();
  if (trimmed.isEmpty()) {
    ClearFuzzySearch();
    return;
  }
  pending_search_ = LibrarySearchInput{.display_query_ = trimmed,
                                       .pending_text_  = trimmed.toStdWString(),
                                       .pending_       = true,
                                       .field_mask_    = BuildSearchFieldMask()};
  library_->RequestLibraryRefresh();
}

void SearchController::ApplyExactSearch(uint elementId) {
  if (elementId == 0) {
    return;
  }
  auto proj = project_->handler().project();
  if (!proj) {
    return;
  }
  auto filter_service = proj->GetSleeveFilterService();
  if (!filter_service) {
    return;
  }
  pending_search_ = LibrarySearchInput{
      .display_query_ =
          SEARCH_TEXT("Image %1", QString::number(static_cast<qulonglong>(elementId))).Render(),
      .filter_     = filter_service->BuildExactFileWhere(static_cast<sl_element_id_t>(elementId)),
      .pending_    = true,
      .field_mask_ = BuildSearchFieldMask()};
  library_->RequestLibraryRefresh();
}

auto SearchController::ApplySemanticSearch(const QString& query) -> QString {
  const QString trimmed = query.trimmed();
  if (trimmed.isEmpty()) {
    return {};
  }
  if (session_->RouteFor(trimmed) != QStringLiteral("semantic")) {
    return SEARCH_TEXT("This query uses field search.").Render();
  }
  return ApplySemanticText(trimmed);
}

auto SearchController::ApplySemanticText(const QString& trimmed) -> QString {
  if (ClassifySearchQuery(trimmed.toStdWString(), true).too_long_) {
    return SEARCH_TEXT("Query is too long for natural language search.").Render();
  }
  auto proj           = project_->handler().project();
  auto filter_service = proj ? proj->GetSleeveFilterService() : nullptr;
  if (!filter_service || !filter_service->HasSemanticSearchProvider()) {
    return SEARCH_TEXT("Natural language search is not available for this project.").Render();
  }
  if (stats_->ActiveSemanticModelKey().empty()) {
    return SEARCH_TEXT("No semantic model is active.").Render();
  }
  pending_search_ = LibrarySearchInput{.display_query_ = trimmed,
                                       .semantic_text_ = trimmed.toStdWString(),
                                       .pending_       = true,
                                       .field_mask_    = BuildSearchFieldMask()};
  library_->RequestLibraryRefresh();
  return {};
}

auto SearchController::LibrarySearch() const -> LibrarySearchInput {
  if (pending_search_.has_value()) {
    return *pending_search_;
  }
  return LibrarySearchInput{.display_query_  = active_search_query_,
                            .filter_         = active_search_filter_node_,
                            .semantic_query_ = accepted_semantic_query_,
                            .pending_        = false,
                            .field_mask_     = BuildSearchFieldMask()};
}

auto SearchController::AcceptLibrarySearch(
    const LibrarySearchInput& used, const std::optional<FilterNode>& filter,
    const std::optional<SemanticQueryEmbedding>& semantic_query) -> bool {
  if (!used.pending_) {
    return false;
  }
  // A search applied after this refresh was submitted stays pending for its own refresh.
  if (pending_search_.has_value() && pending_search_->display_query_ == used.display_query_ &&
      pending_search_->pending_text_ == used.pending_text_ &&
      pending_search_->semantic_text_ == used.semantic_text_) {
    pending_search_.reset();
  }
  if (used.semantic_text_.has_value() && semantic_query.has_value()) {
    active_search_query_ = used.display_query_;
    active_search_filter_node_.reset();
    accepted_semantic_query_ = semantic_query;
    RecordRecentSearch(used.display_query_, true);
    return true;
  }
  accepted_semantic_query_.reset();
  if (!filter.has_value()) {
    // The query parsed to no terms: no search is active.
    const bool changed = !active_search_query_.isEmpty() || active_search_filter_node_.has_value();
    active_search_query_.clear();
    active_search_filter_node_.reset();
    return changed;
  }
  active_search_query_       = used.display_query_;
  active_search_filter_node_ = filter;
  // An exact-file search (photo click) is not a recent search.
  if (used.pending_text_.has_value()) {
    RecordRecentSearch(used.display_query_, false);
  }
  return true;
}

void SearchController::ClearFuzzySearch() {
  const bool had_search = pending_search_.has_value() || !active_search_query_.isEmpty() ||
                          active_search_filter_node_.has_value() ||
                          accepted_semantic_query_.has_value();
  if (!had_search) {
    return;
  }
  ClearSearchState(true);
  // The Inspector filters stay; only the search term leaves the library query.
  library_->RequestLibraryRefresh();
}

void SearchController::CancelSearchRequests() {
  session_->Reset();
  if (worker_ == nullptr) {
    return;
  }
  worker_->Invalidate(SearchRequestKind::kPreview);
  worker_->Invalidate(SearchRequestKind::kApply);
}

void SearchController::SetSearchPreviewThumbnailVisible(uint elementId, uint imageId, bool visible,
                                                        uint maxEdge) {
  if (elementId == 0 || imageId == 0) {
    return;
  }

  const ThumbnailCacheKey key{static_cast<sl_element_id_t>(elementId),
                              SearchPreviewThumbnailResolution(maxEdge)};
  if (!visible) {
    search_preview_visible_thumbnails_.erase(key);
    // A request still on the way releases its own pin when it arrives.
    search_preview_thumbnail_requests_.erase(key);

    if (library_) {
      library_->thumbs().ReleaseStoreImageIfUnpinned(key);
    }
    // Release only a pin of this controller: the Library grid can pin the same key, and a
    // preview shown from the Library item took no pin.
    if (search_preview_pinned_thumbnails_.erase(key) == 0) {
      return;
    }

    auto thumb_svc = project_->handler().thumbnail_service();
    if (!thumb_svc) {
      return;
    }
    try {
      thumb_svc->ReleaseThumbnail(key);
    } catch (...) {
    }
    return;
  }

  search_preview_visible_thumbnails_[key] = static_cast<image_id_t>(imageId);
  RequestSearchPreviewThumbnail(elementId, imageId, maxEdge);
}

void SearchController::RequestSearchPreviewThumbnail(uint elementId, uint imageId, uint maxEdge) {
  if (elementId == 0 || imageId == 0) {
    return;
  }

  auto thumb_svc = project_->handler().thumbnail_service();
  if (!thumb_svc) {
    return;
  }

  const auto              resolution = SearchPreviewThumbnailResolution(maxEdge);
  const ThumbnailCacheKey key{static_cast<sl_element_id_t>(elementId), resolution};
  const auto              request_generation = search_preview_generation_;
  const auto              expected_image_id  = static_cast<image_id_t>(imageId);

  const auto visible_it = search_preview_visible_thumbnails_.find(key);
  if (visible_it == search_preview_visible_thumbnails_.end() || visible_it->second != expected_image_id) {
    return;
  }

  if (const auto* item = library_->FindAlbumItem(static_cast<sl_element_id_t>(elementId));
      item != nullptr && !item->thumb_url.isEmpty()) {
    emit SearchPreviewThumbnailUpdated(elementId, item->thumb_url, false,
                                       item->thumb_missing_source, item->thumb_error_text);
    emit searchPreviewThumbnailUpdated(elementId, item->thumb_url, false,
                                       item->thumb_missing_source, item->thumb_error_text);
    return;
  }

  if (const auto pinned_it = search_preview_pinned_thumbnails_.find(key);
      pinned_it != search_preview_pinned_thumbnails_.end()) {
    emit SearchPreviewThumbnailUpdated(elementId, pinned_it->second, false, false, QString{});
    emit searchPreviewThumbnailUpdated(elementId, pinned_it->second, false, false, QString{});
    return;
  }
  if (search_preview_thumbnail_requests_.find(key) != search_preview_thumbnail_requests_.end()) {
    return;
  }
  const auto request_id = ++search_preview_request_sequence_;
  search_preview_thumbnail_requests_.emplace(key, request_id);

  emit SearchPreviewThumbnailUpdated(elementId, QString{}, true, false, QString{});
  emit searchPreviewThumbnailUpdated(elementId, QString{}, true, false, QString{});

  CallbackDispatcher dispatcher = [](std::function<void()> fn) {
    auto* app = QCoreApplication::instance();
    if (!app) {
      fn();
      return;
    }
    QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
  };

  QPointer<SearchController> self(this);
  try {
    thumb_svc->GetThumbnailDetailed(
        static_cast<sl_element_id_t>(elementId), static_cast<image_id_t>(imageId),
        [self, service = thumb_svc, elementId, imageId, maxEdge, key,
         request_generation, request_id](ThumbnailRequestResult result) {
          auto release_thumbnail = [&]() {
            if (service) {
              try {
                service->ReleaseThumbnail(key);
              } catch (...) {
              }
            }
          };

          if (!self) {
            release_thumbnail();
            return;
          }
          if (self->search_preview_generation_ != request_generation) {
            release_thumbnail();
            return;
          }
          const auto request_it = self->search_preview_thumbnail_requests_.find(key);
          if (request_it == self->search_preview_thumbnail_requests_.end() ||
              request_it->second != request_id) {
            release_thumbnail();
            return;
          }
          const auto visible_it = self->search_preview_visible_thumbnails_.find(key);
          if (visible_it == self->search_preview_visible_thumbnails_.end() ||
              visible_it->second != static_cast<image_id_t>(imageId)) {
            self->search_preview_thumbnail_requests_.erase(key);
            release_thumbnail();
            return;
          }
          if (result.status != ThumbnailRequestStatus::kReady || !result.guard ||
              !result.guard->thumbnail_buffer_) {
            self->search_preview_thumbnail_requests_.erase(key);
            emit self->SearchPreviewThumbnailUpdated(
                elementId, QString{}, false, false,
                result.message.empty() ? QObject::tr("Thumbnail render returned no image.")
                                       : QString::fromUtf8(result.message));
            emit self->searchPreviewThumbnailUpdated(
                elementId, QString{}, false, false,
                result.message.empty() ? QObject::tr("Thumbnail render returned no image.")
                                       : QString::fromUtf8(result.message));
            release_thumbnail();
            return;
          }

          std::thread([self, service, elementId, imageId, maxEdge, key, request_generation,
                       request_id, image_store = self->library_->thumbs().image_store(),
                       guard = std::move(result.guard)]() mutable {
            QString thumb_url;
            QString error_text;
            try {
              auto* buffer = guard->thumbnail_buffer_.get();
              if (buffer && !buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
                buffer->SyncToCPU();
              }
              if (buffer && buffer->cpu_data_valid_) {
                QImage image = album_util::MatRgba32fToQImageCopy(buffer->GetCPUData());
                if (!image.isNull()) {
                  const int edge = static_cast<int>(std::max<uint>(1, maxEdge));
                  QImage scaled =
                      image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                  if (image_store) {
                    thumb_url = image_store->Put(static_cast<sl_element_id_t>(elementId),
                                                 static_cast<uint32_t>(key.resolution),
                                                 std::move(scaled));
                  }
                }
              }
              if (thumb_url.isEmpty()) {
                error_text = QObject::tr("Thumbnail conversion produced no image.");
              }
            } catch (...) {
              error_text = CurrentExceptionText("Unknown thumbnail conversion error.");
            }

            if (self) {
              QMetaObject::invokeMethod(
                  self,
                  [self, service, elementId, imageId, key, request_generation, request_id, thumb_url,
                   error_text]() {
                    if (!self) {
                      if (service) {
                        try {
                          service->ReleaseThumbnail(key);
                        } catch (...) {
                        }
                      }
                      return;
                    }
                    if (self->search_preview_generation_ != request_generation) {
                      if (service) {
                        try {
                          service->ReleaseThumbnail(key);
                        } catch (...) {
                        }
                      }
                      return;
                    }
                    const auto request_it = self->search_preview_thumbnail_requests_.find(key);
                    if (request_it == self->search_preview_thumbnail_requests_.end() ||
                        request_it->second != request_id) {
                      if (service) {
                        try {
                          service->ReleaseThumbnail(key);
                        } catch (...) {
                        }
                      }
                      return;
                    }
                    const auto visible_it = self->search_preview_visible_thumbnails_.find(key);
                    if (visible_it == self->search_preview_visible_thumbnails_.end() ||
                        visible_it->second != static_cast<image_id_t>(imageId)) {
                      self->search_preview_thumbnail_requests_.erase(key);
                      if (service) {
                        try {
                          service->ReleaseThumbnail(key);
                        } catch (...) {
                        }
                      }
                      return;
                    }
                    self->search_preview_thumbnail_requests_.erase(key);
                    if (thumb_url.isEmpty()) {
                      // No image to show: the pin is not kept.
                      if (service) {
                        try {
                          service->ReleaseThumbnail(key);
                        } catch (...) {
                        }
                      }
                    } else {
                      self->search_preview_pinned_thumbnails_[key] = thumb_url;
                    }
                    emit self->SearchPreviewThumbnailUpdated(elementId, thumb_url, false, false,
                                                             error_text);
                    emit self->searchPreviewThumbnailUpdated(elementId, thumb_url, false, false,
                                                             error_text);
                  },
                  Qt::QueuedConnection);
            } else if (service) {
              try {
                service->ReleaseThumbnail(key);
              } catch (...) {
              }
            }
          }).detach();
        },
        true, dispatcher, resolution);
  } catch (...) {
    search_preview_thumbnail_requests_.erase(key);
    emit SearchPreviewThumbnailUpdated(elementId, QString{}, false, false,
                                       CurrentExceptionText("Unknown thumbnail request error."));
    emit searchPreviewThumbnailUpdated(elementId, QString{}, false, false,
                                       CurrentExceptionText("Unknown thumbnail request error."));
  }
}

void SearchController::CancelSearchPreviewThumbnails() {
  // A request still on the way sees the new generation and releases its own pin.
  ++search_preview_generation_;
  if (search_preview_thumbnail_requests_.empty() && search_preview_visible_thumbnails_.empty() &&
      search_preview_pinned_thumbnails_.empty()) {
    return;
  }

  std::unordered_map<ThumbnailCacheKey, bool> store_keys;
  for (const auto& [key, image_id] : search_preview_visible_thumbnails_) {
    (void)image_id;
    store_keys.emplace(key, true);
  }
  for (const auto& [key, thumb_url] : search_preview_pinned_thumbnails_) {
    (void)thumb_url;
    store_keys.emplace(key, true);
  }
  auto pinned_keys = std::move(search_preview_pinned_thumbnails_);
  search_preview_pinned_thumbnails_.clear();
  search_preview_visible_thumbnails_.clear();
  search_preview_thumbnail_requests_.clear();

  if (library_) {
    for (const auto& [key, present] : store_keys) {
      (void)present;
      library_->thumbs().ReleaseStoreImageIfUnpinned(key);
    }
  }

  auto thumb_svc = project_ != nullptr ? project_->handler().thumbnail_service() : nullptr;
  if (!thumb_svc) {
    return;
  }
  for (const auto& [key, thumb_url] : pinned_keys) {
    (void)thumb_url;
    try {
      thumb_svc->ReleaseThumbnail(key);
    } catch (...) {
    }
  }
}

void SearchController::ClearSearchState(bool emitSignal) {
  pending_search_.reset();
  active_search_query_.clear();
  active_search_filter_node_.reset();
  accepted_semantic_query_.reset();
  if (emitSignal) {
    emit SearchStateChanged();
  }
}

}  // namespace alcedo::ui
