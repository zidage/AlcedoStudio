//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Search palette QML (global_search_redesign_plan.md, Phase GS4): SearchPalette.qml loaded
// from the source tree with a real ApplicationModuleHost. Offscreen pointer delivery is not
// reliable, so the tests call the palette functions and the row signals that the mouse and the
// keys call. The C++ tests of SearchController (AlbumBackendSearchWorkerTest) own the behavior.

#include <QApplication>
#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QUrl>
#include <QtGlobal>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/search_session.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

using GlobalSearchDialogQmlTests = ApplicationModuleHostTestFixture;

constexpr int  kPageSize          = 24;
constexpr int  kPreviewImageCount = 6;

constexpr char kHarnessQml[]      = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    width: 1280
    height: 900
    visible: true
    color: "#111214"

    property alias dialog: dialogLoader.item

    Loader {
        id: dialogLoader
        anchors.fill: parent
        asynchronous: false
        source: dialogSourceUrl

        onLoaded: {
            if (!item) {
                return
            }
            item.searchController = appModules.search
            item.interactionPolicyController = appModules.interactionPolicy
            item.blurSource = null
            item.cornerRadius = 0
        }
    }
}
)";

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs, int stepMs = 50) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(stepMs);
  }
  return predicate();
}

/// QSettings reads and writes nothing without an organization name. The palette tests use a
/// test identity and remove the search keys they change, before and after the test.
class ScopedPaletteSettings {
 public:
  ScopedPaletteSettings() {
    QCoreApplication::setOrganizationName(QStringLiteral("PuerhLabTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GlobalSearchDialogQmlTest"));
    RemoveKeys();
  }
  ~ScopedPaletteSettings() { RemoveKeys(); }

  ScopedPaletteSettings(const ScopedPaletteSettings&)            = delete;
  ScopedPaletteSettings& operator=(const ScopedPaletteSettings&) = delete;

 private:
  static void RemoveKeys() {
    QSettings settings;
    for (const auto* key : {"search/naturalLanguageSearchEnabled", "search/semanticEnabled",
                            "search/settingsPanelOpen", "search/recentQueries"}) {
      settings.remove(QLatin1String(key));
    }
  }
};

auto PickPreviewSource() -> std::filesystem::path {
  for (const std::string& subdir :
       {"batch_import", "portrait/dng", "landscape", "airplane", "plant"}) {
    auto images = CollectRawTestImages(subdir, 1);
    if (!images.empty()) {
      return images.front();
    }
  }
  return {};
}

/// Copies of one RAW file named preview_search_<nn>.<ext>, so a field query matches them all.
auto MakePreviewDataset(const std::filesystem::path& temp_dir, int count)
    -> std::vector<std::filesystem::path> {
  const auto source = PickPreviewSource();
  if (source.empty()) {
    return {};
  }
  const auto dataset_dir = temp_dir / "search_palette_preview_dataset";
  std::filesystem::create_directories(dataset_dir);
  std::vector<std::filesystem::path> paths;
  for (int i = 0; i < count; ++i) {
    std::ostringstream name;
    name << "preview_search_" << std::setw(2) << std::setfill('0') << i
         << source.extension().string();
    const auto destination = dataset_dir / name.str();
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing);
    paths.push_back(destination);
  }
  return paths;
}

auto SearchPaletteFileUrl() -> QUrl {
  const auto qml_path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" /
                        "SearchPalette.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(qml_path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(qml_path.string()));
#endif
}

void CollectQuickTreeObjects(QObject* root, std::vector<QObject*>& objects) {
  if (root == nullptr) {
    return;
  }
  objects.push_back(root);
  if (auto* item = qobject_cast<QQuickItem*>(root); item != nullptr) {
    for (QQuickItem* child : item->childItems()) {
      CollectQuickTreeObjects(child, objects);
    }
    return;
  }
  for (QObject* child : root->children()) {
    CollectQuickTreeObjects(child, objects);
  }
}

