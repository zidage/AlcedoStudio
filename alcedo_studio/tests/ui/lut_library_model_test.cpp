//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LUT browser model (plan L5): metadata filters, print presence, search ranking, favorites,
// selection without resets, and bounded row instantiation at 1,000 and 10,000 entries.

#include "ui/alcedo_main/album_backend/lut_library_model.hpp"

#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "lut_library_model_test_support.hpp"
#include "ui/alcedo_main/album_backend/lut_library_query.hpp"

#ifdef _WIN32
#include <windows.h>
// windows.h must precede psapi.h.
#include <psapi.h>
#endif

namespace alcedo::ui::test {
namespace {

/// Independent facts of each fixture entry, written out by hand for expected results.
struct EntryFacts {
  const char* entry_id;
  const char* category;  // "general" or "film_simulation"
  const char* source;    // source ID or ""
  const char* brand;     // normalized film brand or ""
  bool        has_print;
};

const std::vector<EntryFacts> kFacts = {
    {"library:kodak/portra_400.cube", "film_simulation", "spectral_film_lut", "kodak", false},
    {"library:kodak/vision3_2383.cube", "film_simulation", "spectral_film_lut", "kodak", true},
    {"library:fuji/eterna_archive.cube", "film_simulation", "spektrafilm_lut", "fujifilm", true},
    {"library:fuji/provia.cube", "film_simulation", "spektrafilm_lut", "fujifilm", false},
    {"library:third/acme_paper.cube", "film_simulation", "acme_lut", "acme", true},
    {"library:general/teal.cube", "general", "", "", false},
    {"library:general/mono.cube", "general", "my_tool", "", false},
};

auto ClassifiedLibraryFiles() -> std::vector<std::pair<std::string, std::string>> {
  return {
      {"kodak/portra_400.cube",
       CubeWithMetadata(FilmMetadata("user:portra", "spectral_film_lut", "Spectral Film LUT",
                                     "portra-400", "Portra 400", "Kodak"))},
      {"kodak/vision3_2383.cube",
       CubeWithMetadata(FilmMetadata("user:vision3-2383", "spectral_film_lut",
                                     "Spectral Film LUT", "vision3-250d", "Vision3 250D", "Kodak",
                                     PrintFields{"kodak-2383", "2383", "Kodak", "film"}))},
      {"fuji/eterna_archive.cube",
       CubeWithMetadata(FilmMetadata("user:eterna", "spektrafilm_lut", "Spektrafilm LUT", "eterna",
                                     "Eterna", "Fujifilm",
                                     PrintFields{"crystal-archive", "Crystal Archive", "Fujifilm",
                                                 "paper"}))},
      {"fuji/provia.cube", CubeWithMetadata(FilmMetadata("user:provia", "spektrafilm_lut",
                                                         "Spektrafilm LUT", "provia", "Provia",
                                                         "Fujifilm"))},
      {"third/acme_paper.cube",
       CubeWithMetadata(FilmMetadata("acme:paper", "acme_lut", "Acme LUT", "acme-100", "Acme 100",
                                     "Acme", PrintFields{"acme-paper", "Acme Paper", "Acme",
                                                         "paper"}))},
      {"general/teal.cube", CubeWithMetadata({})},
      {"general/mono.cube", CubeWithMetadata(GeneralMetadata("my:mono", "my_tool"))},
  };
}

auto Expected(const std::string& category, const std::string& source, const std::string& brand,
              const std::string& print, bool favorites_only, const std::set<std::string>& favorites)
    -> QStringList {
  QStringList ids;
  for (const EntryFacts& facts : kFacts) {
    const bool film = category != "general";
    if (category != "all" && facts.category != category) continue;
    if (!source.empty() && facts.source != source) continue;
    if (film && !brand.empty() && facts.brand != brand) continue;
    if (film && print == "with_print" && !facts.has_print) continue;
    if (film && print == "no_print" && facts.has_print) continue;
    if (favorites_only && favorites.count(facts.entry_id) == 0) continue;
    ids.push_back(QString::fromUtf8(facts.entry_id));
  }
  ids.sort();
  return ids;
}

auto ChoiceCount(const QVariantList& choices, const QString& value) -> int {
  for (const QVariant& choice : choices) {
    const QVariantMap map = choice.toMap();
    if (map.value(QStringLiteral("value")).toString() == value) {
      return map.value(QStringLiteral("count")).toInt();
    }
  }
  return -1;
}

}  // namespace

TEST(LutLibraryModelTest, FiltersUseIntersectionAndAllRemovesOnePredicate) {
  TemporaryLutLibrary library(ClassifiedLibraryFiles());
  const std::set<std::string> favorites = {"library:kodak/vision3_2383.cube",
                                           "library:general/teal.cube",
                                           "library:third/acme_paper.cube"};
  for (const std::string& id : favorites) {
    ASSERT_EQ(library.Service()->SetFavorite(id, true), LutLibraryService::Status::kOk);
  }
  LutLibraryModel model;
  model.setLibrary(library.Service());
  ASSERT_EQ(model.totalCount(), 7);

  const std::vector<std::string> categories = {"all", "general", "film_simulation"};
  const std::vector<std::string> sources    = {"", "spectral_film_lut", "spektrafilm_lut",
                                               "acme_lut", "my_tool"};
  const std::vector<std::string> brands     = {"", "kodak", "fujifilm", "acme"};
  const std::vector<std::string> prints     = {"all", "with_print", "no_print"};
  int                            checked    = 0;
  for (const std::string& category : categories) {
    for (const std::string& source : sources) {
      for (const std::string& brand : brands) {
        for (const std::string& print : prints) {
          for (const bool favorites_only : {false, true}) {
            model.clearFilters();
            model.setCategory(QString::fromStdString(category));
            model.setSource(QString::fromStdString(source));
            model.setBrand(QString::fromStdString(brand));
            model.setPrint(QString::fromStdString(print));
            model.setFavoritesOnly(favorites_only);
            EXPECT_EQ(SortedRowEntryIds(model),
                      Expected(category, source, brand, print, favorites_only, favorites))
                << category << " / " << source << " / " << brand << " / " << print << " / "
                << favorites_only;
            EXPECT_EQ(model.count(), model.rowCount());
            ++checked;
          }
        }
      }
    }
  }
  EXPECT_EQ(checked, 3 * 5 * 4 * 3 * 2);

  // Facet counts apply every other predicate: with the Kodak brand, the source choice counts
  // only Kodak entries, and each brand count ignores the chosen brand.
  model.clearFilters();
  model.setCategory(QStringLiteral("film_simulation"));
  model.setBrand(QStringLiteral("Kodak"));
  EXPECT_EQ(ChoiceCount(model.sourceChoices(), QStringLiteral("spectral_film_lut")), 2);
  EXPECT_EQ(ChoiceCount(model.sourceChoices(), QStringLiteral("spektrafilm_lut")), 0);
  EXPECT_EQ(ChoiceCount(model.brandChoices(), QStringLiteral("fujifilm")), 2);
  EXPECT_EQ(ChoiceCount(model.brandChoices(), QString()), 5);
}

TEST(LutLibraryModelTest, PrintPresenceCombinesPrintFilmAndPaper) {
  TemporaryLutLibrary library(ClassifiedLibraryFiles());
  LutLibraryModel     model;
  model.setLibrary(library.Service());
  model.setCategory(QStringLiteral("film_simulation"));
  ASSERT_TRUE(model.printFilterAvailable());

  model.setPrint(QStringLiteral("with_print"));
  // Print film (2383) and photographic paper (Crystal Archive, Acme Paper) both count.
  EXPECT_EQ(SortedRowEntryIds(model),
            (QStringList{"library:fuji/eterna_archive.cube", "library:kodak/vision3_2383.cube",
                         "library:third/acme_paper.cube"}));
  model.setPrint(QStringLiteral("no_print"));
  EXPECT_EQ(SortedRowEntryIds(model),
            (QStringList{"library:fuji/provia.cube", "library:kodak/portra_400.cube"}));
  model.setPrint(QStringLiteral("all"));
  EXPECT_EQ(model.count(), 5);
  EXPECT_EQ(ChoiceCount(model.printChoices(), QStringLiteral("with_print")), 3);
  EXPECT_EQ(ChoiceCount(model.printChoices(), QStringLiteral("no_print")), 2);
  EXPECT_EQ(ChoiceCount(model.printChoices(), QStringLiteral("all")), 5);

  // Brand and source still intersect the print presence.
  model.setPrint(QStringLiteral("with_print"));
  model.setBrand(QStringLiteral("Kodak"));
  EXPECT_EQ(RowEntryIds(model), QStringList{"library:kodak/vision3_2383.cube"});
  model.setBrand({});
  model.setSource(QStringLiteral("spektrafilm_lut"));
  EXPECT_EQ(RowEntryIds(model), QStringList{"library:fuji/eterna_archive.cube"});
}

TEST(LutLibraryModelTest, ThirdPartyPrintMetadataAddsPresenceFilter) {
  {
    // A source the application has never seen: print presence works without source code.
    TemporaryLutLibrary library(
        {{"new/with_paper.cube",
          CubeWithMetadata(FilmMetadata("new:a", "new_generator", "New Generator", "stock-a",
                                        "Stock A", "Maker",
                                        PrintFields{"paper-x", "Paper X", "Maker", "paper"}))},
         {"new/negative.cube",
          CubeWithMetadata(FilmMetadata("new:b", "new_generator", "New Generator", "stock-b",
                                        "Stock B", "Maker"))}});
    LutLibraryModel model;
    model.setLibrary(library.Service());
    EXPECT_TRUE(model.printFilterAvailable());
    EXPECT_EQ(ChoiceCount(model.printChoices(), QStringLiteral("all")), 2);
    EXPECT_EQ(ChoiceCount(model.printChoices(), QStringLiteral("with_print")), 1);
    EXPECT_EQ(ChoiceCount(model.printChoices(), QStringLiteral("no_print")), 1);
    EXPECT_EQ(ChoiceCount(model.sourceChoices(), QStringLiteral("new_generator")), 2);
    model.setPrint(QStringLiteral("with_print"));
    EXPECT_EQ(RowEntryIds(model), QStringList{"library:new/with_paper.cube"});
  }
  {
    // Without print metadata the dimension has nothing to narrow.
    TemporaryLutLibrary library({{"plain/look.cube", CubeWithMetadata({})}});
    LutLibraryModel     model;
    model.setLibrary(library.Service());
    EXPECT_FALSE(model.printFilterAvailable());
  }
}

TEST(LutLibraryModelTest, GeneralCategoryClearsFilmPredicates) {
  TemporaryLutLibrary library(ClassifiedLibraryFiles());
  LutLibraryModel     model;
  model.setLibrary(library.Service());
  model.setCategory(QStringLiteral("film_simulation"));
  model.setBrand(QStringLiteral("Kodak"));
  model.setPrint(QStringLiteral("with_print"));
  ASSERT_EQ(RowEntryIds(model), QStringList{"library:kodak/vision3_2383.cube"});

  QSignalSpy filter_spy(&model, &LutLibraryModel::filterChanged);
  model.setCategory(QStringLiteral("general"));
  EXPECT_EQ(filter_spy.count(), 1);
  EXPECT_EQ(model.brand(), QString());
  EXPECT_EQ(model.print(), QStringLiteral("all"));
  EXPECT_FALSE(model.filmFiltersAvailable());
  // General entries have no film metadata and stay reachable.
  EXPECT_EQ(SortedRowEntryIds(model),
            (QStringList{"library:general/mono.cube", "library:general/teal.cube"}));
  // Film predicates do not apply to General; the source predicate still does.
  model.setBrand(QStringLiteral("Kodak"));
  model.setPrint(QStringLiteral("no_print"));
  EXPECT_EQ(model.brand(), QString());
  EXPECT_EQ(model.print(), QStringLiteral("all"));
  model.setSource(QStringLiteral("my_tool"));
  EXPECT_EQ(RowEntryIds(model), QStringList{"library:general/mono.cube"});

  // A valid choice with zero results stays chosen and is shown with a zero count.
  model.setSource(QStringLiteral("acme_lut"));
  EXPECT_EQ(model.count(), 0);
  EXPECT_EQ(model.source(), QStringLiteral("acme_lut"));
  EXPECT_EQ(ChoiceCount(model.sourceChoices(), QStringLiteral("acme_lut")), 0);
}

TEST(LutLibraryModelTest, FuzzySearchRanksExactNameBeforeTypo) {
  TemporaryLutLibrary library({
      {"a/portra.cube", CubeWithMetadata({})},
      {"b/portra_400.cube", CubeWithMetadata({})},
      {"c/portraits.cube", CubeWithMetadata({})},
      {"d/myportra.cube", CubeWithMetadata({})},
      {"e/potra.cube", CubeWithMetadata({})},
      {"f/unrelated.cube", CubeWithMetadata({})},
      {"g/film.cube", CubeWithMetadata({})},
      {"h/film.cube", CubeWithMetadata({})},
      {"i/Übung.cube", CubeWithMetadata({})},
      {"j/look.cube", CubeWithMetadata(GeneralMetadata("user:look", {}, R"(["Velvia"])"))},
  });
  LutLibraryModel model;
  model.setLibrary(library.Service());
  auto search = [&](const QString& text) {
    model.setQueryText(text);
    model.applyQueryNow();
    return RowEntryIds(model);
  };

  // Whole name, exact token, token prefix, substring, one edit: in that order.
  EXPECT_EQ(search(QStringLiteral("portra")),
            (QStringList{"library:a/portra.cube", "library:b/portra_400.cube",
                         "library:c/portraits.cube", "library:d/myportra.cube",
                         "library:e/potra.cube"}));
  // Unicode compatibility forms and case fold: full-width Latin and upper-case umlaut.
  EXPECT_EQ(search(QStringLiteral("ＰＯＲＴＲＡ 400")), QStringList{"library:b/portra_400.cube"});
  EXPECT_EQ(search(QStringLiteral("ÜBUNG")), QStringList{"library:i/Übung.cube"});
  // Aliases are searchable, including one typo in a token of 4-7 characters.
  EXPECT_EQ(search(QStringLiteral("velvia")), QStringList{"library:j/look.cube"});
  EXPECT_EQ(search(QStringLiteral("velvja")), QStringList{"library:j/look.cube"});
  // Equal ranks are ordered by entry ID.
  EXPECT_EQ(search(QStringLiteral("film")),
            (QStringList{"library:g/film.cube", "library:h/film.cube"}));
  // Short tokens need an exact or prefix match; a typo gives an explicit empty result.
  EXPECT_EQ(search(QStringLiteral("por")),
            (QStringList{"library:a/portra.cube", "library:b/portra_400.cube",
                         "library:c/portraits.cube"}));
  EXPECT_TRUE(search(QStringLiteral("pxr")).isEmpty());
  EXPECT_EQ(model.count(), 0);
  // Every token must match: the relative path folder counts as a field.
  EXPECT_EQ(search(QStringLiteral("portra b")), QStringList{"library:b/portra_400.cube"});
  // The same query ranks the same way again.
  EXPECT_EQ(search(QStringLiteral("portra")), search(QStringLiteral("portra")));

  // Bounds: 256 input characters and 16 tokens.
  const LutSearchQuery long_query = ParseLutSearchQuery(QString(300, QLatin1Char('a')));
  EXPECT_EQ(long_query.normalized.size(), kLutQueryMaxCharacters);
  QStringList many;
  for (int i = 0; i < 20; ++i) many.push_back(QStringLiteral("t%1").arg(i));
  EXPECT_EQ(ParseLutSearchQuery(many.join(QLatin1Char(' '))).tokens.size(),
            static_cast<std::size_t>(kLutQueryMaxTokens));
}

TEST(LutLibraryModelTest, QueryAppliesAfterTypingDelay) {
  TemporaryLutLibrary library({{"a/portra.cube", CubeWithMetadata({})},
                               {"b/velvia.cube", CubeWithMetadata({})}});
  LutLibraryModel     model;
  model.setLibrary(library.Service());
  QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
  model.setQueryText(QStringLiteral("p"));
  model.setQueryText(QStringLiteral("po"));
  model.setQueryText(QStringLiteral("portra"));
  EXPECT_EQ(model.count(), 2);
  EXPECT_EQ(reset_spy.count(), 0);
  ASSERT_TRUE(reset_spy.wait(1000));
  EXPECT_EQ(reset_spy.count(), 1);
  EXPECT_EQ(RowEntryIds(model), QStringList{"library:a/portra.cube"});
}

TEST(LutLibraryModelTest, SelectionChangeDoesNotResetRows) {
  TemporaryLutLibrary library(ClassifiedLibraryFiles());
  LutLibraryModel     model;
  model.setLibrary(library.Service());
  const QStringList rows_before = RowEntryIds(model);
  QSignalSpy        reset_spy(&model, &QAbstractItemModel::modelReset);
  QSignalSpy        layout_spy(&model, &QAbstractItemModel::layoutChanged);
  QSignalSpy        removed_spy(&model, &QAbstractItemModel::rowsRemoved);
  QSignalSpy        data_spy(&model, &QAbstractItemModel::dataChanged);

  const QString first  = model.entryIdAt(0);
  const QString second = model.entryIdAt(3);
  model.focusEntry(first);
  model.focusEntry(second);
  EXPECT_EQ(model.focusedRow(), 3);
  EXPECT_TRUE(model.data(model.index(3), LutLibraryModel::FocusedRole).toBool());
  EXPECT_FALSE(model.data(model.index(0), LutLibraryModel::FocusedRole).toBool());
  ASSERT_TRUE(model.focusRelative(1));
  EXPECT_EQ(model.focusedRow(), 4);
  model.setAppliedEntryId(first);
  EXPECT_TRUE(model.data(model.index(0), LutLibraryModel::AppliedRole).toBool());
  ASSERT_TRUE(model.toggleFavorite(second));
  EXPECT_TRUE(model.data(model.index(3), LutLibraryModel::FavoriteRole).toBool());

  EXPECT_EQ(reset_spy.count(), 0);
  EXPECT_EQ(layout_spy.count(), 0);
  EXPECT_EQ(removed_spy.count(), 0);
  EXPECT_GE(data_spy.count(), 5);
  for (const QList<QVariant>& call : data_spy) {
    const QList<int> roles = call.at(2).value<QList<int>>();
    ASSERT_EQ(roles.size(), 1);
    EXPECT_TRUE(roles.front() == LutLibraryModel::FocusedRole ||
                roles.front() == LutLibraryModel::AppliedRole ||
                roles.front() == LutLibraryModel::FavoriteRole);
  }
  EXPECT_EQ(RowEntryIds(model), rows_before);

  // With the favorites filter on, removing a favorite removes only that row.
  model.setFavoritesOnly(true);
  ASSERT_EQ(RowEntryIds(model), QStringList{second});
  ASSERT_TRUE(model.toggleFavorite(first));
  const QStringList favorites_before = RowEntryIds(model);
  reset_spy.clear();
  ASSERT_TRUE(model.toggleFavorite(second));
  EXPECT_EQ(reset_spy.count(), 0);
  EXPECT_EQ(removed_spy.count(), 1);
  EXPECT_EQ(RowEntryIds(model), QStringList{first});
  EXPECT_EQ(favorites_before.size(), 2);
}

namespace {

/// A persisted inventory of @p count classified entries; no CUBE files are read.
auto SyntheticInventory(int count) -> LutLibraryInventory {
  LutLibraryInventory inventory;
  inventory.entries.reserve(static_cast<std::size_t>(count));
  const char* brands[] = {"Kodak", "Fujifilm", "Ilford", "Agfa"};
  for (int i = 0; i < count; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "film_%05d", i);
    LutLibraryEntry entry;
    entry.name                      = name;
    entry.relative_path             = std::string("set_") + std::to_string(i % 37) + "/" + name +
                                      ".cube";
    entry.size                      = 1024;
    entry.modified_time             = i;
    entry.header.lut_3d_size        = 33;
    LutMetadata metadata;
    metadata.id                     = std::string("user:") + name;
    metadata.origin                 = LutOrigin::kUser;
    metadata.category               = i % 5 == 0 ? LutCategory::kGeneral : LutCategory::kFilmSimulation;
    metadata.input_space            = "ACEScc";
    metadata.output_space           = "ACEScc";
    if (metadata.category == LutCategory::kFilmSimulation) {
      metadata.source = LutSourceInfo{"generator_" + std::to_string(i % 3), "Generator"};
      metadata.film   = LutFilmInfo{"stock-" + std::to_string(i), std::string("Stock ") + name,
                                    brands[i % 4]};
      if (i % 2 == 0) metadata.print = LutPrintInfo{"print-1", "Print One", "Kodak"};
    }
    entry.header.metadata = std::move(metadata);
    inventory.entries.push_back(std::move(entry));
  }
  std::sort(inventory.entries.begin(), inventory.entries.end(),
            [](const LutLibraryEntry& a, const LutLibraryEntry& b) {
              return a.relative_path < b.relative_path;
            });
  return inventory;
}

auto ProcessMemoryBytes() -> std::uint64_t {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return counters.WorkingSetSize;
  }
#endif
  return 0;
}

