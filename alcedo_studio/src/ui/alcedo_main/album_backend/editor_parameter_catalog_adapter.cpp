//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_parameter_catalog_adapter.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <cstdint>
#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "app/editor_parameter_catalog.hpp"
#include "app/film_stock_presentation.hpp"
#include "edit/graph/film_print_model.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {
namespace {

auto ToJson(const QVariantMap& map) -> nlohmann::json {
  const QByteArray bytes =
      QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact);
  return nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
}

auto ToVariantMap(const nlohmann::json& json) -> QVariantMap {
  return QJsonDocument::fromJson(QByteArray::fromStdString(json.dump())).object().toVariantMap();
}

auto ToQString(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto ColorName(std::uint32_t rgb) -> QString {
  return QStringLiteral("#%1").arg(rgb, 6, 16, QLatin1Char('0'));
}

auto FilmListName(FilmBrowserList list) -> QString {
  switch (list) {
    case FilmBrowserList::Negative:
      return QStringLiteral("negative");
    case FilmBrowserList::Reversal:
      return QStringLiteral("reversal");
    case FilmBrowserList::Print:
      return QStringLiteral("print");
  }
  return {};
}

auto SwatchShapeName(FilmSwatchShape shape) -> QString {
  switch (shape) {
    case FilmSwatchShape::PhotoNegative:
      return QStringLiteral("photo_negative");
    case FilmSwatchShape::CinemaNegative:
      return QStringLiteral("cinema_negative");
    case FilmSwatchShape::Reversal:
      return QStringLiteral("reversal");
    case FilmSwatchShape::PrintPaper:
      return QStringLiteral("print_paper");
    case FilmSwatchShape::CinemaPrint:
      return QStringLiteral("cinema_print");
    case FilmSwatchShape::DisplayMaterial:
      return QStringLiteral("display_material");
    case FilmSwatchShape::Scan:
      return QStringLiteral("scan");
  }
  return {};
}

auto ToVariantMap(const FilmPresentation& entry) -> QVariantMap {
  const std::string_view label = entry.list == FilmBrowserList::Print
                                     ? FilmPrintMaterialCatalogName(entry.id)
                                     : FilmStockCatalogName(entry.id);
  QVariantList colors;
  for (std::size_t i = 0; i < entry.color_count; ++i) {
    colors.push_back(ColorName(entry.colors[i]));
  }
  QVariantMap map;
  map.insert(QStringLiteral("value"), ToQString(entry.id));
  map.insert(QStringLiteral("label"), ToQString(label));
  map.insert(QStringLiteral("list"), FilmListName(entry.list));
  map.insert(QStringLiteral("brand"), ToQString(entry.brand));
  map.insert(QStringLiteral("family"), ToQString(entry.family));
  map.insert(QStringLiteral("shortLabel"), ToQString(entry.short_label));
  map.insert(QStringLiteral("shape"), SwatchShapeName(entry.shape));
  map.insert(QStringLiteral("colors"), colors);
  return map;
}

auto ToSource(int width, int height) -> EditorParameterSource {
  return EditorParameterSource{static_cast<std::uint32_t>(std::max(width, 0)),
                               static_cast<std::uint32_t>(std::max(height, 0))};
}

auto ParseCropDriver(const QString& driver) -> EditorCropDriver {
  if (driver == QStringLiteral("position")) {
    return EditorCropDriver::Position;
  }
  if (driver == QStringLiteral("width")) {
    return EditorCropDriver::Width;
  }
  if (driver == QStringLiteral("height")) {
    return EditorCropDriver::Height;
  }
  if (driver == QStringLiteral("rotation")) {
    return EditorCropDriver::Rotation;
  }
  if (driver == QStringLiteral("aspect_preset")) {
    return EditorCropDriver::AspectPreset;
  }
  if (driver == QStringLiteral("aspect_size")) {
    return EditorCropDriver::AspectSize;
  }
  return EditorCropDriver::None;
}

/// The complete crop UI value of @p state, or nullopt when @p state is not valid.
auto ValidCropState(const QVariantMap& state, std::string* error) -> std::optional<nlohmann::json> {
  const auto* entry = EditorParameterCatalog::Find("crop_rotate");
  const auto  ui    = ToJson(state);
  if (!EditorParameterCatalog::ValidateObjectUiValue(*entry, ui, error)) {
    return std::nullopt;
  }
  nlohmann::json complete = EditorParameterCatalog::UiDefault(*entry);
  complete.update(ui);
  return complete;
}

}  // namespace

EditorParameterCatalogAdapter::EditorParameterCatalogAdapter(QObject* parent) : QObject(parent) {}

QVariantMap EditorParameterCatalogAdapter::entry(const QString& field) const {
  const auto* found = EditorParameterCatalog::Find(field.toStdString());
  return found != nullptr ? ToVariantMap(EditorParameterCatalog::EntryJson(*found)) : QVariantMap{};
}

QVariantMap EditorParameterCatalogAdapter::propertySpec(const QString& field,
                                                        const QString& name) const {
  const QVariantList properties = entry(field).value(QStringLiteral("properties")).toList();
  for (const QVariant& property : properties) {
    const QVariantMap map = property.toMap();
    if (map.value(QStringLiteral("name")).toString() == name) {
      return map;
    }
  }
  return {};
}

