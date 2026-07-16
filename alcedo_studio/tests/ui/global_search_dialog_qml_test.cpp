//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/album_backend_test_fixture.hpp"

#include <QApplication>
#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QSettings>
#include <QSignalSpy>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

using GlobalSearchDialogQmlTests = ApplicationModuleHostTestFixture;

constexpr int kPageSize        = 24;
constexpr int kSearchItemCount = 30;

constexpr char kHarnessQml[] = R"(
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
            item.backend = appModules
            item.theme = null
            item.blurSource = null
            item.cornerRadius = 0
        }
    }
}
)";

void WaitForImportFinished(ApplicationModuleHost& backend, int timeoutMs = 180000) {
  QSignalSpy spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  const int  stepMs  = 200;
  int        waited  = 0;

  while (backend.import_export()->ImportRunning() && waited < timeoutMs) {
    spy.wait(stepMs);
    waited += stepMs;
  }

  ProcessEvents(500);
}

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs,
               int stepMs = 50) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(stepMs);
  }
  return predicate();
}

auto PickScrollableSearchSource() -> std::filesystem::path {
  for (const std::string& subdir :
       {"batch_import", "portrait/dng", "landscape", "airplane", "plant"}) {
    auto images = CollectRawTestImages(subdir, 1);
    if (!images.empty()) {
      return images.front();
    }
  }
  return {};
}

auto MakeSearchDataset(const std::filesystem::path& tempDir, int count)
    -> std::vector<std::filesystem::path> {
  const auto source = PickScrollableSearchSource();
  if (source.empty()) {
    return {};
  }

  const auto datasetDir = tempDir / "global_search_dialog_scroll_dataset";
  std::filesystem::create_directories(datasetDir);

  std::vector<std::filesystem::path> paths;
  paths.reserve(static_cast<size_t>(count));

  for (int i = 0; i < count; ++i) {
    std::ostringstream name;
    name << "scroll_search_" << std::setw(2) << std::setfill('0') << i
         << source.extension().string();
    const auto dst = datasetDir / name.str();
    std::filesystem::copy_file(source, dst, std::filesystem::copy_options::overwrite_existing);
    paths.push_back(dst);
  }

  return paths;
}

auto GlobalSearchDialogFileUrl() -> QUrl {
  const auto qmlPath = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" /
                       "GlobalSearchDialog.qml";
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(qmlPath.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(qmlPath.string()));
#endif
}

auto FindSearchField(QObject* root) -> QObject* {
  if (root == nullptr) {
    return nullptr;
  }
  const auto objects = root->findChildren<QObject*>();
  const auto it = std::find_if(objects.begin(), objects.end(), [](QObject* object) {
    if (object == nullptr) {
      return false;
    }
    const QString className = QString::fromLatin1(object->metaObject()->className());
    return className.contains(QStringLiteral("TextInput")) &&
           object->property("text").isValid();
  });
  return it != objects.end() ? *it : nullptr;
}

auto FindVisibleListView(QObject* root) -> QObject* {
  if (root == nullptr) {
    return nullptr;
  }
  const auto objects = root->findChildren<QObject*>();
  const auto it = std::find_if(objects.begin(), objects.end(), [](QObject* object) {
    if (object == nullptr) {
      return false;
    }
    const QString className = QString::fromLatin1(object->metaObject()->className());
    if (!className.contains(QStringLiteral("ListView"))) {
      return false;
    }
    return object->property("visible").toBool() && object->property("count").isValid() &&
           object->property("height").toReal() > 0.0;
  });
  return it != objects.end() ? *it : nullptr;
}

void CollectQuickTreeObjects(QObject* root, std::vector<QObject*>& objects) {
  if (root == nullptr) {
    return;
  }

  objects.push_back(root);

  if (auto* item = qobject_cast<QQuickItem*>(root); item != nullptr) {
    const auto visualChildren = item->childItems();
    for (QQuickItem* child : visualChildren) {
      CollectQuickTreeObjects(child, objects);
    }
    return;
  }

  const auto childObjects = root->children();
  for (QObject* child : childObjects) {
    CollectQuickTreeObjects(child, objects);
  }
}

