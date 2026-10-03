//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QSignalSpy>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "image/image.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

namespace alcedo::ui::test {
namespace {

using RatingTests = ApplicationModuleHostTestFixture;

struct SeededProject {
  std::filesystem::path packed_path_{};
  sl_element_id_t       element_id_ = 0;
  image_id_t            image_id_   = 0;
};

struct SeededImages {
  std::filesystem::path      packed_path_{};
  std::vector<SeededProject> images_{};
};

// Seeds one packed project that holds one image per entry of `ratings`.
auto CreateSeededPackedProjectWithImages(const std::filesystem::path& tempDir,
                                         const std::vector<int>&      ratings)
    -> std::optional<SeededImages> {
  const auto db_path     = tempDir / "rating_seed.db";
  const auto meta_path   = tempDir / "rating_seed.json";
  const auto packed_path = tempDir / "rating_seed.alcd";

  auto project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
  SeededImages seeded;
  seeded.packed_path_ = packed_path;
  for (size_t i = 0; i < ratings.size(); ++i) {
    auto image_handle = project->GetImagePoolService()->CreateAndReturnPinnedEmpty();
    if (!image_handle) {
      return std::nullopt;
    }

    const std::wstring name =
        i == 0 ? std::wstring(L"rated.dng") : L"rated_" + std::to_wstring(i) + L".dng";
    auto image         = image_handle.Get();
    image->image_path_ = tempDir / name;
    image->image_name_ = name;
    image->image_type_ = ImageType::DNG;

    ExifDisplayMetaData metadata;
    metadata.model_         = "Rating Test Camera";
    metadata.date_time_str_ = "2026-05-24 10:00:00";
    metadata.rating_        = ratings[i];
    image->SetExifDisplayMetaData(std::move(metadata));

    auto element = project->GetSleeveService()->Write_NoSync<std::shared_ptr<SleeveElement>>(
        [image](FileSystem& fs) {
          return fs.Create(std::filesystem::path(L"/"), image->image_name_, ElementType::FILE);
        });
    auto file = std::dynamic_pointer_cast<SleeveFile>(element);
    if (!file) {
      return std::nullopt;
    }
    file->SetImage(image);
    seeded.images_.push_back(SeededProject{packed_path, file->element_id_, image->image_id_});
  }

  if (!project->GetSleeveService()->Sync().success_) {
    return std::nullopt;
  }
  const auto image_sync = project->GetImagePoolService()->SyncWithStorage();
  if (!image_sync.failed_images_.empty()) {
    return std::nullopt;
  }
  project->SaveProject(meta_path);

  std::filesystem::path snapshot_path;
  if (!project_pack::BuildTempDbSnapshotPath(&snapshot_path, nullptr)) {
    return std::nullopt;
  }
  if (!project_pack::CreateLiveDbSnapshot(project, snapshot_path, nullptr)) {
    return std::nullopt;
  }
  const bool packed =
      project_pack::WritePackedProject(packed_path, meta_path, snapshot_path, nullptr);
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  if (!packed) {
    return std::nullopt;
  }

  return seeded;
}

auto CreateSeededPackedProject(const std::filesystem::path& tempDir, int rating)
    -> std::optional<SeededProject> {
  auto seeded = CreateSeededPackedProjectWithImages(tempDir, {rating});
  if (!seeded.has_value() || seeded->images_.size() != 1) {
    return std::nullopt;
  }
  return seeded->images_.front();
}

auto LoadPackedProject(ApplicationModuleHost& backend, const std::filesystem::path& packedPath) -> bool {
  QSignalSpy project_spy(backend.project(), &ProjectModule::ProjectChanged);
  if (!backend.project()->LoadProject(PathToQString(packedPath))) {
    return false;
  }
  WaitForSignal(project_spy, 15000);
  ProcessEvents(500);
  return backend.project()->ServiceReady();
}

auto ReadPackedImageRating(const std::filesystem::path& packedPath, image_id_t imageId,
                           const std::filesystem::path& tempDir) -> std::optional<int> {
  const auto workspace = tempDir / "rating_unpack";
  std::filesystem::create_directories(workspace);

  std::filesystem::path unpacked_db;
  std::filesystem::path unpacked_meta;
  if (!project_pack::UnpackProjectToWorkspace(packedPath, workspace, "rating_unpack", &unpacked_db,
                                              &unpacked_meta, nullptr)) {
    return std::nullopt;
  }

  ProjectService project(unpacked_db, unpacked_meta, ProjectOpenMode::kLoadExisting);
  const int      rating = project.GetImagePoolService()->Read<int>(
      imageId,
      [](const std::shared_ptr<Image>& image) { return image ? image->exif_display_.rating_ : 0; });

  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  return rating;
}

auto RatingTargets(const std::vector<SeededProject>& images) -> QVariantList {
  QVariantList targets;
  for (const auto& image : images) {
    targets.push_back(QVariantMap{{"elementId", static_cast<uint>(image.element_id_)},
                                  {"imageId", static_cast<uint>(image.image_id_)}});
  }
  return targets;
}

auto ThumbnailRatingByElement(ApplicationModuleHost& backend, sl_element_id_t elementId) -> int {
  for (const QVariant& row : backend.library()->Thumbnails()) {
    const QVariantMap map = row.toMap();
    if (map.value("elementId").toUInt() == static_cast<uint>(elementId)) {
      return map.value("rating").toInt();
    }
  }
  return -1;
}

TEST_F(RatingTests, SetImageRating_SyncsRatingToPackedDatabase) {
  const auto seeded = CreateSeededPackedProject(temp_dir_, 0);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  const QVariantMap result = backend.images()->SetImageRating(seeded->element_id_, seeded->image_id_, 4);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("rating").toInt(), 4);

