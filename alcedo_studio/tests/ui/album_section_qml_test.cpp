//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Production QML of the library sort, group, and section view
// (album_sort_group_and_import_time_plan.md, Phase 3): the Album Inspector field actions and
// AlbumSectionView load from the source tree against a real packed project. The tests call
// the components' own action functions; they do not deliver synthetic pointer input.

#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTest>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/pipeline_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"
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
          "StatsCard.qml", "DateFilterSection.qml", "StarRatingFilter.qml", "ThumbnailGridView.qml",
          "ThumbnailStarRating.qml", "ThumbnailEditMark.qml"}) {
      if (warning.find(file) != std::string::npos) {
        out.push_back(warning);
        break;
      }
    }
  }
  return out;
}

/// Persist one exposure commit on the active Version of @p file_id: the photo is edited.
void PersistExposureCommit(PipelineMgmtService& pipelines, sl_element_id_t file_id) {
  auto           base    = pipelines.LoadHistorySnapshot(file_id);
  const auto     current = pipelines.AcquireCommittedSnapshot(file_id);
  nlohmann::json before =
      current->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure())->ToJson();
  nlohmann::json after = before;
  after["exposure_ev"] = before.at("exposure_ev").get<float>() + 1.0f;

  PipelineEditBatch  batch;
  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
  change.target.field_key              = "exposure";
  change.before_value                  = std::move(before);
  change.after_value                   = std::move(after);
  change.before_enabled                = true;
  change.after_enabled                 = true;
  batch.operation_kind                 = PipelineEditOperationKind::SetParameter;
  batch.presentation_key               = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));

  CommitGraph graph  = *base.graph_;
  auto        commit = EditCommit::MakePipelineEdit(
      graph.GetRootId(), graph.GetActiveVersionRef().head_commit_hash, batch);
  const auto  head   = commit.GetCommitHash();
  ASSERT_TRUE(graph.InsertCommit(std::move(commit)));
  graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
  ASSERT_NE(pipelines.PersistHistory(base, graph), nullptr);
}

/// SixPhotoSpecs with the second photo (rating 5) edited.
auto PackedProjectWithOneEditedPhoto(const std::filesystem::path& dir)
    -> std::optional<PopulatedProject> {
  return CreatePopulatedPackedProject(
      dir, {}, 0, SixPhotoSpecs(),
      [](PipelineMgmtService& pipelines, const std::vector<PopulatedProject::ImageKey>& keys) {
        PersistExposureCommit(pipelines, keys[1].file_id_);
      });
}

void CollectItems(QQuickItem* root, const std::function<bool(QQuickItem*)>& match,
                  std::vector<QQuickItem*>& out) {
  if (root == nullptr) {
    return;
  }
  if (match(root)) {
    out.push_back(root);
  }
  for (auto* child : root->childItems()) {
    CollectItems(child, match, out);
  }
}

auto FirstItem(QQuickItem* root, const QString& object_name) -> QQuickItem* {
  std::vector<QQuickItem*> items;
  CollectItems(root, [&](QQuickItem* item) { return item->objectName() == object_name; }, items);
  return items.empty() ? nullptr : items.front();
}

/// Visible items named @p object_name under @p root (QQuickItem::isVisible includes the
/// parents' visibility).
auto VisibleCount(QQuickItem* root, const QString& object_name) -> int {
  std::vector<QQuickItem*> items;
  CollectItems(
      root,
      [&](QQuickItem* item) { return item->objectName() == object_name && item->isVisible(); },
      items);
  return static_cast<int>(items.size());
}

/// Visible stars of a ThumbnailStarRating that show @p glyph.
auto StarCount(QQuickItem* stars, const QString& glyph) -> int {
  std::vector<QQuickItem*> items;
  CollectItems(
      stars,
      [&](QQuickItem* item) {
        return item->objectName().startsWith(QStringLiteral("thumbnailStar_")) &&
               item->isVisible() && item->property("text").toString() == glyph;
      },
      items);
  return static_cast<int>(items.size());
}

/// The flat-grid card of @p element_id, or nullptr when it has no delegate.
auto GridCard(QQuickWindow* window, sl_element_id_t element_id) -> QQuickItem* {
  std::vector<QQuickItem*> cards;
  CollectItems(
      window ? window->contentItem() : nullptr,
      [&](QQuickItem* item) {
        return item->objectName().startsWith(QStringLiteral("thumbnailGridView_")) &&
               (item->objectName().contains(QStringLiteral("Card")) ||
                item->objectName().contains(QStringLiteral("card_"))) &&
               item->property("elementId").toUInt() == element_id;
      },
      cards);
  return cards.empty() ? nullptr : cards.front();
}