/// The photo preview frames of the palette: items with an element id and a thumbReady state.
auto CollectPreviewFrames(QObject* root) -> std::vector<QObject*> {
  std::vector<QObject*> objects;
  CollectQuickTreeObjects(root, objects);
  std::vector<QObject*> frames;
  for (QObject* object : objects) {
    if (object != nullptr && object->property("thumbReady").isValid() &&
        object->property("elementId").toInt() > 0) {
      frames.push_back(object);
    }
  }
  return frames;
}

auto ReadyFrameUrls(QObject* root) -> std::vector<QString> {
  std::vector<QString> urls;
  for (QObject* frame : CollectPreviewFrames(root)) {
    if (frame->property("thumbReady").toBool()) {
      urls.push_back(frame->property("liveThumbUrl").toString());
    }
  }
  return urls;
}

auto FindNamedObjects(QObject* root, const QString& object_name) -> std::vector<QObject*> {
  std::vector<QObject*> objects;
  CollectQuickTreeObjects(root, objects);
  std::vector<QObject*> named;
  for (QObject* object : objects) {
    if (object != nullptr && object->objectName() == object_name) {
      named.push_back(object);
    }
  }
  return named;
}

/// Saves the rendered window as <name>.png in ALCEDO_SEARCH_PALETTE_SCREENSHOT_DIR when the
/// variable is set (the screenshots of the plan completion record). Does nothing otherwise.
void SaveScreenshotIfRequested(QObject* window, const char* name) {
  const QString dir          = qEnvironmentVariable("ALCEDO_SEARCH_PALETTE_SCREENSHOT_DIR");
  auto*         quick_window = qobject_cast<QQuickWindow*>(window);
  if (dir.isEmpty() || quick_window == nullptr) {
    return;
  }
  ProcessEvents(400);
  const QImage image = quick_window->grabWindow();
  EXPECT_FALSE(image.isNull()) << name;
  EXPECT_TRUE(image.save(dir + QStringLiteral("/") + QLatin1String(name) + QStringLiteral(".png")))
      << name;
}

/// A loaded palette harness over @p backend.
class PaletteHarness {
 public:
  explicit PaletteHarness(ApplicationModuleHost& backend) {
    auto*       app       = qobject_cast<QApplication*>(QCoreApplication::instance());
    static bool style_set = false;
    if (app != nullptr && !style_set) {
      style_set = true;
      QQuickStyle::setStyle(QStringLiteral("Basic"));
      AppTheme::RegisterFonts();
      AppTheme::ApplyApplicationFont(*app);
    }
    engine_.addImportPath(QStringLiteral("qrc:/"));
    backend.AttachQmlEngine(&engine_);
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), &backend);
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("dialogSourceUrl"),
                                              SearchPaletteFileUrl());
    engine_.loadData(QByteArray{kHarnessQml},
                     QUrl(QStringLiteral("file:///SearchPaletteTestHarness.qml")));
    if (engine_.rootObjects().empty()) {
      return;
    }
    window_ = engine_.rootObjects().front();
    WaitUntil(
        [&]() {
          palette_ = qvariant_cast<QObject*>(window_->property("dialog"));
          return palette_ != nullptr;
        },
        10000);
  }

  [[nodiscard]] auto window() const -> QObject* { return window_; }
  [[nodiscard]] auto palette() const -> QObject* { return palette_; }

  auto               Open() -> bool {
    return palette_ != nullptr && QMetaObject::invokeMethod(palette_, "openFromCollection") &&
           WaitUntil([&]() { return palette_->property("visible").toBool(); }, 5000);
  }
  auto Close() -> bool {
    return palette_ != nullptr && QMetaObject::invokeMethod(palette_, "close") &&
           WaitUntil([&]() { return !palette_->property("visible").toBool(); }, 5000);
  }
  auto SearchField() const -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(QStringLiteral("globalSearchField"))
                              : nullptr;
  }
  /// Types @p text and runs the preview at once (the debounce would run it after 140 ms).
  auto Type(const QString& text) -> bool {
    auto* field = SearchField();
    if (field == nullptr) {
      return false;
    }
    field->setProperty("text", text);
    return QMetaObject::invokeMethod(palette_, "refreshPreview");
  }

 private:
  QQmlApplicationEngine engine_;
  QObject*              window_  = nullptr;
  QObject*              palette_ = nullptr;
};