auto CollectSearchRows(QObject* root) -> std::vector<QObject*> {
  std::vector<QObject*> rows;
  if (root == nullptr) {
    return rows;
  }

  std::vector<QObject*> objects;
  CollectQuickTreeObjects(root, objects);
  rows.reserve(static_cast<size_t>(objects.size()));
  for (QObject* object : objects) {
    if (object == nullptr || !object->property("elementId").isValid() ||
        !object->property("thumbReady").isValid()) {
      continue;
    }
    if (object->property("elementId").toInt() <= 0) {
      continue;
    }
    rows.push_back(object);
  }
  return rows;
}

auto RowOrdinal(QObject* row) -> std::optional<int> {
  if (row == nullptr) {
    return std::nullopt;
  }

  const QString title  = row->property("title").toString();
  const QString prefix = QStringLiteral("scroll_search_");
  const int     start  = title.indexOf(prefix);
  if (start < 0) {
    return std::nullopt;
  }

  QString digits;
  for (int i = start + prefix.size(); i < title.size(); ++i) {
    const QChar ch = title.at(i);
    if (!ch.isDigit()) {
      break;
    }
    digits.append(ch);
  }

  bool ok = false;
  const int ordinal = digits.toInt(&ok);
  return ok ? std::optional<int>{ordinal} : std::nullopt;
}

auto RowMatchesMinOrdinal(QObject* row, int minOrdinalInclusive) -> bool {
  if (minOrdinalInclusive <= 0) {
    return true;
  }

  const auto ordinal = RowOrdinal(row);
  return ordinal.has_value() && ordinal.value() >= minOrdinalInclusive;
}

auto BusyOrReadyRowCount(QObject* root, int minOrdinalInclusive = 0) -> int {
  int count = 0;
  for (QObject* row : CollectSearchRows(root)) {
    if (!RowMatchesMinOrdinal(row, minOrdinalInclusive)) {
      continue;
    }
    if (row->property("liveThumbLoading").toBool() || row->property("thumbReady").toBool()) {
      ++count;
    }
  }
  return count;
}

auto HasVisibleRowAtOrAfter(QObject* root, int minOrdinalInclusive) -> bool {
  for (QObject* row : CollectSearchRows(root)) {
    if (RowMatchesMinOrdinal(row, minOrdinalInclusive)) {
      return true;
    }
  }
  return false;
}

auto ReadyRowCount(QObject* root, int minOrdinalInclusive = 0) -> int {
  int readyCount = 0;
  for (QObject* row : CollectSearchRows(root)) {
    if (!RowMatchesMinOrdinal(row, minOrdinalInclusive)) {
      continue;
    }
    if (row->property("thumbReady").toBool()) {
      ++readyCount;
    }
  }
  return readyCount;
}

auto FirstReadyDataUrl(QObject* root, int minOrdinalInclusive = 0) -> QString {
  for (QObject* row : CollectSearchRows(root)) {
    if (!RowMatchesMinOrdinal(row, minOrdinalInclusive)) {
      continue;
    }
    if (!row->property("thumbReady").toBool()) {
      continue;
    }
    return row->property("liveThumbUrl").toString();
  }
  return {};
}

auto DecodeDataUrlImage(const QString& dataUrl) -> QImage {
  const int comma = dataUrl.indexOf(',');
  if (comma < 0) {
    return {};
  }

  const QByteArray encoded = dataUrl.mid(comma + 1).toLatin1();
  QImage           image;
  image.loadFromData(QByteArray::fromBase64(encoded));
  return image;
}

