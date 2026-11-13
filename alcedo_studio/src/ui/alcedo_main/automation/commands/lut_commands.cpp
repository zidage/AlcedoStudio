//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/lut_library_publication.hpp"
#include "app/lut_library_service.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/lut_reference.hpp"
#include "ui/alcedo_main/album_backend/lut_library_query.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_lut_commands.hpp"
#include "utils/lut/lut_library_scan.hpp"
#include "utils/lut/lut_metadata.hpp"

namespace alcedo::automation {
namespace {

constexpr int kDefaultLutPageSize = 50;
constexpr int kMaxLutPageSize     = 500;

auto          ToQString(const std::string& text) -> QString { return QString::fromStdString(text); }

auto          CategoryName(LutCategory category) -> QString {
  return category == LutCategory::kFilmSimulation ? QStringLiteral("film_simulation")
                                                           : QStringLiteral("general");
}

/// Inserts @p key only when @p text is not empty.
void InsertText(QJsonObject& object, const char* key, const std::string& text) {
  if (!text.empty()) {
    object.insert(QLatin1String(key), ToQString(text));
  }
}

auto ReferenceObject(const LutReference& reference) -> QJsonObject {
  return QJsonDocument::fromJson(
             QByteArray::fromStdString(TaggedLutReferenceToJson(reference).dump()))
      .object();
}

/// One LUT of the result. `editor_value` is the `lut` value that the LUT browser applies for
/// this entry: the reference, the display name, and the remembered encodings (the default pair
/// when the library remembers none).
auto LutObject(const LutLibraryService& library, const LutLibraryEntry& entry) -> QJsonObject {
  const std::string  entry_id   = LutLibraryPublication::EntryIdOf(entry);
  const LutReference reference  = LutLibraryPublication::ReferenceForEntry(entry);
  const LutHeader&   header     = entry.header;
  const bool         valid      = entry.header_error == LutHeaderError::kNone;
  const bool         applicable = valid && header.SupportsGradeApplication();

  QJsonObject        lut{{"entry_id", ToQString(entry_id)},
                         {"name", ToQString(entry.DisplayName())},
                         {"file_stem", ToQString(entry.name)},
                         {"path", ToQString(entry.relative_path)},
                         {"official", entry.IsOfficial()},
                         {"favorite", library.IsFavorite(entry_id)},
                         {"applicable", applicable},
                         {"lut_3d_size", header.lut_3d_size},
                         {"reference", ReferenceObject(reference)}};
  InsertText(lut, "title", header.title);
  InsertText(lut, "package", entry.managed_package_id);
  InsertText(lut, "print_option", entry.PrintOptionName());
  if (header.lut_1d_size > 0) {
    lut.insert("lut_1d_size", header.lut_1d_size);
  }
  if (!valid) {
    lut.insert("header_error", ToQString(entry.header_message));
  }

  lut.insert("category", QStringLiteral("general"));
  if (valid && header.metadata) {
    const LutMetadata& metadata = *header.metadata;
    lut.insert("category", CategoryName(metadata.category));
    InsertText(lut, "lut_id", metadata.id);
    if (metadata.source) {
      lut.insert("source", QJsonObject{{"id", ToQString(metadata.source->id)},
                                       {"name", ToQString(metadata.source->name)}});
    }
    if (metadata.film) {
      lut.insert("film", QJsonObject{{"id", ToQString(metadata.film->id)},
                                     {"name", ToQString(metadata.film->name)},
                                     {"brand", ToQString(metadata.film->brand)}});
    }
    if (metadata.print) {
      lut.insert("print", QJsonObject{{"id", ToQString(metadata.print->id)},
                                      {"name", ToQString(metadata.print->name)},
                                      {"brand", ToQString(metadata.print->brand)},
                                      {"kind", metadata.print->kind == LutPrintKind::kPaper
                                                   ? QStringLiteral("paper")
                                                   : QStringLiteral("film")}});
    }
    InsertText(lut, "variant", metadata.variant);
    InsertText(lut, "input_space", metadata.input_space);
    InsertText(lut, "output_space", metadata.output_space);
    InsertText(lut, "description", metadata.description);
    InsertText(lut, "display_requirement", metadata.display_requirement);
  }

  const std::optional<LutRememberedEncodings> remembered = library.RememberedEncodings(entry_id);
  if (remembered) {
    lut.insert("remembered_encodings",
               QJsonObject{{"input_encoding", ToQString(remembered->input_encoding)},
                           {"output_encoding", ToQString(remembered->output_encoding)}});
  }
  if (applicable) {
    const LutRememberedEncodings encodings = remembered.value_or(LutRememberedEncodings{
        std::string(kDefaultLutEncodingId), std::string(kDefaultLutEncodingId)});
    lut.insert("editor_value",
               QJsonObject{{"reference", ReferenceObject(reference)},
                           {"name", ToQString(entry.DisplayName())},
                           {"input_encoding", ToQString(encodings.input_encoding)},
                           {"output_encoding", ToQString(encodings.output_encoding)}});
  }
  return lut;
}

/// The matching entries in the order of the LUT browser: by search rank with a query, else by
/// name; equal entries by entry ID.
auto MatchingEntries(const LutLibraryService& library, const QJsonObject& params)
    -> std::vector<ui::LutSearchKeys> {
  const ui::LutSearchQuery query = ui::ParseLutSearchQuery(params.value("query").toString());
  ui::LutFacetFilter       filter;
  const QString            category = params.value("category").toString();
  if (category == QStringLiteral("general")) {
    filter.category = ui::LutCategoryFilter::kGeneral;
  } else if (category == QStringLiteral("film_simulation")) {
    filter.category = ui::LutCategoryFilter::kFilmSimulation;
  }
  filter.favorites_only            = params.value("favorites").toBool(false);
  const bool        filter_package = params.contains("package");
  const std::string package        = params.value("package").toString().toStdString();

  struct Candidate {
    ui::LutSearchKeys keys;
    ui::LutSearchRank rank;
  };
  std::vector<Candidate>  candidates;
  ui::LutEditDistanceMemo memo(query);
  library.ForEachEntry([&](const LutLibraryEntry& entry) {
    if (filter_package && entry.managed_package_id != package) {
      return;
    }
    ui::LutSearchKeys keys = ui::BuildLutSearchKeys(entry, LutLibraryPublication::EntryIdOf(entry));
    const std::optional<ui::LutSearchRank> rank = ui::RankLutSearchKeys(keys, query, &memo);
    if (rank && ui::LutKeysPassFilter(keys, library.IsFavorite(keys.entry_id), filter)) {
      candidates.push_back({std::move(keys), *rank});
    }
  });

  const bool ranked = !query.Empty();
  std::sort(candidates.begin(), candidates.end(), [ranked](const Candidate& a, const Candidate& b) {
    if (ranked) {
      if (a.rank != b.rank) return a.rank < b.rank;
    } else if (const int order = QString::compare(a.keys.sort_name, b.keys.sort_name); order != 0) {
      return order < 0;
    }
    return a.keys.entry_id < b.keys.entry_id;
  });
  std::vector<ui::LutSearchKeys> matches;
  matches.reserve(candidates.size());
  for (Candidate& candidate : candidates) {
    matches.push_back(std::move(candidate.keys));
  }
  return matches;
}

auto ListResult(const LutLibraryService& library, const QJsonObject& params) -> QJsonObject {
  const std::vector<ui::LutSearchKeys> matches = MatchingEntries(library, params);
  const int                            total   = static_cast<int>(matches.size());
  const int                            offset  = std::min(params.value("offset").toInt(0), total);
  const int                            limit   = params.value("limit").toInt(kDefaultLutPageSize);
  const int                            end     = std::min(total, offset + limit);

  QJsonArray                           luts;
  for (int i = offset; i < end; ++i) {
    library.ReadEntry(
        matches[static_cast<std::size_t>(i)].relative_path,
        [&](const LutLibraryEntry& entry) { luts.push_back(LutObject(library, entry)); });
  }
  return QJsonObject{{"root", library.root_path()},
                     {"inventory_complete", library.inventory_complete()},
                     {"total", total},
                     {"offset", offset},
                     {"limit", limit},
                     {"has_more", end < total},
                     {"luts", luts}};
}

/// True while no operation that builds a new inventory runs: the published inventory is the
/// one of the current root.
auto InventoryReady(const LutLibraryService& library) -> bool {
  const LutLibraryService::Operation operation = library.CurrentOperation();
  return operation != LutLibraryService::Operation::kLoad &&
         operation != LutLibraryService::Operation::kRefresh;
}

}  // namespace

auto RegisterAutomationLutCommands(AutomationCommandRegistry& registry,
                                   ui::ApplicationModuleHost* host, QString* error) -> bool {
  if (host == nullptr || host->lut_library() == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the LUT commands need a host with a LUT library");
    }
    return false;
  }