auto ImportPreviewDataset(ApplicationModuleHost& backend, const std::filesystem::path& temp_dir)
    -> bool {
  const auto dataset = MakePreviewDataset(temp_dir, kPreviewImageCount);
  if (dataset.size() != static_cast<size_t>(kPreviewImageCount)) {
    return false;
  }
  backend.import_export()->StartImport(PathsToQStringList(dataset));
  WaitForImportFinished(backend, 180000);
  return !backend.import_export()->ImportRunning() &&
         backend.import_export()->ImportCompleted() >= kPreviewImageCount &&
         backend.library()->ShownCount() >= kPreviewImageCount;
}

/// Waits until the palette shows three preview frames with ready image://alcedo-thumb/ URLs that
/// resolve to images.
auto WaitForThreeReadyPreviews(ApplicationModuleHost& backend, QObject* window) -> bool {
  if (!WaitUntil([&]() { return ReadyFrameUrls(window).size() == 3; }, 60000)) {
    ADD_FAILURE() << "ready frames: " << ReadyFrameUrls(window).size()
                  << ", frames: " << CollectPreviewFrames(window).size();
    return false;
  }
  EXPECT_TRUE(WaitUntil(
      [&]() {
        for (QObject* frame : CollectPreviewFrames(window)) {
          if (!frame->property("imageLoaded").toBool()) {
            return false;
          }
        }
        return true;
      },
      10000))
      << "A ready preview URL did not load as an image.";
  auto store = backend.library()->thumbs().image_store();
  for (const auto& url : ReadyFrameUrls(window)) {
    EXPECT_TRUE(url.startsWith(QStringLiteral("image://alcedo-thumb/"))) << url.toStdString();
    EXPECT_TRUE(store && !store->ResolveUrl(url).isNull()) << url.toStdString();
  }
  const QString dir = qEnvironmentVariable("ALCEDO_SEARCH_PALETTE_SCREENSHOT_DIR");
  if (!dir.isEmpty() && store && !ReadyFrameUrls(window).empty()) {
    store->ResolveUrl(ReadyFrameUrls(window).front())
        .save(dir + QStringLiteral("/preview_thumbnail_source.png"));
  }
  return true;
}

}  // namespace

TEST_F(GlobalSearchDialogQmlTests, FieldQueryShowsThreePreviewThumbnails) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  ASSERT_TRUE(ImportPreviewDataset(backend, temp_dir_)) << "Need a RAW test image.";
  backend.search()->SetNaturalLanguageSearchEnabled(false);

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr) << "SearchPalette failed to load.";
  ASSERT_TRUE(harness.Open());
  ASSERT_NE(harness.SearchField(), nullptr);

  ASSERT_TRUE(harness.Type(QStringLiteral("preview_search")));
  SaveScreenshotIfRequested(harness.window(), "field_query_loading");
  // The field preview holds SearchSession::kPreviewRows rows; the palette shows them as frames.
  ASSERT_TRUE(WaitUntil(
      [&]() {
        return harness.palette()->property("results").toList().size() ==
                   SearchSession::kPreviewRows &&
               !harness.palette()->property("searchLoading").toBool();
      },
      20000));
  EXPECT_EQ(harness.palette()->property("queryRoute").toString(), QStringLiteral("fuzzy"));
  EXPECT_EQ(CollectPreviewFrames(harness.window()).size(), 3u);
  EXPECT_TRUE(WaitForThreeReadyPreviews(backend, harness.window()));
  SaveScreenshotIfRequested(harness.window(), "field_query_previews");

  // A query without matches shows the empty text in place of the previews.
  ASSERT_TRUE(harness.Type(QStringLiteral("no_photo_has_this_name")));
  ASSERT_TRUE(WaitUntil(
      [&]() {
        return harness.palette()->property("photosStatusText").toString() ==
               QStringLiteral("No matching photos in this folder");
      },
      10000));
  EXPECT_TRUE(CollectPreviewFrames(harness.window()).empty());
  SaveScreenshotIfRequested(harness.window(), "field_query_no_match");
  ASSERT_TRUE(harness.Type(QStringLiteral("preview_search")));
  ASSERT_TRUE(WaitForThreeReadyPreviews(backend, harness.window()));

  // Enter applies the field search to the Library and closes the palette.
  ASSERT_TRUE(QMetaObject::invokeMethod(harness.palette(), "handleSearchSubmit"));
  EXPECT_TRUE(WaitUntil([&]() { return !harness.palette()->property("visible").toBool(); }, 5000));
  EXPECT_TRUE(WaitUntil(
      [&]() { return backend.search()->active_search_query() == QStringLiteral("preview_search"); },
      10000));
  // The frames released their thumbnails when the palette closed.
  EXPECT_TRUE(CollectPreviewFrames(harness.window()).empty());
}

