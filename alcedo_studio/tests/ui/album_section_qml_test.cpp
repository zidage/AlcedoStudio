//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Production QML of the library sort, group, and section view
// (album_sort_group_and_import_time_plan.md, Phase 3): the Album Inspector field actions and
// AlbumSectionView load from the source tree against a real packed project. The tests call
// the components' own action functions; they do not deliver synthetic pointer input.

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
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
#include <map>
#include <optional>
#include <set>
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
#include "image/color_label.hpp"
#include "json.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/library_mutation_operations.hpp"
#include "ui/alcedo_main/album_backend/library_view_preferences.hpp"
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
         {"AlbumSectionView.qml", "InspectorFieldActions.qml", "InspectorSection.qml",
          "AlbumInspectorPanel.qml", "StatsCard.qml", "DateFilterSection.qml",
          "StarRatingFilter.qml", "ThumbnailGridView.qml", "ThumbnailStarRating.qml",
          "ThumbnailEditMark.qml", "JustifiedThumbnailTile.qml", "ThumbnailPinBinding.qml",
          "ThumbnailLayoutAnimation.qml", "ThumbnailColorLabelDots.qml", "ThumbnailDeleteMark.qml",
          "ColorLabelFilterChips.qml"}) {
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

/// Drawn rectangle of @p item in the coordinates of @p view, with its transforms (the zoom
/// animation offset and scale).
auto DrawnRect(QQuickItem* item, QQuickItem* view) -> QRectF {
  return item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
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

/// A QML `var` property as a QVariantList (a JS array reads back as a QJSValue).
auto VarList(QObject* object, const char* name) -> QVariantList {
  QVariant value = object->property(name);
  if (value.userType() == qMetaTypeId<QJSValue>()) {
    value = value.value<QJSValue>().toVariant();
  }
  return value.toList();
}

/// Labels of the values a StatsCard shows, in order.
auto ShownLabels(QObject* card) -> QStringList {
  QStringList labels;
  for (const auto& entry : VarList(card, "visibleItems")) {
    labels << entry.toMap().value(QStringLiteral("label")).toString();
  }
  return labels;
}

/// Six photos with four cameras and five lenses (issue #766 value search).
auto SearchablePhotoSpecs() -> std::vector<LibraryPhotoSpec> {
  return {
      {.model_ = "Canon R5", .lens_ = "RF 50mm F1.2 L"},
      {.model_ = "Canon R6", .lens_ = "RF 24-70mm F2.8 L"},
      {.model_ = "Nikon Z8", .lens_ = "NIKKOR Z 50mm f/1.8 S"},
      {.model_ = "Nikon Z8", .lens_ = "NIKKOR Z 24-120mm f/4 S"},
      {.model_ = "Sony A7", .lens_ = "FE 85mm F1.4 GM"},
      {.model_ = "Sony A7", .lens_ = "FE 85mm F1.4 GM"},
  };
}

}  // namespace

// Issue #766: at the default inspector width (300 px) in English, the hero has no photo count
// line, and the sort and Group actions of every section sit on the title row of its header
// band, together at the right.
TEST_F(AlbumSectionQmlTests, LibraryOverviewKeepsTheActionsOnTheTitleRowAt300Px) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, SixPhotoSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  LoadedComponent inspector(backend, "AlbumInspectorPanel.qml", 300);
  ASSERT_TRUE(WaitUntil([&]() { return inspector.loaded() != nullptr; }))
      << Joined(inspector.warnings());
  auto* window = qobject_cast<QQuickWindow*>(inspector.window());
  ASSERT_NE(window, nullptr);
  ProcessEvents(200);

  // No "Showing ..." line anywhere in the panel.
  std::vector<QQuickItem*> showing;
  CollectItems(
      window->contentItem(),
      [](QQuickItem* item) {
        return item->isVisible() &&
               item->property("text").toString().startsWith(QStringLiteral("Showing"));
      },
      showing);
  EXPECT_TRUE(showing.empty()) << showing.front()->property("text").toString().toStdString();

  const std::vector<std::pair<const char*, const char*>> sections = {
      {"captureDateFilterSection", "date"},   {"importDateFilterSection", "import"},
      {"editTimeInspectorSection", "edited"}, {"cameraInspectorSection", "camera"},
      {"labelInspectorSection", "label"},     {"ratingInspectorSection", "rating"},
      {"lensInspectorSection", "lens"}};
  const auto action_size  = AppTheme::Instance().inspectorHeaderActionSize();
  const auto action_group = 3.0 * action_size + 2.0 * AppTheme::Instance().spaceXs();
  for (const auto& [section_name, field] : sections) {
    SCOPED_TRACE(section_name);
    auto* section = qobject_cast<QQuickItem*>(inspector.Find(section_name));
    ASSERT_NE(section, nullptr);
    EXPECT_NEAR(section->width(), 300.0 - 2.0 * AppTheme::Instance().spaceMd(), 16.0);
    EXPECT_FALSE(section->property("stackHeaderActions").toBool());
    auto* title = qvariant_cast<QQuickItem*>(section->property("titleItem"));
    ASSERT_NE(title, nullptr);
    // Title case, as written: no upper-case transform.
    EXPECT_NE(title->property("text").toString(), title->property("text").toString().toUpper());
    auto* actions =
        FirstItem(section, QStringLiteral("inspectorFieldActions_") + QString::fromLatin1(field));
    ASSERT_NE(actions, nullptr);
    ASSERT_TRUE(actions->isVisible());
    const auto title_rect = title->mapRectToScene(QRectF(0, 0, title->width(), title->height()));
    const auto actions_rect =
        actions->mapRectToScene(QRectF(0, 0, actions->width(), actions->height()));
    const auto section_rect =
        section->mapRectToScene(QRectF(0, 0, section->width(), section->height()));
    EXPECT_NEAR(actions_rect.center().y(), title_rect.center().y(), 1.0);
    EXPECT_GE(actions_rect.left(), title_rect.right() - 0.5);
    EXPECT_LE(actions_rect.right(), section_rect.right());
    EXPECT_LE(actions_rect.width(), action_group + 0.5);
  }
  EXPECT_TRUE(AlbumWarnings(inspector.warnings()).empty())
      << Joined(AlbumWarnings(inspector.warnings()));
}

