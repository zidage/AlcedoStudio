//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/image_controller.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <json.hpp>
#include <numeric>
#include <optional>
#include <unordered_set>
#include <utility>

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"
#include "image/image.hpp"
#include "sleeve/storage.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/editor_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

namespace {
using json                                                      = nlohmann::json;

// Translation registry for image-detail dialog strings. Listed via
// QT_TRANSLATE_NOOP so lupdate registers them under the "Alcedo" context;
// the call sites below resolve them at runtime via Tr().
[[maybe_unused]] constexpr auto kImageDetailsTranslationSources = std::to_array<const char*>({
    QT_TRANSLATE_NOOP("Alcedo", "(unnamed)"),
    QT_TRANSLATE_NOOP("Alcedo", "Capture"),
    QT_TRANSLATE_NOOP("Alcedo", "Gear"),
    QT_TRANSLATE_NOOP("Alcedo", "Exposure"),
    QT_TRANSLATE_NOOP("Alcedo", "Storage"),
    QT_TRANSLATE_NOOP("Alcedo", "Original Size"),
    QT_TRANSLATE_NOOP("Alcedo", "Original Aspect Ratio"),
    QT_TRANSLATE_NOOP("Alcedo", "Captured At"),
    QT_TRANSLATE_NOOP("Alcedo", "Camera Brand"),
    QT_TRANSLATE_NOOP("Alcedo", "Camera Model"),
    QT_TRANSLATE_NOOP("Alcedo", "Lens Brand"),
    QT_TRANSLATE_NOOP("Alcedo", "Lens Model"),
    QT_TRANSLATE_NOOP("Alcedo", "Aperture"),
    QT_TRANSLATE_NOOP("Alcedo", "Shutter"),
    QT_TRANSLATE_NOOP("Alcedo", "ISO"),
    QT_TRANSLATE_NOOP("Alcedo", "Focal Length"),
    QT_TRANSLATE_NOOP("Alcedo", "35mm Equivalent"),
    QT_TRANSLATE_NOOP("Alcedo", "Focus Distance"),
    QT_TRANSLATE_NOOP("Alcedo", "Rating"),
    QT_TRANSLATE_NOOP("Alcedo", "Source Directory"),
    QT_TRANSLATE_NOOP("Alcedo", "Open in file manager"),
    QT_TRANSLATE_NOOP("Alcedo", "Camera"),
    QT_TRANSLATE_NOOP("Alcedo", "Lens"),
    QT_TRANSLATE_NOOP("Alcedo", "Aperture / Shutter"),
    QT_TRANSLATE_NOOP("Alcedo", "Description"),
    QT_TRANSLATE_NOOP("Alcedo", "Rating Reason"),
    QT_TRANSLATE_NOOP("Alcedo", "No AI description yet"),
    QT_TRANSLATE_NOOP("Alcedo", "No rating reason yet"),
    QT_TRANSLATE_NOOP("Alcedo", "Manual edit"),
    QT_TRANSLATE_NOOP("Alcedo", "Description cannot be empty."),
    QT_TRANSLATE_NOOP("Alcedo", "Rating reason cannot be empty."),
    QT_TRANSLATE_NOOP("Alcedo", "Description saved."),
    QT_TRANSLATE_NOOP("Alcedo", "Rating reason saved."),
    QT_TRANSLATE_NOOP("Alcedo", "Failed to save image description."),
    QT_TRANSLATE_NOOP("Alcedo", "Failed to save rating reason."),
});

constexpr const char* kManualAnnotationIdentity = "manual";
constexpr const char* kManualAnnotationModel    = "user";
constexpr const char* kDescribeTaskId           = "describe";
constexpr const char* kScoreTaskId              = "rate";

auto ToVariantIdList(const std::vector<sl_element_id_t>& ids) -> QVariantList {
  QVariantList out;
  out.reserve(static_cast<qsizetype>(ids.size()));
  for (const auto id : ids) {
    out.push_back(static_cast<uint>(id));
  }
  return out;
}

auto DashValue() -> QString { return QString::fromUtf8("\u2014"); }

auto ToDisplayText(const std::string& value) -> QString {
  const QString text = QString::fromUtf8(value.c_str()).trimmed();
  return text.isEmpty() ? DashValue() : text;
}

auto ToOptionalDisplayText(const std::string& value) -> QString {
  return QString::fromUtf8(value.c_str()).trimmed();
}

auto DashIfEmpty(const QString& value) -> QString {
  const QString trimmed = value.trimmed();
  return trimmed.isEmpty() ? DashValue() : trimmed;
}

auto PathToGenericText(const std::filesystem::path& path) -> QString {
  QString text = album_util::PathToQString(path);
  text.replace('\\', '/');
  return text;
}

auto FormatUnsigned(uint64_t value) -> QString {
  return value > 0 ? QString::number(value) : DashValue();
}

auto FormatFixed(double value, int precision, const QString& prefix = QString{},
                 const QString& suffix = QString{}) -> QString {
  if (!std::isfinite(value) || value <= 0.0) {
    return DashValue();
  }
  return prefix + QString::number(value, 'f', precision) + suffix;
}

auto FormatRating(int value) -> QString {
  return value > 0 ? QStringLiteral("%1/5").arg(value) : DashValue();
}

auto JsonNumberOrZero(const json& metadata, const char* key) -> double {
  if (!metadata.contains(key)) {
    return 0.0;
  }
  const auto& value = metadata.at(key);
  return value.is_number() ? value.get<double>() : 0.0;
}

auto JsonUnsignedOrZero(const json& metadata, const char* key) -> uint32_t {
  if (!metadata.contains(key)) {
    return 0;
  }
  const auto& value = metadata.at(key);
  return value.is_number_unsigned() ? value.get<uint32_t>()
         : value.is_number_integer()
             ? static_cast<uint32_t>(std::max<int64_t>(value.get<int64_t>(), 0))
             : 0;
}

auto JsonStringOrEmpty(const json& metadata, const char* key) -> std::string {
  if (!metadata.contains(key)) {
    return {};
  }
  const auto& value = metadata.at(key);
  return value.is_string() ? value.get<std::string>() : std::string{};
}

auto FormatAspectRatio(uint32_t width, uint32_t height) -> QString {
  if (width == 0 || height == 0) {
    return DashValue();
  }

  const double longer_edge  = static_cast<double>(std::max(width, height));
  const double shorter_edge = static_cast<double>(std::min(width, height));
  if (shorter_edge <= 0.0) {
    return DashValue();
  }

  const double normalized_ratio = longer_edge / shorter_edge;

  struct CommonAspectRatio {
    double      ratio;
    const char* label;
  };
  constexpr std::array<CommonAspectRatio, 4> kCommonAspectRatios = {
      CommonAspectRatio{3.0 / 2.0, "3:2"},
      CommonAspectRatio{1.85, "1.85:1"},
      CommonAspectRatio{1.79, "1.79:1"},
      CommonAspectRatio{4.0 / 3.0, "4:3"},
  };
  constexpr double kCommonRatioTolerance = 0.03;

  const auto       nearest               = std::min_element(
      kCommonAspectRatios.begin(), kCommonAspectRatios.end(),
      [normalized_ratio](const CommonAspectRatio& lhs, const CommonAspectRatio& rhs) {
        return std::abs(normalized_ratio - lhs.ratio) < std::abs(normalized_ratio - rhs.ratio);
      });
  if (nearest != kCommonAspectRatios.end() &&
      std::abs(normalized_ratio - nearest->ratio) <= kCommonRatioTolerance) {
    return QString::fromLatin1(nearest->label);
  }

  return QStringLiteral("%1:1").arg(QString::number(normalized_ratio, 'f', 2));
}

auto FormatDimensions(uint32_t width, uint32_t height) -> QString {
  if (width == 0 || height == 0) {
    return DashValue();
  }
  return QStringLiteral("%1 × %2 px").arg(width).arg(height);
}

auto FormatShutterSpeed(const json& metadata) -> QString {
  if (!metadata.contains("ShutterSpeed")) {
    return DashValue();
  }
  const auto& value = metadata.at("ShutterSpeed");
  if (!value.is_array() || value.size() < 2 || !value[0].is_number_integer() ||
      !value[1].is_number_integer()) {
    return DashValue();
  }

  const int64_t numerator   = value[0].get<int64_t>();
  const int64_t denominator = value[1].get<int64_t>();
  if (numerator <= 0 || denominator <= 0) {
    return DashValue();
  }
  if (denominator == 1) {
    return QStringLiteral("%1 s").arg(numerator);
  }
  return QStringLiteral("%1/%2 s").arg(numerator).arg(denominator);
}

auto MakeDetailsRow(const QString& section, const QString& label, const QString& value,
                    bool emphasized = false, const QString& actionId = QString{},
                    const QString& actionValue   = QString{},
                    const QString& actionTooltip = QString{}) -> QVariantMap {
  return QVariantMap{{"section", section},
                     {"label", label},
                     {"value", value},
                     {"emphasized", emphasized},
                     {"actionId", actionId},
                     {"actionValue", actionValue},
                     {"actionTooltip", actionTooltip}};
}

auto MakeInspectionTile(const QString& id, const QString& label, const QString& value,
                        const QString& detail = QString{}, bool editable = false)
    -> QVariantMap {
  return QVariantMap{{"id", id},
                     {"label", label},
                     {"value", value},
                     {"detail", detail},
                     {"editable", editable}};
}

void AppendDetailsRow(QVariantList& rows, const QString& section, const QString& label,
                      const QString& value, bool emphasized = false,
                      const QString& actionId = QString{}, const QString& actionValue = QString{},
                      const QString& actionTooltip = QString{}) {
  rows.push_back(
      MakeDetailsRow(section, label, value, emphasized, actionId, actionValue, actionTooltip));
}

struct SourceDirectoryInfo {
  QString displayText = DashValue();
  QString pathText{};
  bool    canOpen = false;
};

auto ResolveSourceDirectory(const AlbumItem* item, const std::shared_ptr<Image>& image)
    -> SourceDirectoryInfo {
  (void)item;
  std::filesystem::path source_path;
  if (image && !image->image_path_.empty()) {
    source_path = image->image_path_;
  }
  if (source_path.empty()) {
    return {};
  }

  const std::filesystem::path directory = source_path.parent_path();
  if (directory.empty()) {
    return {};
  }

  const QString pathText = PathToGenericText(directory);
  if (pathText.trimmed().isEmpty()) {
    return {};
  }

  return SourceDirectoryInfo{pathText, pathText, true};
}

auto ComposeSubtitle(const json& metadata) -> QString {
  const QString camera = ToOptionalDisplayText(JsonStringOrEmpty(metadata, "Model"));
  const QString lens   = ToOptionalDisplayText(JsonStringOrEmpty(metadata, "Lens"));

  QStringList   parts;
  if (!camera.isEmpty()) {
    parts.push_back(camera);
  }
  if (!lens.isEmpty()) {
    parts.push_back(lens);
  }
  return parts.join(QStringLiteral(" · "));
}

auto ComposeModelIdentity(const QString& provider, const QString& model_id) -> QString {
  QStringList parts;
  if (!provider.trimmed().isEmpty()) {
    parts.push_back(provider.trimmed());
  }
  if (!model_id.trimmed().isEmpty()) {
    parts.push_back(model_id.trimmed());
  }
  return parts.join(QStringLiteral(" · "));
}

auto ResolveTitle(const AlbumItem* item, const std::shared_ptr<Image>& image) -> QString {
  if (item && !item->file_name.trimmed().isEmpty()) {
    return item->file_name.trimmed();
  }
  if (image && !image->image_name_.empty()) {
    const QString from_image = album_util::WStringToQString(image->image_name_).trimmed();
    if (!from_image.isEmpty()) {
      return from_image;
    }
  }
  return Tr("(unnamed)");
}

auto ParseExifDisplayJson(const std::shared_ptr<Image>& image) -> json {
  if (!image) {
    return json::object();
  }
  try {
    const std::string exif_text = image->ExifToJson();
    if (exif_text.empty()) {
      return json::object();
    }
    const json parsed = json::parse(exif_text, nullptr, false);
    return parsed.is_discarded() ? json::object() : parsed;
  } catch (...) {
    return json::object();
  }
}

auto BuildDetailsResult(const AlbumItem* item, const std::shared_ptr<Image>& image,
                        const QString& semantic_tags) -> QVariantMap {
  const json                metadata         = ParseExifDisplayJson(image);
  const QString             section_capture  = Tr("Capture");
  const QString             section_gear     = Tr("Gear");
  const QString             section_exposure = Tr("Exposure");
  const QString             section_storage  = Tr("Storage");
  const uint32_t            width            = JsonUnsignedOrZero(metadata, "ImageWidth");
  const uint32_t            height           = JsonUnsignedOrZero(metadata, "ImageHeight");
  const SourceDirectoryInfo source_directory = ResolveSourceDirectory(item, image);

  QVariantList              rows;
  rows.reserve(15);

  AppendDetailsRow(rows, section_capture, Tr("Original Size"), FormatDimensions(width, height),
                   true);
  AppendDetailsRow(rows, section_capture, Tr("Original Aspect Ratio"),
                   FormatAspectRatio(width, height));
  AppendDetailsRow(rows, section_capture, Tr("Captured At"),
                   ToDisplayText(JsonStringOrEmpty(metadata, "DateTimeString")));

  AppendDetailsRow(rows, section_gear, Tr("Camera Brand"),
                   ToDisplayText(JsonStringOrEmpty(metadata, "Make")));
  AppendDetailsRow(rows, section_gear, Tr("Camera Model"),
                   ToDisplayText(JsonStringOrEmpty(metadata, "Model")), true);
  AppendDetailsRow(rows, section_gear, Tr("Lens Brand"),
                   ToDisplayText(JsonStringOrEmpty(metadata, "LensMake")));
  AppendDetailsRow(rows, section_gear, Tr("Lens Model"),
                   ToDisplayText(JsonStringOrEmpty(metadata, "Lens")), true);

  AppendDetailsRow(rows, section_exposure, Tr("Aperture"),
                   FormatFixed(JsonNumberOrZero(metadata, "Aperture"), 1, "f/"));
  AppendDetailsRow(rows, section_exposure, Tr("Shutter"), FormatShutterSpeed(metadata));
  AppendDetailsRow(rows, section_exposure, Tr("ISO"),
                   FormatUnsigned(JsonUnsignedOrZero(metadata, "ISO")));
  AppendDetailsRow(rows, section_exposure, Tr("Focal Length"),
                   FormatFixed(JsonNumberOrZero(metadata, "FocalLength"), 0, QString{}, " mm"));
  AppendDetailsRow(rows, section_exposure, Tr("35mm Equivalent"),
                   FormatFixed(JsonNumberOrZero(metadata, "FocalLength35mm"), 0, QString{}, " mm"));
  AppendDetailsRow(rows, section_exposure, Tr("Focus Distance"),
                   FormatFixed(JsonNumberOrZero(metadata, "FocusDistanceM"), 2, QString{}, " m"));
  AppendDetailsRow(rows, section_exposure, Tr("Rating"),
                   FormatRating(static_cast<int>(JsonUnsignedOrZero(metadata, "Rating"))));
  AppendDetailsRow(rows, section_storage, Tr("Source Directory"), source_directory.displayText,
                   false, source_directory.canOpen ? QStringLiteral("open-directory") : QString{},
                   source_directory.pathText,
                   source_directory.canOpen ? Tr("Open in file manager") : QString{});

  return QVariantMap{{"success", true},
                     {"message", QString{}},
                     {"title", ResolveTitle(item, image)},
                     {"subtitle", ComposeSubtitle(metadata)},
                     {"semanticTags", semantic_tags},
                     {"rows", rows}};
}

auto BuildInspectionResult(const AlbumItem* item, const std::shared_ptr<Image>& image,
                           sl_element_id_t file_id, image_id_t image_id,
                           const QString& semantic_tags,
                           const std::optional<AiDescription>& description,
                           const std::optional<AiRating>& rating_reason) -> QVariantMap {
  const json                metadata         = ParseExifDisplayJson(image);
  const uint32_t            width            = JsonUnsignedOrZero(metadata, "ImageWidth");
  const uint32_t            height           = JsonUnsignedOrZero(metadata, "ImageHeight");
  const SourceDirectoryInfo source_directory = ResolveSourceDirectory(item, image);

  const QString camera_make  = ToOptionalDisplayText(JsonStringOrEmpty(metadata, "Make"));
  const QString camera_model = ToOptionalDisplayText(JsonStringOrEmpty(metadata, "Model"));
  const QString lens_make    = ToOptionalDisplayText(JsonStringOrEmpty(metadata, "LensMake"));
  const QString lens_model   = ToOptionalDisplayText(JsonStringOrEmpty(metadata, "Lens"));
  const QString aperture     = FormatFixed(JsonNumberOrZero(metadata, "Aperture"), 1, "f/");
  const QString shutter      = FormatShutterSpeed(metadata);
  const QString iso          = FormatUnsigned(JsonUnsignedOrZero(metadata, "ISO"));
  const QString focal =
      FormatFixed(JsonNumberOrZero(metadata, "FocalLength"), 0, QString{}, "mm");
  const int     rating = static_cast<int>(JsonUnsignedOrZero(metadata, "Rating"));
  QString       lens_value = DashIfEmpty(lens_model);
  if (focal != DashValue()) {
    lens_value = lens_value == DashValue() ? focal : QStringLiteral("%1@%2").arg(lens_value, focal);
  }

  const QString caption =
      description.has_value() ? QString::fromStdString(description->caption_).trimmed() : QString{};
  const QString scene =
      description.has_value() ? QString::fromStdString(description->scene_).trimmed() : QString{};
  const QString description_identity =
      description.has_value()
          ? ComposeModelIdentity(QString::fromStdString(description->provider_id_),
                                 QString::fromStdString(description->model_id_))
          : QString{};

  const QString reasons =
      rating_reason.has_value() ? QString::fromStdString(rating_reason->reasons_).trimmed()
                                : QString{};
  const QString reason_identity =
      rating_reason.has_value()
          ? ComposeModelIdentity(QString::fromStdString(rating_reason->provider_id_),
                                 QString::fromStdString(rating_reason->model_id_))
          : QString{};

  QVariantList tiles;
  tiles.reserve(6);
  tiles.push_back(MakeInspectionTile(QStringLiteral("camera"), Tr("Camera"),
                                     DashIfEmpty(camera_model), DashIfEmpty(camera_make)));
  tiles.push_back(MakeInspectionTile(QStringLiteral("lens"), Tr("Lens"), lens_value,
                                     DashIfEmpty(lens_make)));
  tiles.push_back(MakeInspectionTile(
      QStringLiteral("exposure"), Tr("Aperture / Shutter"),
      QStringLiteral("%1 · %2").arg(aperture, shutter)));
  tiles.push_back(MakeInspectionTile(QStringLiteral("iso"), Tr("ISO"), iso));
  tiles.push_back(MakeInspectionTile(QStringLiteral("description"), Tr("Description"),
                                     caption.isEmpty() ? Tr("No AI description yet") : caption,
                                     scene.isEmpty() ? description_identity : scene, true));
  tiles.push_back(MakeInspectionTile(QStringLiteral("rating"), Tr("Rating"),
                                     FormatRating(rating),
                                     reasons.isEmpty() ? Tr("No rating reason yet") : reasons, true));

  return QVariantMap{{"success", true},
                     {"message", QString{}},
                     {"elementId", static_cast<uint>(file_id)},
                     {"fileId", static_cast<uint>(file_id)},
                     {"imageId", static_cast<uint>(image_id)},
                     {"title", ResolveTitle(item, image)},
                     {"subtitle", ComposeSubtitle(metadata)},
                     {"semanticTags", semantic_tags},
                     {"dimensions", FormatDimensions(width, height)},
                     {"aspectRatio", FormatAspectRatio(width, height)},
                     {"capturedAt", ToDisplayText(JsonStringOrEmpty(metadata, "DateTimeString"))},
                     {"sourceDirectory", source_directory.displayText},
                     {"sourceDirectoryPath", source_directory.pathText},
                     {"sourceDirectoryCanOpen", source_directory.canOpen},
                     {"rating", rating},
                     {"description", caption},
                     {"descriptionScene", scene},
                     {"descriptionProvider", description.has_value()
                                                ? QString::fromStdString(description->provider_id_)
                                                : QString{}},
                     {"descriptionModelId", description.has_value()
                                               ? QString::fromStdString(description->model_id_)
                                               : QString{}},
                     {"ratingReason", reasons},
                     {"ratingReasonProvider", rating_reason.has_value()
                                                  ? QString::fromStdString(rating_reason->provider_id_)
                                                  : QString{}},
                     {"ratingReasonModelId", rating_reason.has_value()
                                                 ? QString::fromStdString(rating_reason->model_id_)
                                                 : QString{}},
                     {"tiles", tiles}};
}

void FillManualDescriptionIdentity(AiDescription& description) {
  if (description.task_id_.empty()) description.task_id_ = kDescribeTaskId;
  if (description.provider_id_.empty()) description.provider_id_ = kManualAnnotationIdentity;
  if (description.model_id_.empty()) description.model_id_ = kManualAnnotationModel;
  if (description.prompt_profile_id_.empty()) {
    description.prompt_profile_id_ = kManualAnnotationIdentity;
  }
  if (description.rendition_kind_.empty()) description.rendition_kind_ = kManualAnnotationIdentity;
}

void FillManualRatingIdentity(AiRating& rating) {
  if (rating.task_id_.empty()) rating.task_id_ = kScoreTaskId;
  if (rating.provider_id_.empty()) rating.provider_id_ = kManualAnnotationIdentity;
  if (rating.model_id_.empty()) rating.model_id_ = kManualAnnotationModel;
  if (rating.prompt_profile_id_.empty()) rating.prompt_profile_id_ = kManualAnnotationIdentity;
  if (rating.rendition_kind_.empty()) rating.rendition_kind_ = kManualAnnotationIdentity;
  if (rating.rubric_id_.empty()) rating.rubric_id_ = kManualAnnotationIdentity;
  if (rating.rubric_version_.empty()) rating.rubric_version_ = kManualAnnotationIdentity;
}
}  // namespace