auto SceneCenter(QQuickItem* item) -> QPoint {
  return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0)).toPoint();
}

auto RatingOf(ApplicationModuleHost& backend, sl_element_id_t element_id) -> int {
  const auto row = backend.library()->model().rowByElementId(element_id);
  return row < 0 ? -1 : backend.library()->model().getItemAt(row).value("rating").toInt();
}

/// What LibraryWorkspace does with a card star click: the single-photo rating path.
void ApplyRatingRequest(ApplicationModuleHost& backend, const QList<QVariant>& request) {
  const auto item = request.at(0).toMap();
  backend.library_mutations()->RateTargetsAsUser(
      QVariantList{
          QVariantMap{{"elementId", item.value("elementId")}, {"imageId", item.value("imageId")}}},
      request.at(1).toInt());
}

}  // namespace

TEST_F(AlbumSectionQmlTests, InspectorFieldActionsApplyOneSortAndOneGroupWithoutChangingFilters) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, SixPhotoSpecs());
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
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, SixPhotoSpecs());
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

// The grouped view's scroll anchor (the occurrence at the top of the view and the offset into
// its row) restores the same position in a new view instance, as after an editor round trip,
// and keeps the top photo through a new column count.
TEST_F(AlbumSectionQmlTests, ScrollAnchorRestoresTheTopPhotoAcrossTeardownAndColumnChanges) {
  ApplicationModuleHost         backend;
  std::vector<LibraryPhotoSpec> specs(400);
  for (size_t index = 0; index < specs.size(); ++index) {
    specs[index].model_ = index < 380 ? "Canon R5" : "Sony A7";
  }
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  auto* library = backend.library();
  library->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  auto&           sections = library->section_model();

  LoadedComponent view(backend, "AlbumSectionView.qml", 1000);
  QObject*        list    = nullptr;
  const auto      list_up = [&]() {
    list = view.Find("albumSectionList");
    return list != nullptr && list->property("count").toInt() == sections.rowCount() &&
           list->property("height").toReal() > 0;
  };
  ASSERT_TRUE(WaitUntil(list_up)) << Joined(view.warnings());
  const auto row_height = view.loaded()->property("photoRowHeight").toReal();
  list->setProperty("contentY", 20.0 * row_height + 17.0);
  // Let the delegates of the new position finish before the view is torn down.
  ProcessEvents(500);
  QVariant anchor;
  ASSERT_TRUE(
      QMetaObject::invokeMethod(view.loaded(), "scrollAnchor", Q_RETURN_ARG(QVariant, anchor)));
  const auto anchor_map = anchor.toMap();
  ASSERT_FALSE(anchor_map.isEmpty());
  const auto occurrence = anchor_map.value(QStringLiteral("occurrence")).toLongLong();
  EXPECT_GT(occurrence, 0);
  const auto saved_y = list->property("contentY").toReal();
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));

  // A new view instance starts at the top; the anchor puts it back at the same position.
  // (Tearing down a scrolled 400-photo section view in this harness reports "Object or context
  // destroyed during incubation" also with the view before this change, so warnings are
  // checked before the teardown.)
  view.window()->setProperty("loaderActive", false);
  ProcessEvents(300);
  ProcessEvents(100);
  view.window()->setProperty("loaderActive", true);
  ASSERT_TRUE(WaitUntil(list_up)) << Joined(view.warnings());
  ASSERT_TRUE(
      QMetaObject::invokeMethod(view.loaded(), "restoreScrollAnchor", Q_ARG(QVariant, anchor)));
  ASSERT_TRUE(WaitUntil([&]() {
    return std::abs(list->property("contentY").toReal() - saved_y) < 0.5;
  })) << "contentY "
      << list->property("contentY").toReal() << " expected " << saved_y;

  // A new column count keeps the anchored photo in the top row of the view.
  const auto columns_before = view.loaded()->property("columnCount").toInt();
  view.loaded()->setProperty("zoomLevel", 1);
  ASSERT_TRUE(WaitUntil([&]() {
    return view.loaded()->property("columnCount").toInt() != columns_before &&
           sections.ColumnCount() == view.loaded()->property("columnCount").toInt();
  }));
  ProcessEvents(100);
  const auto top_row  = sections.RowAtOffset(list->property("contentY").toReal(),
                                             view.loaded()->property("headerHeight").toReal(),
                                             view.loaded()->property("photoRowHeight").toReal());
  const auto top_info = sections.RowInfo(top_row);
  const auto first    = top_info.value(QStringLiteral("firstOccurrence")).toLongLong();
  const auto count    = top_info.value(QStringLiteral("occurrenceCount")).toLongLong();
  EXPECT_LE(first, occurrence);
  EXPECT_LT(occurrence, first + std::max<qint64>(count, 1));
}