// Issue #766: the search field of By Camera Model, By Labels, and By Lens shows only the values
// that contain the text (case-insensitive), keeps the selected value, shows every value again
// when cleared, and never changes the library query.
TEST_F(AlbumSectionQmlTests, LibraryOverviewSearchFiltersCameraLabelAndLensValues) {
  ApplicationModuleHost backend;
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, SearchablePhotoSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  LoadedComponent inspector(backend, "AlbumInspectorPanel.qml", 300);
  ASSERT_TRUE(WaitUntil([&]() { return inspector.loaded() != nullptr; }))
      << Joined(inspector.warnings());
  auto* camera = inspector.Find("cameraStatsCard");
  auto* labels = inspector.Find("labelStatsCard");
  auto* lens   = inspector.Find("lensStatsCard");
  ASSERT_NE(camera, nullptr);
  ASSERT_NE(labels, nullptr);
  ASSERT_NE(lens, nullptr);
  ASSERT_TRUE(WaitUntil([&]() {
    return VarList(camera, "model").size() == 4 && VarList(lens, "model").size() == 5;
  })) << "cameras "
      << VarList(camera, "model").size() << " lenses " << VarList(lens, "model").size();
  // The fixture has no semantic labels; the label card gets its values here.
  QVariantList label_values;
  for (const auto& [label, count] : std::vector<std::pair<QString, int>>{
           {"portrait", 5}, {"night", 3}, {"Night sky", 2}, {"street", 1}, {"nightlife", 1}}) {
    label_values << QVariantMap{{"label", label}, {"count", count}};
  }
  labels->setProperty("model", label_values);

  auto* camera_item = qobject_cast<QQuickItem*>(camera);
  auto* input       = FirstItem(camera_item, QStringLiteral("statsCardSearchInput"));
  ASSERT_NE(input, nullptr);
  EXPECT_TRUE(input->isVisible());
  EXPECT_STREQ(input->metaObject()->className(), "QQuickTextInput");
  auto* clear = FirstItem(camera_item, QStringLiteral("statsCardSearchClear"));
  ASSERT_NE(clear, nullptr);
  EXPECT_FALSE(clear->isVisible());

  const auto query_before  = backend.library()->model().count();
  const auto camera_filter = backend.stats()->StatsFilterCamera();

  // Camera: "CANON" matches inside the value, any case; the preview limit does not apply.
  camera->setProperty("filterText", QStringLiteral("CANON"));
  EXPECT_TRUE(clear->isVisible());
  {
    auto shown = ShownLabels(camera);
    shown.sort();
    EXPECT_EQ(shown, (QStringList{"Canon R5", "Canon R6"}));
  }
  // A selected value that does not match stays, first.
  camera->setProperty("selectedLabel", QStringLiteral("Sony A7"));
  EXPECT_EQ(ShownLabels(camera).value(0), QStringLiteral("Sony A7"));
  EXPECT_EQ(ShownLabels(camera).size(), 3);
  // Cleared: all values again (the chip preview plus "+N more").
  camera->setProperty("filterText", QString());
  EXPECT_EQ(ShownLabels(camera).size() + camera->property("hiddenCount").toInt(), 4);
  EXPECT_TRUE(ShownLabels(camera).contains(QStringLiteral("Sony A7")));

  // Labels: "night" matches three values; the selected "portrait" stays.
  labels->setProperty("selectedLabel", QStringLiteral("portrait"));
  labels->setProperty("filterText", QStringLiteral("night"));
  EXPECT_EQ(ShownLabels(labels), (QStringList{"portrait", "night", "Night sky", "nightlife"}));
  labels->setProperty("filterText", QStringLiteral("zzz"));
  EXPECT_EQ(ShownLabels(labels), (QStringList{"portrait"}));
  labels->setProperty("filterText", QString());
  EXPECT_EQ(ShownLabels(labels).size() + labels->property("hiddenCount").toInt(), 5);

  // Lens: "50mm" matches inside two lens names.
  lens->setProperty("filterText", QStringLiteral("50mm"));
  {
    auto shown = ShownLabels(lens);
    shown.sort();
    EXPECT_EQ(shown, (QStringList{"NIKKOR Z 50mm f/1.8 S", "RF 50mm F1.2 L"}));
  }
  lens->setProperty("filterText", QString());
  EXPECT_EQ(ShownLabels(lens).size(), 5);

  // Typing never ran a library query nor changed a filter.
  ProcessEvents(200);
  EXPECT_EQ(backend.library()->model().count(), query_before);
  EXPECT_EQ(backend.stats()->StatsFilterCamera(), camera_filter);
  EXPECT_TRUE(backend.stats()->StatsFilterLens().isEmpty());
  EXPECT_TRUE(backend.stats()->StatsFilterLabel().isEmpty());
  EXPECT_TRUE(AlbumWarnings(inspector.warnings()).empty())
      << Joined(AlbumWarnings(inspector.warnings()));
}

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
// its row) restores the same position in a new view instance, as after an editor round trip.
// A zoom step from the slider keeps the photo at the view centre in the centre row (issue 745).
TEST_F(AlbumSectionQmlTests, ScrollAnchorRestoresTheTopPhotoAndZoomKeepsTheCentrePhoto) {
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

  // A zoom step keeps the photo at the view centre under the view centre. ListView places the
  // rows relative to the created rows, so the drawn cells are read, not the model offsets.
  auto*      list_item = qobject_cast<QQuickItem*>(list);
  const auto centre    = QPointF(list_item->width() / 2.0, list_item->height() / 2.0);
  const auto cell_at_centre = [&]() -> std::pair<qint64, QRectF> {
    std::vector<QQuickItem*> cells;
    CollectNamedItems(list_item, QStringLiteral("albumSectionPhotoCell"), cells);
    for (auto* cell : cells) {
      if (!cell->isVisible()) continue;
      const auto rect = DrawnRect(cell, list_item);
      if (rect.contains(centre)) {
        return {cell->property("occurrence").toLongLong(), rect};
      }
    }
    return {-1, QRectF{}};
  };
  const auto [centre_occurrence, centre_rect] = cell_at_centre();
  ASSERT_GE(centre_occurrence, 0);
  const auto fraction = (centre.y() - centre_rect.top()) / centre_rect.height();
  const auto columns_before = view.loaded()->property("columnCount").toInt();
  view.loaded()->setProperty("zoomLevel", 1);
  ASSERT_TRUE(WaitUntil([&]() {
    return view.loaded()->property("columnCount").toInt() != columns_before &&
           sections.ColumnCount() == view.loaded()->property("columnCount").toInt();
  }));
  ProcessEvents(500);
  std::vector<QQuickItem*> cells;
  CollectNamedItems(list_item, QStringLiteral("albumSectionPhotoCell"), cells);
  bool found = false;
  for (auto* cell : cells) {
    if (!cell->isVisible() || cell->property("occurrence").toLongLong() != centre_occurrence) {
      continue;
    }
    found           = true;
    const auto rect = DrawnRect(cell, list_item);
    EXPECT_NEAR(rect.top() + fraction * rect.height(), centre.y(), 1.0);
  }
  EXPECT_TRUE(found) << "occurrence " << centre_occurrence << " has no cell";
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


// ── Issue 745: zoom animation and justified rows ──

namespace {

/// Store QSettings of this test in @p dir, so the layout setting does not reach the user's
/// settings.
void UseTestSettings(const std::filesystem::path& dir) {
  QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
  QCoreApplication::setApplicationName(QStringLiteral("AlbumSectionQmlTest"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QString::fromStdWString((dir / "settings").wstring()));
  QSettings settings;
  settings.clear();
  settings.sync();
}

/// The justified tiles under @p root by photo index.
auto JustifiedTiles(QQuickItem* root) -> std::map<int, QQuickItem*> {
  std::vector<QQuickItem*> items;
  CollectItems(
      root,
      [](QQuickItem* item) {
        return item->objectName().startsWith(QStringLiteral("thumbnailGridView_tile_"));
      },
      items);
  std::map<int, QQuickItem*> tiles;
  for (auto* item : items) {
    tiles[item->property("photoIndex").toInt()] = item;
  }
  return tiles;
}

auto MixedSizePhotoSpecs(int count) -> std::vector<LibraryPhotoSpec> {
  const std::vector<std::pair<uint32_t, uint32_t>> sizes{
      {6000, 4000}, {4000, 6000}, {5000, 5000}, {7200, 3000}, {3000, 4000}, {6000, 3375}};
  std::vector<LibraryPhotoSpec> specs;
  for (int i = 0; i < count; ++i) {
    LibraryPhotoSpec spec;
    spec.rating_ = i % 3;
    spec.width_  = sizes[static_cast<size_t>(i) % sizes.size()].first;
    spec.height_ = sizes[static_cast<size_t>(i) % sizes.size()].second;
    specs.push_back(spec);
  }
  return specs;
}

auto AspectRatioOfRow(ApplicationModuleHost& backend, int row) -> double {
  auto& model = backend.library()->model();
  return model.data(model.index(row, 0), AlbumThumbnailModel::AspectRatio).toDouble();
}

}  // namespace

// A 6 to 5 column zoom step: on the first frame every card is drawn at its old rectangle, at
// the end every card is at its new cell, and the card under the pointer keeps the point under
// the pointer at the same height for the whole animation.
TEST_F(AlbumSectionQmlTests, FlatGridZoomStepMovesEachCardFromItsOldRectangleToItsNewCell) {
  ApplicationModuleHost backend;
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 300);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto& keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  auto* grid   = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_grid"));
  ASSERT_NE(grid, nullptr);
  ASSERT_TRUE(WaitUntil([&]() { return GridCard(window, keys[0].file_id_) != nullptr; }));
  ProcessEvents(500);
  ASSERT_EQ(view.loaded()->property("columns").toInt(), 6);
  const auto cell_height = grid->property("cellHeight").toReal();
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "restoreContentY",
                                        Q_ARG(QVariant, 4.0 * cell_height + 37.0)));
  ProcessEvents(300);

  std::map<uint, QRectF> old_rects;
  for (const auto& key : keys) {
    auto* card = GridCard(window, key.file_id_);
    if (card == nullptr || !card->isVisible()) {
      continue;
    }
    const auto rect = DrawnRect(card, grid);
    if (rect.bottom() > 0 && rect.top() < grid->height()) {
      old_rects[key.file_id_] = rect;
    }
  }
  ASSERT_GT(old_rects.size(), 12U);
  const QPointF focus(500.0, 400.0);
  uint          focus_id = 0;
  for (const auto& [id, rect] : old_rects) {
    if (rect.contains(focus)) {
      focus_id = id;
    }
  }
  ASSERT_NE(focus_id, 0U) << "no card under the focus point";
  const auto focus_old = old_rects[focus_id];
  const auto fraction  = (focus.y() - focus_old.top()) / focus_old.height();
  const auto focus_y   = [&]() {
    auto* card = GridCard(window, focus_id);
    if (card == nullptr) return -1.0e9;
    const auto rect = DrawnRect(card, grid);
    return rect.top() + fraction * rect.height();
  };

  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "setZoomLevelAt", Q_ARG(QVariant, 3),
                                        Q_ARG(QVariant, focus.y()), Q_ARG(QVariant, focus.x())));
  // First frame: the new layout is applied, the cards are drawn at their old rectangles.
  EXPECT_EQ(view.loaded()->property("columns").toInt(), 5);
  int kept = 0;
  for (const auto& [id, rect] : old_rects) {
    auto* card = GridCard(window, id);
    if (card == nullptr) {
      continue;
    }
    ++kept;
    const auto drawn = DrawnRect(card, grid);
    EXPECT_NEAR(drawn.x(), rect.x(), 0.5) << id;
    EXPECT_NEAR(drawn.y(), rect.y(), 0.5) << id;
    EXPECT_NEAR(drawn.width(), rect.width(), 0.5) << id;
    EXPECT_NEAR(drawn.height(), rect.height(), 0.5) << id;
  }
  EXPECT_GT(kept, 8);
  EXPECT_NEAR(focus_y(), focus.y(), 0.5);
  for (int step = 0; step < 6; ++step) {
    ProcessEvents(40);
    EXPECT_NEAR(focus_y(), focus.y(), 1.0) << "step " << step;
  }

  // Last frame: every card is at its cell of the 5-column layout.
  ProcessEvents(600);
  const auto cell_width = grid->property("cellWidth").toReal();
  const auto content_y  = grid->property("contentY").toReal();
  const auto origin_y   = grid->property("originY").toReal();
  for (size_t index = 0; index < keys.size(); ++index) {
    auto* card = GridCard(window, keys[index].file_id_);
    if (card == nullptr || !card->isVisible()) {
      continue;
    }
    EXPECT_DOUBLE_EQ(card->property("zoomTranslateX").toReal(), 0.0) << index;
    EXPECT_DOUBLE_EQ(card->property("zoomTranslateY").toReal(), 0.0) << index;
    EXPECT_DOUBLE_EQ(card->property("zoomScaleX").toReal(), 1.0) << index;
    EXPECT_DOUBLE_EQ(card->property("zoomScaleY").toReal(), 1.0) << index;
    // GridView keeps its current item (index 0) alive far above the view; only the cells on
    // the screen are checked.
    const auto expected_y = origin_y +
                            std::floor(static_cast<qreal>(index) / 5.0) *
                                grid->property("cellHeight").toReal() -
                            content_y;
    if (expected_y + card->height() <= 0 || expected_y >= grid->height()) {
      continue;
    }
    const auto drawn = DrawnRect(card, grid);
    EXPECT_NEAR(drawn.x(), static_cast<qreal>(index % 5) * cell_width, 0.5) << index;
    EXPECT_NEAR(drawn.y(), expected_y, 0.5) << index;
  }
  EXPECT_NEAR(focus_y(), focus.y(), 1.0);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// Justified rows: the setting is read by a new view; the tiles of a row share one photo height,