TEST_F(GlobalSearchDialogQmlTests, PreviewThumbnailsRenderAfterReopen) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  ASSERT_TRUE(ImportPreviewDataset(backend, temp_dir_)) << "Need a RAW test image.";
  backend.search()->SetNaturalLanguageSearchEnabled(false);
  QSignalSpy     previews(backend.search(), &SearchController::SearchPreviewThumbnailUpdated);

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());
  ASSERT_TRUE(harness.Type(QStringLiteral("preview_search")));
  ASSERT_TRUE(WaitForThreeReadyPreviews(backend, harness.window()));
  ASSERT_TRUE(harness.Close());
  const int updates_before_reopen = previews.count();

  ASSERT_TRUE(harness.Open());
  EXPECT_TRUE(CollectPreviewFrames(harness.window()).empty());
  ASSERT_TRUE(harness.Type(QStringLiteral("preview_search")));
  EXPECT_TRUE(WaitForThreeReadyPreviews(backend, harness.window()));
  EXPECT_GT(previews.count(), updates_before_reopen);
  ASSERT_TRUE(harness.Close());
}

TEST_F(GlobalSearchDialogQmlTests, SemanticQueryShowsEnterHintAndNoPreviews) {
  ScopedPaletteSettings settings;
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  auto* search = backend.search();
  search->SetNaturalLanguageSearchEnabled(true);
  QSignalSpy     responses(search, &SearchController::SearchResponseReady);

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());
  auto* palette = harness.palette();

  ASSERT_TRUE(harness.Type(QStringLiteral("sunset over the mountains")));
  ProcessEvents(300);
  EXPECT_EQ(palette->property("queryRoute").toString(), QStringLiteral("semantic"));
  EXPECT_EQ(palette->property("photosStatusText").toString(),
            QStringLiteral("Press Enter to search by meaning."));
  EXPECT_FALSE(palette->property("photosStatusIsError").toBool());
  EXPECT_FALSE(palette->property("searchLoading").toBool());
  EXPECT_TRUE(palette->property("results").toList().empty());
  EXPECT_TRUE(CollectPreviewFrames(harness.window()).empty());
  EXPECT_EQ(responses.count(), 0) << "Typing a semantic query must send no preview request.";
  const auto route_labels =
      FindNamedObjects(harness.window(), QStringLiteral("searchPaletteRouteLabel"));
  ASSERT_EQ(route_labels.size(), 1u);
  EXPECT_TRUE(route_labels.front()->property("visible").toBool());
  SaveScreenshotIfRequested(harness.window(), "semantic_query_hint");

  // A label query still uses field search and requests the preview.
  ASSERT_TRUE(harness.Type(QStringLiteral("portrait")));
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 1; }, 10000));
  EXPECT_EQ(palette->property("queryRoute").toString(), QStringLiteral("fuzzy"));
  EXPECT_FALSE(route_labels.front()->property("visible").toBool());

  // With the switch off, a natural-language query uses field search.
  search->SetNaturalLanguageSearchEnabled(false);
  ASSERT_TRUE(harness.Type(QStringLiteral("sunset over the mountains")));
  EXPECT_EQ(palette->property("queryRoute").toString(), QStringLiteral("fuzzy"));
  ASSERT_TRUE(harness.Close());
}

