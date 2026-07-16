//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/stats_engine.hpp"

#include <QLocale>
#include <QSettings>
#include <optional>
#include <string>
#include <vector>

#include "storage/controller/semantic/semantic_label_config.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {
auto EscapedSqlString(std::wstring value) -> std::wstring {
  for (size_t pos = 0; (pos = value.find(L'\'', pos)) != std::wstring::npos; pos += 2) {
    value.insert(pos, 1, L'\'');
  }
  return value;
}

auto EscapedUtf8SqlString(const std::string& value) -> std::wstring {
  return EscapedSqlString(QString::fromUtf8(value.c_str()).toStdWString());
}

auto ToStatsRows(const std::vector<alcedo::StatsBucket>& buckets, bool uppercase_labels = false,
                 bool semantic_labels = false) -> QVariantList {
  QString code =
      QSettings{}.value(QStringLiteral("ui/language"), QStringLiteral("system")).toString();
  if (code.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
    code = QLocale::system().bcp47Name();
  }
  const auto   label_language = code.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive)
                                    ? SemanticLabelLanguage::kChinese
                                    : SemanticLabelLanguage::kEnglish;
  QVariantList rows;
  rows.reserve(static_cast<qsizetype>(buckets.size()));
  for (const auto& bucket : buckets) {
    QString label =
        bucket.label_.empty() ? PL_TEXT("(unknown)").Render()
        : semantic_labels
            ? QString::fromUtf8(SemanticLabelDisplayText(bucket.label_, label_language).c_str())
            : QString::fromUtf8(bucket.label_.c_str());
    if (uppercase_labels) {
      label = label.toUpper();
    }
    rows.push_back(QVariantMap{{"label", label}, {"count", bucket.count_}});
  }
  return rows;
}

auto SearchCategoryLabel(const QString& category) -> QString {
  if (category == u"camera") {
    return PL_TEXT("Camera").Render();
  }
  if (category == u"date") {
    return PL_TEXT("Date").Render();
  }
  if (category == u"lens") {
    return PL_TEXT("Lens").Render();
  }
  if (category == u"label") {
    return PL_TEXT("Label").Render();
  }
  return PL_TEXT("Metadata").Render();
}

void AppendRecommendationRows(QVariantList& out, const QVariantList& buckets,
                              const QString& category, int limit) {
  for (const auto& bucket : buckets) {
    if (out.size() >= limit) {
      return;
    }
    const auto row   = bucket.toMap();
    const auto label = row.value(QStringLiteral("label")).toString();
    if (label.isEmpty() || label == PL_TEXT("(unknown)").Render()) {
      continue;
    }
    out.push_back(QVariantMap{{"category", category},
                              {"categoryLabel", SearchCategoryLabel(category)},
                              {"label", label},
                              {"query", label},
                              {"count", row.value(QStringLiteral("count")).toInt()}});
  }
}
}  // namespace

StatsEngine::StatsEngine(ProjectModule* project, LibraryModule* library,
                         FolderController* folders, QObject* parent)
    : QObject(parent), project_(project), library_(library), folders_(folders) {}

void StatsEngine::BindCollaborators(SearchController* search,
                                    SemanticGenerationController* semantic) {
  search_ = search;
  semantic_ = semantic;
}

void StatsEngine::ToggleStatsFilter(const QString& category, const QString& label) {
  ToggleFilter(category, label);
  RebuildThumbnailView();
  emit StatsFilterChanged();
}

void StatsEngine::ClearStatsFilter() {
  ClearFilters();
  RebuildThumbnailView();
  emit StatsFilterChanged();
}

void StatsEngine::RebuildThumbnailView() {
  library_->LoadThumbnailWindow(BuildStatsFilterWhere(), true);
}

bool StatsEngine::LoadMoreThumbnailView() {
  return library_->LoadThumbnailWindow(BuildStatsFilterWhere(), false);
}