auto SearchPreviewReadySignalCount(const QSignalSpy& spy) -> int {
  int readySignals = 0;
  for (const auto& call : spy) {
    if (call.size() < 2) {
      continue;
    }
    if (!call.at(1).toString().isEmpty()) {
      ++readySignals;
    }
  }
  return readySignals;
}

auto VariantToJsonString(const QVariant& value) -> std::string {
  const QJsonValue jsonValue = QJsonValue::fromVariant(value);
  if (jsonValue.isObject()) {
    return QJsonDocument(jsonValue.toObject()).toJson(QJsonDocument::Compact).toStdString();
  }
  if (jsonValue.isArray()) {
    return QJsonDocument(jsonValue.toArray()).toJson(QJsonDocument::Compact).toStdString();
  }
  return value.toString().toStdString();
}

auto CurrentRowSummary(QObject* root) -> std::string {
  std::ostringstream summary;
  bool               first = true;
  for (QObject* row : CollectSearchRows(root)) {
    if (!first) {
      summary << " | ";
    }
    first = false;
    const auto ordinal = RowOrdinal(row);
    summary << "{ord=" << (ordinal.has_value() ? std::to_string(ordinal.value()) : "?")
            << ", title=" << row->property("title").toString().toStdString()
            << ", initUrl=" << !row->property("initialThumbUrl").toString().isEmpty()
            << ", initLoading=" << row->property("initialThumbLoading").toBool()
            << ", ready=" << row->property("thumbReady").toBool()
            << ", loading=" << row->property("liveThumbLoading").toBool()
            << ", liveUrl=" << !row->property("liveThumbUrl").toString().isEmpty() << "}";
  }
  return summary.str();
}

}  // namespace