ImageController::ImageController(ProjectModule* project, LibraryModule* library,
                                 FolderController* folders, IUiStatusSink* status,
                                 QObject* parent)
    : QObject(parent), project_(project), library_(library), folders_(folders),
      status_(status) {}

void ImageController::BindCollaborators(StatsEngine* stats, ImportExportHandler* import_export,
                                        EditorController* editor,
                                        SemanticGenerationController* semantic,
                                        InteractionPolicyController* policy) {
  stats_ = stats;
  import_export_ = import_export;
  editor_ = editor;
  semantic_ = semantic;
  policy_ = policy;
}

auto ImageController::SaveProjectSnapshot() -> bool {
  bool save_ok = true;
  try {
    if (!project_->handler().meta_path().empty()) {
      project_->handler().project()->SaveProject(project_->handler().meta_path());
    }
    QString ignored_error;
    if (!project_->handler().PackageCurrentProjectFiles(&ignored_error)) {
      save_ok = false;
    }
  } catch (...) {
    save_ok = false;
  }
  return save_ok;
}

auto ImageController::CollectDeleteTargets(const QVariantList& targetEntries) const
    -> std::vector<DeleteTarget> {
  std::vector<DeleteTarget> targets;
  targets.reserve(static_cast<size_t>(targetEntries.size()));

  std::unordered_set<sl_element_id_t> seen_element_ids;
  seen_element_ids.reserve(static_cast<size_t>(targetEntries.size()) * 2 + 1);

  for (const QVariant& row_var : targetEntries) {
    const QVariantMap row        = row_var.toMap();
    const auto        element_id = static_cast<sl_element_id_t>(row.value("elementId").toUInt());
    if (element_id == 0 || !seen_element_ids.insert(element_id).second) {
      continue;
    }

    DeleteTarget target;
    target.element_id_ = element_id;
    target.image_id_   = static_cast<image_id_t>(row.value("imageId").toUInt());
    target.folder_id_  = static_cast<sl_element_id_t>(row.value("folderId").toUInt());

    if (const auto* item = library_->FindAlbumItem(element_id); item) {
      if (target.image_id_ == 0) {
        target.image_id_ = item->image_id;
      }
      if (target.folder_id_ == 0) {
        target.folder_id_ = item->folder_id;
      }
      target.file_path_ = item->file_path_;
    }

    targets.push_back(target);
  }

  return targets;
}