void StatsEngine::RefreshStats() {
  auto proj = project_->handler().project();
  if (!proj) {
    date_stats_.clear();
    camera_stats_.clear();
    lens_stats_.clear();
    label_stats_.clear();
    rating_stats_.clear();
    total_photo_count_ = 0;
    emit StatsChanged();
    return;
  }

  auto filter_service = proj->GetSleeveFilterService();
  if (!filter_service) {
    emit StatsChanged();
    return;
  }

  try {
    const auto folder_id = folders_->CurrentFolderElementId();
    if (!folder_id.has_value()) {
      date_stats_.clear();
      camera_stats_.clear();
      lens_stats_.clear();
      label_stats_.clear();
      rating_stats_.clear();
      total_photo_count_ = 0;
      emit StatsChanged();
      return;
    }

    const auto& active_search_filter_where = search_->ActiveSearchFilterWhere();
    const auto  stats                      = filter_service->BuildFolderStats(
        folder_id.value(),
        active_search_filter_where.has_value()
                                  ? std::optional<FilterNode>{FilterNode{.type_      = FilterNode::Type::RawSQL,
                                                                         .op_        = FilterOp::AND,
                                                                         .children_  = {},
                                                                         .condition_ = std::nullopt,
                                                                         .raw_sql_   = active_search_filter_where}}
                                  : std::nullopt);
    total_photo_count_ = stats.total_photo_count_;
    date_stats_        = ToStatsRows(stats.date_stats_);
    camera_stats_      = ToStatsRows(stats.camera_stats_);
    lens_stats_        = ToStatsRows(stats.lens_stats_);
    label_stats_       = ToStatsRows(stats.label_stats_, true, true);
    rating_stats_      = ToStatsRows(stats.rating_stats_);
  } catch (...) {
    // Keep previous stats if service query failed.
  }

  emit StatsChanged();
}

auto StatsEngine::FormatPhotoInfo(int shown, int total) const -> QString {
  if (total <= 0) {
    return PL_TEXT("No images loaded.").Render();
  }
  if (shown == total) {
    return PL_TEXT("Showing %1 images", total).Render();
  }
  return PL_TEXT("Showing %1 of %2", shown, total).Render();
}

auto StatsEngine::MakeThumbMap(const AlbumItem& image, int index) const -> QVariantMap {
  const QString aperture = image.aperture > 0.0 ? QString::number(image.aperture, 'f', 1) : "--";
  const QString focal =
      image.focal_length > 0.0 ? QString::number(image.focal_length, 'f', 0) : "--";

  return QVariantMap{
      {"elementId", static_cast<uint>(image.element_id)},
      {"fileId", static_cast<uint>(image.file_id)},
      {"imageId", static_cast<uint>(image.image_id)},
      {"folderId", static_cast<uint>(image.folder_id)},
      {"scopeType", image.scope_type},
      {"fileName", image.file_name.isEmpty() ? PL_TEXT("(unnamed)").Render() : image.file_name},
      {"cameraModel",
       image.camera_model.isEmpty() ? PL_TEXT("Unknown").Render() : image.camera_model},
      {"extension", image.extension.isEmpty() ? "--" : image.extension},
      {"iso", image.iso},
      {"aperture", aperture},
      {"focalLength", focal},
      {"captureDate",
       image.capture_date.isValid() ? image.capture_date.toString("yyyy-MM-dd") : "--"},
      {"rating", image.rating},
      {"tags", image.tags},
      {"accent", image.accent.isEmpty() ? album_util::AccentForIndex(static_cast<size_t>(index))
                                        : image.accent},
      {"thumbUrl", image.thumb_data_url},
      {"thumbLoading", image.thumb_loading},
      {"thumbMissingSource", image.thumb_missing_source},
      {"thumbErrorText", image.thumb_error_text}};
}

auto StatsEngine::BuildSearchRecommendations(int limit) const -> QVariantList {
  QVariantList rows;
  if (limit <= 0) {
    return rows;
  }
  rows.reserve(limit);
  AppendRecommendationRows(rows, camera_stats_, QStringLiteral("camera"), limit);
  AppendRecommendationRows(rows, date_stats_, QStringLiteral("date"), limit);
  AppendRecommendationRows(rows, lens_stats_, QStringLiteral("lens"), limit);
  AppendRecommendationRows(rows, label_stats_, QStringLiteral("label"), limit);
  return rows;
}

void StatsEngine::ToggleFilter(const QString& category, const QString& label) {
  if (category == u"date") {
    filter_date_ = (filter_date_ == label) ? QString{} : label;
  } else if (category == u"camera") {
    filter_camera_ = (filter_camera_ == label) ? QString{} : label;
  } else if (category == u"lens") {
    filter_lens_ = (filter_lens_ == label) ? QString{} : label;
  } else if (category == u"label") {
    filter_label_ = (filter_label_ == label) ? QString{} : label;
  } else if (category == u"rating") {
    filter_rating_ = (filter_rating_ == label) ? QString{} : label;
  }
}