  const auto persisted_rating =
      ReadPackedImageRating(seeded->packed_path_, seeded->image_id_, temp_dir_);
  ASSERT_TRUE(persisted_rating.has_value());
  EXPECT_EQ(*persisted_rating, 4);
}

TEST_F(RatingTests, LoadProject_RestoresPersistedImageRating) {
  const auto seeded = CreateSeededPackedProject(temp_dir_, 0);
  ASSERT_TRUE(seeded.has_value());

  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
    const QVariantMap result = backend.images()->SetImageRating(seeded->element_id_, seeded->image_id_, 5);
    ASSERT_TRUE(result.value("success").toBool());
  }

  ApplicationModuleHost reloaded;
  ASSERT_TRUE(LoadPackedProject(reloaded, seeded->packed_path_));
  const QVariantList thumbs = reloaded.library()->Thumbnails();
  ASSERT_EQ(thumbs.size(), 1);
  EXPECT_EQ(thumbs.front().toMap().value("rating").toInt(), 5);
}

TEST_F(RatingTests, GetImageRating_ReflectsCurrentRatingForContextMenuState) {
  const auto seeded = CreateSeededPackedProject(temp_dir_, 2);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  QVariantMap rating_state = backend.images()->GetImageRating(seeded->element_id_, seeded->image_id_);
  ASSERT_TRUE(rating_state.value("success").toBool());
  EXPECT_EQ(rating_state.value("rating").toInt(), 2);

  const QVariantMap set_result = backend.images()->SetImageRating(seeded->element_id_, seeded->image_id_, 3);
  ASSERT_TRUE(set_result.value("success").toBool());

  rating_state = backend.images()->GetImageRating(seeded->element_id_, seeded->image_id_);
  ASSERT_TRUE(rating_state.value("success").toBool());
  EXPECT_EQ(rating_state.value("rating").toInt(), 3);

  const QVariantList thumbs = backend.library()->Thumbnails();
  ASSERT_EQ(thumbs.size(), 1);
  EXPECT_EQ(thumbs.front().toMap().value("rating").toInt(), 3);
}

