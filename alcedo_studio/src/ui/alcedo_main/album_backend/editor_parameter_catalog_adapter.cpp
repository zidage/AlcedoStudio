//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_parameter_catalog_adapter.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>
#include <algorithm>
#include <cstdint>
#include <json.hpp>
#include <optional>
#include <string>

#include "app/editor_parameter_catalog.hpp"

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
    qWarning("EditorParameterCatalog: %s", error.c_str());
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
    qWarning("EditorParameterCatalog: %s", error.c_str());
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
    qWarning("EditorParameterCatalog: %s", error.c_str());
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
    qWarning("EditorParameterCatalog: %s", error.c_str());
    return {};
  }
  const auto ratio =
      EditorParameterCatalog::CropLockedAspectRatio(*state, ToSource(sourceWidth, sourceHeight));
  return ratio.has_value() ? QVariant(*ratio) : QVariant{};
}

}  // namespace alcedo::ui
