//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LUT browser page (LUT library plan L6A): loads production EditorLutBrowserPanel.qml from the
// source tree over a real LutLibraryService, LutLibraryModel, and LutLibraryController, and checks
// the tiles, titles, filter rows, and tile application through the page's own functions. Pointer
// delivery is not used: offscreen input is unreliable, and the owners are covered by C++ tests.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "lut_library_model_test_support.hpp"
#include "lut_target_test_support.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/lut_library_controller.hpp"
#include "ui/alcedo_main/album_backend/lut_library_model.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

constexpr const char* kLongUserStem = "a_really_long_user_lut_file_name_without_spaces_final_v2";

/// The `appModules` members the LUT browser page reads.
class LutAppModules   final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject* lutBrowser READ lutBrowser CONSTANT)
  Q_PROPERTY(QObject* lutTarget READ lutTarget CONSTANT)
  Q_PROPERTY(QObject* lutLibrary READ lutLibrary CONSTANT)

 public:
  LutAppModules(LutLibraryModel* browser, LutLibraryController* target, QObject* library)
      : browser_(browser), target_(target), library_(library) {}
  [[nodiscard]] auto lutBrowser() const -> QObject* { return browser_; }
  [[nodiscard]] auto lutTarget() const -> QObject* { return target_; }
  [[nodiscard]] auto lutLibrary() const -> QObject* { return library_; }

 private:
  LutLibraryModel*      browser_;
  LutLibraryController* target_;
  QObject*              library_;
};

auto OfficialFilm(const std::string& lut_id, const std::string& film_id,
                  const std::string& film_name, const std::string& print_json) -> std::string {
  return CubeWithMetadata(R"({"schema":1,"id":"spectral_film_lut:)" + lut_id +
                          R"(","origin":"alcedo","category":"film_simulation",)"
                          R"("source":{"id":"spectral_film_lut","name":"Spectral Film LUT"},)"
                          R"("film":{"id":")" +
                          film_id + R"(","name":")" + film_name + R"(","brand":"Kodak"})" +
                          print_json + R"(,"input_space":"ACEScc","output_space":"ACEScc"})");
}

auto LibraryFiles() -> std::vector<std::pair<std::string, std::string>> {
  return {
      {"films/kodak_vision3_250d.cube",
       OfficialFilm("kodak_vision3_250d", "kodak_vision3_250d", "Vision3 250D", "")},
      {"films/kodak_vision3_250d__kodak_vision_2383.cube",
       OfficialFilm("kodak_vision3_250d:kodak_vision_2383", "kodak_vision3_250d", "Vision3 250D",
                    R"(,"print":{"id":"kodak_vision_2383","name":"Vision 2383","brand":"Kodak",)"
                    R"("kind":"film"})")},
      {"general/teal.cube", CubeWithMetadata(GeneralMetadata("user:teal"))},
      {std::string("general/") + kLongUserStem + ".cube", CubeWithMetadata({})},
      {"general/strip.cube", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n"},
  };
}

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitUntil(const std::function<bool()>& predicate, int timeout_ms) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < timeout_ms) {
    if (predicate()) return true;
    ProcessEvents(20);
  }
  return predicate();
}

/// Items named @p name in the visual item tree under @p root. Repeater and ListView delegates
/// are reachable only through the item tree, not through QObject children.
void CollectItems(QQuickItem* root, const QString& name, QList<QQuickItem*>* found) {
  if (root == nullptr) return;
  if (root->objectName() == name) found->push_back(root);
  for (QQuickItem* child : root->childItems()) CollectItems(child, name, found);
}

constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    width: 660
    height: 720
    visible: true
    color: appTheme.bgCanvasColor

    Loader {
        anchors.fill: parent
        anchors.margins: appTheme.spaceSm
        source: panelSourceUrl
    }
}
)";

/// The shell functions the browser calls on its `host` (Main.qml); counts the calls.
class BrowserHost final : public QObject {
  Q_OBJECT

 public:
  Q_INVOKABLE void openLutImportDialog() { ++import_dialogs; }
  Q_INVOKABLE void openLutSettings() { ++lut_settings; }

