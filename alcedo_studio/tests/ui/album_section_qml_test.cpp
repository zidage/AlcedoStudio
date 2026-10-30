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
#include <tuple>
#include <utility>
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
  // The mark is a small chip on the top-right corner of the photo.
  {
    auto* card = GridCard(window, keys[1].file_id_);
    auto* mark = FirstItem(card, "thumbnailEditMark");
    ASSERT_NE(mark, nullptr);
    const auto card_rect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    const auto mark_rect = mark->mapRectToScene(QRectF(0, 0, mark->width(), mark->height()));
    EXPECT_LT(mark->width(), 24.0);
    EXPECT_GT(mark_rect.center().x(), card_rect.center().x());
    EXPECT_LT(mark_rect.center().y(), card_rect.center().y());
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
  // A rating write keeps the photo rows: no model reset, the same card delegate.
  QSignalSpy resets(&backend.library()->model(), &QAbstractItemModel::modelReset);
  auto*      target_card = GridCard(window, target);
  ASSERT_NE(target_card, nullptr);
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
  // The statistics read can finish after the rows show the rating; wait for it.
  ProcessEvents(500);
  EXPECT_EQ(resets.count(), 0);
  EXPECT_EQ(GridCard(window, target), target_card);

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
  // Look the stars up again: the rating changes their row.
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

// Issue #609: the photo at the top of the grouped view, read from the cells on the screen, and its
// position in the viewport. A cell of a pooled row is not shown.
namespace {

struct ShownPhoto {
  uint  file_id_ = 0;
  qreal y_       = 0;
};

auto ShownCells(QQuickWindow* window, QQuickItem* list) -> std::vector<QQuickItem*> {
  std::vector<QQuickItem*> cells;
  CollectNamedItems(window ? window->contentItem() : nullptr,
                    QStringLiteral("albumSectionPhotoCell"), cells);
  std::erase_if(cells, [&](QQuickItem* cell) {
    auto* row = cell->parentItem() ? cell->parentItem()->parentItem() : nullptr;
    if (!cell->isVisible() || row == nullptr || !row->property("inUse").toBool() ||
        cell->property("fileId").toUInt() == 0) {
      return true;
    }
    const auto position = cell->mapToItem(list, QPointF(0, 0));
    return position.y() + cell->height() <= 0 || position.y() >= list->height();
  });
  return cells;
}

auto FirstShownPhoto(QQuickWindow* window, QQuickItem* list) -> ShownPhoto {
  ShownPhoto best;
  qreal      best_x = 0;
  for (auto* cell : ShownCells(window, list)) {
    const auto position = cell->mapToItem(list, QPointF(0, 0));
    if (best.file_id_ == 0 || position.y() < best.y_ - 0.5 ||
        (std::abs(position.y() - best.y_) <= 0.5 && position.x() < best_x)) {
      best   = ShownPhoto{.file_id_ = cell->property("fileId").toUInt(), .y_ = position.y()};
      best_x = position.x();
    }
  }
  return best;
}

/// The cells and the reason each one is not shown, for a failure message.
auto DescribeCells(QQuickWindow* window, QQuickItem* list) -> std::string {
  std::vector<QQuickItem*> cells;
  CollectNamedItems(window ? window->contentItem() : nullptr,
                    QStringLiteral("albumSectionPhotoCell"), cells);
  std::string out = "cells " + std::to_string(cells.size()) + "\n";
  for (auto* cell : cells) {
    auto*      row      = cell->parentItem() ? cell->parentItem()->parentItem() : nullptr;
    const auto position = cell->mapToItem(list, QPointF(0, 0));
    out +=
        "visible " + std::to_string(cell->isVisible()) + " row " + std::to_string(row != nullptr) +
        " inUse " + std::to_string(row != nullptr && row->property("inUse").toBool()) + " file " +
        std::to_string(cell->property("fileId").toUInt()) + " y " + std::to_string(position.y()) +
        " rowIndex " + std::to_string(row ? row->property("index").toInt() : -1) + " rowFirst " +
        std::to_string(row ? row->property("firstOccurrence").toLongLong() : -1) + " rowIds " +
        std::to_string(row ? row->property("fileIds").toList().size() : -1) + " first " +
        std::to_string(row && !row->property("fileIds").toList().isEmpty()
                           ? row->property("fileIds").toList().front().toUInt()
                           : 0) +
        "\n";
  }
  return out + "list height " + std::to_string(list->height());
}

/// Viewport height of the shown cell of @p file_id; empty when no shown cell has it.
auto ShownPhotoY(QQuickWindow* window, QQuickItem* list, uint file_id) -> std::optional<qreal> {
  for (auto* cell : ShownCells(window, list)) {
    if (cell->property("fileId").toUInt() == file_id) {
      return cell->mapToItem(list, QPointF(0, 0)).y();
    }
  }
  return std::nullopt;
}

}  // namespace

// A delete of two rows of photos above the shown rows and an import of three photos into a new
// group before them (an occurrence range that no loaded page holds) change the rows in place:
// the first shown photo stays the same, at the same height in the viewport. A delete of one
// photo above moves the photos by one cell; the first shown photo keeps its height.
TEST_F(AlbumSectionQmlTests, InPlaceDeleteAndImportKeepTheFirstShownPhoto) {
  ApplicationModuleHost         backend;
  std::vector<LibraryPhotoSpec> specs(400);
  for (auto& spec : specs) {
    spec.added_time_ = Utc(2026, 9, 1, 10);
  }
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  auto* library = backend.library();
  // A search keeps pages at 120 rows.
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  library->SetInspectorGrouping(QStringLiteral("import"), true);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  auto& sections = library->section_model();
  ASSERT_EQ(sections.GroupCount(), 1);

  LoadedComponent view(backend, "AlbumSectionView.qml", 1000);
  QQuickItem*     list    = nullptr;
  const auto      list_up = [&]() {
    list = qobject_cast<QQuickItem*>(view.Find("albumSectionList"));
    return list != nullptr && list->property("count").toInt() == sections.rowCount() &&
           list->height() > 0;
  };
  ASSERT_TRUE(WaitUntil(list_up)) << Joined(view.warnings());
  auto*      window  = qobject_cast<QQuickWindow*>(view.window());
  const auto columns = view.loaded()->property("columnCount").toInt();
  ASSERT_GT(columns, 1);
  // Row 30 at the top of the view, 23 px scrolled into it (ListView.Beginning is 0).
  ASSERT_TRUE(
      QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 30), Q_ARG(int, 0)));
  list->setProperty("contentY", list->property("contentY").toReal() + 23.0);
  ASSERT_TRUE(WaitUntil([&]() { return FirstShownPhoto(window, list).file_id_ != 0; }))
      << DescribeCells(window, list) << " pages " <<
      [&]() {
        std::string text;
        for (const auto start : sections.LoadedPageStarts()) text += std::to_string(start) + ",";
        return text;
      }()
      << " updating " << library->QueryUpdating() << " total " << sections.OccurrenceTotal()
      << " rows " << sections.rowCount() << " contentY " << list->property("contentY").toReal();
  ProcessEvents(500);
  const auto before = FirstShownPhoto(window, list);
  ASSERT_NE(before.file_id_, 0u);
  // The photos of the first occurrences are far above the shown rows.
  const auto& keys        = packed->images_;
  const auto  first_count = static_cast<size_t>(2 * columns + 1);
  ASSERT_TRUE(std::none_of(keys.begin(), keys.begin() + first_count,
                           [&](const auto& key) { return key.file_id_ == before.file_id_; }));

  QSignalSpy resets(&sections, &QAbstractItemModel::modelReset);
  std::vector<std::pair<sl_element_id_t, image_id_t>> targets;
  for (size_t index = 0; index < static_cast<size_t>(2 * columns); ++index) {
    targets.emplace_back(keys[index].file_id_, keys[index].image_id_);
  }
  ASSERT_TRUE(DeleteAndWait(backend, targets));
  ProcessEvents(600);  // the cells finish their move
  const auto after_delete = FirstShownPhoto(window, list);
  EXPECT_EQ(after_delete.file_id_, before.file_id_);
  EXPECT_NEAR(after_delete.y_, before.y_, 1.0);
  EXPECT_EQ(sections.OccurrenceTotal(), 400 - 2 * columns);

  const auto photos = WritePngPhotos(temp_dir_ / "import_source", "album-delete-import-", 3);
  ASSERT_EQ(photos.size(), 3u);
  ASSERT_TRUE(ImportAndWait(backend, photos));
  ASSERT_TRUE(WaitUntil([&]() { return sections.GroupCount() == 2; }));
  ProcessEvents(600);
  const auto after_import = FirstShownPhoto(window, list);
  EXPECT_EQ(after_import.file_id_, before.file_id_);
  EXPECT_NEAR(after_import.y_, before.y_, 1.0);

  ASSERT_TRUE(
      DeleteAndWait(backend, {{keys[first_count - 1].file_id_, keys[first_count - 1].image_id_}}));
  ProcessEvents(600);
  const auto anchor_y = ShownPhotoY(window, list, before.file_id_);
  ASSERT_TRUE(anchor_y.has_value());
  EXPECT_NEAR(*anchor_y, before.y_, 1.0);
  EXPECT_EQ(list->property("count").toInt(), sections.rowCount());
  EXPECT_EQ(resets.count(), 0);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// The flat grid keeps the first shown card at its height through a delete of photos before it.
