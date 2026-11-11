//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Library color labels and the Delete mark (#769, tracking #768): batch writes through
// ImageController, the library rows, the Inspector counts and filters, and persistence in the
// packed project. Every case loads a persisted multi-photo project into the real
// ApplicationModuleHost.

#include <QSignalSpy>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/saved_project_invariants.hpp"

namespace alcedo::ui::test {
namespace {

class LibraryMarksTests : public ApplicationModuleHostTestFixture {
 protected:
  /// Checks the storage invariants of each project package that the test saved (#805).
  void TearDown() override {
    ExpectSavedProjectInvariants(temp_dir_);
    ApplicationModuleHostTestFixture::TearDown();
  }
};

constexpr ColorLabelMask kRed    = ColorLabelBit(ColorLabel::kRed);
constexpr ColorLabelMask kGreen  = ColorLabelBit(ColorLabel::kGreen);
constexpr ColorLabelMask kBlue   = ColorLabelBit(ColorLabel::kBlue);
constexpr ColorLabelMask kGray   = ColorLabelBit(ColorLabel::kGray);

struct StoredMarks {
  ColorLabelMask color_labels_ = 0;
  bool           delete_mark_  = false;
  int            rating_       = 0;
};

auto MarkSpecs(const std::vector<std::pair<ColorLabelMask, bool>>& marks)
    -> std::vector<LibraryPhotoSpec> {
  std::vector<LibraryPhotoSpec> specs;
  for (const auto& [color_labels, delete_mark] : marks) {
    LibraryPhotoSpec spec;
    spec.color_labels_ = color_labels;
    spec.delete_mark_  = delete_mark;
    specs.push_back(spec);
  }
  return specs;
}

auto Targets(const std::vector<PopulatedProject::ImageKey>& images) -> QVariantList {
  QVariantList targets;
  for (const auto& image : images) {
    targets.push_back(QVariantMap{{"elementId", static_cast<uint>(image.file_id_)},
                                  {"imageId", static_cast<uint>(image.image_id_)}});
  }
  return targets;
}

/// Marks of the loaded library row of @p element_id, or empty when the row is not loaded.
auto RowMarks(ApplicationModuleHost& backend, sl_element_id_t element_id)
    -> std::optional<StoredMarks> {
  for (const auto& item : backend.library()->model().items()) {
    if (item.element_id == element_id) {
      return StoredMarks{item.color_labels, item.delete_mark, item.rating};
    }
  }
  return std::nullopt;
}

/// Marks of @p image_id in the saved package at @p packed_path.
auto PackedMarks(const std::filesystem::path& packed_path, image_id_t image_id,
                 const std::filesystem::path& temp_dir) -> std::optional<StoredMarks> {
  const auto workspace = temp_dir / "marks_unpack";
  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  std::filesystem::create_directories(workspace);
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  if (!project_pack::UnpackProjectToWorkspace(packed_path, workspace, "marks_unpack", &db_path,
                                              &meta_path, nullptr)) {
    return std::nullopt;
  }
  std::optional<StoredMarks> marks;
  {
    ProjectService project(db_path, meta_path, ProjectOpenMode::kLoadExisting);
    marks = project.GetImagePoolService()->Read<StoredMarks>(
        image_id, [](const std::shared_ptr<Image>& image) {
          return StoredMarks{image->color_labels_, image->delete_mark_,
                             image->exif_display_.rating_};
        });
  }
  std::filesystem::remove_all(workspace, ec);
  return marks;
}

auto ModelIds(ApplicationModuleHost& backend) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& item : backend.library()->model().items()) {
    ids.push_back(item.element_id);
  }
  return ids;
}

auto ModelIdSet(ApplicationModuleHost& backend) -> std::set<sl_element_id_t> {
  const auto ids = ModelIds(backend);
  return {ids.begin(), ids.end()};
}

auto Counts(StatsEngine& stats) -> std::vector<int> {
  std::vector<int> counts;
  for (const QVariant& count : stats.ColorLabelCounts()) {
    counts.push_back(count.toInt());
  }
  return counts;
}