  int              import_dialogs = 0;
  int              lut_settings   = 0;
};

/// The browser page in a window over a temporary library and a document target.
struct BrowserHarness {
  TemporaryLutLibrary   library;
  DocumentTargetSource  source;
  LutLibraryModel       browser;
  LutLibraryController  target;
  LutAppModules         modules{&browser, &target, library.Service()};
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           panel  = nullptr;
  QQuickItem*           result = nullptr;
  QStringList           warnings;

  explicit BrowserHarness(std::vector<std::pair<std::string, std::string>> files = LibraryFiles())
      : library(std::move(files)) {
    browser.setLibrary(library.Service());
    target.setLibrary(library.Service());
    target.SetTargetSource(&source);
    // ApplicationModuleHost marks the target's LUT as the browser's applied entry the same way.
    QObject::connect(&target, &LutLibraryController::associationChanged, &browser,
                     [this] { browser.setAppliedEntryId(target.associationEntryId()); });
    target.reload();
    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& list) {
      for (const QQmlError& warning : list) warnings << warning.toString();
    });
    AppTheme::Instance().setReduceMotion(true);
    // Production style (main.cpp); the native style rejects the custom control chrome.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    RegisterShortcutRegistryQmlType();
    RegisterEditorAdjustmentQmlTypes();
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(SrcQmlDir());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &modules);
    engine.rootContext()->setContextProperty(
        QStringLiteral("panelSourceUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/EditorLutBrowserPanel.qml")));
    engine.loadData(QByteArray{kHarnessQml}, QUrl(QStringLiteral("file:///LutBrowserHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window == nullptr) return;
    window->show();
    (void)QTest::qWaitForWindowExposed(window);
    (void)WaitUntil(
        [this] {
          panel  = Find(QStringLiteral("editorLutBrowserPanel"));
          result = Find(QStringLiteral("editorLutResultCard"));
          return panel != nullptr && result != nullptr && !Tiles().isEmpty();
        },
        3000);
  }

  auto FindAll(const QString& name) const -> QList<QQuickItem*> {
    QList<QQuickItem*> found;
    if (window != nullptr) CollectItems(window->contentItem(), name, &found);
    return found;
  }

  auto Find(const QString& name) const -> QQuickItem* { return FindAll(name).value(0, nullptr); }

  /// Instantiated tiles that show an entry.
  auto Tiles() const -> QList<QQuickItem*> {
    QList<QQuickItem*> tiles;
    for (QQuickItem* tile : FindAll(QStringLiteral("editorLutTile"))) {
      if (tile->property("present").toBool()) tiles.push_back(tile);
    }
    return tiles;
  }

  auto TileFor(const QString& entry_id) const -> QQuickItem* {
    for (QQuickItem* tile : Tiles()) {
      if (tile->property("entryId").toString() == entry_id) return tile;
    }
    return nullptr;
  }

  auto Warnings() const -> std::string { return warnings.join(QLatin1Char('\n')).toStdString(); }
};

auto ChildNamed(QQuickItem* root, const QString& name) -> QQuickItem* {
  QList<QQuickItem*> found;
  CollectItems(root, name, &found);
  return found.value(0, nullptr);
}

auto TitleLabelOf(QQuickItem* tile) -> QQuickItem* {
  return ChildNamed(tile, QStringLiteral("editorLutTileTitle"));
}

}  // namespace