auto ImageController::ResolveRatingTarget(uint elementId, uint imageId) const -> RatingTarget {
  RatingTarget target;
  target.element_id_ = static_cast<sl_element_id_t>(elementId);
  target.image_id_   = static_cast<image_id_t>(imageId);

  if (target.element_id_ != 0) {
    if (const auto* item = library_->FindAlbumItem(target.element_id_); item) {
      if (target.image_id_ == 0) {
        target.image_id_ = item->image_id;
      }
    }
  }

  return target;
}

auto ImageController::DeleteImages(const QVariantList& targetEntries) -> QVariantMap {
  if (policy_) {
    const QVariantMap policy = policy_->EvaluateDeleteImages(targetEntries);
    if (!policy.value(QStringLiteral("allowed")).toBool()) {
      QString reason = policy.value(QStringLiteral("reason")).toString();
      if (reason.isEmpty()) {
        reason = PL_TEXT("These images cannot be deleted right now.").Render();
      }
      if (status_) {
        status_->SetTaskState(PL_TEXT("%1", reason), 0, false);
      }
      return QVariantMap{{QStringLiteral("success"), false},
                         {QStringLiteral("deletedCount"), 0},
                         {QStringLiteral("failedCount"), targetEntries.size()},
                         {QStringLiteral("deletedElementIds"), QVariantList{}},
                         {QStringLiteral("failedElementIds"), QVariantList{}},
                         {QStringLiteral("message"), reason}};
    }
  }
  const auto  delete_result = DeleteTargets(CollectDeleteTargets(targetEntries));

  QVariantMap result{{"success", false},
                     {"deletedCount", 0},
                     {"failedCount", 0},
                     {"deletedElementIds", QVariantList{}},
                     {"failedElementIds", QVariantList{}},
                     {"message", delete_result.message_}};

  result["success"]           = delete_result.success_;
  result["deletedCount"]      = delete_result.deleted_count_;
  result["failedCount"]       = delete_result.failed_count_;
  result["deletedElementIds"] = ToVariantIdList(delete_result.deleted_element_ids_);
  result["failedElementIds"]  = ToVariantIdList(delete_result.failed_element_ids_);
  return result;
}