TEST_F(GlobalSearchDialogQmlTests, RecommendationRowAppliesStatsFilterAndCloses) {
  ScopedPaletteSettings         settings;
  std::vector<LibraryPhotoSpec> specs(4);
  specs[0].model_   = "Canon EOS R5";
  specs[1].model_   = "Canon EOS R5";
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, specs.size(), specs);
  ASSERT_TRUE(packed.has_value());
  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());

  // The typed query shows the matching filter row.
  ASSERT_TRUE(harness.Type(QStringLiteral("r5")));
  ProcessEvents(100);
  const auto rows = FindNamedObjects(harness.window(), QStringLiteral("searchPaletteFilterRow"));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front()->property("title").toString(), QStringLiteral("Canon EOS R5"));
  EXPECT_EQ(rows.front()->property("trailingText").toString(), QStringLiteral("2"));
  EXPECT_FALSE(rows.front()->property("categoryText").toString().isEmpty());
  SaveScreenshotIfRequested(harness.window(), "filter_row");

  // The row click handler emits activated().
  ASSERT_TRUE(QMetaObject::invokeMethod(rows.front(), "activated"));
  EXPECT_TRUE(WaitUntil([&]() { return !harness.palette()->property("visible").toBool(); }, 5000));
  EXPECT_EQ(backend.stats()->filter_camera(), QStringLiteral("Canon EOS R5"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(backend.library()->TotalCount(), 2);
  EXPECT_TRUE(backend.search()->active_search_query().isEmpty());
}

TEST_F(GlobalSearchDialogQmlTests, RejectedSemanticEnterKeepsPaletteOpen) {
  // Typing arms the 140 ms debounce. Enter must stop it before it applies the natural-language
  // search; otherwise a late preview clears the rejection text. The test project has no
  // semantic model, so the apply is rejected.
  ScopedPaletteSettings settings;
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  auto* search = backend.search();
  search->SetNaturalLanguageSearchEnabled(true);
  QSignalSpy     responses(search, &SearchController::SearchResponseReady);

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());
  auto* palette = harness.palette();

  harness.SearchField()->setProperty("text", QStringLiteral("sunset over the mountains"));
  ASSERT_TRUE(QMetaObject::invokeMethod(palette, "handleSearchSubmit"));
  ProcessEvents(400);

  EXPECT_TRUE(palette->property("visible").toBool());
  const auto status = palette->property("photosStatusText").toString();
  EXPECT_FALSE(status.isEmpty());
  EXPECT_FALSE(status.contains(QStringLiteral("Press Enter"))) << status.toStdString();
  EXPECT_TRUE(palette->property("photosStatusIsError").toBool());
  const auto errors =
      FindNamedObjects(harness.window(), QStringLiteral("searchPalettePhotosError"));
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(errors.front()->property("visible").toBool());
  SaveScreenshotIfRequested(harness.window(), "semantic_enter_rejected");
  EXPECT_EQ(responses.count(), 0);
  EXPECT_TRUE(search->active_search_query().isEmpty());
  EXPECT_FALSE(backend.library()->QueryUpdating());
  ASSERT_TRUE(harness.Close());
}

