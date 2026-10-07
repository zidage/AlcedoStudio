//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LUT browser page (LUT library plan L6A): loads production EditorLutBrowserPanel.qml from the
// source tree over a real LutLibraryService, LutLibraryModel, and LutLibraryController, and checks
// the tiles, titles, filter combo boxes, and tile application through the page's own functions.
// Pointer delivery is not used: offscreen input is unreliable, and the owners are covered by C++
// tests.

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
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
#include <cstdio>
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

/// Index of the listed choice with @p value in a filter combo box, or -1.
auto ChoiceIndex(QQuickItem* combo, const QString& value) -> int {
  const QVariantList choices = combo->property("shownChoices").toList();
  for (int i = 0; i < choices.size(); ++i) {
    if (choices[i].toMap().value(QStringLiteral("value")).toString() == value) return i;
  }
  return -1;
}

/// Activate the choice with @p value, as choosing it in the popup does.
auto ChooseFilter(QQuickItem* combo, const QString& value) -> bool {
  const int index = ChoiceIndex(combo, value);
  return index >= 0 && QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
}

}  // namespace

TEST(EditorLutBrowserPanelQmlTest, BrowserLoadsFilterBarAndOneTilePerResultWithoutWarnings) {
  BrowserHarness h;
  ASSERT_NE(h.window, nullptr) << h.Warnings();
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  EXPECT_NE(h.Find(QStringLiteral("editorLutFilterBar")), nullptr);
  // The filter sidebar and its counts are gone.
  EXPECT_EQ(h.Find(QStringLiteral("editorLutFilterCard")), nullptr);
  EXPECT_EQ(h.Find(QStringLiteral("editorLutFilterToggle")), nullptr);
  // Every combo box at All shows its dimension name.
  for (const auto& [name, title] :
       {std::pair{"editorLutCategoryFilter", "Category"},
        std::pair{"editorLutSourceFilter", "Source"}, std::pair{"editorLutBrandFilter", "Brand"},
        std::pair{"editorLutPrintFilter", "Print"}}) {
    QQuickItem* combo = h.Find(QString::fromLatin1(name));
    ASSERT_NE(combo, nullptr) << name;
    EXPECT_TRUE(combo->isVisible()) << name;
    EXPECT_EQ(combo->property("displayText").toString(), QString::fromLatin1(title)) << name;
  }
  // The footer hint names the user folder.
  QQuickItem* hint = h.Find(QStringLiteral("editorLutUserFolderHint"));
  ASSERT_NE(hint, nullptr);
  EXPECT_TRUE(
      hint->property("text").toString().contains(h.library.Service()->user_directory_path()));
  EXPECT_NE(h.Find(QStringLiteral("editorLutSearchInput")), nullptr);
  // No target block above the tiles and no LUT count: the footer names the target.
  EXPECT_EQ(h.Find(QStringLiteral("editorLutTargetIndicator")), nullptr);
  EXPECT_EQ(h.Find(QStringLiteral("editorLutCountText")), nullptr);
  ASSERT_EQ(h.browser.count(), 5);
  EXPECT_EQ(h.Tiles().size(), h.browser.count());
  EXPECT_GE(h.result->property("columns").toInt(), 2);
  auto* status = h.Find(QStringLiteral("editorLutTargetStatus"));
  ASSERT_NE(status, nullptr);
  ASSERT_FALSE(h.target.hasAssociation());
  EXPECT_EQ(status->property("text").toString(),
            QStringLiteral("Choose a LUT to apply to %1").arg(h.target.targetNodeName()));
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

TEST(EditorLutBrowserPanelQmlTest, CategoryComboSelectsUserAndAllReturnsToEveryLut) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* category = h.Find(QStringLiteral("editorLutCategoryFilter"));
  ASSERT_NE(category, nullptr);
  ASSERT_TRUE(ChooseFilter(category, QStringLiteral("general")));
  EXPECT_EQ(h.browser.category(), QStringLiteral("general"));
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == h.browser.count(); }, 2000));
  EXPECT_EQ(h.browser.count(), 3);
  EXPECT_EQ(category->property("displayText").toString(), QStringLiteral("User"));
  EXPECT_TRUE(h.Find(QStringLiteral("editorLutClearFilters"))->isVisible());
  // Brand and Print do not apply to User LUTs.
  EXPECT_FALSE(h.Find(QStringLiteral("editorLutBrandFilter"))->isVisible());
  EXPECT_FALSE(h.Find(QStringLiteral("editorLutPrintFilter"))->isVisible());

  ASSERT_TRUE(ChooseFilter(category, QStringLiteral("all")));
  EXPECT_EQ(h.browser.category(), QStringLiteral("all"));
  EXPECT_EQ(h.browser.count(), 5);
  EXPECT_EQ(category->property("displayText").toString(), QStringLiteral("Category"));
  EXPECT_FALSE(h.Find(QStringLiteral("editorLutClearFilters"))->isVisible());
  EXPECT_TRUE(h.Find(QStringLiteral("editorLutBrandFilter"))->isVisible());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
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
  // The footer names the applied LUT and its node.
  ASSERT_TRUE(WaitUntil(
      [&] {
        return h.Find(QStringLiteral("editorLutTargetStatus"))->property("text").toString() ==
               QStringLiteral("%1 applied to %2")
                   .arg(h.target.associationName(), h.target.targetNodeName());
      },
      2000));

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
  auto* message = h.Find(QStringLiteral("editorLutTargetStatus"));
  ASSERT_NE(message, nullptr);
  EXPECT_TRUE(message->isVisible());
  EXPECT_EQ(message->property("text").toString(), h.target.targetMessage());
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, FavoritesToggleAndListLayout) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  ASSERT_NE(h.result, nullptr) << h.Warnings();

  // Starring through the page, then the toolbar star shows only favorites.
  const QString teal = QStringLiteral("library:general/teal.cube");
  QVariant      starred;
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "toggleFavorite",
                                        Q_RETURN_ARG(QVariant, starred), Q_ARG(QVariant, teal)));
  ASSERT_TRUE(starred.toBool());
  EXPECT_TRUE(h.browser.isFavorite(teal));
  auto* favorites = h.Find(QStringLiteral("editorLutFavoritesToggle"));
  ASSERT_NE(favorites, nullptr);
  EXPECT_FALSE(favorites->property("selected").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(favorites, "clicked"));
  EXPECT_TRUE(h.browser.favoritesOnly());
  EXPECT_TRUE(favorites->property("selected").toBool());
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 1; }, 2000));
  EXPECT_EQ(h.Tiles().front()->property("entryId").toString(), teal);
  ASSERT_TRUE(QMetaObject::invokeMethod(favorites, "clicked"));
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
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, NarrowPageWrapsTheFilterRowAndKeepsTheResultsFullWidth) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* grid = h.Find(QStringLiteral("editorLutFilterGrid"));
  ASSERT_NE(grid, nullptr);
  // Wide: the four combo boxes share one row.
  ASSERT_TRUE(WaitUntil([&] { return grid->property("columns").toInt() == 4; }, 2000));

  // Narrow: two per row, every combo box inside the page, results across the full width.
  h.window->setWidth(280);
  ASSERT_TRUE(WaitUntil([&] { return grid->property("columns").toInt() == 2; }, 2000));
  ProcessEvents(50);
  for (const char* name : {"editorLutCategoryFilter", "editorLutSourceFilter",
                           "editorLutBrandFilter", "editorLutPrintFilter"}) {
    QQuickItem* combo = h.Find(QString::fromLatin1(name));
    ASSERT_NE(combo, nullptr) << name;
    const QRectF in_panel = combo->mapRectToItem(h.panel, combo->boundingRect());
    EXPECT_GT(combo->width(), 0.0) << name;
    EXPECT_LE(in_panel.right(), h.panel->width() + 0.5) << name;
  }
  const QRectF result = h.result->mapRectToItem(h.panel, h.result->boundingRect());
  EXPECT_NEAR(result.width(), h.panel->width() - 2 * AppTheme::Instance().spaceSm(), 1.0);
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, PrintComboListsEachPrintAndNoPrint) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* print = h.Find(QStringLiteral("editorLutPrintFilter"));
  ASSERT_NE(print, nullptr);
  EXPECT_TRUE(print->isVisible());
  const QString no_print = QString::fromLatin1(kLutNoPrintKey);
  EXPECT_EQ(ChoiceIndex(print, no_print), 1);
  ASSERT_GE(ChoiceIndex(print, QStringLiteral("kodak_vision_2383")), 0);

  ASSERT_TRUE(ChooseFilter(print, QStringLiteral("kodak_vision_2383")));
  EXPECT_EQ(h.browser.print(), QStringLiteral("kodak_vision_2383"));
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 1; }, 2000));
  EXPECT_EQ(h.Tiles().front()->property("entryId").toString(),
            QStringLiteral("library:films/kodak_vision3_250d__kodak_vision_2383.cube"));
  EXPECT_EQ(print->property("displayText").toString(), QStringLiteral("Vision 2383"));

  // No print: the film simulation without a print, not the User LUTs.
  ASSERT_TRUE(ChooseFilter(print, no_print));
  EXPECT_EQ(h.browser.print(), no_print);
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 1; }, 2000));
  EXPECT_EQ(h.Tiles().front()->property("entryId").toString(),
            QStringLiteral("library:films/kodak_vision3_250d.cube"));
  EXPECT_EQ(print->property("displayText").toString(), QStringLiteral("No print"));

  auto* clear = h.Find(QStringLiteral("editorLutClearFilters"));
  ASSERT_NE(clear, nullptr);
  EXPECT_TRUE(clear->isVisible());
  ASSERT_TRUE(QMetaObject::invokeMethod(clear, "clicked"));
  EXPECT_EQ(h.browser.print(), QString());
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 5; }, 2000));
  EXPECT_EQ(print->property("displayText").toString(), QStringLiteral("Print"));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, ChoicesWithoutLutsAreHiddenAndAChosenValueIsNotHighlighted) {
  BrowserHarness h;
  ASSERT_NE(h.panel, nullptr) << h.Warnings();
  auto* category = h.Find(QStringLiteral("editorLutCategoryFilter"));
  auto* print    = h.Find(QStringLiteral("editorLutPrintFilter"));
  ASSERT_NE(category, nullptr);
  ASSERT_NE(print, nullptr);
  ASSERT_GE(ChoiceIndex(category, QStringLiteral("general")), 0);
  QObject* border =
      category->property("background").value<QQuickItem*>()->property("border").value<QObject*>();
  ASSERT_NE(border, nullptr);
  const QVariant idle_color = border->property("color");

  // With the 2383 print chosen, no User LUT matches: User leaves the category list while
  // the model still declares it.
  ASSERT_TRUE(ChooseFilter(print, QStringLiteral("kodak_vision_2383")));
  ASSERT_TRUE(WaitUntil([&] { return h.Tiles().size() == 1; }, 2000));
  EXPECT_EQ(ChoiceIndex(category, QStringLiteral("general")), -1);
  EXPECT_GE(ChoiceIndex(category, QStringLiteral("film_simulation")), 0);
  bool declared = false;
  for (const QVariant& choice : h.browser.categoryChoices()) {
    declared |= choice.toMap().value(QStringLiteral("value")).toString() == "general";
  }
  EXPECT_TRUE(declared);

  // The chosen print keeps the idle border and weight: no outline, no focus border.
  QObject* print_border =
      print->property("background").value<QQuickItem*>()->property("border").value<QObject*>();
  EXPECT_EQ(print_border->property("width").toInt(), 1);
  EXPECT_EQ(print_border->property("color"), idle_color);
  EXPECT_FALSE(print->hasActiveFocus());
  EXPECT_EQ(
      print->property("contentItem").value<QQuickItem*>()->property("font").value<QFont>().weight(),
      QFont::Weight(AppTheme::Instance().fontWeightRegular()));
  EXPECT_TRUE(h.warnings.isEmpty()) << h.Warnings();
}