auto ImageController::AddImagesToFolder(const QVariantList& targetEntries, uint targetFolderId)
    -> QVariantMap {
  QVariantMap result{{"success", false},
                     {"addedCount", 0},
                     {"failedCount", 0},
                     {"addedElementIds", QVariantList{}},
                     {"failedElementIds", QVariantList{}},
                     {"message", QString{}}};

  auto&       ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg    = PL_TEXT("Project is loading. Please wait.");
    result["message"] = msg.Render();
    return result;
  }
  if (!ph.project()) {
    const auto msg    = PL_TEXT("No project is loaded.");
    result["message"] = msg.Render();
    return result;
  }

  const auto targets = CollectDeleteTargets(targetEntries);
  if (targets.empty()) {
    const auto msg    = PL_TEXT("No valid images selected.");
    result["message"] = msg.Render();
    return result;
  }

  const auto target_folder_id = folders_->FolderElementIdForUiId(targetFolderId);
  if (!target_folder_id.has_value() || target_folder_id.value() == 0) {
    const auto msg    = PL_TEXT("Select an album folder.");
    result["message"] = msg.Render();
    return result;
  }

  auto browse = ph.project()->GetAlbumBrowseService();
  if (!browse) {
    const auto msg    = PL_TEXT("Image service is unavailable.");
    result["message"] = msg.Render();
    return result;
  }

  std::vector<sl_element_id_t> ids;
  ids.reserve(targets.size());
  for (const auto& target : targets) {
    if (target.element_id_ != 0) {
      ids.push_back(target.element_id_);
    }
  }

  const auto link_result = browse->LinkFilesToFolder(ids, target_folder_id.value());

  std::vector<sl_element_id_t> added_ids;
  added_ids.reserve(link_result.deleted_files_.size());
  for (const auto& file : link_result.deleted_files_) {
    added_ids.push_back(file.element_id_);
  }

  bool save_ok = true;
  if (!added_ids.empty()) {
    try {
      if (!ph.meta_path().empty()) {
        ph.project()->SaveProject(ph.meta_path());
      }
      QString ignored_error;
      if (!ph.PackageCurrentProjectFiles(&ignored_error)) {
        save_ok = false;
      }
    } catch (...) {
      save_ok = false;
    }
  }

  if (folders_->CurrentFolderElementId() == target_folder_id) {
    library_->ReloadCurrentFolder();
  }

  const int added_count  = static_cast<int>(added_ids.size());
  const int failed_count = static_cast<int>(link_result.failed_element_ids_.size());
  auto      msg          = i18n::LocalizedText{};
  if (added_count == 0) {
    msg = PL_TEXT("No images were added.");
  } else if (failed_count == 0) {
    msg = PL_TEXT("Added %1 image(s) to album.", added_count);
  } else {
    msg = PL_TEXT("Added %1 image(s); %2 failed.", added_count, failed_count);
  }
  if (!save_ok) {
    msg = PL_TEXT("%1 Project state save failed.", msg.Render());
  }

  status_->SetServiceMessage(msg);
  status_->SetTaskState(msg, added_count > 0 ? 100 : 0, false);
  if (added_count > 0) {
    status_->ScheduleIdleTaskStateReset(1200);
  }

  result["success"]          = added_count > 0;
  result["addedCount"]       = added_count;
  result["failedCount"]      = failed_count;
  result["addedElementIds"]  = ToVariantIdList(added_ids);
  result["failedElementIds"] = ToVariantIdList(link_result.failed_element_ids_);
  result["message"]          = msg.Render();
  return result;
}

