//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <exception>
#include <string_view>
#include <utility>

#include "app/film_stock_presentation.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film/film_print_tables.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_film_commands.hpp"

namespace alcedo::automation {
namespace {

constexpr auto kFilmResourcesUnavailable = "film_resources_unavailable";

auto           Text(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto ListName(FilmBrowserList list) -> QString {
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

/// The catalog name of @p entry: the label of `negative` or `print_material` in editor.catalog.
auto CatalogName(const FilmPresentation& entry) -> std::string_view {
  return entry.list == FilmBrowserList::Print ? FilmPrintMaterialCatalogName(entry.id)
                                              : FilmStockCatalogName(entry.id);
}

/// The `odt` property that the browser entry sets.
auto PropertyName(const FilmPresentation& entry) -> QString {
  return entry.list == FilmBrowserList::Print ? QStringLiteral("print_material")
                                              : QStringLiteral("negative");
}

/// True when @p entry passes the filters `list`, `brand`, and `query` of @p params.
auto Matches(const FilmPresentation& entry, const QJsonObject& params) -> bool {
  if (params.contains("list") && params.value("list").toString() != ListName(entry.list)) {
    return false;
  }
  if (params.contains("brand") &&
      params.value("brand").toString().compare(Text(entry.brand), Qt::CaseInsensitive) != 0) {
    return false;
  }
  const QString query = params.value("query").toString().trimmed();
  if (query.isEmpty()) {
    return true;
  }
  for (const std::string_view key :
       {CatalogName(entry), entry.short_label, entry.family, entry.id}) {
    if (Text(key).contains(query, Qt::CaseInsensitive)) {
      return true;
    }
  }
  return false;
}

/// One entry of the result. @p bundle is null when the resources are not available; the entry
/// then has no `balance_light`. A colour reversal film has none: the runtime film catalog holds
/// the balance light of the negatives only.
auto FilmObject(const FilmPresentation& entry, const FilmStockBundle* bundle) -> QJsonObject {
  QJsonObject film{{"id", Text(entry.id)},
                   {"name", Text(CatalogName(entry))},
                   {"list", ListName(entry.list)},
                   {"brand", Text(entry.brand)},
                   {"family", Text(entry.family)},
                   {"short_label", Text(entry.short_label)},
                   {"editor_value", QJsonObject{{PropertyName(entry), Text(entry.id)}}}};
  if (bundle != nullptr && entry.list == FilmBrowserList::Negative) {
    film.insert("balance_light", QString::fromStdString(bundle->ScanBalanceLight(entry.id)));
  }
  return film;
}

auto ParamsSchema() -> QJsonObject {
  return AutomationClosedParamsSchema(QJsonObject{
      {"list", QJsonObject{{"type", "string"},
                           {"enum", QJsonArray{"negative", "reversal", "print"}},
                           {"description",
                            "Only the entries of this list of the film print browser: the "
                            "negatives, the colour reversal films, or the print materials."}}},
      {"brand",
       QJsonObject{{"type", "string"},
                   {"description",
                    "Only the entries of this brand, for example Kodak (case-insensitive). An "
                    "empty string selects negative_scan and cine_look, which have no brand."}}},
      {"query", QJsonObject{{"type", "string"},
                            {"description",
                             "Case-insensitive text that the name, the short label, the family, "
                             "or the identifier contains."}}}});
}

auto ResultSchema() -> QJsonObject {
  const QJsonObject text{{"type", "string"}};
  const QJsonObject film = AutomationObjectSchema(
      QJsonObject{{"id", text},
                  {"name", text},
                  {"list", QJsonObject{{"type", "string"},
                                       {"enum", QJsonArray{"negative", "reversal", "print"}}}},
                  {"brand", text},
                  {"family", text},
                  {"short_label", text},
                  {"balance_light", text},
                  {"editor_value", QJsonObject{{"type", "object"}}}},
      QJsonArray{"id", "name", "list", "brand", "family", "short_label", "editor_value"});
  const QJsonObject warning = AutomationObjectSchema(QJsonObject{{"code", text}, {"message", text}},
                                                     QJsonArray{"code", "message"});
  return AutomationObjectSchema(
      QJsonObject{{"total", QJsonObject{{"type", "integer"}}},
                  {"films", QJsonObject{{"type", "array"}, {"items", film}}},
                  {"warnings", QJsonObject{{"type", "array"}, {"items", warning}}}},
      QJsonArray{"total", "films", "warnings"});
}

void SendList(const FilmStockBundleReader& read_bundle, const QJsonObject& params,
              AutomationReply& reply) {
  const FilmStockBundle* bundle = nullptr;
  QJsonArray             warnings;
  try {
    bundle = &read_bundle();
  } catch (const std::exception& error) {
    warnings.push_back(QJsonObject{
        {"code", QString::fromLatin1(kFilmResourcesUnavailable)},
        {"message", QStringLiteral("The film print resources are not available, so the entries "
                                   "have no balance_light, and a film print render fails: %1")
                        .arg(QString::fromUtf8(error.what()))}});
  }
  QJsonArray films;
  try {
    for (const auto& entry : kFilmPresentations) {
      if (Matches(entry, params)) {
        films.push_back(FilmObject(entry, bundle));
      }
    }
  } catch (const std::exception& error) {
    // The bundle has no balance light of a listed negative: the resources do not match the film
    // lists of this build.
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromUtf8(error.what()));
    return;
  }
  reply.SendResult(QJsonObject{{"total", films.size()}, {"films", films}, {"warnings", warnings}});
}

}  // namespace

auto RegisterAutomationFilmCommands(AutomationCommandRegistry& registry, QString* error) -> bool {
  return RegisterAutomationFilmCommands(
      registry, []() -> const FilmStockBundle& { return SharedFilmPrintTables().Bundle(); }, error);
}

auto RegisterAutomationFilmCommands(AutomationCommandRegistry& registry,
                                    FilmStockBundleReader read_bundle, QString* error) -> bool {
  AutomationCommandSpec list;
  list.method      = QStringLiteral("film.list");
  list.description = QStringLiteral(
      "Lists the films and print materials of the film print browser in browser order: the "
      "negatives, the colour reversal films, then the print materials, each list by brand and "
      "family. 'name' is the name of the option in editor.catalog, 'short_label' the name "
      "without the brand. A negative has 'balance_light', the light that the film is balanced "
      "for (D55 daylight or T3200 tungsten). 'editor_value' is the odt property that the entry "
      "sets; give it to editor.set on 'odt' with \"method\": \"film_print\" (a colour reversal "
      "film reads no print material). When the film print resources are not available, "
      "'warnings' has film_resources_unavailable and no entry has 'balance_light'. Needs no "
      "open project.");
  list.params_schema = ParamsSchema();
  list.result_schema = ResultSchema();
  list.handler       = [read_bundle = std::move(read_bundle)](const QJsonObject& params,
                                                        AutomationReply    reply) {
    SendList(read_bundle, params, reply);
  };
  return registry.Register(std::move(list), error);
}

}  // namespace alcedo::automation