TEST_F(GlobalSearchDialogQmlTests, SettingsPanelStatePersists) {
  ScopedPaletteSettings settings;
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());
  auto* palette = harness.palette();
  EXPECT_FALSE(palette->property("settingsPanelOpen").toBool());
  const auto drawers = FindNamedObjects(harness.window(), QStringLiteral("searchSettingsDrawer"));
  ASSERT_EQ(drawers.size(), 1u);
  auto* drawer = qobject_cast<QQuickItem*>(drawers.front());
  ASSERT_NE(drawer, nullptr);
  EXPECT_FALSE(drawer->isVisible());
  const auto shells = FindNamedObjects(harness.window(), QStringLiteral("searchPaletteShell"));
  ASSERT_EQ(shells.size(), 1u);
  auto* shell = qobject_cast<QQuickItem*>(shells.front());
  ASSERT_NE(shell, nullptr);
  ASSERT_TRUE(WaitUntil([&]() { return shell->width() > 0 && shell->x() > 0; }, 3000));
  const qreal closed_x     = shell->x();
  const qreal closed_width = shell->width();
  // A project without photos and without recent searches shows the empty hint.
  const auto  hints = FindNamedObjects(harness.window(), QStringLiteral("searchPaletteEmptyHint"));
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_TRUE(hints.front()->property("visible").toBool());
  SaveScreenshotIfRequested(harness.window(), "empty_query");

  // The drawer opens below the input row, inside the palette; the palette keeps its width.
  ASSERT_TRUE(QMetaObject::invokeMethod(palette, "toggleSettingsPanel"));
  EXPECT_TRUE(palette->property("settingsPanelOpen").toBool());
  ASSERT_TRUE(WaitUntil(
      [&]() {
        return drawer->isVisible() && qFuzzyCompare(drawer->opacity(), 1.0) && drawer->height() > 0;
      },
      3000));
  EXPECT_DOUBLE_EQ(shell->x(), closed_x);
  EXPECT_DOUBLE_EQ(shell->width(), closed_width);
  EXPECT_DOUBLE_EQ(drawer->width(), shell->width());
  auto* field = qobject_cast<QQuickItem*>(harness.SearchField());
  ASSERT_NE(field, nullptr);
  EXPECT_GT(drawer->mapToItem(shell, QPointF(0, 0)).y(),
            field->mapToItem(shell, QPointF(0, 0)).y());
  SaveScreenshotIfRequested(harness.window(), "settings_drawer_open");
  ASSERT_TRUE(harness.Close());

  ASSERT_TRUE(harness.Open());
  EXPECT_TRUE(palette->property("settingsPanelOpen").toBool());
  EXPECT_TRUE(drawer->isVisible());
  // A new controller reads the stored state.
  const SearchController reader(nullptr, nullptr, nullptr, nullptr);
  EXPECT_TRUE(reader.SearchSettingsPanelOpen());

  // In a narrow window the open palette stays inside the window margins.
  harness.window()->setProperty("width", 900);
  const int margin = AppTheme::Instance().searchPaletteWindowMarginX();
  EXPECT_TRUE(WaitUntil(
      [&]() { return shell->x() >= margin && shell->x() + shell->width() <= 900 - margin; }, 3000))
      << "x=" << shell->x() << " width=" << shell->width();
  SaveScreenshotIfRequested(harness.window(), "narrow_window_settings_drawer");
  harness.window()->setProperty("width", 1280);

  ASSERT_TRUE(QMetaObject::invokeMethod(palette, "toggleSettingsPanel"));
  EXPECT_FALSE(palette->property("settingsPanelOpen").toBool());
  EXPECT_TRUE(WaitUntil([&]() { return !drawer->isVisible(); }, 3000));
  ASSERT_TRUE(harness.Close());
}

TEST_F(GlobalSearchDialogQmlTests, CloseButtonClosesPalette) {
  ScopedPaletteSettings settings;
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());

  const auto buttons =
      FindNamedObjects(harness.window(), QStringLiteral("searchPaletteCloseButton"));
  ASSERT_EQ(buttons.size(), 1u);
  EXPECT_EQ(buttons.front()->property("iconSrc").toUrl(),
            QUrl(QStringLiteral("qrc:/panel_icons/close.svg")));
  ASSERT_TRUE(QMetaObject::invokeMethod(buttons.front(), "clicked"));
  EXPECT_TRUE(WaitUntil([&]() { return !harness.palette()->property("visible").toBool(); }, 5000));
}