void StatsEngine::ClearFilters() {
  filter_date_.clear();
  filter_camera_.clear();
  filter_lens_.clear();
  filter_label_.clear();
  filter_rating_.clear();
}

bool StatsEngine::HasActiveFilter() const {
  return !filter_date_.isEmpty() || !filter_camera_.isEmpty() || !filter_lens_.isEmpty() ||
         !filter_label_.isEmpty() || !filter_rating_.isEmpty();
}

auto StatsEngine::BuildStatsFilterWhere() const -> std::optional<std::wstring> {
  std::vector<std::wstring> conditions;

  if (!filter_date_.isEmpty()) {
    const auto is_unknown = (filter_date_ == PL_TEXT("(unknown)").Render());
    if (is_unknown) {
      conditions.push_back(
          L"(json_extract(i.metadata, '$.DateTimeString') IS NULL "
          L"OR json_extract(i.metadata, '$.DateTimeString') = '')");
    } else {
      const auto date_str = filter_date_.toStdWString();
      conditions.push_back(
          L"CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::VARCHAR = '" + date_str +
          L"'");
    }
  }

  if (!filter_camera_.isEmpty()) {
    const auto cam_str = filter_camera_.toStdWString();
    conditions.push_back(
        L"COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = '" +
        cam_str + L"'");
  }

  if (!filter_lens_.isEmpty()) {
    const auto lens_str = filter_lens_.toStdWString();
    conditions.push_back(
        L"COALESCE(NULLIF(json_extract_string(i.metadata, '$.Lens'), ''), '(unknown)') = '" +
        lens_str + L"'");
  }

  if (!filter_label_.isEmpty()) {
    const auto active_model_key = semantic_ ? semantic_->ActiveModelKey() : std::string{};
    if (active_model_key.empty()) {
      conditions.push_back(L"(1 = 0)");
    } else {
      const auto   aliases = SemanticLabelAliases(filter_label_.toUtf8().toStdString());
      std::wstring label_match;
      for (size_t i = 0; i < aliases.size(); ++i) {
        if (i > 0) {
          label_match += L" OR ";
        }
        label_match += L"LOWER(sl.label) = LOWER('" + EscapedUtf8SqlString(aliases[i]) + L"')";
      }
      conditions.push_back(
          L"EXISTS (SELECT 1 FROM SemanticImageLabel sl WHERE sl.file_id = e.id "
          L"AND sl.model_key = '" +
          EscapedUtf8SqlString(active_model_key) + L"' AND (" + label_match + L"))");
    }
  }

  if (!filter_rating_.isEmpty()) {
    bool ok  = false;
    int  val = filter_rating_.toInt(&ok);
    if (ok) {
      conditions.push_back(L"json_extract(i.metadata, '$.Rating')::INT = " + std::to_wstring(val));
    } else {
      conditions.push_back(L"json_extract(i.metadata, '$.Rating') IS NULL");
    }
  }

  if (conditions.empty()) return std::nullopt;

  std::wstring where;
  for (size_t i = 0; i < conditions.size(); ++i) {
    if (i > 0) where += L" AND ";
    where += conditions[i];
  }
  return where;
}

bool StatsEngine::MatchesActiveFilters(const AlbumItem& image) const {
  if (!HasActiveFilter()) return true;

  if (!filter_date_.isEmpty()) {
    const QString imageDate =
        image.capture_date.isValid() ? image.capture_date.toString("yyyy-MM-dd") : QString{};
    if (filter_date_ == PL_TEXT("(unknown)").Render()) {
      if (image.capture_date.isValid()) return false;
    } else {
      if (imageDate != filter_date_) return false;
    }
  }

  if (!filter_camera_.isEmpty()) {
    if (filter_camera_ == PL_TEXT("(unknown)").Render()) {
      if (!image.camera_model.isEmpty()) return false;
    } else {
      if (image.camera_model != filter_camera_) return false;
    }
  }

  if (!filter_lens_.isEmpty()) {
    if (filter_lens_ == PL_TEXT("(unknown)").Render()) {
      if (!image.lens.isEmpty()) return false;
    } else {
      if (image.lens != filter_lens_) return false;
    }
  }

  if (!filter_label_.isEmpty()) {
    if (!image.tags.contains(filter_label_, Qt::CaseInsensitive)) return false;
  }

  if (!filter_rating_.isEmpty()) {
    bool ok        = false;
    int  filterVal = filter_rating_.toInt(&ok);
    if (ok && image.rating != filterVal) return false;
  }

  return true;
}

}  // namespace alcedo::ui

#undef PL_TEXT
