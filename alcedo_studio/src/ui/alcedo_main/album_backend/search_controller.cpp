//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/search_controller.hpp"

#include <QCoreApplication>
#include <QDate>
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

#include "app/thumbnail_service.hpp"
#include "app/search_query_classifier.hpp"
#include "image/image.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/i18n.hpp"

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

struct SearchTask {
  QString                              query;
  std::wstring                         query_w;
  int                                  offset = 0;
  int                                  limit  = 0;
  bool                                 submit = false;
  SearchQueryClassification            classification;
  std::string                          route_name;
  std::optional<sl_element_id_t>       folder_id;
  std::shared_ptr<SleeveFilterService> semantic_filter_service;
};

struct SearchCoreResult {
  QVariantMap                   response;
  std::vector<FuzzySearchMatch> matches;
};

auto ExecuteSearchTask(const SearchTask& task) -> SearchCoreResult {
  SearchCoreResult result{MakeEmptyResponse(task.offset, task.limit, task.route_name), {}};

  if (task.classification.route_ == SearchQueryRoute::Empty) {
    if (task.submit) {
      result.response["recommendations"] = true;
    }
    return result;
  }

  if (!task.submit && task.classification.route_ == SearchQueryRoute::Semantic) {
    result.response["awaitingSubmit"] = true;
    return result;
  }

  if (task.limit <= 0) {
    return result;
  }
  if (task.classification.route_ != SearchQueryRoute::Semantic) {
    result.response["semanticUnavailable"] = true;
    return result;
  }

  if (!task.folder_id.has_value()) {
    result.response["semanticUnavailable"] = true;
    return result;
  }

  if (task.classification.too_long_) {
    result.response["tooLong"] = true;
    return result;
  }
  if (!task.semantic_filter_service ||
      !task.semantic_filter_service->HasSemanticSearchProvider()) {
    result.response["semanticUnavailable"] = true;
    return result;
  }

  try {
    const auto safe_offset = static_cast<size_t>(std::max(0, task.offset));
    const auto safe_limit  = static_cast<size_t>(std::max(0, task.limit));
    result.matches = task.semantic_filter_service->SearchFolderSemantic(
        task.folder_id.value(), task.query_w, safe_offset, safe_limit);
    const bool has_more = safe_limit > 0 && result.matches.size() == safe_limit;
    const auto approximate_total =
        safe_offset + result.matches.size() + (has_more ? static_cast<size_t>(1) : 0U);
    result.response["offset"]  = std::max(0, task.offset);
    result.response["limit"]   = std::max(0, task.limit);
    result.response["total"]   = static_cast<int>(std::min<size_t>(
        approximate_total, static_cast<size_t>(std::numeric_limits<int>::max())));
    result.response["hasMore"] = has_more;
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
                                   FolderController* folders, StatsEngine* stats,
                                   QObject* parent)
    : QObject(parent), project_(project), library_(library), folders_(folders),
      stats_(stats) {
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
}

SearchController::~SearchController() { CancelSearchPreviewThumbnails(); }

bool SearchController::HasActiveSearchFilter() const {
  return active_search_filter_where_.has_value() && !active_search_filter_where_->empty();
}

auto SearchController::ActiveSearchFilterWhere() const -> const std::optional<std::wstring>& {
  return active_search_filter_where_;
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

auto SearchController::SearchPreview(const QString& query, int offset, int limit) -> QVariantMap {
  const QString trimmed = query.trimmed();
  const auto    classification =
      ClassifySearchQuery(trimmed.toStdWString(), natural_language_search_enabled_);
  const auto route_name = std::string(SearchQueryRouteName(classification.route_));

  // Semantic preview must not run on every keystroke. Typing only signals that
  // an explicit submit (Enter / Search button) is required.
  if (classification.route_ == SearchQueryRoute::Semantic) {
    auto response   = MakeEmptyResponse(offset, limit, route_name);
    response["awaitingSubmit"] = true;
    return response;
  }
  if (classification.route_ == SearchQueryRoute::Empty || limit <= 0) {
    return MakeEmptyResponse(offset, limit, route_name);
  }
  return RunTraditionalPreview(trimmed, offset, limit, route_name);
}

auto SearchController::RunTraditionalPreview(const QString& query, int offset, int limit,
                                             const std::string& route_name) -> QVariantMap {
  auto response = MakeEmptyResponse(offset, limit, route_name);
  QVariantList rows;
  const QString trimmed = query.trimmed();
  if (trimmed.isEmpty() || limit <= 0) {
    response["rows"] = rows;
    return response;
  }

  auto proj = project_->handler().project();
  if (!proj) {
    response["rows"] = rows;
    return response;
  }
  auto filter_service = proj->GetSleeveFilterService();
  if (!filter_service) {
    response["rows"] = rows;
    return response;
  }
  const auto folder_id = folders_->CurrentFolderElementId();
  if (!folder_id.has_value()) {
    response["rows"] = rows;
    return response;
  }

  try {
    const auto safe_offset = std::max(0, offset);
    const auto safe_limit  = std::max(0, limit);
    const auto field_mask  = BuildSearchFieldMask();
    const auto total       = filter_service->CountSearchResults(folder_id.value(),
                                                                trimmed.toStdWString(), field_mask);
    const auto matches     = filter_service->SearchFolder(
        folder_id.value(), trimmed.toStdWString(), static_cast<size_t>(safe_offset),
        static_cast<size_t>(safe_limit), field_mask);
    response["offset"]  = safe_offset;
    response["limit"]   = safe_limit;
    response["total"]   = static_cast<int>(std::min<size_t>(
        total, static_cast<size_t>(std::numeric_limits<int>::max())));
    response["hasMore"] = static_cast<size_t>(safe_offset) + matches.size() < total;
    rows                = BuildResultRows(matches);
  } catch (...) {
  }
  response["rows"] = rows;
  return response;
}

auto SearchController::BuildResultRows(const std::vector<alcedo::FuzzySearchMatch>& matches)
    -> QVariantList {
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(matches.size()));

  auto proj = project_->handler().project();
  for (const auto& match : matches) {
    QVariantMap row{{"elementId", static_cast<uint>(match.file_id_)},
                    {"fileId", static_cast<uint>(match.file_id_)},
                    {"imageId", static_cast<uint>(match.image_id_)},
                    {"fileName", QString::fromUtf8(match.file_name_.c_str())},
                    {"cameraModel", SEARCH_TEXT("Unknown").Render()},
                    {"lens", QString{}},
                    {"captureDate", QStringLiteral("--")},
                    {"rating", 0},
                    {"thumbUrl", QString{}},
                    {"thumbLoading", false},
                    {"thumbMissingSource", false},
                    {"thumbErrorText", QString{}}};

    if (const auto* item = library_->FindAlbumItem(match.file_id_); item != nullptr) {
      row["thumbUrl"]           = item->thumb_data_url;
      row["thumbLoading"]       = item->thumb_loading;
      row["thumbMissingSource"] = item->thumb_missing_source;
      row["thumbErrorText"]     = item->thumb_error_text;
    }

    if (proj) {
      try {
        proj->GetImagePoolService()->Read<void>(
            match.image_id_, [&row](std::shared_ptr<Image> image) {
              if (!image) {
                return;
              }
              if (!image->image_name_.empty()) {
                row["fileName"] = album_util::WStringToQString(image->image_name_);
              }
              const auto& exif = image->exif_display_;
              if (!exif.model_.empty()) {
                row["cameraModel"] = QString::fromUtf8(exif.model_.c_str());
              }
              row["lens"]              = QString::fromUtf8(exif.lens_.c_str());
              const QDate capture_date = album_util::DateFromExifString(exif.date_time_str_);
              if (capture_date.isValid()) {
                row["captureDate"] = capture_date.toString(QStringLiteral("yyyy-MM-dd"));
              }
              row["rating"] = exif.rating_;
            });
      } catch (...) {
      }
    }

    rows.push_back(std::move(row));
  }
  return rows;
}

auto SearchController::SubmitSearch(const QString& query, int offset, int limit) -> QVariantMap {
  const QString trimmed = query.trimmed();
  const auto    classification =
      ClassifySearchQuery(trimmed.toStdWString(), natural_language_search_enabled_);
  const auto route_name = std::string(SearchQueryRouteName(classification.route_));

  if (classification.route_ == SearchQueryRoute::Empty) {
    auto response      = MakeEmptyResponse(offset, limit, route_name);
    response["recommendations"] = true;
    return response;
  }
  // Label and Traditional routes use the ordinary SQL path.
  if (classification.route_ != SearchQueryRoute::Semantic) {
    return RunTraditionalPreview(trimmed, offset, limit, route_name);
  }

  // Semantic route: the only path that may reach the semantic provider. Guard
  // the prompt length before embedding, and surface a clean unavailable state
  // instead of silently falling back to a C++ vector scan.
  if (classification.too_long_) {
    auto response  = MakeEmptyResponse(offset, limit, route_name);
    response["tooLong"] = true;
    return response;
  }

  auto proj = project_->handler().project();
  auto filter_service = proj ? proj->GetSleeveFilterService() : nullptr;
  const auto folder_id = folders_->CurrentFolderElementId();
  if (!filter_service || !filter_service->HasSemanticSearchProvider() || !folder_id.has_value()) {
    auto response            = MakeEmptyResponse(offset, limit, route_name);
    response["semanticUnavailable"] = true;
    return response;
  }

  auto response = MakeEmptyResponse(offset, limit, route_name);
  QVariantList rows;
  try {
    const auto safe_offset = static_cast<size_t>(std::max(0, offset));
    const auto safe_limit  = static_cast<size_t>(std::max(0, limit));
    const auto matches     = filter_service->SearchFolderSemantic(
        folder_id.value(), trimmed.toStdWString(), safe_offset, safe_limit);
    const bool has_more = safe_limit > 0 && matches.size() == safe_limit;
    const auto approximate_total =
        safe_offset + matches.size() + (has_more ? static_cast<size_t>(1) : static_cast<size_t>(0));
    response["offset"]  = std::max(0, offset);
    response["limit"]   = std::max(0, limit);
    response["total"]   = static_cast<int>(std::min<size_t>(
        approximate_total, static_cast<size_t>(std::numeric_limits<int>::max())));
    response["hasMore"] = has_more;
    rows                = BuildResultRows(matches);
  } catch (const std::exception& e) {
    response["semanticUnavailable"] = true;
    response["semanticErrorText"]   = QString::fromUtf8(e.what());
  } catch (...) {
    response["semanticUnavailable"] = true;
  }
  response["rows"] = rows;
  return response;
}

auto SearchController::RequestSubmitSearch(const QString& query, int offset, int limit,
                                           const QString& mode) -> qulonglong {
  return RequestSearch(query, offset, limit, mode, true);
}

auto SearchController::RequestSearch(const QString& query, int offset, int limit,
                                     const QString& mode, bool submit) -> qulonglong {
  const auto request_id = static_cast<qulonglong>(++search_response_request_sequence_);
  const auto trimmed    = query.trimmed();
  const auto classification = ClassifySearchQuery(trimmed.toStdWString(), natural_language_search_enabled_);

  if (!submit || classification.route_ != SearchQueryRoute::Semantic) {
    auto response = submit ? SubmitSearch(trimmed, offset, limit)
                           : SearchPreview(trimmed, offset, limit);
    QMetaObject::invokeMethod(
        this,
        [this, request_id, mode, response = std::move(response)]() {
          emit SearchResponseReady(request_id, mode, response);
          emit searchResponseReady(request_id, mode, response);
        },
        Qt::QueuedConnection);
    return request_id;
  }

  auto       task       = SearchTask{
            .query          = trimmed,
            .query_w        = trimmed.toStdWString(),
            .offset         = offset,
            .limit          = limit,
            .submit         = submit,
            .classification = classification,
  };
  task.route_name = std::string(SearchQueryRouteName(task.classification.route_));

  if (auto project = project_->handler().project()) {
    task.semantic_filter_service = project->GetSleeveFilterService();
  }
  task.folder_id = folders_->CurrentFolderElementId();

  QPointer<SearchController> self(this);
  std::thread([self, request_id, mode, task = std::move(task)]() mutable {
    auto result = std::make_shared<SearchCoreResult>(ExecuteSearchTask(task));
    if (!self) {
      return;
    }
    QMetaObject::invokeMethod(
        self,
        [self, request_id, mode, result]() {
          if (!self) {
            return;
          }
          result->response["rows"] = self->BuildResultRows(result->matches);
          emit self->SearchResponseReady(request_id, mode, result->response);
          emit self->searchResponseReady(request_id, mode, result->response);
        },
        Qt::QueuedConnection);
  }).detach();

  return request_id;
}

auto SearchController::ClassifyQuery(const QString& query) const -> QString {
  const auto classification =
      ClassifySearchQuery(query.trimmed().toStdWString(), natural_language_search_enabled_);
  return QString::fromUtf8(SearchQueryRouteName(classification.route_).data(),
                           static_cast<int>(SearchQueryRouteName(classification.route_).size()));
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

  auto proj = project_->handler().project();
  if (!proj) {
    return;
  }
  auto filter_service = proj->GetSleeveFilterService();
  if (!filter_service) {
    return;
  }

  auto where = filter_service->BuildFuzzySearchWhere(trimmed.toStdWString(),
                                                       BuildSearchFieldMask());
  if (!where.has_value()) {
    ClearFuzzySearch();
    return;
  }

  active_search_query_        = trimmed;
  active_search_filter_where_ = std::move(where);
  stats_->ClearFilters();
  stats_->RebuildThumbnailView();
  stats_->RefreshStats();
  emit stats_->StatsFilterChanged();
  emit SearchStateChanged();
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

  active_search_query_ =
      SEARCH_TEXT("Image %1", QString::number(static_cast<qulonglong>(elementId))).Render();
  active_search_filter_where_ =
      filter_service->BuildExactFileWhere(static_cast<sl_element_id_t>(elementId));
  stats_->ClearFilters();
  stats_->RebuildThumbnailView();
  stats_->RefreshStats();
  emit stats_->StatsFilterChanged();
  emit SearchStateChanged();
}

void SearchController::ClearFuzzySearch() {
  if (active_search_query_.isEmpty() && !active_search_filter_where_.has_value()) {
    return;
  }
  ClearSearchState(true);
  stats_->RebuildThumbnailView();
  stats_->RefreshStats();
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
    search_preview_thumbnail_requests_.erase(key);

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
      item != nullptr && !item->thumb_data_url.isEmpty()) {
    emit SearchPreviewThumbnailUpdated(elementId, item->thumb_data_url, false,
                                       item->thumb_missing_source, item->thumb_error_text);
    emit searchPreviewThumbnailUpdated(elementId, item->thumb_data_url, false,
                                       item->thumb_missing_source, item->thumb_error_text);
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
                       request_id,
                       guard = std::move(result.guard)]() mutable {
            QString data_url;
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
                  data_url       = album_util::DataUrlFromImage(
                      image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
              }
              if (data_url.isEmpty()) {
                error_text = QObject::tr("Thumbnail conversion produced no image.");
              }
            } catch (...) {
              error_text = CurrentExceptionText("Unknown thumbnail conversion error.");
            }

            if (self) {
              QMetaObject::invokeMethod(
                  self,
                  [self, service, elementId, imageId, key, request_generation, request_id, data_url,
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
                    emit self->SearchPreviewThumbnailUpdated(elementId, data_url, false, false,
                                                             error_text);
                    emit self->searchPreviewThumbnailUpdated(elementId, data_url, false, false,
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
  ++search_preview_generation_;
  if (search_preview_thumbnail_requests_.empty() && search_preview_visible_thumbnails_.empty()) {
    return;
  }

  std::unordered_map<ThumbnailCacheKey, bool> keys_to_release;
  for (const auto& [key, image_id] : search_preview_visible_thumbnails_) {
    (void)image_id;
    keys_to_release.emplace(key, true);
  }
  for (const auto& [key, request_id] : search_preview_thumbnail_requests_) {
    (void)request_id;
    keys_to_release.emplace(key, true);
  }

  auto thumb_svc = project_->handler().thumbnail_service();
  search_preview_visible_thumbnails_.clear();
  search_preview_thumbnail_requests_.clear();
  if (!thumb_svc) {
    return;
  }

  for (const auto& [key, present] : keys_to_release) {
    (void)present;
    try {
      thumb_svc->ReleaseThumbnail(key);
    } catch (...) {
    }
  }
}

void SearchController::ClearSearchState(bool emitSignal) {
  active_search_query_.clear();
  active_search_filter_where_.reset();
  if (emitSignal) {
    emit SearchStateChanged();
  }
}

}  // namespace alcedo::ui