// a complete row fills the view width, each tile width follows the photo aspect ratio; the hover
// details show the camera line and rate a photo with a star click; a zoom step keeps the tile
// under the pointer; and the classic setting brings the cards back.
TEST_F(AlbumSectionQmlTests, JustifiedRowsFillTheWidthFollowTheAspectRatioAndRateFromTheDetails) {
  UseTestSettings(temp_dir_);
  ApplicationModuleHost backend;
  ASSERT_TRUE(backend.library_view()->setGridLayout(QStringLiteral("justified")));
  EXPECT_EQ(QSettings{}.value("library/gridLayout").toString(), QStringLiteral("justified"));
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, MixedSizePhotoSpecs(60));
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto& keys = packed->images_;
  // The worker reads the oriented dimensions into the aspectRatio role.
  std::set<long> aspect_ratios;
  for (int row = 0; row < backend.library()->model().count(); ++row) {
    aspect_ratios.insert(std::lround(AspectRatioOfRow(backend, row) * 1000.0));
  }
  EXPECT_EQ(aspect_ratios, (std::set<long>{667, 750, 1000, 1500, 1778, 2400}));

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  auto* flick  = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_justified"));
  ASSERT_NE(flick, nullptr);
  EXPECT_EQ(view.loaded()->property("layoutMode").toString(), QStringLiteral("justified"));
  ASSERT_TRUE(WaitUntil([&]() { return JustifiedTiles(flick).size() > 6; }))
      << Joined(view.warnings());
  ProcessEvents(400);
  EXPECT_EQ(GridCard(window, keys[0].file_id_), nullptr);

  const auto caption = view.loaded()->property("justifiedCaptionHeight").toReal();
  const auto spacing = view.loaded()->property("justifiedSpacing").toReal();
  std::map<qreal, std::vector<QQuickItem*>> rows;
  for (const auto& [index, tile] : JustifiedTiles(flick)) {
    rows[tile->y()].push_back(tile);
  }
  ASSERT_GE(rows.size(), 3U);
  int complete_rows = 0;
  for (auto it = rows.begin(); std::next(it) != rows.end(); ++it) {
    auto& tiles = it->second;
    std::sort(tiles.begin(), tiles.end(),
              [](QQuickItem* a, QQuickItem* b) { return a->x() < b->x(); });
    qreal width = 0.0;
    for (auto* tile : tiles) {
      EXPECT_DOUBLE_EQ(tile->height(), tiles.front()->height());
      const auto photo_height = tile->height() - caption;
      EXPECT_NEAR(tile->width() / photo_height,
                  AspectRatioOfRow(backend, tile->property("photoIndex").toInt()), 1e-3);
      width += tile->width();
    }
    width += spacing * static_cast<qreal>(tiles.size() - 1);
    EXPECT_NEAR(width, flick->width(), 0.5) << "row at " << it->first;
    EXPECT_NEAR(tiles.front()->x(), 0.0, 0.5);
    ++complete_rows;
  }
  EXPECT_GE(complete_rows, 2);

  // Hover details and a star click on the second tile.
  QSignalSpy requests(view.loaded(), SIGNAL(ratingRequested(QVariant, int)));
  ASSERT_TRUE(requests.isValid());
  auto* tile = JustifiedTiles(flick).at(1);
  ASSERT_NE(tile, nullptr);
  const auto target  = tile->property("elementId").toUInt();
  auto*      details = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_details"));
  auto*      camera  = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_camera"));
  auto*      stars   = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_stars"));
  ASSERT_NE(details, nullptr);
  ASSERT_NE(camera, nullptr);
  ASSERT_NE(stars, nullptr);
  EXPECT_DOUBLE_EQ(details->property("opacity").toReal(), 0.0);
  QTest::mouseMove(window, SceneCenter(tile));
  ASSERT_TRUE(WaitUntil([&]() { return details->property("opacity").toReal() >= 1.0; }, 3000));
  EXPECT_TRUE(
      camera->property("text").toString().contains(QStringLiteral("Synthetic Album Camera")));
  auto* star4 = FirstItem(stars, QStringLiteral("thumbnailStar_4"));
  ASSERT_NE(star4, nullptr);
  QTest::mouseMove(window, SceneCenter(star4));
  ASSERT_TRUE(WaitUntil([&]() { return stars->property("previewRating").toInt() == 4; }, 3000));
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, SceneCenter(star4));
  ASSERT_TRUE(WaitUntil([&]() { return requests.count() == 1; }, 3000));
  EXPECT_EQ(requests.at(0).at(0).toMap().value("elementId").toUInt(), target);
  EXPECT_EQ(requests.at(0).at(1).toInt(), 4);
  ApplyRatingRequest(backend, requests.at(0));
  EXPECT_EQ(RatingOf(backend, target), 4);
  ASSERT_TRUE(WaitUntil([&]() { return StarCount(stars, QStringLiteral("★")) == 4; }, 3000));
  QTest::mouseMove(window, QPoint(5, 5));
  ProcessEvents(300);

  // A zoom step keeps the point of the tile under the pointer at its height.
  const QPointF focus(300.0, 300.0);
  int           focus_index = -1;
  QRectF        focus_old;
  for (const auto& [index, item] : JustifiedTiles(flick)) {
    const auto rect = DrawnRect(item, flick);
    if (rect.contains(focus)) {
      focus_index = index;
      focus_old   = rect;
    }
  }
  ASSERT_GE(focus_index, 0);
  const auto fraction = (focus.y() - focus_old.top()) / focus_old.height();
  const auto focus_y  = [&]() {
    const auto tiles = JustifiedTiles(flick);
    const auto it    = tiles.find(focus_index);
    if (it == tiles.end()) return -1.0e9;
    const auto rect = DrawnRect(it->second, flick);
    return rect.top() + fraction * rect.height();
  };
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "setZoomLevelAt", Q_ARG(QVariant, 2),
                                        Q_ARG(QVariant, focus.y()), Q_ARG(QVariant, focus.x())));
  EXPECT_NEAR(focus_y(), focus.y(), 0.5);
  ProcessEvents(700);
  EXPECT_NEAR(focus_y(), focus.y(), 1.0);
  EXPECT_DOUBLE_EQ(view.loaded()->property("justifiedRowHeight").toReal(), 280.0);

  // The classic setting shows the cards again.
  ASSERT_TRUE(backend.library_view()->setGridLayout(QStringLiteral("classic")));
  ASSERT_TRUE(WaitUntil([&]() {
    if (!JustifiedTiles(flick).empty()) return false;
    for (const auto& key : keys) {
      if (GridCard(window, key.file_id_) != nullptr) return true;
    }
    return false;
  }));
  ProcessEvents(300);
  EXPECT_TRUE(JustifiedTiles(flick).empty());
  EXPECT_EQ(QSettings{}.value("library/gridLayout").toString(), QStringLiteral("classic"));
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}