auto FindTaskOfKind(ApplicationModuleHost& backend, const QString& kind) -> QVariantMap {
  auto* registry = qobject_cast<BackgroundTaskController*>(backend.background_tasks());
  if (registry == nullptr) {
    return {};
  }
  for (const QVariant& task : registry->Tasks()) {
    const QVariantMap map = task.toMap();
    if (map.value("kind").toString() == kind) {
      return map;
    }
  }
  return {};
}

TEST_F(LibraryMarksTests, SetImageColorLabelKeepsTheOtherCodesOfEachImage) {
  const auto seeded = CreatePopulatedPackedProject(
      temp_dir_, {}, 0, MarkSpecs({{kRed, false}, {kBlue | kGray, false}, {0, false}}));
  ASSERT_TRUE(seeded.has_value());
  const auto& images = seeded->images_;

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  auto& model = backend.library()->model();
  QSignalSpy model_reset_spy(&model, SIGNAL(modelReset()));
  QSignalSpy data_changed_spy(&model, &QAbstractItemModel::dataChanged);

  const QVariantList first_two = Targets({images[0], images[1]});
  QVariantMap result = backend.images()->SetImageColorLabel(
      first_two, static_cast<int>(ColorLabel::kGreen), true);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("markedCount").toInt(), 2);
  EXPECT_EQ(result.value("failedCount").toInt(), 0);

  // The rows change in place: no model reset, and the changed roles are the marks.
  EXPECT_EQ(model_reset_spy.count(), 0);
  ASSERT_EQ(data_changed_spy.count(), 2);
  const auto roles = qvariant_cast<QList<int>>(data_changed_spy.front().at(2));
  EXPECT_TRUE(roles.contains(AlbumThumbnailModel::ColorLabels));
  EXPECT_TRUE(roles.contains(AlbumThumbnailModel::DeleteMark));

  EXPECT_EQ(RowMarks(backend, images[0].file_id_)->color_labels_, kRed | kGreen);
  EXPECT_EQ(RowMarks(backend, images[1].file_id_)->color_labels_, kBlue | kGray | kGreen);
  EXPECT_EQ(RowMarks(backend, images[2].file_id_)->color_labels_, 0);

  // Clearing Red removes only Red, and only where it was set.
  result = backend.images()->SetImageColorLabel(first_two, static_cast<int>(ColorLabel::kRed),
                                                false);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(RowMarks(backend, images[0].file_id_)->color_labels_, kGreen);
  EXPECT_EQ(RowMarks(backend, images[1].file_id_)->color_labels_, kBlue | kGray | kGreen);

  const auto first  = PackedMarks(seeded->packed_path_, images[0].image_id_, temp_dir_);
  const auto second = PackedMarks(seeded->packed_path_, images[1].image_id_, temp_dir_);
  const auto third  = PackedMarks(seeded->packed_path_, images[2].image_id_, temp_dir_);
  ASSERT_TRUE(first.has_value() && second.has_value() && third.has_value());
  EXPECT_EQ(first->color_labels_, kGreen);
  EXPECT_EQ(second->color_labels_, kBlue | kGray | kGreen);
  EXPECT_EQ(third->color_labels_, 0);
}

TEST_F(LibraryMarksTests, SetImageDeleteMarkPersistsAndARatingWriteKeepsTheMarks) {
  const auto seeded =
      CreatePopulatedPackedProject(temp_dir_, {}, 0, MarkSpecs({{kBlue, false}, {0, false}}));
  ASSERT_TRUE(seeded.has_value());
  const auto& images = seeded->images_;

  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
    const QVariantMap result = backend.images()->SetImageDeleteMark(Targets({images[0]}), true);
    ASSERT_TRUE(result.value("success").toBool())
        << result.value("message").toString().toStdString();
    EXPECT_TRUE(RowMarks(backend, images[0].file_id_)->delete_mark_);
    EXPECT_FALSE(RowMarks(backend, images[1].file_id_)->delete_mark_);

    // A rating writes the whole Image row; the marks stay.
    const QVariantMap rated =
        backend.images()->SetImageRating(images[0].file_id_, images[0].image_id_, 3);
    ASSERT_TRUE(rated.value("success").toBool());
  }

  const auto stored = PackedMarks(seeded->packed_path_, images[0].image_id_, temp_dir_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->color_labels_, kBlue);
  EXPECT_TRUE(stored->delete_mark_);
  EXPECT_EQ(stored->rating_, 3);

  // A reopened project shows the marks on its rows.
  ApplicationModuleHost reloaded;
  ASSERT_TRUE(LoadPackedProject(reloaded, seeded->packed_path_));
  const auto row = RowMarks(reloaded, images[0].file_id_);
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->color_labels_, kBlue);
  EXPECT_TRUE(row->delete_mark_);
}

