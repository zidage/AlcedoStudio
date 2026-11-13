//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/export_recipe_builder.hpp"

#include <QLocale>
#include <QMetaType>
#include <QSettings>
#include <cmath>
#include <limits>
#include <utility>

#include "io/image/export_recipe.hpp"

namespace alcedo::ui {
namespace {

constexpr double kNotANumber         = std::numeric_limits<double>::quiet_NaN();
constexpr auto   kUserPresetsKey     = "export/recipePresets";
constexpr auto   kLastUsedOptionsKey = "export/lastOptions";
const QString    kBuiltInPresetPrefix = QStringLiteral("builtin:");
const QString    kUserPresetPrefix    = QStringLiteral("user:");

auto             IsIntegerInRange(double value, int minimum, int maximum) -> bool {
  return std::isfinite(value) && value == std::floor(value) && value >= minimum && value <= maximum;
}

auto RangeError(const QString& field, int minimum, int maximum) -> ExportRecipeError {
  return {field, QStringLiteral("%1 must be in [%2, %3]").arg(field).arg(minimum).arg(maximum)};
}

auto EnumError(const QString& field, const QStringList& allowed) -> ExportRecipeError {
  return {field, QStringLiteral("%1 must be one of: %2").arg(field, allowed.join(", "))};
}

auto IsText(const QVariant& value) -> bool { return value.typeId() == QMetaType::QString; }

/// A pixel size or dpi value: a number, or panel text read like JavaScript Number(text.trim()).
auto ReadNumber(const QVariant& value) -> double {
  if (!IsText(value)) {
    bool         ok     = false;
    const double result = value.toDouble(&ok);
    return ok ? result : kNotANumber;
  }
  const QString text   = value.toString().trimmed();
  bool          ok     = false;
  const double  result = text.toDouble(&ok);
  return ok ? result : kNotANumber;
}

/// A print size: a number, or panel text in the default locale (Number.fromLocaleString).
auto ReadLocaleNumber(const QVariant& value) -> double {
  if (!IsText(value)) {
    return ReadNumber(value);
  }
  bool         ok     = false;
  const double result = QLocale().toDouble(value.toString().trimmed(), &ok);
  return ok ? result : kNotANumber;
}

/// The resize mode name of ImportExportHandler::BuildExportQueue.
auto HandlerResizeMode(const QString& resize_mode) -> QString {
  if (resize_mode == QStringLiteral("long_edge")) {
    return QStringLiteral("longEdge");
  }
  if (resize_mode == QStringLiteral("print")) {
    return QStringLiteral("physical");
  }
  return resize_mode;
}

auto ErrorMap(const std::optional<ExportRecipeError>& error, const QString& ok_key) -> QVariantMap {
  if (!error.has_value()) {
    return QVariantMap{{ok_key, true}, {"field", QString()}, {"message", QString()}};
  }
  return QVariantMap{{ok_key, false}, {"field", error->field}, {"message", error->message}};
}

/// The map form of the options that a preset sets: every option except the destination.
auto PresetOptionsMap(const ExportRecipeOptions& options) -> QVariantMap {
  QVariantMap map = ExportRecipeBuilder::ToVariantMap(options);
  map.remove(QStringLiteral("out_dir"));
  map.remove(QStringLiteral("subfolder_enabled"));
  map.remove(QStringLiteral("subfolder"));
  return map;
}

auto MakeBuiltInPreset(const QString& key, const QString& name, ExportRecipeOptions options)
    -> ExportRecipePreset {
  return ExportRecipePreset{.id       = kBuiltInPresetPrefix + key,
                            .name     = name,
                            .built_in = true,
                            .options  = std::move(options)};
}

auto IsValidPresetName(const QString& name) -> bool {
  return !name.isEmpty() && name.size() <= ExportRecipeBuilder::kMaxPresetNameSize;
}

/// The QSettings form of one user preset.
auto StoredUserPreset(const QString& name, const ExportRecipeOptions& options) -> QVariantMap {
  return QVariantMap{{QStringLiteral("name"), name},
                     {QStringLiteral("options"), PresetOptionsMap(options)}};
}

}  // namespace

ExportRecipeBuilder::ExportRecipeBuilder(ImportExportHandler* import_export, QObject* parent)
    : QObject(parent), import_export_(import_export) {}

auto ExportRecipeBuilder::Defaults() -> ExportRecipeOptions { return ExportRecipeOptions{}; }

auto ExportRecipeBuilder::Formats() -> QStringList {
  return {QStringLiteral("JPEG"), QStringLiteral("PNG"), QStringLiteral("TIFF"),
          QStringLiteral("EXR")};
}

auto ExportRecipeBuilder::AllowedBitDepths(const QString& format) -> QList<int> {
  if (format == QStringLiteral("JPEG")) return {8};
  if (format == QStringLiteral("PNG")) return {8, 16};
  if (format == QStringLiteral("TIFF")) return {8, 16, 32};
  if (format == QStringLiteral("EXR")) return {16, 32};
  return {};
}

auto ExportRecipeBuilder::PreferredBitDepth(const QString& format) -> int {
  return format == QStringLiteral("JPEG") ? 8 : 16;
}

auto ExportRecipeBuilder::TiffCompressions() -> QStringList {
  return {QStringLiteral("NONE"), QStringLiteral("LZW"), QStringLiteral("ZIP")};
}

auto ExportRecipeBuilder::ResizeModes() -> QStringList {
  return {QStringLiteral("original"), QStringLiteral("long_edge"), QStringLiteral("bounds"),
          QStringLiteral("print")};
}

auto ExportRecipeBuilder::PrintUnits() -> QStringList {
  return {QStringLiteral("mm"), QStringLiteral("cm"), QStringLiteral("in")};
}

auto ExportRecipeBuilder::CheckFileNamePattern(const QString& pattern) -> std::optional<QString> {
  const auto parsed = ParseExportFileNamePattern(pattern.toStdWString());
  if (parsed.success_) {
    return std::nullopt;
  }
  return QString::fromStdString(parsed.message_);
}

auto ExportRecipeBuilder::Validate(const ExportRecipeOptions& options)
    -> std::optional<ExportRecipeError> {
  if (!Formats().contains(options.format)) {
    return EnumError(QStringLiteral("format"), Formats());
  }
  const QList<int> bit_depths = AllowedBitDepths(options.format);
  if (!bit_depths.contains(options.bit_depth)) {
    QStringList allowed;
    for (const int depth : bit_depths) {
      allowed.push_back(QString::number(depth));
    }
    return ExportRecipeError{QStringLiteral("bit_depth"),
                             QStringLiteral("bit_depth for %1 must be one of: %2")
                                 .arg(options.format, allowed.join(", "))};
  }
  if (options.quality < kMinQuality || options.quality > kMaxQuality) {
    return RangeError(QStringLiteral("quality"), kMinQuality, kMaxQuality);
  }
  if (options.ultra_hdr_quality < kMinQuality || options.ultra_hdr_quality > kMaxQuality) {
    return RangeError(QStringLiteral("ultra_hdr_quality"), kMinQuality, kMaxQuality);
  }
  if (!IsIntegerInRange(options.ultra_hdr_long_edge, kMinLongEdge, kUltraHdrMaxLongEdge)) {
    return RangeError(QStringLiteral("ultra_hdr_long_edge"), kMinLongEdge, kUltraHdrMaxLongEdge);
  }
  if (options.png_level < kMinPngLevel || options.png_level > kMaxPngLevel) {
    return RangeError(QStringLiteral("png_level"), kMinPngLevel, kMaxPngLevel);
  }
  if (!TiffCompressions().contains(options.tiff_compression)) {
    return EnumError(QStringLiteral("tiff_compression"), TiffCompressions());
  }
  if (!ResizeModes().contains(options.resize_mode)) {
    return EnumError(QStringLiteral("resize_mode"), ResizeModes());
  }
  if (options.resize_mode == QStringLiteral("long_edge") &&
      !IsIntegerInRange(options.long_edge, kMinLongEdge, kMaxLongEdge)) {
    return RangeError(QStringLiteral("long_edge"), kMinLongEdge, kMaxLongEdge);
  }
  if (options.resize_mode == QStringLiteral("bounds")) {
    if (!IsIntegerInRange(options.width, kMinPixelBound, kMaxPixelBound)) {
      return RangeError(QStringLiteral("width"), kMinPixelBound, kMaxPixelBound);
    }
    if (!IsIntegerInRange(options.height, kMinPixelBound, kMaxPixelBound)) {
      return RangeError(QStringLiteral("height"), kMinPixelBound, kMaxPixelBound);
    }
  }
  if (options.resize_mode == QStringLiteral("print")) {
    for (const auto& [field, value] :
         {std::pair{QStringLiteral("print_width"), options.print_width},
          std::pair{QStringLiteral("print_height"), options.print_height}}) {
      if (!std::isfinite(value) || value <= 0.0 || value > kMaxPrintSize) {
        return ExportRecipeError{field, QStringLiteral("%1 must be more than 0 and at most %2")
                                            .arg(field)
                                            .arg(static_cast<int>(kMaxPrintSize))};
      }
    }
    if (!IsIntegerInRange(options.dpi, kMinDpi, kMaxDpi)) {
      return RangeError(QStringLiteral("dpi"), kMinDpi, kMaxDpi);
    }
    if (!PrintUnits().contains(options.print_unit)) {
      return EnumError(QStringLiteral("print_unit"), PrintUnits());
    }
  }
  if (const auto pattern_error = CheckFileNamePattern(options.file_name_pattern)) {
    return ExportRecipeError{QStringLiteral("file_name_pattern"), *pattern_error};
  }
  return std::nullopt;
}

auto ExportRecipeBuilder::OutputFolder(const ExportRecipeOptions& options) -> QString {
  const QString subfolder = options.subfolder.trimmed();
  if (!options.subfolder_enabled || subfolder.isEmpty()) {
    return options.out_dir;
  }
  QString base = options.out_dir;
  while (base.endsWith(QLatin1Char('/')) || base.endsWith(QLatin1Char('\\'))) {
    base.chop(1);
  }
  return base + QLatin1Char('/') + subfolder;
}

auto ExportRecipeBuilder::FromVariantMap(const QVariantMap& values) -> ExportRecipeOptions {
  ExportRecipeOptions options;
  const auto          text = [&values](const char* key, QString* target) {
    if (values.contains(key)) *target = values.value(key).toString();
  };
  const auto integer = [&values](const char* key, int* target) {
    if (values.contains(key)) *target = values.value(key).toInt();
  };
  const auto flag = [&values](const char* key, bool* target) {
    if (values.contains(key)) *target = values.value(key).toBool();
  };
  const auto number = [&values](const char* key, double* target) {
    if (values.contains(key)) *target = ReadNumber(values.value(key));
  };
  const auto locale_number = [&values](const char* key, double* target) {
    if (values.contains(key)) *target = ReadLocaleNumber(values.value(key));
  };
  text("format", &options.format);
  integer("bit_depth", &options.bit_depth);
  integer("quality", &options.quality);
  integer("png_level", &options.png_level);
  text("tiff_compression", &options.tiff_compression);
  flag("include_metadata", &options.include_metadata);
  flag("embed_icc_profile", &options.embed_icc_profile);
  flag("include_gps", &options.include_gps);
  integer("ultra_hdr_quality", &options.ultra_hdr_quality);
  flag("ultra_hdr_dither", &options.ultra_hdr_dither);
  number("ultra_hdr_long_edge", &options.ultra_hdr_long_edge);
  text("out_dir", &options.out_dir);
  flag("subfolder_enabled", &options.subfolder_enabled);
  text("subfolder", &options.subfolder);
  text("resize_mode", &options.resize_mode);
  number("long_edge", &options.long_edge);
  number("width", &options.width);
  number("height", &options.height);
  locale_number("print_width", &options.print_width);
  locale_number("print_height", &options.print_height);
  text("print_unit", &options.print_unit);
  number("dpi", &options.dpi);
  text("file_name_pattern", &options.file_name_pattern);
  return options;
}

auto ExportRecipeBuilder::ToVariantMap(const ExportRecipeOptions& options) -> QVariantMap {
  return QVariantMap{{"format", options.format},
                     {"bit_depth", options.bit_depth},
                     {"quality", options.quality},
                     {"png_level", options.png_level},
                     {"tiff_compression", options.tiff_compression},
                     {"include_metadata", options.include_metadata},
                     {"embed_icc_profile", options.embed_icc_profile},
                     {"include_gps", options.include_gps},
                     {"ultra_hdr_quality", options.ultra_hdr_quality},
                     {"ultra_hdr_dither", options.ultra_hdr_dither},
                     {"ultra_hdr_long_edge", options.ultra_hdr_long_edge},
                     {"out_dir", options.out_dir},
                     {"subfolder_enabled", options.subfolder_enabled},
                     {"subfolder", options.subfolder},
                     {"resize_mode", options.resize_mode},
                     {"long_edge", options.long_edge},
                     {"width", options.width},
                     {"height", options.height},
                     {"print_width", options.print_width},
                     {"print_height", options.print_height},
                     {"print_unit", options.print_unit},
                     {"dpi", options.dpi},
                     {"file_name_pattern", options.file_name_pattern}};
}

auto ExportRecipeBuilder::BuiltInPresets() -> QList<ExportRecipePreset> {
  QList<ExportRecipePreset> presets;
  presets.push_back(
      MakeBuiltInPreset(QStringLiteral("standard"), QStringLiteral("Standard"), Defaults()));

  // Web pages and online galleries: small files that load fast, without the location.
  ExportRecipeOptions web = Defaults();
  web.quality             = 80;
  web.resize_mode         = QStringLiteral("long_edge");
  web.long_edge           = 2048;
  web.include_gps         = false;
  web.ultra_hdr_quality   = 85;
  web.ultra_hdr_long_edge = 2048;
  presets.push_back(MakeBuiltInPreset(QStringLiteral("web"), QStringLiteral("Web"), web));

  // Social networks: at most 1080 px wide and 1350 px high (the 4:5 portrait limit), without
  // the location.
  ExportRecipeOptions social = Defaults();
  social.quality             = 90;
  social.resize_mode         = QStringLiteral("bounds");
  social.width               = 1080;
  social.height              = 1350;
  social.include_gps         = false;
  social.ultra_hdr_quality   = 90;
  social.ultra_hdr_long_edge = 1350;
  presets.push_back(
      MakeBuiltInPreset(QStringLiteral("social"), QStringLiteral("Social Media"), social));

  // Email attachments and quick previews: small files, without the location.
  ExportRecipeOptions email = Defaults();
  email.quality             = 75;
  email.resize_mode         = QStringLiteral("long_edge");
  email.long_edge           = 1280;
  email.include_gps         = false;
  email.ultra_hdr_quality   = 75;
  email.ultra_hdr_long_edge = 1280;
  presets.push_back(MakeBuiltInPreset(QStringLiteral("email"), QStringLiteral("Email"), email));

  // Delivery masters at full resolution.
  ExportRecipeOptions max_jpeg = Defaults();
  max_jpeg.quality             = 100;
  max_jpeg.ultra_hdr_quality   = 100;
  presets.push_back(MakeBuiltInPreset(QStringLiteral("max_jpeg"),
                                      QStringLiteral("Maximum Quality JPEG"), max_jpeg));

  // Editing in other applications and printing: lossless 16-bit files.
  ExportRecipeOptions tiff = Defaults();
  tiff.format              = QStringLiteral("TIFF");
  tiff.bit_depth           = 16;
  tiff.tiff_compression    = QStringLiteral("ZIP");
  tiff.ultra_hdr_quality   = 100;
  presets.push_back(
      MakeBuiltInPreset(QStringLiteral("tiff16"), QStringLiteral("16-bit TIFF"), tiff));

  // Archives: half-float OpenEXR at full resolution.
  ExportRecipeOptions exr = Defaults();
  exr.format              = QStringLiteral("EXR");
  exr.bit_depth           = 16;
  exr.ultra_hdr_quality   = 100;
  presets.push_back(
      MakeBuiltInPreset(QStringLiteral("exr_archive"), QStringLiteral("EXR Archive"), exr));
  return presets;
}

auto ExportRecipeBuilder::UserPresets() -> QList<ExportRecipePreset> {
  QList<ExportRecipePreset> presets;
  const QVariantList        stored = QSettings{}.value(QLatin1String(kUserPresetsKey)).toList();
  for (const QVariant& entry : stored) {
    const QVariantMap map  = entry.toMap();
    const QString     name = map.value(QStringLiteral("name")).toString().trimmed();
    if (!IsValidPresetName(name)) {
      continue;
    }
    ExportRecipeOptions options = FromVariantMap(map.value(QStringLiteral("options")).toMap());
    if (Validate(options).has_value()) {
      continue;
    }
    presets.push_back(ExportRecipePreset{.id       = kUserPresetPrefix + name,
                                         .name     = name,
                                         .built_in = false,
                                         .options  = std::move(options)});
    if (presets.size() >= kMaxUserPresetCount) {
      break;
    }
  }
  return presets;
}

auto ExportRecipeBuilder::Presets() -> QList<ExportRecipePreset> {
  return BuiltInPresets() + UserPresets();
}

auto ExportRecipeBuilder::HasSamePresetOptions(const ExportRecipeOptions& a,
                                               const ExportRecipeOptions& b) -> bool {
  if (a.format != b.format || a.bit_depth != b.bit_depth ||
      a.include_metadata != b.include_metadata || a.embed_icc_profile != b.embed_icc_profile ||
      a.include_gps != b.include_gps || a.ultra_hdr_quality != b.ultra_hdr_quality ||
      a.ultra_hdr_dither != b.ultra_hdr_dither ||
      a.ultra_hdr_long_edge != b.ultra_hdr_long_edge || a.resize_mode != b.resize_mode ||
      a.file_name_pattern != b.file_name_pattern) {
    return false;
  }
  if (a.format == QStringLiteral("JPEG") && a.quality != b.quality) return false;
  if (a.format == QStringLiteral("PNG") && a.png_level != b.png_level) return false;
  if (a.format == QStringLiteral("TIFF") && a.tiff_compression != b.tiff_compression) {
    return false;
  }
  if (a.resize_mode == QStringLiteral("long_edge")) return a.long_edge == b.long_edge;
  if (a.resize_mode == QStringLiteral("bounds")) {
    return a.width == b.width && a.height == b.height;
  }
  if (a.resize_mode == QStringLiteral("print")) {
    return a.print_width == b.print_width && a.print_height == b.print_height &&
           a.print_unit == b.print_unit && a.dpi == b.dpi;
  }
  return true;
}

auto ExportRecipeBuilder::MatchingPresetId(const ExportRecipeOptions& options) -> QString {
  for (const auto& preset : Presets()) {
    if (HasSamePresetOptions(options, preset.options)) {
      return preset.id;
    }
  }
  return {};
}

auto ExportRecipeBuilder::ApplyPreset(const ExportRecipeOptions& current,
                                      const ExportRecipePreset& preset) -> ExportRecipeOptions {
  ExportRecipeOptions result = preset.options;
  result.out_dir             = current.out_dir;
  result.subfolder_enabled   = current.subfolder_enabled;
  result.subfolder           = current.subfolder;
  return result;
}

auto ExportRecipeBuilder::SaveUserPreset(const QString& name, const ExportRecipeOptions& options,
                                         const QString& replaced_id)
    -> std::variant<QString, ExportRecipeError> {
  const QString normalized_name = name.trimmed();
  if (!IsValidPresetName(normalized_name)) {
    return ExportRecipeError{
        QStringLiteral("name"),
        QStringLiteral("name must have 1 to %1 characters").arg(kMaxPresetNameSize)};
  }
  if (auto error = Validate(options)) {
    return *error;
  }

  QList<ExportRecipePreset> presets  = UserPresets();
  const auto                index_of = [&presets](const QString& preset_name) -> qsizetype {
    for (qsizetype index = 0; index < presets.size(); ++index) {
      if (presets[index].name.compare(preset_name, Qt::CaseInsensitive) == 0) {
        return index;
      }
    }
    return -1;
  };

  const ExportRecipePreset saved{.id       = kUserPresetPrefix + normalized_name,
                                 .name     = normalized_name,
                                 .built_in = false,
                                 .options  = options};
  qsizetype                replace_index = -1;
  if (replaced_id.startsWith(kUserPresetPrefix)) {
    replace_index = index_of(replaced_id.mid(kUserPresetPrefix.size()));
  }
  const qsizetype same_name_index = index_of(normalized_name);
  if (replace_index >= 0 && same_name_index >= 0 && replace_index != same_name_index) {
    presets.removeAt(same_name_index);
    if (same_name_index < replace_index) --replace_index;
  } else if (replace_index < 0) {
    replace_index = same_name_index;
  }
  if (replace_index >= 0) {
    presets[replace_index] = saved;
  } else {
    if (presets.size() >= kMaxUserPresetCount) {
      return ExportRecipeError{
          QStringLiteral("name"),
          QStringLiteral("at most %1 presets can be saved").arg(kMaxUserPresetCount)};
    }
    presets.push_back(saved);
  }

  QVariantList stored;
  for (const auto& preset : presets) {
    stored.push_back(StoredUserPreset(preset.name, preset.options));
  }
  QSettings{}.setValue(QLatin1String(kUserPresetsKey), stored);
  emit PresetsChanged();
  return saved.id;
}

auto ExportRecipeBuilder::DeleteUserPreset(const QString& id) -> bool {
  if (!id.startsWith(kUserPresetPrefix)) {
    return false;
  }
  const QString name    = id.mid(kUserPresetPrefix.size());
  bool          removed = false;
  QVariantList  stored;
  for (const auto& preset : UserPresets()) {
    if (!removed && preset.name.compare(name, Qt::CaseInsensitive) == 0) {
      removed = true;
      continue;
    }
    stored.push_back(StoredUserPreset(preset.name, preset.options));
  }
  if (!removed) {
    return false;
  }
  QSettings{}.setValue(QLatin1String(kUserPresetsKey), stored);
  emit PresetsChanged();
  return true;
}

auto ExportRecipeBuilder::LastUsedOptions() -> ExportRecipeOptions {
  const QVariantMap stored = QSettings{}.value(QLatin1String(kLastUsedOptionsKey)).toMap();
  if (stored.isEmpty()) {
    return Defaults();
  }
  ExportRecipeOptions options = FromVariantMap(stored);
  const ExportRecipeOptions defaults = Defaults();
  options.out_dir           = defaults.out_dir;
  options.subfolder_enabled = defaults.subfolder_enabled;
  options.subfolder         = defaults.subfolder;
  return Validate(options).has_value() ? defaults : options;
}

void ExportRecipeBuilder::SaveLastUsedOptions(const ExportRecipeOptions& options) {
  if (Validate(options).has_value()) {
    return;
  }
  QSettings{}.setValue(QLatin1String(kLastUsedOptionsKey), PresetOptionsMap(options));
}

auto ExportRecipeBuilder::Start(const ExportRecipeOptions& options, const QVariantList& targets)
    -> std::optional<ExportRecipeError> {
  if (auto error = Validate(options)) {
    return error;
  }
  if (import_export_ == nullptr) {
    return ExportRecipeError{QStringLiteral("export"),
                             QStringLiteral("The export handler is not available.")};
  }
  // The argument form of the former export panel: the SDR long edge is set only in the long
  // edge mode, and every mode except the original size enables the SDR resize.
  const bool resize_enabled = options.resize_mode != QStringLiteral("original");
  const int  long_edge =
      options.resize_mode == QStringLiteral("long_edge") ? static_cast<int>(options.long_edge) : 0;
  QVariantMap recipe{
      {"includeMetadata", options.include_metadata},
      {"embedIccProfile", options.embed_icc_profile},
      {"includeGps", options.include_gps},
      {"fileNamePattern", options.file_name_pattern},
      {"resizeMode", HandlerResizeMode(options.resize_mode)},
      {"widthPixels", std::isfinite(options.width) ? static_cast<int>(options.width) : 0},
      {"heightPixels", std::isfinite(options.height) ? static_cast<int>(options.height) : 0},
      {"physicalWidth", options.print_width},
      {"physicalHeight", options.print_height},
      {"physicalUnit", options.print_unit},
      {"dpi", std::isfinite(options.dpi) ? static_cast<int>(options.dpi) : 0}};
  import_export_->StartExportWithRecipeOptionsForTargets(
      OutputFolder(options), resize_enabled, long_edge,
      static_cast<int>(options.ultra_hdr_long_edge), options.format,
      options.quality, options.bit_depth, options.png_level, options.tiff_compression,
      options.ultra_hdr_quality, options.ultra_hdr_dither, recipe, targets);
  if (import_export_->export_inflight()) {
    return std::nullopt;
  }
  QString message = import_export_->ExportStatus();
  if (const QString summary = import_export_->ExportErrorSummary(); !summary.isEmpty()) {
    message += QLatin1Char(' ') + summary;
  }
  return ExportRecipeError{QStringLiteral("export"), message};
}

auto ExportRecipeBuilder::DefaultOptions() const -> QVariantMap { return ToVariantMap(Defaults()); }

auto ExportRecipeBuilder::AllowedBitDepthsFor(const QString& format) const -> QVariantList {
  QVariantList depths;
  for (const int depth : AllowedBitDepths(format)) {
    depths.push_back(depth);
  }
  return depths;
}

auto ExportRecipeBuilder::PreferredBitDepthFor(const QString& format) const -> int {
  return PreferredBitDepth(format);
}

auto ExportRecipeBuilder::IsFileNamePatternValid(const QString& pattern) const -> bool {
  return !CheckFileNamePattern(pattern).has_value();
}

auto ExportRecipeBuilder::ValidateOptions(const QVariantMap& options) const -> QVariantMap {
  return ErrorMap(Validate(FromVariantMap(options)), QStringLiteral("valid"));
}

auto ExportRecipeBuilder::OutputFolderFor(const QVariantMap& options) const -> QString {
  return OutputFolder(FromVariantMap(options));
}

auto ExportRecipeBuilder::PresetList() const -> QVariantList {
  QVariantList list;
  for (const auto& preset : Presets()) {
    list.push_back(QVariantMap{{QStringLiteral("id"), preset.id},
                               {QStringLiteral("name"), preset.name},
                               {QStringLiteral("builtIn"), preset.built_in}});
  }
  return list;
}

auto ExportRecipeBuilder::MatchingPresetIdFor(const QVariantMap& options) const -> QString {
  return MatchingPresetId(FromVariantMap(options));
}

auto ExportRecipeBuilder::OptionsWithPreset(const QVariantMap& options,
                                            const QString&     preset_id) const -> QVariantMap {
  for (const auto& preset : Presets()) {
    if (preset.id == preset_id) {
      return ToVariantMap(ApplyPreset(FromVariantMap(options), preset));
    }
  }
  return options;
}

auto ExportRecipeBuilder::SavePreset(const QString& name, const QVariantMap& options,
                                     const QString& replaced_id) -> QVariantMap {
  const auto result = SaveUserPreset(name, FromVariantMap(options), replaced_id);
  if (const auto* error = std::get_if<ExportRecipeError>(&result)) {
    return QVariantMap{{QStringLiteral("saved"), false},
                       {QStringLiteral("id"), QString()},
                       {QStringLiteral("field"), error->field},
                       {QStringLiteral("message"), error->message}};
  }
  return QVariantMap{{QStringLiteral("saved"), true},
                     {QStringLiteral("id"), std::get<QString>(result)},
                     {QStringLiteral("field"), QString()},
                     {QStringLiteral("message"), QString()}};
}

auto ExportRecipeBuilder::DeletePreset(const QString& id) -> bool { return DeleteUserPreset(id); }

auto ExportRecipeBuilder::LastUsedOptionsMap() const -> QVariantMap {
  return ToVariantMap(LastUsedOptions());
}

void ExportRecipeBuilder::SaveLastUsedOptionsMap(const QVariantMap& options) {
  SaveLastUsedOptions(FromVariantMap(options));
}

void ExportRecipeBuilder::SetInteractionPolicy(InteractionPolicyController* interaction_policy) {
  interaction_policy_ = interaction_policy;
}

auto ExportRecipeBuilder::StartExport(const QVariantMap& options, const QVariantList& targets)
    -> QVariantMap {
  if (const QString refusal = UserStateChangeRefusal(interaction_policy_); !refusal.isEmpty()) {
    return ErrorMap(ExportRecipeError{QString(), refusal}, QStringLiteral("started"));
  }
  return ErrorMap(Start(FromVariantMap(options), targets), QStringLiteral("started"));
}

}  // namespace alcedo::ui