TEST_F(AlbumSectionQmlTests, FlatGridDeleteBeforeTheShownCardsKeepsTheFirstShownCard) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 300);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  auto* grid   = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_grid"));
  ASSERT_NE(grid, nullptr);
  ASSERT_TRUE(WaitUntil([&]() { return GridCard(window, keys[0].file_id_) != nullptr; }));
  ProcessEvents(500);
  const auto cell_height = grid->property("cellHeight").toReal();
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "restoreContentY",
                                        Q_ARG(QVariant, 12.0 * cell_height + 9.0)));
  ProcessEvents(800);
  // Shown cards: element id and viewport position.
  const auto shown_cards = [&]() {
    std::vector<std::tuple<uint, qreal, qreal>> out;
    for (const auto& key : keys) {
      auto* card = GridCard(window, key.file_id_);
      if (card == nullptr || !card->isVisible()) {
        continue;
      }
      const auto position = card->mapToItem(grid, QPointF(0, 0));
      if (position.y() + card->height() > 0 && position.y() < grid->height()) {
        out.emplace_back(key.file_id_, position.y(), position.x());
      }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
      return std::abs(std::get<1>(a) - std::get<1>(b)) > 0.5 ? std::get<1>(a) < std::get<1>(b)
                                                             : std::get<2>(a) < std::get<2>(b);
    });
    return out;
  };
  const auto before = shown_cards();
  ASSERT_FALSE(before.empty());
  const auto anchor = std::get<0>(before.front());
  ASSERT_NE(anchor, keys[0].file_id_);

  QSignalSpy resets(&backend.library()->model(), &QAbstractItemModel::modelReset);
  ASSERT_TRUE(DeleteAndWait(backend, {{keys[0].file_id_, keys[0].image_id_},
                                      {keys[1].file_id_, keys[1].image_id_},
                                      {keys[2].file_id_, keys[2].image_id_}}));
  ProcessEvents(800);
  const auto after = shown_cards();
  const auto it    = std::find_if(after.begin(), after.end(),
                                  [&](const auto& card) { return std::get<0>(card) == anchor; });
  ASSERT_NE(it, after.end());
  EXPECT_NEAR(std::get<1>(*it), std::get<1>(before.front()), 1.0);
  EXPECT_EQ(resets.count(), 0);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// The flat grid has a scroll bar for all photos of the result, also the photos that are not