TEST_F(RatingTests, SetImageRating_UpdatesLoadedThumbnailWithoutModelReset) {
  const auto seeded = CreateSeededPackedProject(temp_dir_, 0);
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  auto* model = qobject_cast<AlbumThumbnailModel*>(backend.library()->ThumbnailModel());
  ASSERT_NE(model, nullptr);
  QSignalSpy model_reset_spy(model, SIGNAL(modelReset()));
  QSignalSpy data_changed_spy(model, &QAbstractItemModel::dataChanged);

  const QVariantMap result = backend.images()->SetImageRating(seeded->element_id_, seeded->image_id_, 4);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();

  EXPECT_EQ(model_reset_spy.count(), 0);
  ASSERT_EQ(data_changed_spy.count(), 1);
  const QList<QVariant> changed_args = data_changed_spy.takeFirst();
  const QModelIndex     top_left = changed_args.at(0).value<QModelIndex>();
  const QModelIndex     bottom_right = changed_args.at(1).value<QModelIndex>();
  const auto            roles = qvariant_cast<QList<int>>(changed_args.at(2));
  EXPECT_EQ(top_left.row(), 0);
  EXPECT_EQ(bottom_right.row(), 0);
  EXPECT_TRUE(roles.contains(AlbumThumbnailModel::Rating));
  ASSERT_EQ(model->count(), 1);
  EXPECT_EQ(model->getItemAt(0).value("rating").toInt(), 4);
}

TEST_F(RatingTests, SetImageRatings_RatesEveryTargetAndSyncsToPackedDatabase) {
  const auto seeded = CreateSeededPackedProjectWithImages(temp_dir_, {0, 1, 2});
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  const QVariantMap result = backend.images()->SetImageRatings(RatingTargets(seeded->images_), 4);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("rating").toInt(), 4);
  EXPECT_EQ(result.value("ratedCount").toInt(), 3);
  EXPECT_EQ(result.value("failedCount").toInt(), 0);
  EXPECT_EQ(result.value("ratedElementIds").toList().size(), 3);

  for (const auto& image : seeded->images_) {
    EXPECT_EQ(ThumbnailRatingByElement(backend, image.element_id_), 4);
    const QVariantMap state = backend.images()->GetImageRating(image.element_id_, image.image_id_);
    EXPECT_EQ(state.value("rating").toInt(), 4);
    const auto persisted = ReadPackedImageRating(seeded->packed_path_, image.image_id_, temp_dir_);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(*persisted, 4);
  }
}

TEST_F(RatingTests, SetImageRatings_LeavesUnselectedImagesUnchanged) {
  const auto seeded = CreateSeededPackedProjectWithImages(temp_dir_, {1, 1, 3});
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  QVariantList targets = RatingTargets({seeded->images_[0], seeded->images_[1]});
  targets.push_back(targets.front());  // A duplicate entry is rated once.
  const QVariantMap result = backend.images()->SetImageRatings(targets, 0);
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("ratedCount").toInt(), 2);

  EXPECT_EQ(ThumbnailRatingByElement(backend, seeded->images_[0].element_id_), 0);
  EXPECT_EQ(ThumbnailRatingByElement(backend, seeded->images_[1].element_id_), 0);
  EXPECT_EQ(ThumbnailRatingByElement(backend, seeded->images_[2].element_id_), 3);
  const auto untouched =
      ReadPackedImageRating(seeded->packed_path_, seeded->images_[2].image_id_, temp_dir_);
  ASSERT_TRUE(untouched.has_value());
  EXPECT_EQ(*untouched, 3);
}

TEST_F(RatingTests, SetImageRatings_RejectsOutOfRangeRatingAndEmptyTargets) {
  const auto seeded = CreateSeededPackedProjectWithImages(temp_dir_, {2, 2});
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  EXPECT_FALSE(backend.images()
                   ->SetImageRatings(RatingTargets(seeded->images_), 6)
                   .value("success")
                   .toBool());
  EXPECT_FALSE(backend.images()->SetImageRatings(QVariantList{}, 3).value("success").toBool());
  for (const auto& image : seeded->images_) {
    EXPECT_EQ(ThumbnailRatingByElement(backend, image.element_id_), 2);
  }
}

