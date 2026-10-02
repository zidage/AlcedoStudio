//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Production QML of the library sort, group, and section view
// (album_sort_group_and_import_time_plan.md, Phase 3): the Album Inspector field actions and
// AlbumSectionView load from the source tree against a real packed project. The tests call
// the components' own action functions; they do not deliver synthetic pointer input.

#include <gtest/gtest.h>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "ui/album_backend_seeded_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

using AlbumSectionQmlTests   = ApplicationModuleHostTestFixture;

// Loads one production QML file from the source tree into a fixed-size window.
constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    width: 1100
    height: 900
    visible: true
    property alias loaded: harnessLoader.item
    property alias loaderActive: harnessLoader.active

    Loader {
        id: harnessLoader
        width: componentWidth
        height: 860
        source: componentSourceUrl
    }
}
)";

auto           QmlSourceUrl(const char* file) -> QUrl {
  const auto path =
      std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml" / file;
#ifdef _WIN32
  return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
#else
  return QUrl::fromLocalFile(QString::fromStdString(path.string()));
#endif
}

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs = 15000) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(10);
  }
  return predicate();
}

auto Utc(int year, unsigned month, unsigned day, int hour) -> std::time_t {
  using namespace std::chrono;
  return static_cast<std::time_t>(
      (sys_days{std::chrono::year{year} / month / day} + hours{hour}).time_since_epoch().count());
}

auto SixPhotoSpecs() -> std::vector<LibraryPhotoSpec> {
  return {
      {.model_      = "Canon R5",
       .date_time_  = "2026-06-07 09:00:00",
       .rating_     = 3,
       .added_time_ = Utc(2026, 9, 1, 10)},
      {.model_      = "Canon R5",
       .date_time_  = "2026-06-07 18:00:00",
       .rating_     = 5,
       .added_time_ = Utc(2026, 9, 1, 11)},
      {.model_      = "Nikon Z8",
       .date_time_  = "2026-06-06 10:00:00",
       .rating_     = 3,
       .added_time_ = Utc(2026, 9, 2, 10)},
      {.model_      = "Nikon Z8",
       .date_time_  = "2026-06-06 11:00:00",
       .rating_     = 1,
       .added_time_ = Utc(2026, 9, 2, 11)},
      {.model_      = "Sony A7",
       .date_time_  = "2026-06-05 12:00:00",
       .rating_     = 5,
       .added_time_ = Utc(2026, 8, 30, 10)},
      {.model_ = "Sony A7", .date_time_ = "", .rating_ = 0, .added_time_ = Utc(2026, 8, 30, 11)},
  };
}

/// The production QML engine setup of the album surfaces, with one component loaded.
class LoadedComponent {
 public:
  LoadedComponent(ApplicationModuleHost& backend, const char* file, int width) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    AppTheme::RegisterFonts();
    RegisterShortcutRegistryQmlType();
    engine_.addImportPath(QStringLiteral("qrc:/"));
    backend.AttachQmlEngine(&engine_);
    engine_.rootContext()->setContextProperty(QStringLiteral("appModules"), &backend);
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_.rootContext()->setContextProperty(QStringLiteral("componentSourceUrl"),
                                              QmlSourceUrl(file));
    engine_.rootContext()->setContextProperty(QStringLiteral("componentWidth"), width);
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& list) {
      for (const auto& warning : list) {
        warnings_.push_back(warning.toString().toStdString());
      }
    });
    engine_.loadData(QByteArray{kHarnessQml}, QUrl(QStringLiteral("file:///AlbumHarness.qml")));
  }

  auto window() -> QObject* {
    return engine_.rootObjects().empty() ? nullptr : engine_.rootObjects().front();
  }
  auto loaded() -> QObject* {
    auto* root = window();
    return root ? qvariant_cast<QObject*>(root->property("loaded")) : nullptr;
  }
  auto Find(const char* object_name) -> QObject* {
    auto* root = window();
    return root ? root->findChild<QObject*>(QString::fromLatin1(object_name)) : nullptr;
  }
  auto warnings() const -> const std::vector<std::string>& { return warnings_; }

 private:
  QQmlApplicationEngine    engine_;
  std::vector<std::string> warnings_;
};

/// Items named @p object_name in the visual tree under @p root. Delegate items of views have
/// no QObject parent, so QObject::findChildren does not see them.
void CollectNamedItems(QQuickItem* root, const QString& object_name,
                       std::vector<QQuickItem*>& out) {
  if (root == nullptr) {
    return;
  }
  if (root->objectName() == object_name) {
    out.push_back(root);
  }
  for (auto* child : root->childItems()) {
    CollectNamedItems(child, object_name, out);
  }
}