// loaded: its size is the shown part of all rows, it follows a scroll, and a drag of its handle
// to the end shows the last photos and loads the pages up to them.
TEST_F(AlbumSectionQmlTests, FlatGridScrollBarDragShowsAndLoadsTheLastPhotos) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 300);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  // A search keeps pages at 120 rows, so 180 photos are not loaded at first.
  backend.search()->ApplyFuzzySearch(QStringLiteral("album-delete"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  auto* library = backend.library();
  ASSERT_EQ(library->TotalCount(), 300);
  ASSERT_EQ(library->ShownCount(), 120);
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  auto* grid   = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_grid"));
  auto* bar    = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_scrollBar"));
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(bar, nullptr);
  ASSERT_TRUE(WaitUntil([&]() { return GridCard(window, keys[0].file_id_) != nullptr; }));
  ProcessEvents(500);

  // 300 photos at 6 columns are 50 rows; the bar covers all of them, not the 20 loaded rows.
  const auto columns     = view.loaded()->property("columns").toInt();
  const auto cell_height = grid->property("cellHeight").toReal();
  const auto all_rows    = std::ceil(300.0 / columns);
  EXPECT_NEAR(bar->property("size").toReal(), grid->height() / (all_rows * cell_height), 0.01);
  EXPECT_LT(bar->property("size").toReal(), 1.0);
  EXPECT_NEAR(bar->property("position").toReal(), 0.0, 1e-6);

  // A scroll moves the bar.
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "restoreContentY",
                                        Q_ARG(QVariant, 5.0 * cell_height)));
  ProcessEvents(100);
  EXPECT_NEAR(bar->property("position").toReal(), 5.0 / all_rows, 0.01);
  EXPECT_TRUE(bar->property("active").toBool());

  // Drag the handle to the end of the bar.
  auto* handle = qvariant_cast<QQuickItem*>(bar->property("contentItem"));
  ASSERT_NE(handle, nullptr);
  const auto start = SceneCenter(handle);
  const auto end   = bar->mapToScene(QPointF(bar->width() / 2.0, bar->height() + 40.0)).toPoint();
  QTest::mouseMove(window, start);
  QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
  for (int step = 1; step <= 10; ++step) {
    QTest::mouseMove(window, start + (end - start) * step / 10);
    ProcessEvents(10);
  }
  QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
  ASSERT_TRUE(WaitUntil([&]() { return library->ShownCount() == 300; }))
      << "shown " << library->ShownCount() << " contentY " << grid->property("contentY").toReal();
  ASSERT_TRUE(WaitUntil([&]() { return GridCard(window, keys.back().file_id_) != nullptr; }));
  const auto max_y = all_rows * cell_height - grid->height();
  EXPECT_NEAR(grid->property("contentY").toReal() - grid->property("originY").toReal(), max_y, 1.0);
  EXPECT_NEAR(bar->property("position").toReal() + bar->property("size").toReal(), 1.0, 0.01);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// A rubber band drag over the flat grid selects the photos that it covers, also after a zoom