// The persisted natural-language switch reaches SearchController at construction, but
// InteractionPolicyController receives it only from the palette. Opening the palette pushes it,
// so the field check boxes are disabled from the first frame after a restart.
TEST_F(GlobalSearchDialogQmlTests, NaturalLanguageSwitchStateIsReadOnOpen) {
  ScopedPaletteSettings settings;
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  auto* search = backend.search();
  ASSERT_NE(backend.interaction_policy(), nullptr);
  search->SetNaturalLanguageSearchEnabled(true);
  search->SetSearchSettingsPanelOpen(true);
  ASSERT_TRUE(backend.interaction_policy()->property("canChangeSearchFieldFilters").toBool());

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());

  EXPECT_FALSE(backend.interaction_policy()->property("canChangeSearchFieldFilters").toBool());
  for (const auto* name : {"searchFieldFilenameCheckBox", "searchFieldExifCheckBox",
                           "searchFieldAiDescriptionCheckBox", "searchFieldAiTagsCheckBox"}) {
    const auto boxes = FindNamedObjects(harness.window(), QLatin1String(name));
    ASSERT_EQ(boxes.size(), 1u) << name;
    EXPECT_FALSE(boxes.front()->property("enabled").toBool()) << name;
  }
  const auto switches =
      FindNamedObjects(harness.window(), QStringLiteral("naturalLanguageSearchSwitch"));
  ASSERT_EQ(switches.size(), 1u);
  EXPECT_TRUE(switches.front()->property("checked").toBool());
  // The disabled look of the check boxes replaces the reason text.
  EXPECT_TRUE(
      FindNamedObjects(harness.window(), QStringLiteral("searchFieldFiltersReason")).empty());
  SaveScreenshotIfRequested(harness.window(), "disabled_field_check_boxes");
  ASSERT_TRUE(harness.Close());
  search->SetNaturalLanguageSearchEnabled(false);
}

TEST_F(GlobalSearchDialogQmlTests, SearchControllerClassifiesAndRoutesBySemanticToggle) {
  ScopedPaletteSettings settings;
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto* search_controller = backend.search();
  ASSERT_NE(search_controller, nullptr);
  search_controller->SetNaturalLanguageSearchEnabled(false);
  ASSERT_FALSE(search_controller->natural_language_search_enabled());

  EXPECT_EQ(search_controller->ClassifyQuery(QString()).toStdString(), "empty");
  EXPECT_EQ(search_controller->ClassifyQuery(QStringLiteral("portrait")).toStdString(), "label");
  EXPECT_EQ(search_controller->ClassifyQuery(QStringLiteral("人像")).toStdString(), "label");
  EXPECT_EQ(search_controller->ClassifyQuery(QStringLiteral("Canon")).toStdString(), "traditional");
  EXPECT_EQ(search_controller->ClassifyQuery(QStringLiteral("2024-03-01")).toStdString(),
            "traditional");
  // Switch off: natural language stays on the ordinary path.
  EXPECT_EQ(
      search_controller->ClassifyQuery(QStringLiteral("sunset over the mountains")).toStdString(),
      "traditional");

  search_controller->SetNaturalLanguageSearchEnabled(true);
  ASSERT_TRUE(search_controller->natural_language_search_enabled());
  // Switch on: label and metadata routes are unchanged; only natural language goes semantic.
  EXPECT_EQ(search_controller->ClassifyQuery(QStringLiteral("portrait")).toStdString(), "label");
  EXPECT_EQ(search_controller->ClassifyQuery(QStringLiteral("Canon")).toStdString(), "traditional");
  EXPECT_EQ(
      search_controller->ClassifyQuery(QStringLiteral("sunset over the mountains")).toStdString(),
      "semantic");
  EXPECT_TRUE(
      QSettings{}.value(QStringLiteral("search/naturalLanguageSearchEnabled"), false).toBool());

  // A semantic submit without a ready model reports the unavailable state; it does not fall
  // back to a C++ vector scan.
  QSignalSpy responses(search_controller, &SearchController::SearchResponseReady);
  const auto submit_id = search_controller->RequestSubmitSearch(
      QStringLiteral("sunset over the mountains"), 0, kPageSize);
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 1; }, 10000));
  EXPECT_EQ(responses.at(0).at(0).toULongLong(), submit_id);
  const auto semantic_response = responses.at(0).at(2).toMap();
  EXPECT_EQ(semantic_response.value("route").toString().toStdString(), "semantic");
  EXPECT_TRUE(semantic_response.value("semanticUnavailable").toBool());
  EXPECT_TRUE(semantic_response.value("rows").toList().empty());

  // A typing request on the semantic route does not call the provider.
  search_controller->RequestSearch(QStringLiteral("sunset over the mountains"), 0, kPageSize);
  ASSERT_TRUE(WaitUntil([&]() { return responses.count() == 2; }, 10000));
  const auto typing_response = responses.at(1).at(2).toMap();
  EXPECT_TRUE(typing_response.value("awaitingSubmit").toBool());
  EXPECT_TRUE(typing_response.value("rows").toList().empty());
  search_controller->SetNaturalLanguageSearchEnabled(false);
}