TEST_F(GlobalSearchDialogQmlTests,
       ScrolledSearchResultsRenderPreviewThumbnailsAndReopenStillWorks) {
  auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
  ASSERT_NE(app, nullptr);

  QQuickStyle::setStyle(QStringLiteral("Material"));
  AppTheme::RegisterFonts();
  AppTheme::ApplyApplicationFont(*app);

  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  const auto searchDataset = MakeSearchDataset(temp_dir_, kSearchItemCount);
  ASSERT_EQ(searchDataset.size(), static_cast<size_t>(kSearchItemCount))
      << "Need at least 30 searchable RAW files to exercise page 2 thumbnail loading.";

  backend.import_export()->StartImport(PathsToQStringList(searchDataset));
  WaitForImportFinished(backend);

  ASSERT_FALSE(backend.import_export()->ImportRunning());
  ASSERT_GE(backend.import_export()->ImportCompleted(), kSearchItemCount);
  ASSERT_GE(backend.library()->ShownCount(), kSearchItemCount);

  auto* searchController = backend.search();
  ASSERT_NE(searchController, nullptr);
  QSignalSpy previewSpy(searchController, &SearchController::SearchPreviewThumbnailUpdated);

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/"));
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &backend);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("dialogSourceUrl"),
                                           GlobalSearchDialogFileUrl());
  engine.loadData(QByteArray{kHarnessQml},
                  QUrl(QStringLiteral("file:///GlobalSearchDialogTestHarness.qml")));

  ASSERT_FALSE(engine.rootObjects().empty()) << "QML harness failed to load.";

  QObject* windowRoot = engine.rootObjects().front();
  ASSERT_NE(windowRoot, nullptr);

  QObject* dialog = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    dialog = qvariant_cast<QObject*>(windowRoot->property("dialog"));
    return dialog != nullptr;
  }, 10000))
      << "GlobalSearchDialog failed to instantiate from "
      << GlobalSearchDialogFileUrl().toString().toStdString();

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openFromCollection"));
  ASSERT_TRUE(WaitUntil([&]() { return dialog->property("visible").toBool(); }, 5000));

  QObject* searchField = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    searchField = FindSearchField(windowRoot);
    return searchField != nullptr;
  }, 5000))
      << "Search field not found in GlobalSearchDialog object tree.";

  searchField->setProperty("text", QStringLiteral("scroll_search"));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "refreshPreview"));

  ASSERT_TRUE(WaitUntil([&]() {
    return dialog->property("searchTotal").toInt() >= kSearchItemCount &&
           dialog->property("results").toList().size() == kPageSize;
  }, 20000))
      << "Search preview did not return the expected first page. total="
      << dialog->property("searchTotal").toInt()
      << " rows=" << dialog->property("results").toList().size();

  QObject* resultList = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    resultList = FindVisibleListView(windowRoot);
    return resultList != nullptr && resultList->property("contentHeight").toReal() > 0.0;
  }, 10000))
      << "Visible result ListView was not created.";

  QObject* resultContent = qvariant_cast<QObject*>(resultList->property("contentItem"));
  ASSERT_NE(resultContent, nullptr) << "ListView contentItem is missing.";

  ASSERT_TRUE(WaitUntil([&]() { return BusyOrReadyRowCount(resultContent, 0) > 0; }, 10000))
      << "Visible search rows never entered loading/ready state. signals="
      << previewSpy.count() << " readySignals="
      << SearchPreviewReadySignalCount(previewSpy)
      << " previewThumbs=" << VariantToJsonString(dialog->property("previewThumbs"))
      << " rows=" << CurrentRowSummary(resultContent);

  ASSERT_TRUE(WaitUntil([&]() { return ReadyRowCount(resultContent, 0) > 0; }, 60000))
      << "First page never rendered any preview thumbnails. rows="
      << CurrentRowSummary(resultContent);

  const QString firstPageUrl = FirstReadyDataUrl(resultContent, 0);
  ASSERT_FALSE(firstPageUrl.isEmpty());
  EXPECT_FALSE(DecodeDataUrlImage(firstPageUrl).isNull())
      << "First rendered preview did not decode as an image.";

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "loadMorePreview"));

  ASSERT_TRUE(WaitUntil([&]() {
    return dialog->property("results").toList().size() >= kSearchItemCount &&
           !dialog->property("searchLoading").toBool();
  }, 30000))
      << "Scrolling near the end did not append the second search page. rows="
      << dialog->property("results").toList().size()
      << " loading=" << dialog->property("searchLoading").toBool();

  ASSERT_TRUE(QMetaObject::invokeMethod(resultList, "positionViewAtEnd"));

  ASSERT_TRUE(WaitUntil([&]() { return HasVisibleRowAtOrAfter(resultContent, kPageSize); }, 10000))
      << "Second-page delegates never entered the visible QML tree. rows="
      << CurrentRowSummary(resultContent);

  ASSERT_TRUE(WaitUntil([&]() { return BusyOrReadyRowCount(resultContent, kPageSize) > 0; },
                        10000))
      << "Second-page rows never entered loading/ready state after scroll. signals="
      << previewSpy.count() << " readySignals="
      << SearchPreviewReadySignalCount(previewSpy)
      << " previewThumbs=" << VariantToJsonString(dialog->property("previewThumbs"))
      << " rows=" << CurrentRowSummary(resultContent);

  ASSERT_TRUE(WaitUntil([&]() { return ReadyRowCount(resultContent, kPageSize) > 0; }, 60000))
      << "Second-page rows never rendered a preview thumbnail after scroll. rows="
      << CurrentRowSummary(resultContent);

  const QString secondPageUrl = FirstReadyDataUrl(resultContent, kPageSize);
  ASSERT_FALSE(secondPageUrl.isEmpty());
  EXPECT_FALSE(DecodeDataUrlImage(secondPageUrl).isNull())
      << "Second-page rendered preview did not decode as an image.";

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
  ASSERT_TRUE(WaitUntil([&]() { return !dialog->property("visible").toBool(); }, 5000));

  const int readySignalCountBeforeReopen = SearchPreviewReadySignalCount(previewSpy);

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openFromCollection"));
  ASSERT_TRUE(WaitUntil([&]() { return dialog->property("visible").toBool(); }, 5000));

  searchField = FindSearchField(windowRoot);
  ASSERT_NE(searchField, nullptr);
  searchField->setProperty("text", QStringLiteral("scroll_search"));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "refreshPreview"));

  ASSERT_TRUE(WaitUntil([&]() {
    return dialog->property("searchTotal").toInt() >= kSearchItemCount &&
           dialog->property("results").toList().size() == kPageSize;
  }, 20000))
      << "Reopened dialog did not rebuild the first search page.";

  resultList = FindVisibleListView(windowRoot);
  ASSERT_NE(resultList, nullptr);
  resultContent = qvariant_cast<QObject*>(resultList->property("contentItem"));
  ASSERT_NE(resultContent, nullptr);

  ASSERT_TRUE(WaitUntil([&]() { return BusyOrReadyRowCount(resultContent, 0) > 0; }, 10000))
      << "Reopened dialog never entered loading/ready state. signals="
      << previewSpy.count() << " readySignals="
      << SearchPreviewReadySignalCount(previewSpy)
      << " previewThumbs=" << VariantToJsonString(dialog->property("previewThumbs"))
      << " rows=" << CurrentRowSummary(resultContent);

  ASSERT_TRUE(WaitUntil([&]() { return ReadyRowCount(resultContent, 0) > 0; }, 60000))
      << "Reopened dialog stayed on placeholder previews. rows="
      << CurrentRowSummary(resultContent);

  const QString reopenedUrl = FirstReadyDataUrl(resultContent, 0);
  ASSERT_FALSE(reopenedUrl.isEmpty());
  EXPECT_FALSE(DecodeDataUrlImage(reopenedUrl).isNull())
      << "Reopened dialog produced a non-decodable preview image.";

  EXPECT_GT(SearchPreviewReadySignalCount(previewSpy), readySignalCountBeforeReopen)
      << "Reopening the dialog did not emit any new ready preview updates.";
}