TEST_F(AlbumSectionQmlTests, LargeSectionTraversalKeepsCellsAndLoadedRowsBounded) {
  ApplicationModuleHost        backend;
  std::vector<LibraryPhotoSpec> specs(400);
  for (size_t index = 0; index < specs.size(); ++index) {
    specs[index].model_ = index < 380 ? "Canon R5" : "Sony A7";
  }
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
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

// Issue #589, Library at 4 columns: the edit mark shows only on the edited photo; the stars
// show the rating; a click on star 4 asks for rating 4 of that photo only, the card then
// shows four filled stars, and a second click on star 4 clears the rating.
TEST_F(AlbumSectionQmlTests, FlatGridAtFourColumnsShowsTheEditMarkAndRatesFromTheStars) {
  ApplicationModuleHost backend;
  const auto            packed = PackedProjectWithOneEditedPhoto(temp_dir_);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  view.loaded()->setProperty("zoomLevel", 2);
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  ASSERT_NE(window, nullptr);
  ASSERT_TRUE(WaitUntil([&]() {
    for (const auto& key : keys) {
      if (GridCard(window, key.file_id_) == nullptr) return false;
    }
    return view.loaded()->property("columns").toInt() == 4;
  })) << Joined(view.warnings());
  // The zoom change relayouts and animates the cards; read positions after it settles.
  ProcessEvents(1000);

  // Edit mark: only the edited photo.
  for (size_t index = 0; index < keys.size(); ++index) {
    auto* card = GridCard(window, keys[index].file_id_);
    ASSERT_NE(card, nullptr) << index;
    EXPECT_EQ(VisibleCount(card, QStringLiteral("thumbnailEditMark")), index == 1 ? 1 : 0) << index;
    EXPECT_EQ(card->property("isEdited").toBool(), index == 1) << index;
  }
  // Stars: the rating as filled stars of five; an unrated photo shows none at rest.
  auto* rated_stars = FirstItem(GridCard(window, keys[0].file_id_), "thumbnailGridStars");
  ASSERT_NE(rated_stars, nullptr);
  EXPECT_EQ(StarCount(rated_stars, QStringLiteral("★")), 3);
  EXPECT_EQ(StarCount(rated_stars, QStringLiteral("☆")), 2);
  EXPECT_TRUE(rated_stars->property("starsShown").toBool());
  auto* unrated_stars = FirstItem(GridCard(window, keys[5].file_id_), "thumbnailGridStars");
  ASSERT_NE(unrated_stars, nullptr);
  EXPECT_FALSE(unrated_stars->property("starsShown").toBool());

  QSignalSpy requests(view.loaded(), SIGNAL(ratingRequested(QVariant, int)));
  ASSERT_TRUE(requests.isValid());
  QSignalSpy selections(view.loaded(), SIGNAL(replaceSelection(QVariant)));
  ASSERT_TRUE(selections.isValid());
  const auto target = keys[3].file_id_;  // rating 1
  // A rating write refreshes the library, which can create new card delegates: look the
  // stars up again for every read.
  const auto stars  = [&]() -> QQuickItem* {
    auto* card = GridCard(window, target);
    return card ? FirstItem(card, "thumbnailGridStars") : nullptr;
  };
  const auto star4_center = [&]() -> QPoint {
    auto* star = stars() ? FirstItem(stars(), "thumbnailStar_4") : nullptr;
    return star ? SceneCenter(star) : QPoint(-1, -1);
  };
  const auto preview_rating = [&]() {
    return stars() ? stars()->property("previewRating").toInt() : -1;
  };
  ASSERT_NE(stars(), nullptr);
  ASSERT_NE(GridCard(window, keys[0].file_id_), nullptr);
  const auto away    = SceneCenter(GridCard(window, keys[0].file_id_)) + QPoint(0, -40);

  auto*      overlay = view.Find("thumbnailGridView_inputOverlay");
  ASSERT_NE(overlay, nullptr);
  ASSERT_NE(GridCard(window, target), nullptr);
  QTest::mouseMove(window, SceneCenter(GridCard(window, target)));
  QTest::mouseMove(window, star4_center());
  ASSERT_TRUE(WaitUntil([&]() { return preview_rating() == 4; }, 3000))
      << "hovered index " << overlay->property("hoveredIndex").toInt() << " hovered star "
      << overlay->property("hoveredStar").toInt();
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, star4_center());
  ASSERT_TRUE(WaitUntil([&]() { return requests.count() == 1; }, 3000));
  EXPECT_EQ(requests.at(0).at(0).toMap().value("elementId").toUInt(), target);
  EXPECT_EQ(requests.at(0).at(1).toInt(), 4);
  ApplyRatingRequest(backend, requests.at(0));
  EXPECT_EQ(RatingOf(backend, target), 4);
  QTest::mouseMove(window, away);
  ASSERT_TRUE(WaitUntil(
      [&]() { return preview_rating() == 0 && StarCount(stars(), QStringLiteral("★")) == 4; },
      3000))
      << "filled " << StarCount(stars(), QStringLiteral("★"));
  EXPECT_EQ(StarCount(stars(), QStringLiteral("☆")), 1);
  // The click rated one photo; it did not change the selection.
  EXPECT_EQ(selections.count(), 0);

  // Wait past the double-click interval, then click star 4 again: the rating clears.
  QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
  QTest::mouseMove(window, SceneCenter(GridCard(window, target)));
  QTest::mouseMove(window, star4_center());
  ASSERT_TRUE(WaitUntil([&]() { return preview_rating() == 4; }, 3000));
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, star4_center());
  ASSERT_TRUE(WaitUntil([&]() { return requests.count() == 2; }, 3000));
  EXPECT_EQ(requests.at(1).at(1).toInt(), 0);
  ApplyRatingRequest(backend, requests.at(1));
  EXPECT_EQ(RatingOf(backend, target), 0);
  QTest::mouseMove(window, away);
  ASSERT_TRUE(WaitUntil(
      [&]() { return stars() != nullptr && !stars()->property("starsShown").toBool(); }, 3000));
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// Issue #589, Library at 8 columns: the stars are display only. A click on them selects the
// card and does not ask for a rating; an unrated photo shows no stars, also on hover.
TEST_F(AlbumSectionQmlTests, FlatGridAtEightColumnsStarsSelectTheCardAndDoNotRate) {
  ApplicationModuleHost backend;
  const auto            packed = PackedProjectWithOneEditedPhoto(temp_dir_);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  view.loaded()->setProperty("zoomLevel", 5);
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  ASSERT_NE(window, nullptr);
  ASSERT_TRUE(WaitUntil([&]() {
    for (const auto& key : keys) {
      if (GridCard(window, key.file_id_) == nullptr) return false;
    }
    return view.loaded()->property("columns").toInt() == 8;
  })) << Joined(view.warnings());
  ProcessEvents(1000);
  // One caption line: the file name, the edit mark of the edited photo, the filled stars.
  EXPECT_EQ(VisibleCount(GridCard(window, keys[1].file_id_), "thumbnailEditMark"), 1);
  EXPECT_EQ(VisibleCount(GridCard(window, keys[0].file_id_), "thumbnailEditMark"), 0);
  EXPECT_EQ(VisibleCount(GridCard(window, keys[1].file_id_), "thumbnailGridStars"), 0);

  QSignalSpy requests(view.loaded(), SIGNAL(ratingRequested(QVariant, int)));
  ASSERT_TRUE(requests.isValid());
  QSignalSpy selections(view.loaded(), SIGNAL(replaceSelection(QVariant)));
  ASSERT_TRUE(selections.isValid());
  const auto target  = keys[1].file_id_;  // rating 5
  auto*      compact = FirstItem(GridCard(window, target), "thumbnailGridCompactStars");
  ASSERT_NE(compact, nullptr);
  ASSERT_TRUE(compact->isVisible());
  EXPECT_EQ(StarCount(compact, QStringLiteral("★")), 5);
  EXPECT_EQ(StarCount(compact, QStringLiteral("☆")), 0);
  auto* star3 = FirstItem(compact, "thumbnailStar_3");
  ASSERT_NE(star3, nullptr);
  QTest::mouseMove(window, SceneCenter(star3));
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, SceneCenter(star3));
  // LibraryWorkspace applies replaceSelection to the Library selection.
  ASSERT_TRUE(WaitUntil([&]() { return selections.count() == 1; })) << "the card was not selected";
  const auto selected = selections.at(0).at(0).toList();
  ASSERT_EQ(selected.size(), 1);
  EXPECT_EQ(selected.front().toMap().value("elementId").toUInt(), target);
  ProcessEvents(100);
  EXPECT_EQ(requests.count(), 0);
  EXPECT_EQ(RatingOf(backend, target), 5);
  EXPECT_EQ(compact->property("previewRating").toInt(), 0);

  // Hover over an unrated card: still no stars.
  auto* unrated_card = GridCard(window, keys[5].file_id_);
  ASSERT_NE(unrated_card, nullptr);
  QTest::mouseMove(window, SceneCenter(unrated_card));
  ASSERT_TRUE(WaitUntil([&]() { return unrated_card->property("isHovered").toBool(); }));
  EXPECT_EQ(VisibleCount(unrated_card, "thumbnailGridCompactStars"), 0);
  EXPECT_EQ(VisibleCount(unrated_card, "thumbnailGridStars"), 0);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// Issue #589, grouped view at 4 columns: the same edit mark and star control in the photo