TEST_F(LibraryMarksTests, InspectorCountsAndFiltersFollowTheMarks) {
  const auto seeded = CreatePopulatedPackedProject(
      temp_dir_, {}, 0,
      MarkSpecs({{kRed, false}, {kRed | kBlue, true}, {kBlue, false}, {0, true}, {0, false}}));
  ASSERT_TRUE(seeded.has_value());
  const auto& images = seeded->images_;

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  auto* stats = backend.stats();
  EXPECT_EQ(Counts(*stats), (std::vector<int>{2, 0, 0, 0, 2, 0, 0}));
  EXPECT_EQ(stats->DeleteMarkCount(), 2);
  EXPECT_EQ(RowMarks(backend, images[1].file_id_)->color_labels_, kRed | kBlue);
  EXPECT_TRUE(RowMarks(backend, images[1].file_id_)->delete_mark_);

  // Blue: the photos with Blue, the counts of that match set, and the filter state.
  stats->ToggleStatsFilter(QStringLiteral("colorLabel"),
                           QString::number(static_cast<int>(ColorLabel::kBlue)));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(stats->StatsFilterColorLabel(), static_cast<int>(ColorLabel::kBlue));
  EXPECT_EQ(ModelIdSet(backend),
            (std::set<sl_element_id_t>{images[1].file_id_, images[2].file_id_}));
  EXPECT_EQ(Counts(*stats), (std::vector<int>{1, 0, 0, 0, 2, 0, 0}));
  EXPECT_EQ(stats->DeleteMarkCount(), 1);

  // Blue and Deleted together.
  stats->ToggleStatsFilter(QStringLiteral("deleted"), QString{});
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_TRUE(stats->StatsFilterDeleteMark());
  EXPECT_EQ(ModelIds(backend), (std::vector<sl_element_id_t>{images[1].file_id_}));

  // A mark write that changes the filtered set refreshes the library.
  const QVariantMap result =
      backend.images()->SetImageDeleteMark(Targets({images[1]}), false);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_TRUE(ModelIds(backend).empty());

  // Deleted alone; an unknown code leaves the filter unchanged.
  stats->ToggleStatsFilter(QStringLiteral("colorLabel"),
                           QString::number(static_cast<int>(ColorLabel::kBlue)));
  stats->ToggleStatsFilter(QStringLiteral("colorLabel"), QStringLiteral("7"));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(stats->StatsFilterColorLabel(), -1);
  EXPECT_EQ(ModelIds(backend), (std::vector<sl_element_id_t>{images[3].file_id_}));

  stats->ClearStatsFilter();
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_FALSE(stats->StatsFilterDeleteMark());
  EXPECT_EQ(ModelIds(backend).size(), images.size());
  EXPECT_EQ(stats->DeleteMarkCount(), 1);
}

TEST_F(LibraryMarksTests, AMarkWriteWithoutAMarkFilterKeepsTheRowsAndUpdatesTheCounts) {
  const auto seeded =
      CreatePopulatedPackedProject(temp_dir_, {}, 0, MarkSpecs({{0, false}, {0, false}}));
  ASSERT_TRUE(seeded.has_value());
  const auto& images = seeded->images_;

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  auto&      model = backend.library()->model();
  QSignalSpy model_reset_spy(&model, SIGNAL(modelReset()));
  QSignalSpy stats_spy(backend.stats(), &StatsEngine::StatsChanged);

  const QVariantMap result = backend.images()->SetImageColorLabel(
      Targets(images), static_cast<int>(ColorLabel::kGray), true);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  ASSERT_TRUE(WaitForSignal(stats_spy, 15000));
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  EXPECT_EQ(model_reset_spy.count(), 0);
  EXPECT_EQ(Counts(*backend.stats()), (std::vector<int>{0, 0, 0, 0, 0, 0, 2}));
}