namespace {

/// Shown photo delegates (classic cards and justified tiles) by element id.
auto ShownPhotoItems(QQuickItem* root) -> std::map<uint, QQuickItem*> {
  std::vector<QQuickItem*> items;
  CollectItems(
      root,
      [](QQuickItem* item) {
        const auto name = item->objectName();
        return item->isVisible() && (name == QStringLiteral("thumbnailGridView_firstCard") ||
                                     name.startsWith(QStringLiteral("thumbnailGridView_card_")) ||
                                     name.startsWith(QStringLiteral("thumbnailGridView_tile_")));
      },
      items);
  std::map<uint, QQuickItem*> out;
  for (auto* item : items) {
    out[item->property("elementId").toUInt()] = item;
  }
  return out;
}

/// Zoom steps from the slider (the view centre is the focus) deep in a long result: after each
/// step and its settle timers, the photo nearest the view centre is drawn with the same point
/// (the centre, or the nearest edge when the centre is in a gap) at the same viewport height.
void ExpectSliderZoomKeepsTheCentrePhoto(ApplicationModuleHost& backend, const char* layout,
                                         const std::filesystem::path& dir) {
  UseTestSettings(dir);
  ASSERT_TRUE(backend.library_view()->setGridLayout(QString::fromLatin1(layout)));
  const auto packed = CreatePopulatedPackedProject(dir, {}, 0, MixedSizePhotoSpecs(600));
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* shown_view = qobject_cast<QQuickItem*>(view.Find(
      std::string(layout) == "justified" ? "thumbnailGridView_justified" : "thumbnailGridView_grid"));
  ASSERT_NE(shown_view, nullptr);
  ASSERT_TRUE(WaitUntil([&]() { return ShownPhotoItems(shown_view).size() > 6; }));
  ProcessEvents(500);
  ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "restoreContentY", Q_ARG(QVariant, 6000.0)));
  ProcessEvents(500);

  const QPointF centre(shown_view->width() / 2.0, shown_view->height() / 2.0);
  for (const int level : {3, 2, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4}) {
    // The photo that the view anchors: the photo nearest the centre.
    QVariant focus_index;
    ASSERT_TRUE(QMetaObject::invokeMethod(view.loaded(), "nearestCardIndexAt",
                                          Q_RETURN_ARG(QVariant, focus_index),
                                          Q_ARG(QVariant, centre.x()), Q_ARG(QVariant, centre.y())));
    uint   focus_id = 0;
    double fraction = 0.0;
    double anchor_y = 0.0;  // Viewport height of the anchored point of that photo.
    for (const auto& [id, item] : ShownPhotoItems(shown_view)) {
      if (item->property("cardIndex").toInt() != focus_index.toInt()) {
        continue;
      }
      const auto rect = DrawnRect(item, shown_view);
      focus_id        = id;
      fraction        = std::clamp((centre.y() - rect.top()) / rect.height(), 0.0, 1.0);
      anchor_y        = rect.top() + fraction * rect.height();
    }
    ASSERT_NE(focus_id, 0U) << layout << " level " << level << ": no photo at the centre";
    view.loaded()->setProperty("zoomLevel", level);
    ProcessEvents(900);
    const auto items = ShownPhotoItems(shown_view);
    const auto it    = items.find(focus_id);
    ASSERT_NE(it, items.end()) << layout << " level " << level << ": the centre photo left";
    const auto rect = DrawnRect(it->second, shown_view);
    EXPECT_NEAR(rect.top() + fraction * rect.height(), anchor_y, 1.0)
        << layout << " level " << level << " contentY "
        << view.loaded()->property("contentY").toReal();
  }
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

}  // namespace

