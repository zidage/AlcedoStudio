//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_behavior_preferences.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QSettings>
#include <exception>
#include <json.hpp>
#include <optional>

#include "app/editor_parameter_catalog.hpp"
#include "edit/graph/drt_node_model.hpp"

namespace alcedo::ui {
namespace {

constexpr auto kFilmPrintMethod = "film_print";

/// The `film_print` object @p film_print in the form of DrtParamsModel::ToJson, read through the
/// load of the Model. Nullopt when the Model does not load it.
auto CanonicalFilmPrint(const nlohmann::json& film_print) -> std::optional<nlohmann::json> {
  if (!film_print.is_object()) {
    return std::nullopt;
  }
  DrtParamsModel model;
  try {
    model.LoadJson({{"method", kFilmPrintMethod}, {"film_print", film_print}});
  } catch (const std::exception&) {
    return std::nullopt;
  }
  const auto json = model.ToJson();
  const auto it   = json.find("film_print");
  if (it == json.end()) {
    return std::nullopt;
  }
  return *it;
}

/// The `odt` UI value of method `film_print` with the canonical object @p film_print. Empty when
/// the catalog does not read it.
auto FilmPrintUiValue(const nlohmann::json& film_print) -> QVariantMap {
  std::string error;
  const auto  ui = EditorParameterCatalog::ToUiValue(
      "odt", {{"odt", {{"method", kFilmPrintMethod}, {"film_print", film_print}}}}, &error);
  if (!ui.has_value()) {
    return {};
  }
  return QJsonDocument::fromJson(QByteArray::fromStdString(ui->dump())).object().toVariantMap();
}

}  // namespace

EditorBehaviorPreferences::EditorBehaviorPreferences(QObject* parent) : QObject(parent) {
  const QSettings settings;
  const QString   stored =
      settings.value(QLatin1String(kLockedNodeMaskActionSettingKey), QLatin1String(kAskAction))
          .toString();
  locked_node_mask_action_ =
      IsLockedNodeMaskAction(stored) ? stored : QString::fromLatin1(kAskAction);

  const auto film_text =
      settings.value(QLatin1String(kFilmPrintSettingsKey)).toString().toStdString();
  if (!film_text.empty()) {
    const auto film_print =
        CanonicalFilmPrint(nlohmann::json::parse(film_text, nullptr, /*allow_exceptions=*/false));
    if (film_print.has_value()) {
      film_print_json_     = film_print->dump();
      film_print_ui_value_ = FilmPrintUiValue(*film_print);
    }
  }
}

auto EditorBehaviorPreferences::IsLockedNodeMaskAction(const QString& action) -> bool {
  return action == QLatin1String(kAskAction) || action == QLatin1String(kNewLayerAction) ||
         action == QLatin1String(kCurrentNodeAction);
}

bool EditorBehaviorPreferences::setLockedNodeMaskAction(const QString& action) {
  if (!IsLockedNodeMaskAction(action)) {
    return false;
  }
  if (action == locked_node_mask_action_) {
    return true;
  }
  locked_node_mask_action_ = action;
  QSettings{}.setValue(QLatin1String(kLockedNodeMaskActionSettingKey), locked_node_mask_action_);
  emit lockedNodeMaskActionChanged();
  return true;
}

bool EditorBehaviorPreferences::rememberFilmPrint(const QString& odtModelJson) {
  const auto parsed =
      nlohmann::json::parse(odtModelJson.toStdString(), nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) {
    return false;
  }
  const auto  wrapped = parsed.find("odt");
  const auto& odt     = wrapped != parsed.end() ? *wrapped : parsed;
  if (!odt.is_object()) {
    return false;
  }
  const auto method = odt.find("method");
  if (method == odt.end() || *method != kFilmPrintMethod || !odt.contains("film_print")) {
    return false;
  }
  const auto film_print = CanonicalFilmPrint(odt.at("film_print"));
  if (!film_print.has_value()) {
    return false;
  }
  auto text = film_print->dump();
  if (text == film_print_json_) {
    return true;
  }
  auto ui_value = FilmPrintUiValue(*film_print);
  if (ui_value.isEmpty()) {
    return false;
  }
  QSettings{}.setValue(QLatin1String(kFilmPrintSettingsKey), QString::fromStdString(text));
  film_print_json_     = std::move(text);
  film_print_ui_value_ = std::move(ui_value);
  emit filmPrintUiValueChanged();
  return true;
}

}  // namespace alcedo::ui