TEST(EditorLutBrowserPanelQmlTest, OpeningAndClearingFiltersScrollTheAppliedLutIntoView) {
  std::vector<std::pair<std::string, std::string>> files;
  for (int i = 0; i < 40; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "user/look_%02d.cube", i);
    files.emplace_back(name, CubeWithMetadata({}));
  }
  BrowserHarness h(std::move(files));
  ASSERT_NE(h.result, nullptr) << h.Warnings();
  h.panel->setProperty("viewMode", QStringLiteral("list"));
  const QString last = QStringLiteral("library:user/look_39.cube");
  QVariant      applied;
  ASSERT_TRUE(QMetaObject::invokeMethod(h.result, "activateEntry", Q_RETURN_ARG(QVariant, applied),
                                        Q_ARG(QVariant, last)));
  ASSERT_EQ(h.source.ApplyQueued(), 1);
  h.target.reload();
  ASSERT_EQ(h.browser.appliedEntryId(), last);

  auto* rows = h.Find(QStringLiteral("editorLutTileRows"));
  ASSERT_NE(rows, nullptr);
  ASSERT_TRUE(WaitUntil(
      [&] { return rows->property("contentHeight").toReal() > rows->height() + 100; }, 2000));
  auto applied_tile_in_view = [&] {
    QQuickItem* tile = h.TileFor(last);
    if (tile == nullptr) return false;
    const QRectF in_rows = tile->mapRectToItem(rows, tile->boundingRect());
    return in_rows.top() >= -0.5 && in_rows.bottom() <= rows->height() + 0.5;
  };

  // The rail restores a stored position when the page opens; the applied LUT wins.
  ASSERT_TRUE(QMetaObject::invokeMethod(h.panel, "restoreListContentY", Q_ARG(QVariant, 0)));
  ASSERT_TRUE(WaitUntil(applied_tile_in_view, 2000));
  EXPECT_GT(rows->property("contentY").toReal(), 0.0);

  // Scrolled away under a filter, clearing the filters shows the applied LUT again.
  rows->setProperty("contentY", 0);
  ASSERT_FALSE(applied_tile_in_view());
  h.browser.setSource(QStringLiteral("none"));
  auto* clear = h.Find(QStringLiteral("editorLutClearFilters"));
  ASSERT_NE(clear, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return clear->isVisible(); }, 2000));
  ASSERT_TRUE(QMetaObject::invokeMethod(clear, "clicked"));
  ASSERT_TRUE(WaitUntil(applied_tile_in_view, 2000));
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