// Slider zoom steps deep in a long result keep the photo at the view centre at the centre
// height after every step, in the classic grid and in the justified rows.
TEST_F(AlbumSectionQmlTests, ClassicGridSliderZoomStepsKeepTheCentrePhotoAtTheCentre) {
  ApplicationModuleHost backend;
  ExpectSliderZoomKeepsTheCentrePhoto(backend, "classic", temp_dir_);
}

TEST_F(AlbumSectionQmlTests, JustifiedRowsSliderZoomStepsKeepTheCentrePhotoAtTheCentre) {
  ApplicationModuleHost backend;
  ExpectSliderZoomKeepsTheCentrePhoto(backend, "justified", temp_dir_);
}

// Issue #766: a Library Overview section folds with the shared fold motion. The header stays;
// the content keeps its size and a clip reveals it with the fold progress, which also drives the
// content opacity and the glyph turn. The date content stays loaded while it folds.
// reduceMotion makes the fold a cut.
TEST_F(AlbumSectionQmlTests, LibraryOverviewSectionsFoldWithTheSharedMotion) {
  UseTestSettings(temp_dir_);
  auto&      theme                = AppTheme::Instance();
  const bool reduce_motion_before = theme.reduceMotion();
  theme.setReduceMotion(false);
  ApplicationModuleHost backend;
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, SearchablePhotoSpecs());
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));

  LoadedComponent inspector(backend, "AlbumInspectorPanel.qml", 300);
  ASSERT_TRUE(WaitUntil([&]() { return inspector.loaded() != nullptr; }))
      << Joined(inspector.warnings());
  auto* lens = qobject_cast<QQuickItem*>(inspector.Find("lensInspectorSection"));
  ASSERT_NE(lens, nullptr);
  auto* clip   = FirstItem(lens, QStringLiteral("inspectorSectionBodyClip"));
  auto* body   = FirstItem(lens, QStringLiteral("inspectorSectionBody"));
  auto* glyph  = FirstItem(lens, QStringLiteral("inspectorSectionGlyph"));
  auto* header = FirstItem(lens, QStringLiteral("inspectorSectionHeader"));
  ASSERT_NE(clip, nullptr);
  ASSERT_NE(body, nullptr);
  ASSERT_NE(glyph, nullptr);
  ASSERT_NE(header, nullptr);
  const auto full_height = [&]() { return lens->property("bodyFullHeight").toReal(); };
  ASSERT_TRUE(WaitUntil([&]() {
    return full_height() > 40.0 && std::abs(clip->height() - full_height()) < 0.5;
  })) << "clip "
      << clip->height() << " full " << full_height();
  const auto full        = full_height();
  const auto body_height = body->height();
  const auto header_y    = header->mapToItem(lens, QPointF(0, 0)).y();
  EXPECT_NEAR(glyph->rotation(), 90.0, 0.5);

  // Pinned half way through a close: half the content shows at half opacity, at full size.
  lens->setProperty("expanded", false);
  ASSERT_TRUE(QMetaObject::invokeMethod(lens, "driveFoldProgress", Q_ARG(QVariant, 0.5)));
  ProcessEvents(20);
  EXPECT_NEAR(clip->height(), full * 0.5, 1.0);
  EXPECT_NEAR(body->height(), body_height, 0.5);
  EXPECT_NEAR(body->opacity(), 0.5, 0.01);
  EXPECT_NEAR(glyph->rotation(), 45.0, 0.5);
  EXPECT_NEAR(header->mapToItem(lens, QPointF(0, 0)).y(), header_y, 0.5);
  EXPECT_NEAR(header->height(), 36.0, 0.5);
  ASSERT_TRUE(QMetaObject::invokeMethod(lens, "endFoldDrive"));
  ASSERT_TRUE(WaitUntil([&]() { return !clip->isVisible(); }, 2000)) << "clip " << clip->height();
  EXPECT_NEAR(glyph->rotation(), 0.0, 0.5);

  // The date content stays loaded while the section folds.
  auto* capture = qobject_cast<QQuickItem*>(inspector.Find("captureDateFilterSection"));
  ASSERT_NE(capture, nullptr);
  auto* calendar = FirstItem(capture, QStringLiteral("dateFilterCalendarLoader"));
  ASSERT_NE(calendar, nullptr);
  capture->setProperty("expanded", false);
  ASSERT_TRUE(QMetaObject::invokeMethod(capture, "driveFoldProgress", Q_ARG(QVariant, 0.5)));
  ProcessEvents(20);
  EXPECT_TRUE(calendar->property("active").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(capture, "endFoldDrive"));
  ASSERT_TRUE(WaitUntil([&]() { return !calendar->property("active").toBool(); }, 2000));

  // An open runs over time: the clip passes through heights between closed and open.
  lens->setProperty("expanded", true);
  bool          saw_between = false;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 2000 && std::abs(clip->height() - full) >= 0.5) {
    ProcessEvents(5);
    saw_between = saw_between || (clip->height() > 1.0 && clip->height() < full - 1.0);
  }
  EXPECT_NEAR(clip->height(), full, 0.5);
  EXPECT_TRUE(saw_between);

  // reduceMotion: the close is a cut.
  theme.setReduceMotion(true);
  lens->setProperty("expanded", false);
  EXPECT_TRUE(WaitUntil([&]() { return !clip->isVisible(); }, 100)) << "clip " << clip->height();
  theme.setReduceMotion(reduce_motion_before);
  EXPECT_TRUE(AlbumWarnings(inspector.warnings()).empty())
      << Joined(AlbumWarnings(inspector.warnings()));
}

namespace {

constexpr ColorLabelMask kRedGreenBlue = ColorLabelBit(ColorLabel::kRed) |
                                         ColorLabelBit(ColorLabel::kGreen) |
                                         ColorLabelBit(ColorLabel::kBlue);
constexpr ColorLabelMask kPurpleGray =
    ColorLabelBit(ColorLabel::kPurple) | ColorLabelBit(ColorLabel::kGray);

/// Six photos with color labels and Delete marks (#771): the third has a long file name and all
/// seven codes; the second and the sixth have the Delete mark.
auto MarkedPhotoSpecs() -> std::vector<LibraryPhotoSpec> {
  std::vector<LibraryPhotoSpec>                    specs(6);
  const std::vector<std::pair<uint32_t, uint32_t>> sizes{{6000, 4000}, {4000, 6000}, {6000, 4000},
                                                         {5000, 5000}, {6000, 4000}, {4000, 6000}};
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].width_  = sizes[i].first;
    specs[i].height_ = sizes[i].second;
  }
  specs[0].color_labels_ = kRedGreenBlue;
  specs[0].rating_       = 4;
  specs[1].delete_mark_  = true;
  specs[2].file_name_    = L"_A7R0981_landscape_final_client_selects_export_version_two.dng";
  specs[2].color_labels_ = kColorLabelMaskAll;
  specs[2].rating_       = 3;
  specs[3].color_labels_ = ColorLabelBit(ColorLabel::kOrange);
  specs[5].color_labels_ = kPurpleGray;
  specs[5].delete_mark_  = true;
  return specs;
}