TEST_F(RatingTests, SetImageRatings_BlockedWhenAnyTargetHasRatingLock) {
  const auto seeded = CreateSeededPackedProjectWithImages(temp_dir_, {1, 1});
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));
  auto* registry = qobject_cast<BackgroundTaskController*>(backend.background_tasks());
  ASSERT_NE(registry, nullptr);

  BackgroundTaskSnapshot snapshot;
  snapshot.kind_            = BackgroundTaskKind::ImageAnalysis;
  snapshot.state_           = BackgroundTaskState::Running;
  snapshot.title_           = QStringLiteral("analysis");
  snapshot.cancelable_      = false;
  snapshot.shutdown_policy_ = BackgroundTaskShutdownPolicy::CancelAndWait;
  snapshot.locks_.push_back(InteractionLock{InteractionCapability::EditImageRating,
                                            seeded->images_[1].element_id_,
                                            QStringLiteral("This image is being analyzed.")});
  registry->RegisterTask(snapshot);

  const QVariantMap result = backend.images()->SetImageRatings(RatingTargets(seeded->images_), 5);
  EXPECT_FALSE(result.value("success").toBool());
  EXPECT_TRUE(result.value("message").toString().contains(QStringLiteral("analyzed")));
  for (const auto& image : seeded->images_) {
    EXPECT_EQ(ThumbnailRatingByElement(backend, image.element_id_), 1);
  }
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

TEST_F(RatingTests, StartSetImageRatings_SavesOnWorkerAsBackgroundTask) {
  const auto seeded = CreateSeededPackedProjectWithImages(temp_dir_, {0, 1, 2});
  ASSERT_TRUE(seeded.has_value());

  ApplicationModuleHost backend;
  ASSERT_TRUE(LoadPackedProject(backend, seeded->packed_path_));

  QSignalSpy        finished_spy(backend.images(), &ImageController::ImageRatingsFinished);
  const QVariantMap started =
      backend.images()->StartSetImageRatings(RatingTargets(seeded->images_), 5);
  ASSERT_TRUE(started.value("started").toBool()) << started.value("message").toString().toStdString();

  // The call returns before the save finishes; the library already shows the new stars.
  EXPECT_EQ(finished_spy.count(), 0);
  for (const auto& image : seeded->images_) {
    EXPECT_EQ(ThumbnailRatingByElement(backend, image.element_id_), 5);
  }
  const QVariantMap running_task = FindTaskOfKind(backend, QStringLiteral("ratingUpdate"));
  ASSERT_FALSE(running_task.isEmpty());
  EXPECT_EQ(running_task.value("state").toString(), QStringLiteral("running"));

  // A second batch is refused while the first one is still saving.
  EXPECT_FALSE(backend.images()
                   ->StartSetImageRatings(RatingTargets(seeded->images_), 1)
                   .value("started")
                   .toBool());

  ASSERT_TRUE(WaitForSignal(finished_spy, 15000));
  const QVariantMap result = finished_spy.takeFirst().at(0).toMap();
  ASSERT_TRUE(result.value("success").toBool()) << result.value("message").toString().toStdString();
  EXPECT_EQ(result.value("ratedCount").toInt(), 3);
  EXPECT_EQ(FindTaskOfKind(backend, QStringLiteral("ratingUpdate")).value("state").toString(),
            QStringLiteral("succeeded"));

  for (const auto& image : seeded->images_) {
    EXPECT_EQ(ThumbnailRatingByElement(backend, image.element_id_), 5);
    const auto persisted = ReadPackedImageRating(seeded->packed_path_, image.image_id_, temp_dir_);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_EQ(*persisted, 5);
  }
}

TEST_F(RatingTests, ImageMetadata_NormalizesRatingToFiveStarStandard) {
  ExifDisplayMetaData metadata;
  metadata.FromJson({{"Rating", 99}});
  EXPECT_EQ(metadata.rating_, ExifDisplayMetaData::kMaxRating);

  metadata.FromJson({{"Rating", -1}});
  EXPECT_EQ(metadata.rating_, ExifDisplayMetaData::kMinRating);
}

}  // namespace
}  // namespace alcedo::ui::test
