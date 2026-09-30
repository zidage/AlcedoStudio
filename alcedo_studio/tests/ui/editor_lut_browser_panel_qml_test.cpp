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

/// The browser page in a window over a temporary library and a document target.
struct BrowserHarness {
  TemporaryLutLibrary   library{LibraryFiles()};
  DocumentTargetSource  source;
  LutLibraryModel       browser;
  LutLibraryController  target;
  LutAppModules         modules{&browser, &target, library.Service()};
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QQuickItem*           panel  = nullptr;
  QQuickItem*           result = nullptr;
  QStringList           warnings;

  BrowserHarness() {
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

}  // namespace alcedo::ui::test

#include "editor_lut_browser_panel_qml_test.moc"