TEST(EditorLutBrowserPanelQmlTest, BrowserLoadsFilterCardAndOneTilePerResultWithoutWarnings) {
  BrowserHarness h;
  ASSERT_NE(h.window, nullptr) << h.Warnings();
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  EXPECT_NE(h.Find(QStringLiteral("editorLutFilterCard")), nullptr);
  EXPECT_NE(h.Find(QStringLiteral("editorLutSearchInput")), nullptr);
  EXPECT_NE(h.Find(QStringLiteral("editorLutTargetIndicator")), nullptr);
  ASSERT_EQ(h.browser.count(), 5);
  EXPECT_EQ(h.Tiles().size(), h.browser.count());
  EXPECT_GE(h.result->property("columns").toInt(), 2);
  auto* count_text = h.Find(QStringLiteral("editorLutCountText"));
  ASSERT_NE(count_text, nullptr);
  EXPECT_EQ(count_text->property("text").toString(), QStringLiteral("5 LUTs"));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, TileTitlesAreCompleteAndExcludeThePrint) {
  BrowserHarness h;
  ASSERT_NE(h.result, nullptr) << h.Warnings();
  QQuickItem* with_print =
      h.TileFor(QStringLiteral("library:films/kodak_vision3_250d__kodak_vision_2383.cube"));
  QQuickItem* without_print = h.TileFor(QStringLiteral("library:films/kodak_vision3_250d.cube"));
  ASSERT_NE(with_print, nullptr);
  ASSERT_NE(without_print, nullptr);
  EXPECT_EQ(TitleLabelOf(with_print)->property("text").toString(),
            QStringLiteral("Kodak Vision3 250D"));
  EXPECT_EQ(TitleLabelOf(without_print)->property("text").toString(),
            QStringLiteral("Kodak Vision3 250D"));
  auto* print_label = ChildNamed(with_print, QStringLiteral("editorLutTilePrint"));
  ASSERT_NE(print_label, nullptr);
  EXPECT_TRUE(print_label->isVisible());
  EXPECT_EQ(print_label->property("text").toString(), QStringLiteral("Vision 2383"));

  // A long name without spaces wraps onto several lines and is never elided.
  QQuickItem* long_tile = h.TileFor(QStringLiteral("library:general/") +
                                    QString::fromLatin1(kLongUserStem) + QStringLiteral(".cube"));
  ASSERT_NE(long_tile, nullptr);
  QQuickItem* long_title = TitleLabelOf(long_tile);
  EXPECT_EQ(long_title->property("text").toString(), QString::fromLatin1(kLongUserStem));
  EXPECT_FALSE(long_title->property("truncated").toBool());
  EXPECT_GT(long_title->property("lineCount").toInt(), 1);
  EXPECT_GE(long_tile->height(), long_title->height());
}

TEST(EditorLutBrowserPanelQmlTest, FacetRowSelectsItsChoiceAndSecondChoiceReturnsToAll) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* general = h.Find(QStringLiteral("editorLutFacet_category_general"));
  ASSERT_NE(general, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(general, "activate"));
  EXPECT_EQ(h.browser.category(), QStringLiteral("general"));
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == h.browser.count(); }, 2000));
  EXPECT_EQ(h.browser.count(), 3);
  // Brand and Print do not apply to General LUTs.
  auto* brand = h.Find(QStringLiteral("editorLutBrandSection"));
  ASSERT_NE(brand, nullptr);
  EXPECT_FALSE(brand->isVisible());

  general = h.Find(QStringLiteral("editorLutFacet_category_general"));
  ASSERT_NE(general, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(general, "activate"));
  EXPECT_EQ(h.browser.category(), QStringLiteral("all"));
  EXPECT_EQ(h.browser.count(), 5);
}