auto Joined(const std::vector<std::string>& lines) -> std::string {
  std::string out;
  for (const auto& line : lines) {
    out += line + "\n";
  }
  return out;
}

/// Warnings that name a QML file changed by this feature.
auto AlbumWarnings(const std::vector<std::string>& warnings) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& warning : warnings) {
    for (const auto* file :
         {"AlbumSectionView.qml", "InspectorFieldActions.qml", "AlbumInspectorPanel.qml",
          "StatsCard.qml", "DateFilterSection.qml", "StarRatingFilter.qml"}) {
      if (warning.find(file) != std::string::npos) {
        out.push_back(warning);
        break;
      }
    }
  }
  return out;
}

}  // namespace

TEST_F(AlbumSectionQmlTests, InspectorFieldActionsApplyOneSortAndOneGroupWithoutChangingFilters) {
  ApplicationModuleHost backend;
  const auto            packed = CreateSeededPackedProject(temp_dir_, {}, 0, SixPhotoSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  auto* library = backend.library();
  backend.stats()->ToggleStatsFilter(QStringLiteral("camera"), QStringLiteral("Canon R5"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  LoadedComponent inspector(backend, "AlbumInspectorPanel.qml", 420);
  ASSERT_TRUE(WaitUntil([&]() { return inspector.loaded() != nullptr; }))
      << Joined(inspector.warnings());
  auto* import_section = inspector.Find("importDateFilterSection");
  ASSERT_NE(import_section, nullptr);
  EXPECT_TRUE(import_section->property("expanded").toBool());
  EXPECT_FALSE(import_section->property("activityAvailable").toBool());
  QObject* import_style_nav = nullptr;
  for (auto* child : import_section->findChildren<QObject*>()) {
    if (child->objectName() == QStringLiteral("dateFilterStyleNav")) import_style_nav = child;
  }
  ASSERT_NE(import_style_nav, nullptr);
  EXPECT_FALSE(import_style_nav->property("visible").toBool());

  // Sort action of the rating header: one active arrow, the filter stays.
  QObject* descending = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    descending = inspector.Find("inspectorSortDescending_rating");
    return descending != nullptr;
  })) << Joined(inspector.warnings());
  ASSERT_TRUE(QMetaObject::invokeMethod(descending, "activate"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(library->SortFieldName(), QStringLiteral("rating"));
  EXPECT_TRUE(library->SortDescending());
  EXPECT_TRUE(descending->property("selected").toBool());
  EXPECT_FALSE(inspector.Find("inspectorSortAscending_rating")->property("selected").toBool());
  EXPECT_EQ(backend.stats()->StatsFilterCamera(), QStringLiteral("Canon R5"));
  EXPECT_TRUE(import_section->property("expanded").toBool());

  // Group actions: one selected at a time; grouping the sorted field keeps its sort.
  auto* group_camera = inspector.Find("inspectorGroupButton_camera");
  auto* group_rating = inspector.Find("inspectorGroupButton_rating");
  ASSERT_NE(group_camera, nullptr);
  ASSERT_NE(group_rating, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(group_camera, "activate"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_TRUE(group_camera->property("selected").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(group_rating, "activate"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_FALSE(group_camera->property("selected").toBool());
  EXPECT_TRUE(group_rating->property("selected").toBool());
  EXPECT_EQ(library->GroupFieldName(), QStringLiteral("rating"));
  EXPECT_EQ(library->SortFieldName(), QStringLiteral("rating"));
  EXPECT_EQ(backend.stats()->StatsFilterCamera(), QStringLiteral("Canon R5"));
  EXPECT_TRUE(import_section->property("expanded").toBool());

  // The same arrow again clears the explicit sort; the group stays.
  ASSERT_TRUE(QMetaObject::invokeMethod(descending, "activate"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_TRUE(library->SortFieldName().isEmpty());
  EXPECT_FALSE(descending->property("selected").toBool());
  EXPECT_EQ(library->GroupFieldName(), QStringLiteral("rating"));
  EXPECT_TRUE(AlbumWarnings(inspector.warnings()).empty())
      << Joined(AlbumWarnings(inspector.warnings()));
}

TEST_F(AlbumSectionQmlTests, SectionViewShowsGroupRowsAndReleasesOccurrencePinsOnTeardown) {
  ApplicationModuleHost backend;
  const auto            packed = CreateSeededPackedProject(temp_dir_, {}, 0, SixPhotoSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  auto* library = backend.library();
  library->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  auto& sections = library->section_model();
  ASSERT_EQ(sections.GroupCount(), 3);

  LoadedComponent view(backend, "AlbumSectionView.qml", 1000);
  QObject*        list = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    list = view.Find("albumSectionList");
    return list != nullptr && list->property("count").toInt() == sections.rowCount();
  })) << Joined(view.warnings());
  // Three cameras of two photos each: three headers and three photo rows.
  EXPECT_EQ(sections.rowCount(), 6);
  const auto cell_count = [&]() {
    std::vector<QQuickItem*> cells;
    auto*                    window = qobject_cast<QQuickWindow*>(view.window());
    CollectNamedItems(window ? window->contentItem() : nullptr,
                      QStringLiteral("albumSectionPhotoCell"), cells);
    return cells.size();
  };
  ASSERT_TRUE(WaitUntil([&]() { return cell_count() == 6; }))
      << "cells " << cell_count() << " list height " << list->property("height").toReal()
      << " content height " << list->property("contentHeight").toReal() << "\n"
      << Joined(view.warnings());

  // Every visible cell pinned its photo once for its group.
  for (const auto& image : packed->images_) {
    EXPECT_TRUE(WaitUntil([&]() {
      return library->thumbs().VisibleOccurrenceCount(image.file_id_) == 1u;
    })) << image.file_id_;
  }

  // Collapsing a group hides its photo row, not its header; counts stay.
  sections.SetGroupCollapsed(0, true);
  ASSERT_TRUE(WaitUntil([&]() { return list->property("count").toInt() == 5; }));
  EXPECT_EQ(sections.UniqueFileCount(), 6);
  sections.ExpandAll();
  ASSERT_TRUE(WaitUntil([&]() { return list->property("count").toInt() == 6; }));

  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));

  // Tearing the view down releases every occurrence pin.
  view.window()->setProperty("loaderActive", false);
  ProcessEvents(200);
  for (const auto& image : packed->images_) {
    EXPECT_EQ(library->thumbs().VisibleOccurrenceCount(image.file_id_), 0u) << image.file_id_;
    EXPECT_FALSE(library->thumbs().IsThumbnailPinned(image.file_id_)) << image.file_id_;
  }
}

TEST_F(AlbumSectionQmlTests, LargeSectionTraversalKeepsCellsAndLoadedRowsBounded) {
  ApplicationModuleHost        backend;
  std::vector<LibraryPhotoSpec> specs(400);
  for (size_t index = 0; index < specs.size(); ++index) {
    specs[index].model_ = index < 380 ? "Canon R5" : "Sony A7";
  }
  const auto packed = CreateSeededPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  auto* library = backend.library();
  // A search keeps pages at 120 rows, so the traversal crosses page boundaries.
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  library->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  auto& sections = library->section_model();
  ASSERT_EQ(sections.OccurrenceTotal(), 400);

  LoadedComponent view(backend, "AlbumSectionView.qml", 1000);
  QObject*        list = nullptr;
  ASSERT_TRUE(WaitUntil([&]() {
    list = view.Find("albumSectionList");
    return list != nullptr && list->property("count").toInt() == sections.rowCount();
  })) << Joined(view.warnings());
  auto*      window     = qobject_cast<QQuickWindow*>(view.window());
  const auto cell_count = [&]() {
    std::vector<QQuickItem*> cells;
    CollectNamedItems(window ? window->contentItem() : nullptr,
                      QStringLiteral("albumSectionPhotoCell"), cells);
    return cells.size();
  };

  size_t     max_cells      = 0;
  size_t     max_pages      = 0;
  int        max_rows       = 0;
  const auto content_height = list->property("contentHeight").toReal();
  for (const double fraction : {0.0, 0.5, 0.95, 0.2, 0.8}) {
    list->setProperty("contentY", fraction * (content_height - list->property("height").toReal()));
    ProcessEvents(300);
    max_cells = std::max(max_cells, cell_count());
    max_pages = std::max(max_pages, sections.LoadedPageCount());
    max_rows  = std::max(max_rows, library->model().count());
  }
  std::cout << "[album sections] max cells " << max_cells << ", max loaded pages " << max_pages
            << ", max photo rows " << max_rows << "\n";
  RecordProperty("max_visible_cells", static_cast<int>(max_cells));
  RecordProperty("max_loaded_pages", static_cast<int>(max_pages));
  // The viewport plus one viewport of cache buffer above and below, in photo rows of the
  // view's geometry, plus the partly covered rows at both edges, times the column count:
  // never one cell per photo.
  const auto row_height = view.loaded()->property("photoRowHeight").toReal();
  const auto columns    = view.loaded()->property("columnCount").toInt();
  const auto cell_bound = static_cast<size_t>(
      columns * (std::ceil(3.0 * list->property("height").toReal() / row_height) + 4));
  RecordProperty("cell_bound", static_cast<int>(cell_bound));
  EXPECT_LE(max_cells, cell_bound);
  EXPECT_LT(max_cells, 400u);
  EXPECT_LE(max_pages, 3u);
  EXPECT_LE(max_rows, 360);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

}  // namespace alcedo::ui::test