// cells; a star click asks for that rating of that photo only.
TEST_F(AlbumSectionQmlTests, SectionCellsAtFourColumnsShowTheEditMarkAndRateFromTheStars) {
  ApplicationModuleHost backend;
  const auto            packed = PackedProjectWithOneEditedPhoto(temp_dir_);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  auto* library = backend.library();
  library->SetInspectorGrouping(QStringLiteral("camera"), true);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "AlbumSectionView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  view.loaded()->setProperty("zoomLevel", 2);
  auto*      window   = qobject_cast<QQuickWindow*>(view.window());
  const auto cell_for = [&](sl_element_id_t id) -> QQuickItem* {
    std::vector<QQuickItem*> cells;
    CollectItems(
        window ? window->contentItem() : nullptr,
        [&](QQuickItem* item) {
          return item->objectName() == QStringLiteral("albumSectionPhotoCell") &&
                 item->property("fileId").toUInt() == id;
        },
        cells);
    return cells.empty() ? nullptr : cells.front();
  };
  ASSERT_TRUE(WaitUntil([&]() {
    for (const auto& key : keys) {
      auto* cell = cell_for(key.file_id_);
      if (cell == nullptr || cell->property("photo").isNull()) return false;
    }
    return view.loaded()->property("visualColumns").toInt() == 4;
  })) << Joined(view.warnings());
  // The new column count rebuilds the rows; read the cells after it settles.
  ProcessEvents(1000);

  for (size_t index = 0; index < keys.size(); ++index) {
    EXPECT_EQ(VisibleCount(cell_for(keys[index].file_id_), "thumbnailEditMark"), index == 1 ? 1 : 0)
        << index;
  }
  auto* rated = FirstItem(cell_for(keys[0].file_id_), "albumSectionStars");
  ASSERT_NE(rated, nullptr);
  EXPECT_EQ(StarCount(rated, QStringLiteral("★")), 3);

  QSignalSpy requests(view.loaded(), SIGNAL(ratingRequested(QVariant, int)));
  ASSERT_TRUE(requests.isValid());
  QSignalSpy selections(view.loaded(), SIGNAL(replaceSelection(QVariant)));
  ASSERT_TRUE(selections.isValid());
  const auto target = keys[3].file_id_;  // rating 1
  auto*      stars  = FirstItem(cell_for(target), "albumSectionStars");
  ASSERT_NE(stars, nullptr);
  auto* star4 = FirstItem(stars, "thumbnailStar_4");
  ASSERT_NE(star4, nullptr);
  QTest::mouseMove(window, SceneCenter(cell_for(target)));
  QTest::mouseMove(window, SceneCenter(star4));
  ASSERT_TRUE(WaitUntil([&]() { return stars->property("previewRating").toInt() == 4; }, 3000))
      << "card hovered " << stars->property("cardHovered").toBool() << " hovered star "
      << stars->property("hoveredStar").toInt();
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, SceneCenter(star4));
  ASSERT_TRUE(WaitUntil([&]() { return requests.count() == 1; }, 3000));
  EXPECT_EQ(requests.at(0).at(0).toMap().value("elementId").toUInt(), target);
  EXPECT_EQ(requests.at(0).at(1).toInt(), 4);
  ApplyRatingRequest(backend, requests.at(0));
  QTest::mouseMove(window, QPoint(2, 2));
  // The rating write can refresh the library and rebuild the cells: look the stars up again.
  ASSERT_TRUE(WaitUntil(
      [&]() {
        auto* cell = cell_for(target);
        return cell != nullptr &&
               StarCount(FirstItem(cell, "albumSectionStars"), QStringLiteral("★")) == 4;
      },
      3000));
  // The star click rated one photo; it did not change the selection.
  EXPECT_EQ(selections.count(), 0);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

}  // namespace alcedo::ui::test