TEST_F(GlobalSearchDialogQmlTests, SearchControllerClassifiesAndRoutesBySemanticToggle) {
  // QSettings is inert until an organization/application name is set (the real
  // app sets this in main.cpp). Use a test-scoped identity so the persistence
  // assertion is meaningful and isolated, then clean up the key.
  QCoreApplication::setOrganizationName(QStringLiteral("PuerhLabTest"));
  QCoreApplication::setApplicationName(QStringLiteral("GlobalSearchDialogQmlTest"));
  QSettings{}.remove(QStringLiteral("search/naturalLanguageSearchEnabled"));

  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto* searchController = backend.search();
  ASSERT_NE(searchController, nullptr);

  // Deterministic start state (QSettings persists across runs).
  searchController->SetNaturalLanguageSearchEnabled(false);
  ASSERT_FALSE(searchController->natural_language_search_enabled());

  EXPECT_EQ(searchController->ClassifyQuery(QString()).toStdString(), "empty");
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("portrait")).toStdString(), "label");
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("人像")).toStdString(), "label");
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("Canon")).toStdString(), "traditional");
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("2024-03-01")).toStdString(),
            "traditional");
  // Toggle off: natural language stays on the ordinary path.
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("sunset over the mountains"))
                .toStdString(),
            "traditional");

  searchController->SetNaturalLanguageSearchEnabled(true);
  ASSERT_TRUE(searchController->natural_language_search_enabled());
  // Toggle on: label and metadata routes are unchanged; only NL goes semantic.
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("portrait")).toStdString(), "label");
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("Canon")).toStdString(), "traditional");
  EXPECT_EQ(searchController->ClassifyQuery(QStringLiteral("sunset over the mountains"))
                .toStdString(),
            "semantic");

  // Persistence: the toggle writes through to QSettings.
  EXPECT_TRUE(
      QSettings{}.value(QStringLiteral("search/naturalLanguageSearchEnabled"), false).toBool());

  // SubmitSearch routing: semantic route surfaces an unavailable state when the
  // active model/runtime path is not ready; it must not fall back to a C++ vector scan.
  const auto semanticResp = searchController->SubmitSearch(
      QStringLiteral("sunset over the mountains"), 0, kPageSize);
  EXPECT_EQ(semanticResp.value("route").toString().toStdString(), "semantic");
  EXPECT_TRUE(semanticResp.value("semanticUnavailable").toBool());
  EXPECT_TRUE(semanticResp.value("rows").toList().empty());

  // Typing (SearchPreview) on a semantic route must NOT call the provider: it
  // returns an awaiting-submit stub.
  const auto typingResp =
      searchController->SearchPreview(QStringLiteral("sunset over the mountains"), 0, kPageSize);
  EXPECT_EQ(typingResp.value("route").toString().toStdString(), "semantic");
  EXPECT_TRUE(typingResp.value("awaitingSubmit").toBool());
  EXPECT_TRUE(typingResp.value("rows").toList().empty());

  // Label and empty submit routes carry their route name on the response.
  EXPECT_EQ(searchController->SubmitSearch(QStringLiteral("portrait"), 0, kPageSize)
                .value("route")
                .toString()
                .toStdString(),
            "label");
  EXPECT_EQ(searchController->SubmitSearch(QString(), 0, kPageSize)
                .value("route")
                .toString()
                .toStdString(),
            "empty");

  // Restore default to avoid polluting other tests / future runs. Remove the
  // legacy pre-rename key too so a stale "true" can never migrate back in.
  searchController->SetNaturalLanguageSearchEnabled(false);
  QSettings{}.remove(QStringLiteral("search/naturalLanguageSearchEnabled"));
  QSettings{}.remove(QStringLiteral("search/semanticEnabled"));
}