/// Codes of the visible dots of a ThumbnailColorLabelDots.
auto ShownDotCodes(QQuickItem* dots) -> ColorLabelMask {
  ColorLabelMask mask = 0;
  if (dots == nullptr || !dots->isVisible()) {
    return mask;
  }
  for (int code = 0; code < kColorLabelCount; ++code) {
    auto* dot = FirstItem(dots, QStringLiteral("thumbnailColorLabelDot_%1").arg(code));
    if (dot != nullptr && dot->isVisible()) {
      mask = WithColorLabel(mask, static_cast<ColorLabel>(code), true);
    }
  }
  return mask;
}

auto SceneRect(QQuickItem* item) -> QRectF {
  return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

/// Dot diameter of a ThumbnailColorLabelDots (the colored part, without the ring).
auto DotDiameter(QQuickItem* dots) -> qreal { return dots->property("dotSize").toReal(); }

/// The caption shows the name, then the whole dots, inside @p box: the dots start after the
/// name, end inside the box, and keep their full width (the name elides instead).
void ExpectNameThenDotsInside(QQuickItem* name, QQuickItem* dots, QQuickItem* box,
                              const std::string& where) {
  ASSERT_NE(name, nullptr) << where;
  ASSERT_NE(dots, nullptr) << where;
  ASSERT_NE(box, nullptr) << where;
  const auto name_rect = SceneRect(name);
  const auto dots_rect = SceneRect(dots);
  const auto box_rect  = SceneRect(box);
  EXPECT_GE(dots_rect.left() + 0.5, name_rect.right()) << where;
  EXPECT_LE(dots_rect.right(), box_rect.right() + 0.5) << where;
  EXPECT_GE(dots_rect.left(), box_rect.left()) << where;
  EXPECT_NEAR(dots->width(), dots->implicitWidth(), 0.5) << where;
}

auto TileByElement(QQuickItem* root, sl_element_id_t element_id) -> QQuickItem* {
  for (const auto& [index, tile] : JustifiedTiles(root)) {
    if (tile->property("elementId").toUInt() == element_id) {
      return tile;
    }
  }
  return nullptr;
}

auto MarkTargets(const PopulatedProject::ImageKey& key) -> QVariantList {
  return QVariantList{QVariantMap{{"elementId", static_cast<uint>(key.file_id_)},
                                  {"imageId", static_cast<uint>(key.image_id_)}}};
}

/// Adds Yellow and the Delete mark to @p key through the ImageController batch writes.
void MarkYellowAndDeleted(ApplicationModuleHost& backend, const PopulatedProject::ImageKey& key) {
  auto label = backend.images()->SetImageColorLabel(MarkTargets(key),
                                                    static_cast<int>(ColorLabel::kYellow), true);
  ASSERT_TRUE(label.value("success").toBool()) << label.value("message").toString().toStdString();
  auto mark = backend.images()->SetImageDeleteMark(MarkTargets(key), true);
  ASSERT_TRUE(mark.value("success").toBool()) << mark.value("message").toString().toStdString();
}

auto ColorOf(QQuickItem* item) -> QColor { return item->property("color").value<QColor>(); }

}  // namespace