constexpr const char* kBoundedListQml = R"(
import QtQuick
ListView {
    width: 240
    height: 400
    cacheBuffer: 80
    property int created: 0
    delegate: Item {
        width: 240
        height: 20
        required property string displayName
        Component.onCompleted: ListView.view.created += 1
    }
}
)";

/// Delegates a 400 px ListView creates for @p model, after scrolling to the middle.
auto CreatedDelegates(LutLibraryModel* model) -> int {
  QQmlEngine    engine;
  QQmlComponent component(&engine);
  component.setData(kBoundedListQml, QUrl());
  std::unique_ptr<QObject> object(component.create());
  EXPECT_NE(object, nullptr) << component.errorString().toStdString();
  if (!object) return -1;
  auto*        list = qobject_cast<QQuickItem*>(object.get());
  QQuickWindow window;
  window.resize(240, 400);
  list->setParentItem(window.contentItem());
  list->setProperty("model", QVariant::fromValue(static_cast<QObject*>(model)));
  QMetaObject::invokeMethod(list, "forceLayout");
  list->setProperty("contentY", 20.0 * model->rowCount() / 2.0);
  QMetaObject::invokeMethod(list, "forceLayout");
  return list->property("created").toInt();
}

struct QueryTiming {
  double p50_ms;
  double p95_ms;
};