TEST_F(GlobalSearchDialogQmlTests, SemanticTypingShowsAwaitingSubmitAndLabelUsesOrdinaryPath) {
  auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
  ASSERT_NE(app, nullptr);

  QQuickStyle::setStyle(QStringLiteral("Material"));
  AppTheme::RegisterFonts();
  AppTheme::ApplyApplicationFont(*app);

  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto* searchController = backend.search();
  ASSERT_NE(searchController, nullptr);
  searchController->SetNaturalLanguageSearchEnabled(true);

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/"));
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &backend);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("dialogSourceUrl"),
                                           GlobalSearchDialogFileUrl());
  engine.loadData(QByteArray{kHarnessQml},
                  QUrl(QStringLiteral("file:///GlobalSearchDialogSemanticHarness.qml")));

  ASSERT_FALSE(engine.rootObjects().empty()) << "QML harness failed to load.";
  QObject* windowRoot = engine.rootObjects().front();
  ASSERT_NE(windowRoot, nullptr);

  QObject* dialog = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    dialog = qvariant_cast<QObject*>(windowRoot->property("dialog"));
    return dialog != nullptr;
  }, 10000));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openFromCollection"));
  ASSERT_TRUE(WaitUntil([&]() { return dialog->property("visible").toBool(); }, 5000));

  QObject* searchField = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    searchField = FindSearchField(windowRoot);
    return searchField != nullptr;
  }, 5000));

  // A natural-language query with the toggle on must not run the semantic net
  // on typing: the preview stays empty and signals an explicit submit is needed.
  searchField->setProperty("text", QStringLiteral("sunset over the mountains"));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "refreshPreview"));
  ProcessEvents(200);
  EXPECT_EQ(dialog->property("currentRoute").toString().toStdString(), "semantic");
  EXPECT_TRUE(dialog->property("results").toList().empty());
  EXPECT_FALSE(dialog->property("naturalLanguageStatusText").toString().isEmpty());

  // A label query still uses the ordinary path on typing even with the toggle on.
  searchField->setProperty("text", QStringLiteral("portrait"));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "refreshPreview"));
  ProcessEvents(200);
  EXPECT_EQ(dialog->property("currentRoute").toString().toStdString(), "label");
  EXPECT_FALSE(dialog->property("naturalLanguagePreviewActive").toBool());

  // Turning the toggle off restores ordinary routing for natural language.
  searchController->SetNaturalLanguageSearchEnabled(false);
  searchField->setProperty("text", QStringLiteral("sunset over the mountains"));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "refreshPreview"));
  ProcessEvents(200);
  EXPECT_EQ(dialog->property("currentRoute").toString().toStdString(), "traditional");

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
}