// Twenty characters typed quickly, each one sending its own request (the worst case, where the
// 140 ms debounce elapses between keys). The search worker runs at most the first and the newest
// request, and the palette applies only the last response.
TEST_F(GlobalSearchDialogQmlTests, TypingTwentyCharactersQuicklyAppliesOnlyTheLastResponse) {
  // 101 synthetic files, so the 20-character name album-delete-100.dng exists.
  const auto packed_project = CreatePopulatedPackedProject(temp_dir_, {}, 101);
  ASSERT_TRUE(packed_project.has_value());
  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, packed_project->packed_path_));
  auto* search_controller = backend.search();
  search_controller->SetNaturalLanguageSearchEnabled(false);

  PaletteHarness harness(backend);
  ASSERT_NE(harness.palette(), nullptr);
  ASSERT_TRUE(harness.Open());
  auto*    palette       = harness.palette();
  QObject* preview_timer = nullptr;
  for (QObject* object : palette->findChildren<QObject*>()) {
    if (QString::fromLatin1(object->metaObject()->className()).contains(QStringLiteral("Timer")) &&
        object->property("interval").toInt() == 140) {
      preview_timer = object;
    }
  }
  ASSERT_NE(preview_timer, nullptr) << "The 140 ms typing debounce timer was not found.";

  const QString query = QStringLiteral("album-delete-100.dng");
  ASSERT_EQ(query.size(), 20);
  QSignalSpy responses(search_controller, &SearchController::SearchResponseReady);
  QSignalSpy applied_results(palette, SIGNAL(resultsChanged()));

  for (int length = 1; length <= query.size(); ++length) {
    ASSERT_TRUE(harness.Type(query.left(length)));
  }
  // Every keystroke sent its request above; the debounce must not send a 21st.
  ASSERT_TRUE(QMetaObject::invokeMethod(preview_timer, "stop"));
  const auto last_request_id = palette->property("activeSearchRequestId").toULongLong();
  ASSERT_NE(last_request_id, 0u);

  ASSERT_TRUE(WaitUntil(
      [&]() { return !palette->property("searchLoading").toBool() && responses.count() >= 1; },
      10000));
  // A stale response would arrive right after the first; give it the time.
  ProcessEvents(300);

  ASSERT_EQ(responses.count(), 1) << "Only the newest request may deliver a response.";
  EXPECT_EQ(responses.at(0).at(0).toULongLong(), last_request_id);
  EXPECT_EQ(applied_results.count(), 1) << "The palette must apply exactly one response.";
  const auto rows = palette->property("results").toList();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows.front().toMap().value("fileName").toString(), query);
  EXPECT_EQ(palette->property("lastQuery").toString(), query);
  ASSERT_TRUE(harness.Close());
}

}  // namespace alcedo::ui::test