auto MeasureQueries(LutLibraryModel* model) -> QueryTiming {
  const QStringList queries = {QStringLiteral("film"),    QStringLiteral("stock 01"),
                               QStringLiteral("kodak"),   QStringLiteral("fujiflim"),
                               QStringLiteral("set_3"),   QStringLiteral("print one"),
                               QStringLiteral("stock"),   QStringLiteral("film_0999"),
                               QStringLiteral("ilford"),  QStringLiteral("generator")};
  std::vector<double> samples;
  for (int round = 0; round < 3; ++round) {
    for (const QString& query : queries) {
      QElapsedTimer timer;
      timer.start();
      model->setQueryText(query);
      model->applyQueryNow();
      samples.push_back(static_cast<double>(timer.nsecsElapsed()) / 1.0e6);
    }
  }
  std::sort(samples.begin(), samples.end());
  auto at = [&](double q) {
    return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
  };
  return {at(0.5), at(0.95)};
}

}  // namespace

TEST(LutLibraryModelTest, TenThousandEntriesKeepVisibleRowsBounded) {
  std::vector<int> created;
  for (const int size : {1000, 10000}) {
    const LutLibraryInventory inventory = SyntheticInventory(size);
    TemporaryLutLibrary       library({}, &inventory);
    ASSERT_EQ(library.Service()->EntryCount(), static_cast<std::size_t>(size));
    const std::uint64_t memory_before = ProcessMemoryBytes();
    QElapsedTimer       build_timer;
    build_timer.start();
    LutLibraryModel model;
    model.setLibrary(library.Service());
    const double build_ms      = static_cast<double>(build_timer.nsecsElapsed()) / 1.0e6;
    const std::uint64_t memory_after = ProcessMemoryBytes();
    ASSERT_EQ(model.count(), size);
    created.push_back(CreatedDelegates(&model));

    const QueryTiming timing = MeasureQueries(&model);
    QElapsedTimer     focus_timer;
    focus_timer.start();
    for (int i = 0; i < 100; ++i) model.focusEntry(model.entryIdAt(i));
    const double focus_ms = static_cast<double>(focus_timer.nsecsElapsed()) / 1.0e6 / 100.0;
    const std::string prefix = "entries_" + std::to_string(size) + "_";
    ::testing::Test::RecordProperty(prefix + "index_build_ms", std::to_string(build_ms));
    ::testing::Test::RecordProperty(prefix + "query_p50_ms", std::to_string(timing.p50_ms));
    ::testing::Test::RecordProperty(prefix + "query_p95_ms", std::to_string(timing.p95_ms));
    ::testing::Test::RecordProperty(prefix + "focus_ms", std::to_string(focus_ms));
    ::testing::Test::RecordProperty(
        prefix + "model_working_set_delta_bytes",
        std::to_string(memory_after > memory_before ? memory_after - memory_before : 0));
    std::printf("[measure] %d entries: index %.2f ms, query p50 %.2f ms, p95 %.2f ms, focus %.3f ms,"
                " working set delta %.1f MiB\n",
                size, build_ms, timing.p50_ms, timing.p95_ms, focus_ms,
                static_cast<double>(memory_after > memory_before ? memory_after - memory_before
                                                                 : 0) /
                    (1024.0 * 1024.0));
#ifdef NDEBUG
    // Plan 10.1 targets for a Release build.
    EXPECT_LT(timing.p95_ms, 100.0);
    EXPECT_LT(focus_ms, 16.0);
#endif
  }
  ASSERT_EQ(created.size(), 2u);
  // The view creates delegates for the rows around its two scroll positions (400 px of 20 px
  // rows plus an 80 px buffer on each side); ten times more entries create no more delegates.
  const int per_position = 400 / 20 + 2 * 80 / 20 + 2;
  EXPECT_GT(created[0], 0);
  EXPECT_LE(created[0], 2 * per_position);
  EXPECT_EQ(created[1], created[0]);
}

}  // namespace alcedo::ui::test