// Regression: when natural-language search is enabled and the app is restarted,
// the persisted NL state is restored into SearchController (read from QSettings in
// its ctor), but InteractionPolicyController's naturalLanguageSearchEnabled copy is
// only pushed imperatively on toggle — so on restart it lags and the field-filter
// checkboxes would wrongly enable. The dialog's onOpened re-syncs the policy
// controller to SearchController's state so the drawer shows the correct disabled
// state on the first open after restart.
TEST_F(GlobalSearchDialogQmlTests, NaturalLanguageSearchGate_SyncedOnDialogOpen) {
  auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
  ASSERT_NE(app, nullptr);

  QQuickStyle::setStyle(QStringLiteral("Material"));
  AppTheme::RegisterFonts();
  AppTheme::ApplyApplicationFont(*app);

  QCoreApplication::setOrganizationName(QStringLiteral("PuerhLabTest"));
  QCoreApplication::setApplicationName(QStringLiteral("GlobalSearchDialogQmlTest"));
  QSettings{}.remove(QStringLiteral("search/naturalLanguageSearchEnabled"));
  QSettings{}.remove(QStringLiteral("search/semanticEnabled"));

  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto* searchController = backend.search();
  ASSERT_NE(searchController, nullptr);
  ASSERT_NE(backend.interaction_policy(), nullptr);

  // Persist NL enabled (what a restart would restore). SearchController knows it
  // now; the policy controller does NOT — the gate is still open.
  searchController->SetNaturalLanguageSearchEnabled(true);
  ASSERT_TRUE(searchController->natural_language_search_enabled());
  EXPECT_TRUE(backend.interaction_policy()
                  ->property("canChangeSearchFieldFilters")
                  .toBool())
      << "Before the dialog syncs, the policy controller should still allow field filters.";

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/"));
  engine.rootContext()->setContextProperty(QStringLiteral("appModules"), &backend);
  engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
  engine.rootContext()->setContextProperty(QStringLiteral("dialogSourceUrl"),
                                           GlobalSearchDialogFileUrl());
  engine.loadData(QByteArray{kHarnessQml},
                  QUrl(QStringLiteral("file:///GlobalSearchDialogGateSyncHarness.qml")));

  ASSERT_FALSE(engine.rootObjects().empty()) << "QML harness failed to load.";
  QObject* windowRoot = engine.rootObjects().front();
  ASSERT_NE(windowRoot, nullptr);

  QObject* dialog = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    dialog = qvariant_cast<QObject*>(windowRoot->property("dialog"));
    return dialog != nullptr;
  }, 10000));
  // Opening the dialog must re-sync the policy controller's NL state from the
  // persisted SearchController value via onOpened.
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "openFromCollection"));
  ASSERT_TRUE(WaitUntil([&]() { return dialog->property("visible").toBool(); }, 5000));

  EXPECT_FALSE(backend.interaction_policy()
                   ->property("canChangeSearchFieldFilters")
                   .toBool())
      << "Opening the dialog must sync the NL gate so field filters are disabled.";

  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));

  // Restore default to avoid polluting other tests / future runs.
  searchController->SetNaturalLanguageSearchEnabled(false);
  QSettings{}.remove(QStringLiteral("search/naturalLanguageSearchEnabled"));
  QSettings{}.remove(QStringLiteral("search/semanticEnabled"));
}

}  // namespace alcedo::ui::test