QVariantList EditorParameterCatalogAdapter::options(const QString& field,
                                                    const QString& name) const {
  return propertySpec(field, name).value(QStringLiteral("options")).toList();
}

QVariantList EditorParameterCatalogAdapter::odtEotfOptions(const QString& encodingSpace) const {
  QVariantList result;
  for (const auto& option : EditorParameterCatalog::OdtEotfOptions(encodingSpace.toStdString())) {
    QVariantMap map;
    map.insert(QStringLiteral("value"), QString::fromUtf8(option.value.data(),
                                                          static_cast<qsizetype>(option.value.size())));
    map.insert(QStringLiteral("label"), QString::fromUtf8(option.label.data(),
                                                          static_cast<qsizetype>(option.label.size())));
    result.push_back(map);
  }
  return result;
}

bool EditorParameterCatalogAdapter::filmIsReversal(const QString& film) const {
  return IsFilmReversalIdentifier(film.toStdString());
}

bool EditorParameterCatalogAdapter::filmIsNegativeScan(const QString& film,
                                                  const QString& printMaterial) const {
  return IsFilmNegativeScan(film.toStdString(), printMaterial.toStdString());
}

QVariantList EditorParameterCatalogAdapter::filmBrowserEntries(const QString& list) const {
  QVariantList result;
  for (const auto& entry : kFilmPresentations) {
    if (FilmListName(entry.list) == list) {
      result.push_back(ToVariantMap(entry));
    }
  }
  return result;
}

QVariantMap EditorParameterCatalogAdapter::filmBrowserEntry(const QString& id) const {
  const auto entry = FindFilmPresentation(id.toStdString());
  return entry ? ToVariantMap(*entry) : QVariantMap{};
}

QVariantList EditorParameterCatalogAdapter::filmBrands() const {
  QVariantList result;
  for (const auto& brand : kFilmBrands) {
    QVariantMap map;
    map.insert(QStringLiteral("name"), ToQString(brand.name));
    map.insert(QStringLiteral("color"), ColorName(brand.color));
    result.push_back(map);
  }
  return result;
}

QString EditorParameterCatalogAdapter::filmDefault(const QString& list) const {
  if (list == QStringLiteral("reversal")) {
    return ToQString(kFilmDefaultReversal);
  }
  return ToQString(kFilmDefaultNegative);
}

QVariant EditorParameterCatalogAdapter::filmGrainAfterMethodChange(const QString& previousMethod,
                                                                   const QString& method,
                                                                   double         filmGrain) const {
  const auto grain = EditorParameterCatalog::FilmGrainAfterMethodChange(
      previousMethod.toStdString(), method.toStdString(), filmGrain);
  return grain ? QVariant(*grain) : QVariant{};
}

QVariantMap EditorParameterCatalogAdapter::uiDefault(const QString& field) const {
  const auto* found = EditorParameterCatalog::Find(field.toStdString());
  if (found == nullptr || found->kind != EditorParameterValueKind::Object) {
    return {};
  }
  return ToVariantMap(EditorParameterCatalog::UiDefault(*found));
}

QVariantMap EditorParameterCatalogAdapter::uiValue(const QString&  field,
                                                   const QVariant& snapshotEntry) const {
  std::string error;
  const auto  ui = EditorParameterCatalog::ToUiValue(
      field.toStdString(), ToJson(snapshotEntry.toMap()), &error);
  if (!ui.has_value() || !ui->is_object()) {
    qCWarning(alcedo::diag::editorLog, "EditorParameterCatalog: %s", error.c_str());
    return {};
  }
  return ToVariantMap(*ui);
}

QString EditorParameterCatalogAdapter::modelParamsJson(const QString&     field,
                                                       const QVariantMap& uiState) const {
  std::string error;
  const auto  model =
      EditorParameterCatalog::UiStateToModelJson(field.toStdString(), ToJson(uiState), &error);
  if (!model.has_value()) {
    qCWarning(alcedo::diag::editorLog, "EditorParameterCatalog: %s", error.c_str());
    return {};
  }
  return QString::fromStdString(model->dump());
}

QVariantMap EditorParameterCatalogAdapter::constrainCrop(const QVariantMap& cropState,
                                                         const QString& driver, int sourceWidth,
                                                         int sourceHeight) const {
  std::string error;
  const auto  state = ValidCropState(cropState, &error);
  if (!state.has_value()) {
    qCWarning(alcedo::diag::editorLog, "EditorParameterCatalog: %s", error.c_str());
    return {};
  }
  return ToVariantMap(EditorParameterCatalog::ConstrainCrop(*state, ParseCropDriver(driver),
                                                            ToSource(sourceWidth, sourceHeight)));
}

QVariant EditorParameterCatalogAdapter::cropLockedAspectRatio(const QVariantMap& cropState,
                                                              int                sourceWidth,
                                                              int sourceHeight) const {
  std::string error;
  const auto  state = ValidCropState(cropState, &error);
  if (!state.has_value()) {
    qCWarning(alcedo::diag::editorLog, "EditorParameterCatalog: %s", error.c_str());
    return {};
  }
  const auto ratio =
      EditorParameterCatalog::CropLockedAspectRatio(*state, ToSource(sourceWidth, sourceHeight));
  return ratio.has_value() ? QVariant(*ratio) : QVariant{};
}

}  // namespace alcedo::ui