// Issue #771, classic cards at 4 and 8 columns: the dots after the file name match the color
// labels of each photo, in code order, and stay whole inside the card with a long file name;
// the Delete mark scrim and its badge show only on the marked photos. A mark change updates
// the same card in place.
TEST_F(AlbumSectionQmlTests, ClassicCardsShowTheLabelDotsAfterTheNameAndTheDeleteScrim) {
  ApplicationModuleHost backend;
  const auto            specs  = MarkedPhotoSpecs();
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent view(backend, "ThumbnailGridView.qml", 1000);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* window = qobject_cast<QQuickWindow*>(view.window());
  ASSERT_NE(window, nullptr);
  const auto wait_for_columns = [&](int level, int columns) {
    view.loaded()->setProperty("zoomLevel", level);
    return WaitUntil([&]() {
      for (const auto& key : keys) {
        if (GridCard(window, key.file_id_) == nullptr) return false;
      }
      return view.loaded()->property("columns").toInt() == columns;
    });
  };
  ASSERT_TRUE(wait_for_columns(2, 4)) << Joined(view.warnings());
  ProcessEvents(1000);

  const auto& theme = AppTheme::Instance();
  for (size_t index = 0; index < keys.size(); ++index) {
    const auto where = "4 columns, photo " + std::to_string(index);
    auto*      card  = GridCard(window, keys[index].file_id_);
    ASSERT_NE(card, nullptr) << where;
    auto* dots = FirstItem(card, QStringLiteral("thumbnailGridLabelDots"));
    auto* name = FirstItem(card, QStringLiteral("thumbnailGridName"));
    ASSERT_NE(dots, nullptr) << where;
    ASSERT_NE(name, nullptr) << where;
    EXPECT_EQ(ShownDotCodes(dots), specs[index].color_labels_) << where;
    EXPECT_EQ(VisibleCount(card, QStringLiteral("thumbnailGridCompactLabelDots")), 0) << where;
    if (specs[index].color_labels_ != 0) {
      EXPECT_DOUBLE_EQ(DotDiameter(dots), 9.0) << where;
      ExpectNameThenDotsInside(name, dots, card, where);
    }
    const bool deleted = specs[index].delete_mark_;
    EXPECT_EQ(VisibleCount(card, QStringLiteral("thumbnailGridDeleteMark")), deleted ? 1 : 0)
        << where;
    EXPECT_EQ(ColorOf(name), deleted ? theme.textMutedColor() : theme.textColor()) << where;
    if (deleted) {
      auto* badge = FirstItem(card, QStringLiteral("thumbnailDeleteMarkBadge"));
      ASSERT_NE(badge, nullptr) << where;
      EXPECT_DOUBLE_EQ(badge->width(), 34.0) << where;
      auto*      scrim       = FirstItem(card, QStringLiteral("thumbnailGridDeleteMark"));
      const auto scrim_color = ColorOf(scrim);
      EXPECT_EQ(scrim_color.red(), 6) << where;
      EXPECT_EQ(scrim_color.green(), 6) << where;
      EXPECT_EQ(scrim_color.blue(), 6) << where;
      EXPECT_NEAR(scrim_color.alphaF(), 0.72, 0.005) << where;
      // The scrim covers the photo box.
      EXPECT_EQ(SceneRect(scrim), SceneRect(scrim->parentItem())) << where;
    }
  }
  // The long name elides; all seven dots stay.
  auto* long_name = FirstItem(GridCard(window, keys[2].file_id_), "thumbnailGridName");
  ASSERT_NE(long_name, nullptr);
  EXPECT_TRUE(long_name->property("truncated").toBool());
  // A short name keeps its full width, and the dots follow it.
  EXPECT_FALSE(FirstItem(GridCard(window, keys[0].file_id_), "thumbnailGridName")
                   ->property("truncated")
                   .toBool());

  // 8 columns: name, 7 px dots, filled stars on one line; a 24 px badge.
  ASSERT_TRUE(wait_for_columns(5, 8)) << Joined(view.warnings());
  ProcessEvents(1000);
  for (size_t index = 0; index < keys.size(); ++index) {
    const auto where = "8 columns, photo " + std::to_string(index);
    auto*      card  = GridCard(window, keys[index].file_id_);
    ASSERT_NE(card, nullptr) << where;
    auto* dots = FirstItem(card, QStringLiteral("thumbnailGridCompactLabelDots"));
    auto* name = FirstItem(card, QStringLiteral("thumbnailGridCompactName"));
    ASSERT_NE(dots, nullptr) << where;
    EXPECT_EQ(ShownDotCodes(dots), specs[index].color_labels_) << where;
    EXPECT_EQ(VisibleCount(card, QStringLiteral("thumbnailGridLabelDots")), 0) << where;
    if (specs[index].color_labels_ != 0) {
      EXPECT_DOUBLE_EQ(DotDiameter(dots), 7.0) << where;
      ExpectNameThenDotsInside(name, dots, card, where);
      auto* stars = FirstItem(card, QStringLiteral("thumbnailGridCompactStars"));
      if (stars != nullptr && stars->isVisible()) {
        EXPECT_LE(SceneRect(dots).right(), SceneRect(stars).left() + 0.5) << where;
      }
    }
    if (specs[index].delete_mark_) {
      auto* badge = FirstItem(card, QStringLiteral("thumbnailDeleteMarkBadge"));
      ASSERT_NE(badge, nullptr) << where;
      EXPECT_TRUE(badge->isVisible()) << where;
      EXPECT_DOUBLE_EQ(badge->width(), 24.0) << where;
    }
  }

  // A mark change updates the drawn card: no model reset, the same delegate, no scroll move.
  auto* grid = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_grid"));
  ASSERT_NE(grid, nullptr);
  QSignalSpy resets(&backend.library()->model(), &QAbstractItemModel::modelReset);
  auto*      target_card = GridCard(window, keys[4].file_id_);
  ASSERT_NE(target_card, nullptr);
  const auto content_y = grid->property("contentY").toReal();
  MarkYellowAndDeleted(backend, keys[4]);
  ASSERT_TRUE(WaitUntil([&]() {
    auto* card = GridCard(window, keys[4].file_id_);
    return card != nullptr &&
           ShownDotCodes(FirstItem(card, QStringLiteral("thumbnailGridCompactLabelDots"))) ==
               ColorLabelBit(ColorLabel::kYellow) &&
           VisibleCount(card, QStringLiteral("thumbnailGridDeleteMark")) == 1;
  }));
  ProcessEvents(300);
  EXPECT_EQ(resets.count(), 0);
  EXPECT_EQ(GridCard(window, keys[4].file_id_), target_card);
  EXPECT_DOUBLE_EQ(grid->property("contentY").toReal(), content_y);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// Issue #771, justified rows: the caption is the file name and the dots, inside the tile with a
// long file name; the Delete mark scrim and badge show only on the marked photos, and a mark
// change updates the same tile in place.
TEST_F(AlbumSectionQmlTests, JustifiedTilesShowTheLabelDotsAfterTheNameAndTheDeleteScrim) {
  UseTestSettings(temp_dir_);
  ApplicationModuleHost backend;
  ASSERT_TRUE(backend.library_view()->setGridLayout(QStringLiteral("justified")));
  const auto specs  = MarkedPhotoSpecs();
  const auto packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  // 640 px: the long name does not fit in its tile.
  LoadedComponent view(backend, "ThumbnailGridView.qml", 640);
  ASSERT_TRUE(WaitUntil([&]() { return view.loaded() != nullptr; })) << Joined(view.warnings());
  auto* flick = qobject_cast<QQuickItem*>(view.Find("thumbnailGridView_justified"));
  ASSERT_NE(flick, nullptr);
  ASSERT_TRUE(WaitUntil([&]() {
    for (const auto& key : keys) {
      if (TileByElement(flick, key.file_id_) == nullptr) return false;
    }
    return true;
  })) << Joined(view.warnings());
  ProcessEvents(400);

  const auto& theme = AppTheme::Instance();
  for (size_t index = 0; index < keys.size(); ++index) {
    const auto where = "photo " + std::to_string(index);
    auto*      tile  = TileByElement(flick, keys[index].file_id_);
    ASSERT_NE(tile, nullptr) << where;
    auto* dots = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_labelDots"));
    auto* name = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_name"));
    ASSERT_NE(dots, nullptr) << where;
    ASSERT_NE(name, nullptr) << where;
    EXPECT_EQ(ShownDotCodes(dots), specs[index].color_labels_) << where;
    if (specs[index].color_labels_ != 0) {
      ExpectNameThenDotsInside(name, dots, tile, where);
      EXPECT_EQ(dots->property("ringColor").value<QColor>(), theme.glassPanelColor()) << where;
    }
    const bool deleted = specs[index].delete_mark_;
    EXPECT_EQ(VisibleCount(tile, QStringLiteral("justifiedThumbnailTile_deleteMark")),
              deleted ? 1 : 0)
        << where;
    EXPECT_EQ(VisibleCount(tile, QStringLiteral("thumbnailDeleteMarkBadge")), deleted ? 1 : 0)
        << where;
    EXPECT_EQ(ColorOf(name), deleted ? theme.textMutedColor() : theme.textColor()) << where;
    if (deleted) {
      auto* scrim = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_deleteMark"));
      auto* photo = FirstItem(tile, QStringLiteral("justifiedThumbnailTile_photo"));
      ASSERT_NE(photo, nullptr) << where;
      EXPECT_EQ(SceneRect(scrim), SceneRect(photo)) << where;
    }
  }
  auto* long_name = FirstItem(TileByElement(flick, keys[2].file_id_),
                              QStringLiteral("justifiedThumbnailTile_name"));
  ASSERT_NE(long_name, nullptr);
  EXPECT_TRUE(long_name->property("truncated").toBool());

  QSignalSpy resets(&backend.library()->model(), &QAbstractItemModel::modelReset);
  auto*      target_tile = TileByElement(flick, keys[4].file_id_);
  ASSERT_NE(target_tile, nullptr);
  const auto content_y = flick->property("contentY").toReal();
  MarkYellowAndDeleted(backend, keys[4]);
  ASSERT_TRUE(WaitUntil([&]() {
    auto* tile = TileByElement(flick, keys[4].file_id_);
    return tile != nullptr &&
           ShownDotCodes(FirstItem(tile, QStringLiteral("justifiedThumbnailTile_labelDots"))) ==
               ColorLabelBit(ColorLabel::kYellow) &&
           VisibleCount(tile, QStringLiteral("justifiedThumbnailTile_deleteMark")) == 1;
  }));
  ProcessEvents(300);
  EXPECT_EQ(resets.count(), 0);
  EXPECT_EQ(TileByElement(flick, keys[4].file_id_), target_tile);
  EXPECT_DOUBLE_EQ(flick->property("contentY").toReal(), content_y);
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// Issue #771, grouped view at 4 columns: the photo cells show the same dots and Delete mark.
TEST_F(AlbumSectionQmlTests, SectionCellsShowTheLabelDotsAndTheDeleteScrim) {
  ApplicationModuleHost backend;
  const auto            specs  = MarkedPhotoSpecs();
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  backend.library()->SetInspectorGrouping(QStringLiteral("camera"), true);
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
  ProcessEvents(1000);

  for (size_t index = 0; index < keys.size(); ++index) {
    const auto where = "photo " + std::to_string(index);
    auto*      cell  = cell_for(keys[index].file_id_);
    ASSERT_NE(cell, nullptr) << where;
    auto* dots = FirstItem(cell, QStringLiteral("albumSectionLabelDots"));
    ASSERT_NE(dots, nullptr) << where;
    EXPECT_EQ(ShownDotCodes(dots), specs[index].color_labels_) << where;
    if (specs[index].color_labels_ != 0) {
      ExpectNameThenDotsInside(FirstItem(cell, QStringLiteral("albumSectionName")), dots, cell,
                               where);
    }
    EXPECT_EQ(VisibleCount(cell, QStringLiteral("albumSectionDeleteMark")),
              specs[index].delete_mark_ ? 1 : 0)
        << where;
  }

  MarkYellowAndDeleted(backend, keys[4]);
  ASSERT_TRUE(WaitUntil([&]() {
    auto* cell = cell_for(keys[4].file_id_);
    return cell != nullptr &&
           ShownDotCodes(FirstItem(cell, QStringLiteral("albumSectionLabelDots"))) ==
               ColorLabelBit(ColorLabel::kYellow) &&
           VisibleCount(cell, QStringLiteral("albumSectionDeleteMark")) == 1;
  }));
  EXPECT_TRUE(AlbumWarnings(view.warnings()).empty()) << Joined(AlbumWarnings(view.warnings()));
}

// Issue #772: By Custom Label sits between By Labels and By Rating. It has one chip per code in
// code order with the user name and the match-set count, then "Deleted". A chip click toggles
// the filter of that chip; Deleted shows only the photos with the Delete mark.
TEST_F(AlbumSectionQmlTests, LibraryOverviewCustomLabelChipsFollowTheNamesAndFilterTheLibrary) {
  UseTestSettings(temp_dir_);
  ApplicationModuleHost backend;
  const auto            specs  = MarkedPhotoSpecs();
  const auto            packed = CreatePopulatedPackedProject(temp_dir_, {}, 0, specs);
  ASSERT_TRUE(packed.has_value());
  ASSERT_TRUE(LoadPackedProject(backend, packed->packed_path_));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  const auto&     keys = packed->images_;

  LoadedComponent inspector(backend, "AlbumInspectorPanel.qml", 300);
  ASSERT_TRUE(WaitUntil([&]() { return inspector.loaded() != nullptr; }))
      << Joined(inspector.warnings());
  auto* window = qobject_cast<QQuickWindow*>(inspector.window());
  ASSERT_NE(window, nullptr);
  ProcessEvents(200);

  // Section order: By Labels, By Custom Label, By Rating.
  auto* labels  = qobject_cast<QQuickItem*>(inspector.Find("labelInspectorSection"));
  auto* custom  = qobject_cast<QQuickItem*>(inspector.Find("customLabelInspectorSection"));
  auto* ratings = qobject_cast<QQuickItem*>(inspector.Find("ratingInspectorSection"));
  ASSERT_NE(labels, nullptr);
  ASSERT_NE(custom, nullptr);
  ASSERT_NE(ratings, nullptr);
  EXPECT_LT(labels->y(), custom->y());
  EXPECT_LT(custom->y(), ratings->y());
  EXPECT_NE(FirstItem(custom, QStringLiteral("customLabelSettingsAction")), nullptr);
  EXPECT_EQ(FirstItem(custom, QStringLiteral("inspectorFieldActions_colorLabel")), nullptr);

  // Expected counts of the match set (all six photos).
  std::vector<int> code_counts(kColorLabelCount, 0);
  int              deleted_count = 0;
  for (const auto& spec : specs) {
    for (int code = 0; code < kColorLabelCount; ++code) {
      code_counts[static_cast<size_t>(code)] +=
          HasColorLabel(spec.color_labels_, static_cast<ColorLabel>(code)) ? 1 : 0;
    }
    deleted_count += spec.delete_mark_ ? 1 : 0;
  }

  const auto chip = [&](int code) {
    return FirstItem(custom, QStringLiteral("colorLabelChip_%1").arg(code));
  };
  auto* deleted_chip = FirstItem(custom, QStringLiteral("colorLabelChipDeleted"));
  ASSERT_NE(deleted_chip, nullptr);
  const QStringList names = backend.library_view()->color_label_names();
  ASSERT_EQ(names.size(), kColorLabelCount);
  for (int code = 0; code < kColorLabelCount; ++code) {
    SCOPED_TRACE(code);
    auto* item = chip(code);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isVisible());
    EXPECT_EQ(item->property("name").toString(), names[code]);
    EXPECT_EQ(item->property("count").toInt(), code_counts[static_cast<size_t>(code)]);
    EXPECT_EQ(item->property("countText").toString(),
              QString::number(code_counts[static_cast<size_t>(code)]));
    // Code order, Deleted last.
    if (code > 0) {
      const auto previous = chip(code - 1)->mapToScene(QPointF(0, 0));
      const auto current  = item->mapToScene(QPointF(0, 0));
      EXPECT_TRUE(current.y() > previous.y() + 0.5 ||
                  (std::abs(current.y() - previous.y()) < 0.5 && current.x() > previous.x()));
    }
  }
  EXPECT_EQ(deleted_chip->property("count").toInt(), deleted_count);
  const auto last_code = chip(kColorLabelCount - 1)->mapToScene(QPointF(0, 0));
  const auto deleted   = deleted_chip->mapToScene(QPointF(0, 0));
  EXPECT_TRUE(deleted.y() > last_code.y() + 0.5 ||
              (std::abs(deleted.y() - last_code.y()) < 0.5 && deleted.x() > last_code.x()));

  // A rename in Settings renames the chip.
  ASSERT_TRUE(backend.library_view()->SetColorLabelName(0, QStringLiteral("Client picks")));
  ASSERT_TRUE(WaitUntil(
      [&]() { return chip(0)->property("name").toString() == QStringLiteral("Client picks"); }));

  // A chip click sets the color label filter: only photos with Orange stay.
  ASSERT_TRUE(QMetaObject::invokeMethod(chip(1), "clicked"));
  ASSERT_TRUE(WaitUntil([&]() { return backend.stats()->StatsFilterColorLabel() == 1; }));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_TRUE(WaitUntil([&]() { return chip(1)->property("selected").toBool(); }));
  std::set<sl_element_id_t> orange;
  for (size_t i = 0; i < specs.size(); ++i) {
    if (HasColorLabel(specs[i].color_labels_, ColorLabel::kOrange)) {
      orange.insert(keys[i].file_id_);
    }
  }
  std::set<sl_element_id_t> shown;
  for (const auto& item : backend.library()->model().items()) {
    shown.insert(item.element_id);
  }
  EXPECT_EQ(shown, orange);

  // A second click clears it.
  ASSERT_TRUE(QMetaObject::invokeMethod(chip(1), "clicked"));
  ASSERT_TRUE(WaitUntil([&]() { return backend.stats()->StatsFilterColorLabel() == -1; }));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_TRUE(
      WaitUntil([&]() { return backend.library()->model().items().size() == specs.size(); }));

  // Deleted shows only the photos with the Delete mark.
  ASSERT_TRUE(QMetaObject::invokeMethod(deleted_chip, "clicked"));
  ASSERT_TRUE(WaitUntil([&]() { return backend.stats()->StatsFilterDeleteMark(); }));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_TRUE(WaitUntil([&]() { return deleted_chip->property("selected").toBool(); }));
  std::set<sl_element_id_t> marked;
  for (size_t i = 0; i < specs.size(); ++i) {
    if (specs[i].delete_mark_) {
      marked.insert(keys[i].file_id_);
    }
  }
  shown.clear();
  for (const auto& item : backend.library()->model().items()) {
    shown.insert(item.element_id);
  }
  EXPECT_EQ(shown, marked);
  EXPECT_FALSE(chip(1)->property("selected").toBool());
  EXPECT_TRUE(AlbumWarnings(inspector.warnings()).empty())
      << Joined(AlbumWarnings(inspector.warnings()));
}

}  // namespace alcedo::ui::test