TEST(EditorLutBrowserPanelQmlTest, TileActivationTogglesTheTargetLutOnlyWhenATargetExists) {
  BrowserHarness h;
  ASSERT_NE(h.result, nullptr) << h.Warnings();
  const QString teal = QStringLiteral("library:general/teal.cube");
  QVariant      applied;
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "activateEntry", Q_RETURN_ARG(QVariant, applied),
                                        Q_ARG(QVariant, teal)));
  EXPECT_TRUE(applied.toBool());
  EXPECT_EQ(h.browser.focusedEntryId(), teal);
  ASSERT_EQ(h.source.submit_count, 1);
  ASSERT_EQ(h.source.queued.front().first.node_id, kGradeB);
  ASSERT_EQ(h.source.ApplyQueued(), 1);
  h.target.reload();
  EXPECT_EQ(h.browser.appliedEntryId(), teal);

  // The applied tile shows an outline, not a filled well.
  QQuickItem* teal_tile = h.TileFor(teal);
  ASSERT_NE(teal_tile, nullptr);
  QQuickItem* chrome = ChildNamed(teal_tile, QStringLiteral("editorLutTileChrome"));
  ASSERT_NE(chrome, nullptr);
  EXPECT_EQ(chrome->property("border").value<QObject*>()->property("width").toInt(),
            AppTheme::Instance().graphSelectionOutlineWidth());

  // Choosing the applied tile again removes the LUT: one clearing write, no remove button.
  EXPECT_EQ(h.Find(QStringLiteral("editorLutRemoveButton")), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "activateEntry", Q_RETURN_ARG(QVariant, applied),
                                        Q_ARG(QVariant, teal)));
  EXPECT_TRUE(applied.toBool());
  ASSERT_EQ(h.source.submit_count, 2);
  ASSERT_EQ(h.source.queued.size(), 1u);
  const EditorLutWrite& cleared = h.source.queued.front().second;
  ASSERT_TRUE(cleared.reference.has_value());
  EXPECT_TRUE(IsEmptyLutReference(*cleared.reference));
  EXPECT_FALSE(cleared.strength.has_value());
  ASSERT_EQ(h.source.ApplyQueued(), 1);
  h.target.reload();
  EXPECT_FALSE(h.target.hasAssociation());
  EXPECT_TRUE(h.browser.appliedEntryId().isEmpty());

  // An arrow key at the grid edge does nothing; it does not toggle the focused tile.
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "activateEntry", Q_RETURN_ARG(QVariant, applied),
                                        Q_ARG(QVariant, teal)));
  ASSERT_EQ(h.source.ApplyQueued(), 1);
  h.target.reload();
  // Name order puts teal last, so the row below it does not exist.
  ASSERT_EQ(h.browser.focusedRow(), h.browser.count() - 1);
  const int submits = h.source.submit_count;
  const int columns = h.result->property("columns").toInt();
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "moveFocus", Q_ARG(QVariant, columns)));
  EXPECT_EQ(h.source.submit_count, submits);
  EXPECT_EQ(h.browser.appliedEntryId(), teal);
  const int submits_before_target_loss = h.source.submit_count;

  // Without a Color Grade target, a tile only takes focus and the indicator gives the reason.
  h.source.selected                    = h.source.Mutable().Drt()->Id();
  h.target.reload();
  ASSERT_FALSE(h.target.canApply());
  const QString film = QStringLiteral("library:films/kodak_vision3_250d.cube");
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "activateEntry", Q_RETURN_ARG(QVariant, applied),
                                        Q_ARG(QVariant, film)));
  EXPECT_FALSE(applied.toBool());
  EXPECT_EQ(h.browser.focusedEntryId(), film);
  EXPECT_EQ(h.source.submit_count, submits_before_target_loss);
  auto* message = h.Find(QStringLiteral("editorLutTargetMessage"));
  ASSERT_NE(message, nullptr);
  EXPECT_TRUE(message->isVisible());
  EXPECT_EQ(message->property("text").toString(), h.target.targetMessage());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, FavoritesFacetListLayoutAndFilterFold) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  ASSERT_NE(h.result, nullptr) << h.Warnings();

  // Starring through the page updates the Favorites facet; the facet is a filter choice.
  const QString teal = QStringLiteral("library:general/teal.cube");
  QVariant      starred;
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "toggleFavorite",
                                        Q_RETURN_ARG(QVariant, starred), Q_ARG(QVariant, teal)));
  ASSERT_TRUE(starred.toBool());
  EXPECT_TRUE(h.browser.isFavorite(teal));
  EXPECT_EQ(h.Find(QStringLiteral("editorLutFavoritesOnly")), nullptr);
  auto* favorites = h.Find(QStringLiteral("editorLutFacet_favorites_favorites"));
  ASSERT_NE(favorites, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(favorites, "activate"));
  EXPECT_TRUE(h.browser.favoritesOnly());
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 1; }, 2000));
  EXPECT_EQ(h.Tiles().front()->property("entryId").toString(), teal);
  favorites = h.Find(QStringLiteral("editorLutFacet_favorites_favorites"));
  ASSERT_NE(favorites, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(favorites, "activate"));
  EXPECT_FALSE(h.browser.favoritesOnly());
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 5; }, 2000));

  // The list layout keeps one column of rows without the cube placeholder.
  h.panel->setProperty("viewMode", QStringLiteral("list"));
  ASSERT_TRUE(WaitUntil([&] { return h.result->property("columns").toInt() == 1; }, 2000));
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 5; }, 2000));
  for (QQuickItem* tile : h.Tiles()) {
    QQuickItem* cube = ChildNamed(tile, QStringLiteral("editorLutTileCube"));
    ASSERT_NE(cube, nullptr);
    EXPECT_FALSE(cube->isVisible());
    EXPECT_TRUE(ChildNamed(tile, QStringLiteral("editorLutFavoriteStar"))->isVisible());
  }
  h.panel->setProperty("viewMode", QStringLiteral("grid"));
  ASSERT_TRUE(WaitUntil([&] { return h.result->property("columns").toInt() >= 2; }, 2000));

  // Folding the filters gives the results the full width.
  auto* filter_host = h.Find(QStringLiteral("editorLutFilterHost"));
  ASSERT_NE(filter_host, nullptr);
  const qreal docked_width = h.result->width();
  h.panel->setProperty("filtersVisible", false);
  ASSERT_TRUE(WaitUntil([&] { return !filter_host->isVisible(); }, 2000));
  EXPECT_GT(h.result->width(), docked_width);
  EXPECT_NEAR(h.result->x(), 0.0, 0.5);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, NarrowPageShrinksTilesThenClosesTheFilters) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* filter_host = h.Find(QStringLiteral("editorLutFilterHost"));
  ASSERT_NE(filter_host, nullptr);
  const int min_tile = AppTheme::Instance().editorLutTileMinWidth();
  auto tiles_have_width = [&](qreal below) {
    const QList<QQuickItem*> tiles = h.Tiles();
    if (tiles.isEmpty()) return false;
    for (QQuickItem* tile : tiles) {
      if (tile->width() <= 0.0 || tile->width() >= below) return false;
    }
    return true;
  };

  // Narrower than one full tile column beside the sidebar: the sidebar stays docked and the
  // single column shrinks instead of being covered.
  h.window->setWidth(340);
  ASSERT_TRUE(WaitUntil([&] { return tiles_have_width(min_tile); }, 2000));
  EXPECT_TRUE(h.panel->property("filterDocked").toBool());
  EXPECT_TRUE(h.panel->property("filtersOpen").toBool());
  EXPECT_TRUE(filter_host->isVisible());
  EXPECT_EQ(h.result->property("columns").toInt(), 1);
  EXPECT_GE(h.result->x(), filter_host->width());

  // Narrower still: the sidebar closes and the results take the whole page. The user's
  // choice is kept, so the sidebar returns once the page is wide enough.
  h.window->setWidth(280);
  ASSERT_TRUE(WaitUntil([&] { return !filter_host->isVisible(); }, 2000));
  EXPECT_FALSE(h.panel->property("filtersOpen").toBool());
  EXPECT_TRUE(h.panel->property("filtersVisible").toBool());
  EXPECT_NEAR(h.result->x(), 0.0, 0.5);
  EXPECT_TRUE(tiles_have_width(h.result->width()));
  h.window->setWidth(660);
  ASSERT_TRUE(WaitUntil([&] { return filter_host->isVisible(); }, 2000));
  EXPECT_TRUE(h.panel->property("filtersOpen").toBool());

  // Opened by hand on a narrow page, the sidebar floats over the results; narrowing the
  // page again after it docks closes it.
  h.window->setWidth(280);
  ASSERT_TRUE(WaitUntil([&] { return !filter_host->isVisible(); }, 2000));
  ASSERT_TRUE(QMetaObject::invokeMethod(h.panel, "toggleFilters"));
  ASSERT_TRUE(WaitUntil([&] { return filter_host->isVisible(); }, 2000));
  auto* filter_card = h.Find(QStringLiteral("editorLutFilterCard"));
  ASSERT_NE(filter_card, nullptr);
  EXPECT_TRUE(filter_card->property("floating").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(h.panel, "toggleFilters"));
  ASSERT_TRUE(WaitUntil([&] { return !filter_host->isVisible(); }, 2000));
  EXPECT_FALSE(h.panel->property("filtersVisible").toBool());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, PrintSectionListsEachPrintFilm) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* section = h.Find(QStringLiteral("editorLutPrintSection"));
  ASSERT_NE(section, nullptr);
  EXPECT_TRUE(section->isVisible());
  EXPECT_EQ(h.Find(QStringLiteral("editorLutFacet_print_with_print")), nullptr);
  auto* print = h.Find(QStringLiteral("editorLutFacet_print_kodak_vision_2383"));
  ASSERT_NE(print, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(print, "activate"));
  EXPECT_EQ(h.browser.print(), QStringLiteral("kodak_vision_2383"));
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 1; }, 2000));
  EXPECT_EQ(h.Tiles().front()->property("entryId").toString(),
            QStringLiteral("library:films/kodak_vision3_250d__kodak_vision_2383.cube"));
  EXPECT_TRUE(h.Find(QStringLiteral("editorLutClearFilters"))->isVisible());

  print = h.Find(QStringLiteral("editorLutFacet_print_kodak_vision_2383"));
  ASSERT_NE(print, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(print, "activate"));
  EXPECT_EQ(h.browser.print(), QString());
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 5; }, 2000));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, EmptyLibraryLinksToLutSettings) {
  BrowserHarness h({});
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  ASSERT_NE(h.result, nullptr) << h.Warnings();
  BrowserHost host;
  h.panel->setProperty("host", QVariant::fromValue(static_cast<QObject*>(&host)));
  ASSERT_TRUE(WaitUntil([&] { return !h.library.Service()->busy(); }, 3000));
  ProcessEvents(50);

  EXPECT_TRUE(h.Tiles().isEmpty());
  QQuickItem* settings = h.Find(QStringLiteral("editorLutEmptySettingsButton"));
  ASSERT_NE(settings, nullptr);
  EXPECT_TRUE(settings->isVisible());
  EXPECT_TRUE(h.Find(QStringLiteral("editorLutEmptyImportButton"))->isVisible());
  ASSERT_TRUE(QMetaObject::invokeMethod(settings, "clicked"));
  EXPECT_EQ(host.lut_settings, 1);
  EXPECT_EQ(host.import_dialogs, 0);
  // Nothing was applied to the target.
  EXPECT_EQ(h.source.submit_count, 0);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

// L4: the LUT adjustment panel (EditorLutControlPanel.qml) shows the stored input and output
// encodings in its two combos and the ACES 2.0 note for a display-referred side, and follows a
// stored change without a submit. Pointer delivery is not used; the combo writes are covered by
// LutLibraryControllerTest.EncodingSelectionSubmitsOneSettledWriteOfThatSideOnly.
TEST(EditorLutBrowserPanelQmlTest, ControlPanelShowsStoredEncodingsAndDisplayNote) {
  TemporaryLutLibrary  library(LibraryFiles());
  DocumentTargetSource source;
  LutLibraryModel      browser;
  LutLibraryController target;
  browser.setLibrary(library.Service());
  target.setLibrary(library.Service());
  target.SetTargetSource(&source);
  const auto write_lut = [&](EditorLutWrite write) {
    EditorParameterTarget field;
    field.owner_kind             = EditorParameterOwnerKind::ColorGrade;
    field.node_id                = kGradeB;
    field.adjustment_instance_id = AdjustmentInstanceId{source.LmtInstance(kGradeB)};
    field.field_key              = "lut";
    std::string error;
    EXPECT_TRUE(ApplyEditorParameterWrite(source.Mutable(), field, write, &error)) << error;
    target.reload();
  };
  EditorLutWrite stored;
  stored.reference       = LibraryLutReference{"general/teal.cube"};
  stored.input_encoding  = "sony_slog3_sgamut3cine";
  stored.output_encoding = "rec709_bt1886";
  write_lut(stored);

  LutAppModules         modules{&browser, &target, library.Service()};
  QQmlApplicationEngine engine;
  QStringList           warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, [&warnings](const QList<QQmlError>& list) {
    for (const QQmlError& warning : list) warnings << warning.toString();
  });
  AppTheme::Instance().setReduceMotion(true);
  QQuickStyle::setStyle(QStringLiteral("Basic"));
  RegisterShortcutRegistryQmlType();
  RegisterEditorAdjustmentQmlTypes();
  engine.addImportPath(QStringLiteral("qrc:/"));
  engine.addImportPath(SrcQmlDir());
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &modules);
  engine.rootContext()->setContextProperty(
      QStringLiteral("panelSourceUrl"),
      QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/EditorLutControlPanel.qml")));
  engine.loadData(QByteArray{kHarnessQml}, QUrl(QStringLiteral("file:///LutControlHarness.qml")));
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
  ASSERT_NE(window, nullptr) << warnings.join(QLatin1Char('\n')).toStdString();
  window->show();
  (void)QTest::qWaitForWindowExposed(window);
  const auto find = [window](const QString& name) {
    return ChildNamed(window->contentItem(), name);
  };
  ASSERT_TRUE(WaitUntil([&] { return find(QStringLiteral("editorAdjustmentPanel_lut")); }, 3000))
      << warnings.join(QLatin1Char('\n')).toStdString();

  QQuickItem* input_combo  = find(QStringLiteral("editorLutInputEncodingCombo"));
  QQuickItem* output_combo = find(QStringLiteral("editorLutOutputEncodingCombo"));
  QQuickItem* output_note  = find(QStringLiteral("editorLutOutputDisplayNote"));
  QQuickItem* input_note   = find(QStringLiteral("editorLutInputDisplayNote"));
  ASSERT_NE(input_combo, nullptr);
  ASSERT_NE(output_combo, nullptr);
  ASSERT_NE(output_note, nullptr);
  ASSERT_NE(input_note, nullptr);
  EXPECT_EQ(input_combo->property("displayText").toString(),
            QStringLiteral("Sony S-Log3 / S-Gamut3.Cine"));
  EXPECT_EQ(output_combo->property("displayText").toString(), QStringLiteral("Rec.709 BT.1886"));
  EXPECT_TRUE(input_combo->isEnabled());
  EXPECT_TRUE(output_note->isVisible());
  EXPECT_FALSE(input_note->isVisible());

  EditorLutWrite scene_output;
  scene_output.output_encoding = "acescc";
  write_lut(scene_output);
  ASSERT_TRUE(WaitUntil(
      [&] {
        return output_combo->property("displayText").toString() == QStringLiteral("ACEScc (AP1)");
      },
      2000));
  EXPECT_FALSE(output_note->isVisible());
  EXPECT_EQ(input_combo->property("displayText").toString(),
            QStringLiteral("Sony S-Log3 / S-Gamut3.Cine"));
  EXPECT_EQ(source.submit_count, 0) << "loading the encodings never submits";

  // L5: the "remember" checkbox follows the library and its toggle writes the library only.
  QQuickItem* remember = find(QStringLiteral("editorLutRememberEncodings"));
  ASSERT_NE(remember, nullptr);
  EXPECT_TRUE(remember->isVisible());
  EXPECT_FALSE(remember->property("checked").toBool());
  ASSERT_TRUE(target.setRememberEncodings(true));
  ASSERT_TRUE(WaitUntil([&] { return remember->property("checked").toBool(); }, 2000));
  ASSERT_TRUE(QMetaObject::invokeMethod(remember, "toggle"));
  ASSERT_TRUE(WaitUntil([&] { return !remember->property("checked").toBool(); }, 2000));
  EXPECT_FALSE(library.Service()->RememberedEncodings("library:general/teal.cube").has_value());
  EXPECT_EQ(source.submit_count, 0) << "the checkbox never submits an edit";
  EditorLutWrite no_lut;
  no_lut.reference = LutReference{};
  write_lut(no_lut);
  ASSERT_TRUE(WaitUntil([&] { return !remember->isVisible(); }, 2000));
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
}

}  // namespace alcedo::ui::test

#include "editor_lut_browser_panel_qml_test.moc"
