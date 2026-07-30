//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QSignalSpy>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "image/image.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

using RatingTests = ApplicationModuleHostTestFixture;

struct SeededProject {
  std::filesystem::path packed_path_{};
  sl_element_id_t       element_id_ = 0;
  image_id_t            image_id_   = 0;
};

auto CreateSeededPackedProject(const std::filesystem::path& tempDir, int rating)
    -> std::optional<SeededProject> {
  const auto db_path     = tempDir / "rating_seed.db";
  const auto meta_path   = tempDir / "rating_seed.json";
  const auto packed_path = tempDir / "rating_seed.alcd";

  auto project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
  auto image_handle = project->GetImagePoolService()->CreateAndReturnPinnedEmpty();
  if (!image_handle) {
    return std::nullopt;
  }

  auto image         = image_handle.Get();
  image->image_path_ = tempDir / "rated.dng";
  image->image_name_ = L"rated.dng";
  image->image_type_ = ImageType::DNG;

  ExifDisplayMetaData metadata;
  metadata.model_         = "Rating Test Camera";
  metadata.date_time_str_ = "2026-05-24 10:00:00";
  metadata.rating_        = rating;
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

  const sl_element_id_t element_id = file->element_id_;
  const image_id_t      image_id   = image->image_id_;

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

  return SeededProject{packed_path, element_id, image_id};
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

TEST_F(RatingTests, ImageMetadata_NormalizesRatingToFiveStarStandard) {
  ExifDisplayMetaData metadata;
  metadata.FromJson({{"Rating", 99}});
  EXPECT_EQ(metadata.rating_, ExifDisplayMetaData::kMaxRating);

  metadata.FromJson({{"Rating", -1}});
  EXPECT_EQ(metadata.rating_, ExifDisplayMetaData::kMinRating);
}

}  // namespace
}  // namespace alcedo::ui::test
