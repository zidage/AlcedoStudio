//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// editor.catalog and film.list through AutomationCommandRegistry::Dispatch, the path of every
// socket request.

#include "ui/alcedo_main/automation/automation_editor_commands.hpp"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/editor_parameter_catalog.hpp"
#include "app/film_stock_presentation.hpp"
#include "automation/automation_protocol.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "edit/runtime/film/film_stock_bundle.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_curve_commands.hpp"
#include "ui/alcedo_main/automation/automation_film_commands.hpp"

namespace alcedo::automation {
namespace {

/// Dispatches one request and returns the responses that it sent.
auto Dispatch(const AutomationCommandRegistry& registry, const QString& method,
              QJsonObject params = {}) -> std::vector<AutomationResponse> {
  std::vector<AutomationResponse> responses;
  registry.Dispatch(AutomationRequest{QJsonValue(1), method, std::move(params)},
                    AutomationReply(QJsonValue(1), [&responses](AutomationResponse response) {
                      responses.push_back(std::move(response));
                    }));
  return responses;
}

auto FindField(const QJsonArray& fields, const QString& name) -> QJsonObject {
  for (const auto& field : fields) {
    if (field.toObject().value("field").toString() == name) {
      return field.toObject();
    }
  }
  return {};
}

TEST(AutomationEditorCommandsTest, EditorCatalogListsEveryFieldInUiUnits) {
  AutomationCommandRegistry registry;
  QString                   error;
  ASSERT_TRUE(RegisterAutomationEditorCatalogCommand(registry, &error)) << error.toStdString();

  const auto responses = Dispatch(registry, QStringLiteral("editor.catalog"));
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_FALSE(responses[0].error.has_value()) << responses[0].error->message.toStdString();
  const QJsonArray fields = responses[0].result.toObject().value("fields").toArray();
  ASSERT_EQ(static_cast<std::size_t>(fields.size()), EditorParameterCatalog::Entries().size());
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    const QJsonObject field = FindField(fields, QString::fromUtf8(entry.field.data(),
                                                                  static_cast<int>(entry.field.size())));
    ASSERT_FALSE(field.isEmpty()) << entry.field;
    if (entry.kind == EditorParameterValueKind::Model) {
      EXPECT_EQ(field.value("kind").toString(), QStringLiteral("model"));
      EXPECT_FALSE(field.value("model_shape").toString().isEmpty()) << entry.field;
      EXPECT_TRUE(field.value("ui_default").isObject()) << entry.field;
      continue;
    }
    if (entry.kind == EditorParameterValueKind::Object) {
      EXPECT_EQ(field.value("kind").toString(), QStringLiteral("object"));
      EXPECT_EQ(static_cast<std::size_t>(field.value("properties").toArray().size()),
                entry.properties.size())
          << entry.field;
      EXPECT_TRUE(field.value("ui_default").isObject()) << entry.field;
      continue;
    }
    EXPECT_EQ(field.value("kind").toString(), QStringLiteral("scalar"));
    EXPECT_DOUBLE_EQ(field.value("ui_min").toDouble(), entry.range.minimum) << entry.field;
    EXPECT_DOUBLE_EQ(field.value("ui_max").toDouble(), entry.range.maximum) << entry.field;
    EXPECT_DOUBLE_EQ(field.value("ui_default").toDouble(), entry.range.default_value)
        << entry.field;
    EXPECT_DOUBLE_EQ(field.value("ui_step").toDouble(), entry.range.step) << entry.field;
  }
  const QJsonObject saturation = FindField(fields, QStringLiteral("saturation"));
  EXPECT_EQ(saturation.value("panel").toString(), QStringLiteral("look"));
  EXPECT_DOUBLE_EQ(saturation.value("ui_min").toDouble(), -100.0);
  EXPECT_DOUBLE_EQ(saturation.value("ui_max").toDouble(), 100.0);
  const QJsonObject raw = FindField(fields, QStringLiteral("raw_decode"));
  EXPECT_EQ(raw.value("panel").toString(), QStringLiteral("raw"));
  const QJsonObject method = raw.value("properties").toArray().first().toObject();
  EXPECT_EQ(method.value("name").toString(), QStringLiteral("method"));
  EXPECT_EQ(method.value("type").toString(), QStringLiteral("option"));
  EXPECT_EQ(method.value("options").toArray().size(), 3);
}

TEST(AutomationEditorCommandsTest, EditorCatalogRejectsUnknownParameter) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(RegisterAutomationEditorCatalogCommand(registry));