// change while scrolled, which moves the origin of the grid content away from 0.
TEST_F(AlbumSectionQmlTests, FlatGridRubberBandSelectsTheCoveredPhotosAfterTheOriginMoved) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 300);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  auto* grid   = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_grid"));
  ASSERT_NE(grid, nullptr);
  ASSERT_TRUE(WaitUntil([&]() { return GridCard(window, keys[0].file_id_) != nullptr; }));
  ProcessEvents(500);
  QSignalSpy selections(view.loaded(), SIGNAL(replaceSelection(QVariant)));
  ASSERT_TRUE(selections.isValid());

  // Drags from the center of the card at `from` to the center of the card at `to` and returns
  // the selected element ids.
  const auto drag = [&](size_t from, size_t to) {
    std::vector<uint> ids;
    auto*             first = GridCard(window, keys[from].file_id_);
    auto*             last  = GridCard(window, keys[to].file_id_);
    EXPECT_NE(first, nullptr);
    EXPECT_NE(last, nullptr);
    if (first == nullptr || last == nullptr) return ids;
    selections.clear();
    const auto start = SceneCenter(first);
    const auto end   = SceneCenter(last);
    QTest::mouseMove(window, start);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 10; ++step) {
      QTest::mouseMove(window, start + (end - start) * step / 10);
      ProcessEvents(10);
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end);
    ProcessEvents(100);
    if (selections.isEmpty()) return ids;
    for (const auto& item : selections.last().at(0).toList()) {
      ids.push_back(item.toMap().value("elementId").toUInt());
    }
    return ids;
  };
  const auto expected = [&](size_t from, int columns) {
    return std::vector<uint>{keys[from].file_id_, keys[from + 1].file_id_,
                             keys[from + static_cast<size_t>(columns)].file_id_,
                             keys[from + static_cast<size_t>(columns) + 1].file_id_};
  };

  auto columns = view.loaded()->property("columns").toInt();
  EXPECT_EQ(drag(0, static_cast<size_t>(columns) + 1), expected(0, columns));

  // Scroll down, change the zoom, and scroll back to the top.
  const auto cell_height = grid->property("cellHeight").toReal();
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "restoreContentY",
                                        Q_ARG(QVariant, 12.0 * cell_height + 9.0)));
  ProcessEvents(500);
  view.loaded()->setProperty("zoomLevel", 2);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded()->property("columns").toInt() == 4; }));
  ProcessEvents(1000);
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "restoreContentY", Q_ARG(QVariant, -1.0e9)));
  ProcessEvents(800);
  columns = 4;
  ASSERT_TRUE(WaitUntil([&]() { return GridCard(window, keys[0].file_id_) != nullptr; }));
  const auto origin_y = grid->property("originY").toReal();
  EXPECT_NE(origin_y, 0.0) << "the zoom change did not move the content origin";
  EXPECT_EQ(drag(0, static_cast<size_t>(columns) + 1), expected(0, columns))
      << "originY " << origin_y << " contentY " << grid->property("contentY").toReal();
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

}  // namespace alcedo::ui::test