TEST_F(LibraryMarksTests, StartSetImageColorLabelSavesOnWorkerAsBackgroundTask) {
  const auto seeded = CreatePopulatedPackedProject(
      temp_dir_, {}, 0, MarkSpecs({{0, false}, {kRed, false}, {0, true}}));
  ASSERT_TRUE(seeded.has_value());
  const auto& images = seeded->images_;

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  QSignalSpy        finished_spy(backend.images(), &ImageController::ImageMarksFinished);
  const QVariantMap started = backend.images()->StartSetImageColorLabel(
      Targets(images), static_cast<int>(ColorLabel::kYellow), true);
  ASSERT_TRUE(started.value("started").toBool())
      << started.value("message").toString().toStdString();

  // The call returns before the save; the rows already show the new code.
  EXPECT_EQ(finished_spy.count(), 0);
  const ColorLabelMask yellow = ColorLabelBit(ColorLabel::kYellow);
  EXPECT_EQ(RowMarks(backend, images[0].file_id_)->color_labels_, yellow);
  EXPECT_EQ(RowMarks(backend, images[1].file_id_)->color_labels_, kRed | yellow);
  const QVariantMap running_task = FindTaskOfKind(backend, QStringLiteral("libraryMarkUpdate"));
  ASSERT_FALSE(running_task.isEmpty());
  EXPECT_EQ(running_task.value("state").toString(), QStringLiteral("running"));

  // A second batch is refused while the first one is still saving.
  EXPECT_FALSE(backend.images()
                   ->StartSetImageDeleteMark(Targets(images), true)
                   .value("started")
                   .toBool());

  ASSERT_TRUE(WaitForSignal(finished_spy, 15000));
  const QVariantMap result = finished_spy.takeFirst().at(0).toMap();
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("markedCount").toInt(), 3);
  EXPECT_EQ(FindTaskOfKind(backend, QStringLiteral("libraryMarkUpdate")).value("state").toString(),
            QStringLiteral("succeeded"));

  const auto stored = PackedMarks(seeded->packed_path_, images[2].image_id_, temp_dir_);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->color_labels_, yellow);
  EXPECT_TRUE(stored->delete_mark_);
}

TEST_F(LibraryMarksTests, MarkBatchesRefuseUnknownCodesEmptyTargetsAndLockedImages) {
  const auto seeded =
      CreatePopulatedPackedProject(temp_dir_, {}, 0, MarkSpecs({{kRed, false}, {kRed, false}}));
  ASSERT_TRUE(seeded.has_value());
  const auto& images = seeded->images_;

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  auto* controller = backend.images();
  EXPECT_FALSE(controller->SetImageColorLabel(Targets(images), 7, true).value("success").toBool());
  EXPECT_FALSE(controller->SetImageColorLabel(Targets(images), -1, true).value("success").toBool());
  EXPECT_FALSE(controller->SetImageDeleteMark(QVariantList{}, true).value("success").toBool());

  // A delete of one target holds its EditImageMarks lock: the whole batch is refused.
  auto* registry = qobject_cast<BackgroundTaskController*>(backend.background_tasks());
  ASSERT_NE(registry, nullptr);
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_            = BackgroundTaskKind::ImageDelete;
  snapshot.state_           = BackgroundTaskState::Running;
  snapshot.title_           = QStringLiteral("delete");
  snapshot.cancelable_      = false;
  snapshot.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.locks_.push_back(InteractionLock{InteractionCapability::EditImageMarks,
                                            images[1].file_id_,
                                            QStringLiteral("This image is being deleted.")});
  registry->RegisterTask(snapshot);

  const QVariantMap result = controller->SetImageDeleteMark(Targets(images), true);
  EXPECT_FALSE(result.value("success").toBool());
  EXPECT_TRUE(result.value("message").toString().contains(QStringLiteral("deleted")));
  for (const auto& image : images) {
    EXPECT_EQ(RowMarks(backend, image.file_id_)->color_labels_, kRed);
    EXPECT_FALSE(RowMarks(backend, image.file_id_)->delete_mark_);
  }
}

}  // namespace
}  // namespace alcedo::ui::test