auto ImageController::DeleteTargets(const std::vector<DeleteTarget>& targets)
    -> DeleteExecutionResult {
  DeleteExecutionResult result;

  auto&                 ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg = PL_TEXT("Project is loading. Please wait.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }
  if (!ph.project()) {
    const auto msg = PL_TEXT("No project is loaded.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }

  auto* ie = import_export_;
  if (ie && ie->current_import_job() && !ie->current_import_job()->IsCancelationAcked()) {
    const auto msg = PL_TEXT("Cannot delete images while import is running.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }
  if (ie && ie->export_inflight()) {
    const auto msg = PL_TEXT("Cannot delete images while export is running.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }

  if (targets.empty()) {
    const auto msg = PL_TEXT("No valid images selected for deletion.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }

  const auto folder_id = folders_->CurrentFolderElementId();
  if (!folder_id.has_value()) {
    const auto msg = PL_TEXT("Folder scope is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }
  const bool                          delete_from_library = folder_id.value() == 0;

  std::unordered_set<sl_element_id_t> target_ids;
  target_ids.reserve(targets.size() * 2 + 1);
  std::vector<sl_element_id_t> delete_element_ids;
  delete_element_ids.reserve(targets.size());
  std::vector<DeleteTarget> resolved_targets = targets;
  for (auto& target : resolved_targets) {
    if (target.image_id_ == 0 || target.file_path_.empty()) {
      if (const auto* item = library_->FindAlbumItem(target.element_id_); item) {
        if (target.image_id_ == 0) {
          target.image_id_ = item->image_id;
        }
        if (target.file_path_.empty()) {
          target.file_path_ = item->file_path_;
        }
        if (target.folder_id_ == 0) {
          target.folder_id_ = item->folder_id;
        }
      }
    }

    target_ids.insert(target.element_id_);
    if (target.element_id_ != 0) {
      delete_element_ids.push_back(target.element_id_);
    }
  }

  if (editor_ && editor_->editor_active() &&
      target_ids.contains(editor_->editor_element_id())) {
    editor_->FinalizeEditorSession(true);
  }

  auto proj         = ph.project();
  auto browse       = proj->GetAlbumBrowseService();
  auto image_pool   = proj->GetImagePoolService();
  auto export_svc   = ph.export_service();
  auto pipeline_svc = ph.pipeline_service();

  if (!browse) {
    const auto msg = PL_TEXT("Image service is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result.message_ = msg.Render();
    return result;
  }

  const auto delete_result =
      browse->DeleteFilesInFolderByElementIds(folder_id.value(), delete_element_ids);
  std::vector<sl_element_id_t> deleted_ids;
  deleted_ids.reserve(delete_result.deleted_files_.size());
  for (const auto& file : delete_result.deleted_files_) {
    deleted_ids.push_back(file.element_id_);
  }

  std::unordered_set<sl_element_id_t> deleted_id_set(deleted_ids.begin(), deleted_ids.end());
  std::vector<sl_element_id_t>        failed_ids;
  failed_ids.reserve(resolved_targets.size());
  for (const auto& target : resolved_targets) {
    if (target.element_id_ != 0 && !deleted_id_set.contains(target.element_id_)) {
      failed_ids.push_back(target.element_id_);
    }
  }

  std::vector<sl_element_id_t> cleanup_element_ids;
  std::vector<image_id_t>      cleanup_image_ids;
  cleanup_element_ids.reserve(deleted_ids.size());
  cleanup_image_ids.reserve(deleted_ids.size());

  for (const auto& target : resolved_targets) {
    if (!deleted_id_set.contains(target.element_id_)) {
      continue;
    }

    if (!delete_from_library) {
      continue;
    }

    try {
      library_->thumbs().RemoveThumbnailState(target.element_id_, target.image_id_);
    } catch (...) {
    }

    cleanup_element_ids.push_back(target.element_id_);
    if (target.image_id_ != 0) {
      cleanup_image_ids.push_back(target.image_id_);
    }
  }

  bool save_ok = true;

  if (!cleanup_element_ids.empty() && export_svc) {
    try {
      export_svc->RemoveExportTasks(cleanup_element_ids);
    } catch (...) {
    }
  }
  if (!cleanup_element_ids.empty() && pipeline_svc) {
    try {
      pipeline_svc->DeletePipelines(cleanup_element_ids);
    } catch (...) {
    }
  }
  if (!cleanup_image_ids.empty() && image_pool) {
    try {
      image_pool->RemoveBatch(cleanup_image_ids);
    } catch (...) {
      save_ok = false;
    }
  }

  if (!cleanup_image_ids.empty() && image_pool) {
    try {
      image_pool->SyncWithStorage();
    } catch (...) {
      save_ok = false;
    }
  }

  if (!deleted_ids.empty()) {
    try {
      if (!ph.meta_path().empty()) {
        proj->SaveProject(ph.meta_path());
      }
      QString ignored_error;
      if (!ph.PackageCurrentProjectFiles(&ignored_error)) {
        save_ok = false;
      }
    } catch (...) {
      save_ok = false;
    }
  }

  if (!deleted_ids.empty()) {
    library_->ReloadCurrentFolder();
  }

  const int deleted_count = static_cast<int>(deleted_ids.size());
  const int failed_count  = static_cast<int>(failed_ids.size());

  auto      msg           = i18n::LocalizedText{};
  if (deleted_count == 0) {
    msg = delete_from_library ? PL_TEXT("No images were deleted.")
                              : PL_TEXT("No images were removed from this album.");
  } else if (failed_count == 0) {
    msg = delete_from_library ? PL_TEXT("Deleted %1 image(s).", deleted_count)
                              : PL_TEXT("Removed %1 image(s) from album.", deleted_count);
  } else {
    msg = delete_from_library
              ? PL_TEXT("Deleted %1 image(s); %2 failed.", deleted_count, failed_count)
              : PL_TEXT("Removed %1 image(s); %2 failed.", deleted_count, failed_count);
  }
  if (!save_ok) {
    msg = PL_TEXT("%1 Project state save failed.", msg.Render());
  }

  status_->SetServiceMessage(msg);
  status_->SetTaskState(msg, deleted_count > 0 ? 100 : 0, false);
  if (deleted_count > 0) {
    status_->ScheduleIdleTaskStateReset(1500);
  }

  result.success_             = deleted_count > 0;
  result.deleted_count_       = deleted_count;
  result.failed_count_        = failed_count;
  result.deleted_element_ids_ = std::move(deleted_ids);
  result.failed_element_ids_  = std::move(failed_ids);
  result.message_             = msg.Render();
  return result;
}

auto ImageController::GetImageDetails(uint elementId, uint imageId) -> QVariantMap {
  QVariantMap result{{"success", false},
                     {"message", QString{}},
                     {"title", QString{}},
                     {"subtitle", QString{}},
                     {"semanticTags", QString{}},
                     {"rows", QVariantList{}}};

  auto&       ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg = PL_TEXT("Project is loading. Please wait.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
  if (!ph.project()) {
    const auto msg = PL_TEXT("No project is loaded.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  image_id_t  resolved_image_id   = static_cast<image_id_t>(imageId);
  const auto  resolved_element_id = static_cast<sl_element_id_t>(elementId);
  const auto* item =
      resolved_element_id != 0 ? library_->FindAlbumItem(resolved_element_id) : nullptr;
  const auto resolved_file_id =
      item != nullptr && item->file_id != 0 ? item->file_id : resolved_element_id;
  if (resolved_image_id == 0 && item) {
    resolved_image_id = item->image_id;
  }
  if (resolved_image_id == 0) {
    const auto msg = PL_TEXT("No valid image was selected.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  auto image_pool = ph.project()->GetImagePoolService();
  if (!image_pool) {
    const auto msg = PL_TEXT("Image service is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  try {
    const QString semantic_tags =
        resolved_file_id != 0
            ? (semantic_ ? semantic_->LabelDisplayText(resolved_file_id) : QString{})
            : QString{};
    return image_pool->Read<QVariantMap>(
        resolved_image_id,
        [item, semantic_tags](const std::shared_ptr<Image>& image) {
          return BuildDetailsResult(item, image, semantic_tags);
        });
  } catch (const std::exception&) {
    const auto msg = PL_TEXT("Failed to load image details.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  } catch (...) {
    const auto msg = PL_TEXT("Failed to load image details.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
}

auto ImageController::GetFocusedImageInspection(uint elementId, uint imageId) -> QVariantMap {
  QVariantMap result{{"success", false},
                     {"message", QString{}},
                     {"elementId", elementId},
                     {"imageId", imageId},
                     {"title", QString{}},
                     {"subtitle", QString{}},
                     {"semanticTags", QString{}},
                     {"rating", 0},
                     {"description", QString{}},
                     {"ratingReason", QString{}},
                     {"tiles", QVariantList{}}};

  auto& ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg = PL_TEXT("Project is loading. Please wait.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
  if (!ph.project()) {
    const auto msg = PL_TEXT("No project is loaded.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  image_id_t  resolved_image_id   = static_cast<image_id_t>(imageId);
  const auto  resolved_element_id = static_cast<sl_element_id_t>(elementId);
  const auto* item =
      resolved_element_id != 0 ? library_->FindAlbumItem(resolved_element_id) : nullptr;
  const auto resolved_file_id =
      item != nullptr && item->file_id != 0 ? item->file_id : resolved_element_id;
  if (resolved_image_id == 0 && item) {
    resolved_image_id = item->image_id;
  }
  if (resolved_image_id == 0 || resolved_file_id == 0) {
    const auto msg = PL_TEXT("No valid image was selected.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  auto project     = ph.project();
  auto image_pool  = project->GetImagePoolService();
  auto storage_svc = project->GetStorage();
  if (!image_pool || !storage_svc) {
    const auto msg = PL_TEXT("Image service is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  try {
    auto& ai = storage_svc->GetAiStore();
    const std::optional<AiDescription> description = ai.GetActiveUnderstanding(resolved_file_id);
    const std::optional<AiRating>      rating_reason = ai.GetActiveRating(resolved_file_id);
    const QString semantic_tags =
        semantic_ ? semantic_->LabelDisplayText(resolved_file_id) : QString{};
    return image_pool->Read<QVariantMap>(
        resolved_image_id, [item, resolved_file_id, resolved_image_id, semantic_tags, description,
                            rating_reason](const std::shared_ptr<Image>& image) {
          return BuildInspectionResult(item, image, resolved_file_id, resolved_image_id,
                                       semantic_tags, description, rating_reason);
        });
  } catch (const std::exception&) {
    const auto msg = PL_TEXT("Failed to load image details.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  } catch (...) {
    const auto msg = PL_TEXT("Failed to load image details.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
}

auto ImageController::GetImageRating(uint elementId, uint imageId) -> QVariantMap {
  QVariantMap result{{"success", false}, {"message", QString{}}, {"rating", 0}};

  auto&       ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg    = PL_TEXT("Project is loading. Please wait.");
    result["message"] = msg.Render();
    return result;
  }
  if (!ph.project()) {
    const auto msg    = PL_TEXT("No project is loaded.");
    result["message"] = msg.Render();
    return result;
  }

  const RatingTarget target = ResolveRatingTarget(elementId, imageId);
  if (target.image_id_ == 0) {
    const auto msg    = PL_TEXT("No valid image was selected.");
    result["message"] = msg.Render();
    return result;
  }

  auto image_pool = ph.project()->GetImagePoolService();
  if (!image_pool) {
    const auto msg    = PL_TEXT("Image service is unavailable.");
    result["message"] = msg.Render();
    return result;
  }

  try {
    const int rating =
        image_pool->Read<int>(target.image_id_, [](const std::shared_ptr<Image>& image) -> int {
          if (!image) {
            return 0;
          }
          if (image->has_exif_display_.load()) {
            return ExifDisplayMetaData::NormalizeRating(image->exif_display_.rating_);
          }
          if (image->has_exif_json_.load()) {
            ExifDisplayMetaData metadata;
            metadata.FromJson(image->exif_json_);
            return ExifDisplayMetaData::NormalizeRating(metadata.rating_);
          }
          return 0;
        });
    result["success"] = true;
    result["rating"]  = rating;
    return result;
  } catch (...) {
    const auto msg    = PL_TEXT("Failed to load image rating.");
    result["message"] = msg.Render();
    return result;
  }
}

auto ImageController::SetImageRating(uint elementId, uint imageId, int rating) -> QVariantMap {
  QVariantMap result{{"success", false}, {"message", QString{}}, {"rating", 0}};

  if (rating < ExifDisplayMetaData::kMinRating || rating > ExifDisplayMetaData::kMaxRating) {
    const auto msg    = PL_TEXT("Rating must be between 0 and 5.");
    result["message"] = msg.Render();
    return result;
  }

  auto& ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg = PL_TEXT("Project is loading. Please wait.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
  if (!ph.project()) {
    const auto msg = PL_TEXT("No project is loaded.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  const RatingTarget target = ResolveRatingTarget(elementId, imageId);
  if (target.image_id_ == 0) {
    const auto msg = PL_TEXT("No valid image was selected.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  auto proj       = ph.project();
  auto image_pool = proj->GetImagePoolService();
  if (!image_pool) {
    const auto msg = PL_TEXT("Image service is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  try {
    image_pool->Write_NoSync<void>(target.image_id_, [rating](const std::shared_ptr<Image>& image) {
      if (!image) {
        return;
      }
      ExifDisplayMetaData metadata;
      if (image->has_exif_display_.load()) {
        metadata = image->exif_display_;
      } else if (image->has_exif_json_.load()) {
        metadata.FromJson(image->exif_json_);
      }
      metadata.rating_ = ExifDisplayMetaData::NormalizeRating(rating);
      image->SetExifDisplayMetaData(std::move(metadata));
    });

    const auto sync_status = image_pool->SyncWithStorage();
    const auto failed_it =
        std::find_if(sync_status.failed_images_.begin(), sync_status.failed_images_.end(),
                     [target](const ImagePoolSyncErrorResult& error) {
                       return error.image_id_ == target.image_id_;
                     });
    if (failed_it != sync_status.failed_images_.end()) {
      const auto msg = PL_TEXT("Failed to save image rating.");
      status_->SetTaskState(msg, 0, false);
      result["message"] = msg.Render();
      return result;
    }

    for (auto& item : library_->view_state().all_images_) {
      if ((target.element_id_ != 0 && item.element_id == target.element_id_) ||
          (target.element_id_ == 0 && item.image_id == target.image_id_)) {
        item.rating = rating;
      }
    }
    library_->model().updateRating(target.element_id_, target.image_id_, rating);
    stats_->RefreshStats();

    bool save_ok = true;
    try {
      if (!ph.meta_path().empty()) {
        proj->SaveProject(ph.meta_path());
      }
      QString ignored_error;
      if (!ph.PackageCurrentProjectFiles(&ignored_error)) {
        save_ok = false;
      }
    } catch (...) {
      save_ok = false;
    }

    auto msg = rating == 0 ? PL_TEXT("Image rating cleared.")
                           : PL_TEXT("Image rating set to %1/5.", rating);
    if (!save_ok) {
      msg = PL_TEXT("%1 Project state save failed.", msg.Render());
    }
    status_->SetServiceMessage(msg);
    status_->SetTaskState(msg, save_ok ? 100 : 0, false);
    if (save_ok) {
      status_->ScheduleIdleTaskStateReset(1200);
    }

    result["success"] = true;
    result["rating"]  = rating;
    result["message"] = msg.Render();
    return result;
  } catch (...) {
    const auto msg = PL_TEXT("Failed to save image rating.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
}

auto ImageController::SetImageDescription(uint elementId, const QString& caption) -> QVariantMap {
  QVariantMap result{{"success", false}, {"message", QString{}}, {"caption", QString{}}};

  const QString trimmed_caption = caption.trimmed();
  if (trimmed_caption.isEmpty()) {
    const auto msg    = PL_TEXT("Description cannot be empty.");
    result["message"] = msg.Render();
    return result;
  }

  auto& ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg = PL_TEXT("Project is loading. Please wait.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
  auto project = ph.project();
  if (!project) {
    const auto msg = PL_TEXT("No project is loaded.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  const auto raw_element_id = static_cast<sl_element_id_t>(elementId);
  const auto* item = raw_element_id != 0 ? library_->FindAlbumItem(raw_element_id) : nullptr;
  const auto file_id = item != nullptr && item->file_id != 0 ? item->file_id : raw_element_id;
  if (file_id == 0) {
    const auto msg = PL_TEXT("No valid image was selected.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  auto storage_svc = project->GetStorage();
  if (!storage_svc) {
    const auto msg = PL_TEXT("Image service is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  try {
    auto&         ai       = storage_svc->GetAiStore();
    AiDescription updated  = ai.GetActiveUnderstanding(file_id).value_or(AiDescription{});
    updated.file_id_       = file_id;
    updated.caption_       = trimmed_caption.toStdString();
    updated.active_        = true;
    FillManualDescriptionIdentity(updated);
    if (!ai.UpsertUnderstanding(updated)) {
      const auto msg    = PL_TEXT("Failed to save image description.");
      result["message"] = msg.Render();
      return result;
    }

    stats_->RebuildThumbnailView();
    bool      save_ok = SaveProjectSnapshot();
    auto      msg     = PL_TEXT("Description saved.");
    if (!save_ok) {
      msg = PL_TEXT("%1 Project state save failed.", msg.Render());
    }
    status_->SetServiceMessage(msg);
    status_->SetTaskState(msg, save_ok ? 100 : 0, false);
    if (save_ok) {
      status_->ScheduleIdleTaskStateReset(1200);
    }

    result["success"] = true;
    result["caption"] = trimmed_caption;
    result["message"] = msg.Render();
    return result;
  } catch (...) {
    const auto msg = PL_TEXT("Failed to save image description.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
}

auto ImageController::SetImageRatingReasons(uint elementId, const QString& reasons)
    -> QVariantMap {
  QVariantMap result{{"success", false}, {"message", QString{}}, {"reasons", QString{}}};

  const QString trimmed_reasons = reasons.trimmed();
  if (trimmed_reasons.isEmpty()) {
    const auto msg    = PL_TEXT("Rating reason cannot be empty.");
    result["message"] = msg.Render();
    return result;
  }

  auto& ph = project_->handler();
  if (ph.project_loading()) {
    const auto msg = PL_TEXT("Project is loading. Please wait.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
  auto project = ph.project();
  if (!project) {
    const auto msg = PL_TEXT("No project is loaded.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  const auto raw_element_id = static_cast<sl_element_id_t>(elementId);
  const auto* item = raw_element_id != 0 ? library_->FindAlbumItem(raw_element_id) : nullptr;
  const auto file_id = item != nullptr && item->file_id != 0 ? item->file_id : raw_element_id;
  if (file_id == 0) {
    const auto msg = PL_TEXT("No valid image was selected.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  auto storage_svc = project->GetStorage();
  if (!storage_svc) {
    const auto msg = PL_TEXT("Image service is unavailable.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }

  try {
    auto&    ai      = storage_svc->GetAiStore();
    AiRating updated = ai.GetActiveRating(file_id).value_or(AiRating{});
    updated.file_id_ = file_id;
    updated.rating_  = 0;
    updated.reasons_ = trimmed_reasons.toStdString();
    updated.active_  = true;
    FillManualRatingIdentity(updated);
    if (!ai.UpsertRatingReasons(updated)) {
      const auto msg    = PL_TEXT("Failed to save rating reason.");
      result["message"] = msg.Render();
      return result;
    }

    bool save_ok = SaveProjectSnapshot();
    auto msg     = PL_TEXT("Rating reason saved.");
    if (!save_ok) {
      msg = PL_TEXT("%1 Project state save failed.", msg.Render());
    }
    status_->SetServiceMessage(msg);
    status_->SetTaskState(msg, save_ok ? 100 : 0, false);
    if (save_ok) {
      status_->ScheduleIdleTaskStateReset(1200);
    }

    result["success"] = true;
    result["reasons"] = trimmed_reasons;
    result["message"] = msg.Render();
    return result;
  } catch (...) {
    const auto msg = PL_TEXT("Failed to save rating reason.");
    status_->SetTaskState(msg, 0, false);
    result["message"] = msg.Render();
    return result;
  }
}

// Phase 7a: the light half of the star-rating path (extracted from `SetImageRating`
// above). `Write_NoSync` sets the image MODIFIED and mutates the in-memory
// `exif_display_.rating_`; the view-state patch + `thumbnail_model_.updateRating` keep
// the UI in sync without a DB round-trip. No `SyncWithStorage`/`SaveProject`/`Package`/
// `RefreshStats` — the batched AI scoring run flushes once at job end via
// `FlushPendingStarRatings`, so a batch does one DB flush instead of one per image.
// `SetImageRating` (manual single click) is unchanged and still does a full sync+save.
void ImageController::ApplyStarRatingLight(uint elementId, uint imageId, int rating) {
  const RatingTarget target = ResolveRatingTarget(elementId, imageId);
  if (target.image_id_ == 0) {
    return;
  }
  auto proj = project_->handler().project();
  if (!proj) {
    return;
  }
  auto image_pool = proj->GetImagePoolService();
  if (!image_pool) {
    return;
  }
  try {
    image_pool->Write_NoSync<void>(target.image_id_,
                                   [rating](const std::shared_ptr<Image>& image) {
                                     if (!image) {
                                       return;
                                     }
                                     ExifDisplayMetaData metadata;
                                     if (image->has_exif_display_.load()) {
                                       metadata = image->exif_display_;
                                     } else if (image->has_exif_json_.load()) {
                                       metadata.FromJson(image->exif_json_);
                                     }
                                     metadata.rating_ = ExifDisplayMetaData::NormalizeRating(rating);
                                     image->SetExifDisplayMetaData(std::move(metadata));
                                   });
    for (auto& item : library_->view_state().all_images_) {
      if ((target.element_id_ != 0 && item.element_id == target.element_id_) ||
          (target.element_id_ == 0 && item.image_id == target.image_id_)) {
        item.rating = rating;
      }
    }
    library_->model().updateRating(target.element_id_, target.image_id_, rating);
  } catch (...) {
    // Best-effort light write: a transient pool failure leaves the prior rating; the
    // batch flush at job end is the durability point. Match `PersistImageHdrFlag`'s
    // swallow-and-return policy for the light path.
  }
}

// Phase 7a: the batched flush half. `SyncWithStorage` flushes every MODIFIED image row
// (the pending star writes from `ApplyStarRatingLight`) in a single transaction;
// `RefreshStats` re-runs the rating-bucket GROUP BY so star-filter stats reflect the
// new stars. No `SaveProject`/`Package` — the `.alcd` packaged snapshot is left stale
// until the next normal save/close; the live DB is authoritative (same as any DB change
// between manual saves).
void ImageController::FlushPendingStarRatings() {
  auto proj = project_->handler().project();
  if (!proj) {
    return;
  }
  try {
    auto image_pool = proj->GetImagePoolService();
    if (image_pool) {
      image_pool->SyncWithStorage();
    }
  } catch (...) {
  }
  stats_->RefreshStats();
}


auto ImageController::GetImageRatingReasons(uint elementId) -> QVariantMap {
  QVariantMap result{{QStringLiteral("hasReasons"), false},
                     {QStringLiteral("reasons"), QString{}},
                     {QStringLiteral("provider"), QString{}},
                     {QStringLiteral("modelId"), QString{}},
                     {QStringLiteral("rubricId"), QString{}},
                     {QStringLiteral("rubricVersion"), QString{}}};
  if (!project_) {
    return result;
  }
  auto project = project_->handler().project();
  if (!project) {
    return result;
  }
  const auto row =
      project->GetStorage()->GetAiStore().GetActiveRating(elementId);
  if (!row.has_value() || row->reasons_.empty()) {
    return result;
  }
  result[QStringLiteral("hasReasons")] = true;
  result[QStringLiteral("reasons")] = QString::fromStdString(row->reasons_);
  result[QStringLiteral("provider")] = QString::fromStdString(row->provider_id_);
  result[QStringLiteral("modelId")] = QString::fromStdString(row->model_id_);
  result[QStringLiteral("rubricId")] = QString::fromStdString(row->rubric_id_);
  result[QStringLiteral("rubricVersion")] = QString::fromStdString(row->rubric_version_);
  return result;
}

auto ImageController::GetImageDescription(uint elementId) -> QVariantMap {
  QVariantMap result{{QStringLiteral("hasDescription"), false},
                     {QStringLiteral("caption"), QString{}},
                     {QStringLiteral("scene"), QString{}},
                     {QStringLiteral("provider"), QString{}},
                     {QStringLiteral("modelId"), QString{}}};
  if (!project_) {
    return result;
  }
  auto project = project_->handler().project();
  if (!project) {
    return result;
  }
  const auto row =
      project->GetStorage()->GetAiStore().GetActiveUnderstanding(elementId);
  if (!row.has_value() || row->caption_.empty()) {
    return result;
  }
  result[QStringLiteral("hasDescription")] = true;
  result[QStringLiteral("caption")] = QString::fromStdString(row->caption_);
  result[QStringLiteral("scene")] = QString::fromStdString(row->scene_);
  result[QStringLiteral("provider")] = QString::fromStdString(row->provider_id_);
  result[QStringLiteral("modelId")] = QString::fromStdString(row->model_id_);
  return result;
}

bool ImageController::OpenDirectoryInFileManager(const QString& dirUrlOrPath) {
  const auto dir_path_opt = album_util::InputToPath(dirUrlOrPath);
  if (!dir_path_opt.has_value()) {
    if (status_) {
      status_->SetServiceMessage(PL_TEXT("Source directory is unavailable."));
    }
    return false;
  }
  const std::filesystem::path dir_path = dir_path_opt.value().lexically_normal();
  std::error_code ec;
  if (!std::filesystem::exists(dir_path, ec) || ec ||
      !std::filesystem::is_directory(dir_path, ec) || ec) {
    if (status_) {
      status_->SetServiceMessage(PL_TEXT("Source directory is unavailable."));
    }
    return false;
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(album_util::PathToQString(dir_path)))) {
    if (status_) {
      status_->SetServiceMessage(PL_TEXT("Failed to open source directory."));
    }
    return false;
  }
  return true;
}

}  // namespace alcedo::ui

#undef PL_TEXT