  const auto responses =
      Dispatch(registry, QStringLiteral("editor.catalog"), QJsonObject{{"field", "saturation"}});
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_TRUE(responses[0].error.has_value());
  EXPECT_EQ(responses[0].error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(responses[0].error->data.toObject().value("pointer").toString(),
            QStringLiteral("/field"));
}

// film.list (issue #718) --------------------------------------------------------------------

/// A bundle reader with no resource directory: it fails as the render does without resources.
auto MissingBundle() -> const FilmStockBundle& {
  static const std::shared_ptr<const FilmStockBundle> bundle =
      FilmStockBundle::Open(std::filesystem::path("no_film_print_resources_here"));
  return *bundle;
}

/// The film.list result of @p params with @p read_bundle. Fails the test on an error.
auto FilmList(FilmStockBundleReader read_bundle, QJsonObject params = {}) -> QJsonObject {
  AutomationCommandRegistry registry;
  QString                   error;
  EXPECT_TRUE(RegisterAutomationFilmCommands(registry, std::move(read_bundle), &error))
      << error.toStdString();
  const auto responses = Dispatch(registry, QStringLiteral("film.list"), std::move(params));
  EXPECT_EQ(responses.size(), 1U);
  if (responses.size() != 1U) {
    return {};
  }
  EXPECT_FALSE(responses[0].error.has_value()) << responses[0].error->message.toStdString();
  return responses[0].result.toObject();
}

auto FilmIds(const QJsonObject& result) -> QStringList {
  QStringList ids;
  for (const auto& film : result.value("films").toArray()) {
    ids.push_back(film.toObject().value("id").toString());
  }
  return ids;
}

/// The identifiers of the browser entries that @p keep accepts, in browser order.
template <typename Keep>
auto PresentationIds(Keep keep) -> QStringList {
  QStringList ids;
  for (const auto& entry : kFilmPresentations) {
    if (keep(entry)) {
      ids.push_back(QString::fromUtf8(entry.id.data(), static_cast<qsizetype>(entry.id.size())));
    }
  }
  return ids;
}

auto ToQString(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

TEST(AutomationEditorCommandsTest, FilmListReturnsEveryFilmIdentifierOnceInBrowserOrder) {
  const QJsonObject result = FilmList(MissingBundle);
  const QStringList ids    = FilmIds(result);

  std::set<QString> expected;
  for (const auto& list : {std::span<const std::string_view>(kFilmNegativeIds),
                           std::span<const std::string_view>(kFilmReversalIds),
                           std::span<const std::string_view>(kFilmPrintMaterialIds)}) {
    for (const auto id : list) {
      expected.insert(ToQString(id));
    }
  }
  EXPECT_EQ(static_cast<std::size_t>(ids.size()), expected.size());
  EXPECT_EQ(std::set<QString>(ids.begin(), ids.end()), expected);
  EXPECT_EQ(ids, PresentationIds([](const FilmPresentation&) { return true; }));
  EXPECT_EQ(result.value("total").toInt(), ids.size());
}

TEST(AutomationEditorCommandsTest, FilmListEntryHasTheBrowserFieldsAndTheCatalogName) {
  const QJsonObject result = FilmList(MissingBundle, QJsonObject{{"query", "kodak_portra_400"}});
  ASSERT_EQ(FilmIds(result), QStringList{QStringLiteral("kodak_portra_400")});
  const QJsonObject film = result.value("films").toArray().first().toObject();
  EXPECT_EQ(film.value("name").toString(), QStringLiteral("Kodak Portra 400"));
  EXPECT_EQ(film.value("list").toString(), QStringLiteral("negative"));
  EXPECT_EQ(film.value("brand").toString(), QStringLiteral("Kodak"));
  EXPECT_EQ(film.value("family").toString(), QStringLiteral("Photo"));
  EXPECT_EQ(film.value("short_label").toString(), QStringLiteral("Portra 400"));
  EXPECT_EQ(film.value("editor_value").toObject(), (QJsonObject{{"negative", "kodak_portra_400"}}));

  const QJsonObject print = FilmList(MissingBundle, QJsonObject{{"query", "kodak_2383"}});
  ASSERT_EQ(FilmIds(print), QStringList{QStringLiteral("kodak_2383")});
  const QJsonObject paper = print.value("films").toArray().first().toObject();
  EXPECT_EQ(paper.value("name").toString(), QStringLiteral("Kodak Vision 2383"));
  EXPECT_EQ(paper.value("family").toString(), QStringLiteral("Cinema Print"));
  EXPECT_EQ(paper.value("editor_value").toObject(),
            (QJsonObject{{"print_material", "kodak_2383"}}));

  // Negative Scan and Cine Look are above the brand groups, as in the browser.
  const QJsonObject scan = FilmList(MissingBundle, QJsonObject{{"query", "negative_scan"}});
  ASSERT_EQ(FilmIds(scan), QStringList{QStringLiteral("negative_scan")});
  EXPECT_EQ(scan.value("films").toArray().first().toObject().value("brand").toString(), QString());
}

TEST(AutomationEditorCommandsTest, FilmListFiltersByListBrandAndQuery) {
  EXPECT_EQ(FilmIds(FilmList(MissingBundle, QJsonObject{{"list", "reversal"}})),
            PresentationIds([](const FilmPresentation& entry) {
              return entry.list == FilmBrowserList::Reversal;
            }));
  EXPECT_EQ(FilmIds(FilmList(MissingBundle, QJsonObject{{"list", "print"}})).size(),
            static_cast<qsizetype>(kFilmPrintMaterialIds.size()));
  EXPECT_EQ(FilmIds(FilmList(MissingBundle, QJsonObject{{"list", "negative"}})).size(),
            static_cast<qsizetype>(kFilmNegativeIds.size()));

  // The brand is case-insensitive; an empty brand selects the entries above the brand groups.
  EXPECT_EQ(
      FilmIds(FilmList(MissingBundle, QJsonObject{{"brand", "fujifilm"}})),
      PresentationIds([](const FilmPresentation& entry) { return entry.brand == "Fujifilm"; }));
  EXPECT_EQ(FilmIds(FilmList(MissingBundle, QJsonObject{{"brand", ""}})),
            (QStringList{QStringLiteral("negative_scan"), QStringLiteral("cine_look")}));

  // The query matches the name, the short label, the family, and the identifier.
  EXPECT_EQ(
      FilmIds(FilmList(MissingBundle, QJsonObject{{"query", "PORTRA"}})),
      (QStringList{QStringLiteral("kodak_portra_400"), QStringLiteral("kodak_portra_160"),
                   QStringLiteral("kodak_portra_800"), QStringLiteral("kodak_portra_endura")}));
  EXPECT_EQ(FilmIds(FilmList(MissingBundle, QJsonObject{{"query", "classic cinema"}})),
            PresentationIds(
                [](const FilmPresentation& entry) { return entry.family == "Classic Cinema"; }));
  EXPECT_EQ(FilmIds(FilmList(MissingBundle, QJsonObject{{"query", "Vision3 500T"}})),
            QStringList{QStringLiteral("kodak_5219")});

  // The filters combine.
  EXPECT_EQ(
      FilmIds(FilmList(MissingBundle,
                       QJsonObject{{"list", "print"}, {"brand", "Kodak"}, {"query", "paper"}})),
      (QStringList{QStringLiteral("kodak_endura_premier"), QStringLiteral("kodak_portra_endura"),
                   QStringLiteral("kodak_supra_endura")}));
}

// curve.presets (issue #891) -----------------------------------------------------------------

TEST(AutomationEditorCommandsTest, CurvePresetsListsThePresetsWithTheirPointsAndEditorValue) {
  AutomationCommandRegistry registry;
  QString                   error;
  ASSERT_TRUE(RegisterAutomationCurveCommands(registry, &error)) << error.toStdString();
  const auto responses = Dispatch(registry, QStringLiteral("curve.presets"), QJsonObject{});
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_FALSE(responses[0].error.has_value()) << responses[0].error->message.toStdString();
  const auto result  = responses[0].result.toObject();
  const auto presets = result.value("presets").toArray();
  const auto data    = ToneCurvePresets();
  ASSERT_EQ(static_cast<std::size_t>(presets.size()), data.size());
  EXPECT_EQ(result.value("total").toInt(), presets.size());
  for (qsizetype i = 0; i < presets.size(); ++i) {
    const auto preset = presets.at(i).toObject();
    const auto id     = preset.value("id").toString();
    EXPECT_EQ(id.toStdString(), data[static_cast<std::size_t>(i)].id);
    EXPECT_EQ(preset.value("name").toString().toStdString(),
              data[static_cast<std::size_t>(i)].display_name);
    EXPECT_EQ(static_cast<std::size_t>(preset.value("points").toArray().size()),
              data[static_cast<std::size_t>(i)].points.size());
    EXPECT_EQ(preset.value("editor_value").toObject(), (QJsonObject{{"points", id}}));
  }
  EXPECT_EQ(presets.at(4).toObject().value("id").toString(), QStringLiteral("film_like"));
  EXPECT_EQ(presets.at(4).toObject().value("group").toString(), QStringLiteral("Rendering"));
}

TEST(AutomationEditorCommandsTest, FilmListRejectsAnUnknownList) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(RegisterAutomationFilmCommands(registry, MissingBundle));
  const auto responses =
      Dispatch(registry, QStringLiteral("film.list"), QJsonObject{{"list", "slide"}});
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_TRUE(responses[0].error.has_value());
  EXPECT_EQ(responses[0].error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(responses[0].error->data.toObject().value("pointer").toString(),
            QStringLiteral("/list"));
}

TEST(AutomationEditorCommandsTest, FilmListWithoutResourcesWarnsAndHasNoBalanceLight) {
  const QJsonObject result   = FilmList(MissingBundle);
  const QJsonArray  warnings = result.value("warnings").toArray();
  ASSERT_EQ(warnings.size(), 1);
  EXPECT_EQ(warnings.first().toObject().value("code").toString(),
            QStringLiteral("film_resources_unavailable"));
  EXPECT_FALSE(warnings.first().toObject().value("message").toString().isEmpty());
  const QJsonArray films = result.value("films").toArray();
  ASSERT_EQ(static_cast<std::size_t>(films.size()), kFilmPresentations.size());
  for (const auto& film : films) {
    const QJsonObject entry = film.toObject();
    EXPECT_FALSE(entry.contains("balance_light")) << entry.value("id").toString().toStdString();
    EXPECT_FALSE(entry.value("name").toString().isEmpty())
        << entry.value("id").toString().toStdString();
    EXPECT_TRUE(entry.contains("list") && entry.contains("brand") && entry.contains("family") &&
                entry.contains("short_label") && entry.contains("editor_value"));
  }
}

#ifdef ALCEDO_FILM_PRINT_RESOURCE_DIR
TEST(AutomationEditorCommandsTest, FilmListGivesEachNegativeTheBalanceLightOfTheFilmCatalog) {
  static const std::shared_ptr<const FilmStockBundle> bundle =
      FilmStockBundle::Open(std::filesystem::path(ALCEDO_FILM_PRINT_RESOURCE_DIR));
  const QJsonObject result = FilmList([]() -> const FilmStockBundle& { return *bundle; });
  EXPECT_TRUE(result.value("warnings").toArray().isEmpty());
  for (const auto& film : result.value("films").toArray()) {
    const QJsonObject entry = film.toObject();
    const std::string id    = entry.value("id").toString().toStdString();
    if (entry.value("list").toString() == QStringLiteral("negative")) {
      EXPECT_EQ(entry.value("balance_light").toString().toStdString(), bundle->ScanBalanceLight(id))
          << id;
    } else {
      // The runtime film catalog holds the balance light of the negatives only.
      EXPECT_FALSE(entry.contains("balance_light")) << id;
    }
  }
  const QJsonObject tungsten = FilmList([]() -> const FilmStockBundle& { return *bundle; },
                                        QJsonObject{{"query", "kodak_5219"}});
  EXPECT_EQ(tungsten.value("films").toArray().first().toObject().value("balance_light"),
            QJsonValue(QStringLiteral("T3200")));
  const QJsonObject daylight = FilmList([]() -> const FilmStockBundle& { return *bundle; },
                                        QJsonObject{{"query", "kodak_portra_400"}});
  EXPECT_EQ(daylight.value("films").toArray().first().toObject().value("balance_light"),
            QJsonValue(QStringLiteral("D55")));
}
#endif

}  // namespace
}  // namespace alcedo::automation