  AutomationCommandSpec list;
  list.method      = QStringLiteral("lut.list");
  list.description = QStringLiteral(
      "Lists the LUTs of the LUT library with the rules of the LUT browser. A 'query' matches the "
      "name, file name, aliases, source, film, print, and path, and sorts the matches by search "
      "rank; without a query the LUTs are sorted by name. 'editor_value' of a LUT is the value "
      "for editor.set on the 'lut' field that the LUT browser applies: the reference, the name, "
      "and the remembered encodings. A LUT that the grade cannot apply has 'applicable' false "
      "and no 'editor_value'. Waits while the library loads or refreshes its inventory. Needs "
      "no open project.");
  list.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"query",
       QJsonObject{{"type", "string"},
                   {"description", "Search text, as in the search field of the LUT browser."}}},
      {"category", QJsonObject{{"type", "string"},
                               {"enum", QJsonArray{"general", "film_simulation"}},
                               {"description", "Only LUTs of this category."}}},
      {"package",
       QJsonObject{{"type", "string"},
                   {"description",
                    "Only LUTs of this installed official package, for example "
                    "spectral_film_lut. An empty string selects the LUTs outside packages."}}},
      {"favorites",
       QJsonObject{{"type", "boolean"}, {"description", "Only favorite LUTs. Default false."}}},
      {"offset", QJsonObject{{"type", "integer"},
                             {"minimum", 0},
                             {"description", "The first LUT to return. Default 0."}}},
      {"limit", QJsonObject{{"type", "integer"},
                            {"minimum", 1},
                            {"maximum", kMaxLutPageSize},
                            {"description", "The number of LUTs to return. Default 50."}}}});
  list.result_schema = AutomationObjectSchema(
      QJsonObject{{"root", QJsonObject{{"type", "string"}}},
                  {"inventory_complete", QJsonObject{{"type", "boolean"}}},
                  {"total", QJsonObject{{"type", "integer"}}},
                  {"offset", QJsonObject{{"type", "integer"}}},
                  {"limit", QJsonObject{{"type", "integer"}}},
                  {"has_more", QJsonObject{{"type", "boolean"}}},
                  {"luts", QJsonObject{{"type", "array"}}}},
      QJsonArray{"root", "inventory_complete", "total", "offset", "limit", "has_more", "luts"});
  list.handler = [host](const QJsonObject& params, AutomationReply reply) {
    LutLibraryService* library = host->lut_library();
    if (InventoryReady(*library)) {
      reply.SendResult(ListResult(*library, params));
      return;
    }
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), library);
    QObject::connect(library, &LutLibraryService::OperationStateChanged, wait.data(),
                     [wait, library, params] {
                       if (wait == nullptr || !InventoryReady(*library)) {
                         return;
                       }
                       wait->SendResult(ListResult(*library, params));
                     });
  };
  return registry.Register(std::move(list), error);
}

}  // namespace alcedo::automation
