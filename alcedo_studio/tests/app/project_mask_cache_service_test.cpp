//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>
#include <json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/project_mask_cache_service.hpp"
#include "app/project_package_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo {
namespace {

auto TestRoot(std::string_view name) -> std::filesystem::path {
  auto              root = std::filesystem::path{"build/tmp/project_mask_cache"} / std::string(name);
  std::error_code   ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

auto WriteText(const std::filesystem::path& path, std::string_view text) -> void {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(stream) << path;
  stream << text;
}

auto ReadText(const std::filesystem::path& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

auto MakeRecord(sl_element_id_t image_id, std::string node_id, std::uint8_t fill,
                std::string fingerprint) -> ProjectMaskCacheRecord {
  ProjectMaskCacheRecord record;
  record.identity.image_id     = image_id;
  record.identity.node_id      = NodeId{std::move(node_id)};
  record.output_extent         = {2, 2};
  record.reference_bounds      = {0.0f, 0.0f, 1.0f, 1.0f};
  record.source_grid           = {2, 2};
  record.recipe_fingerprint    = std::move(fingerprint);
  record.producer_id           = "host-test";
  record.pixels                = {fill, fill, fill, fill};
  return record;
}

constexpr auto kStrokeHistory = R"({"strokes":[{"id":"stroke.1","x":8.0,"y":12.0}]})";

}  // namespace

TEST(ProjectMaskCacheServiceTest, ProjectMaskCacheSettingsSurviveSaveAndReopen) {
  const auto root       = TestRoot("settings_reopen");
  const auto db_path    = root / "project.db";
  const auto meta_path  = root / "project.json";
  const auto cache_root = std::filesystem::path{"build/tmp/project_mask_cache"} /
                          "settings_reopen_external_root";
  std::error_code ignored;
  std::filesystem::remove_all(cache_root, ignored);
  std::filesystem::create_directories(cache_root);
  std::string uuid;
  std::uint64_t revision = 0;
  {
    ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
    uuid = project.GetProjectUUID();
    project.SaveProject(meta_path);
    std::string error;
    ASSERT_TRUE(project.SetMaskCacheRoot(cache_root, 0, &error)) << error;
    revision = project.GetMaskCacheSettings().settings_revision;
    ASSERT_TRUE(project.SetMaskCacheRetention(ProjectMaskCacheRetention::DeleteOnProjectClose,
                                              revision, &error))
        << error;
    revision = project.GetMaskCacheSettings().settings_revision;
    ASSERT_TRUE(project.FlushMaskCacheWrites(&error)) << error;
  }

  {
    QString pack_error;
    const auto packed = root / "project.alcd";
    ASSERT_TRUE(ProjectPackageService{}.WritePackedProject(packed, meta_path, db_path, &pack_error))
        << pack_error.toStdString();
  }

  {
    ProjectService project(db_path, meta_path, ProjectOpenMode::kLoadExisting);
    EXPECT_EQ(project.GetProjectUUID(), uuid);
    const auto settings = project.GetMaskCacheSettings();
    EXPECT_EQ(ResolveProjectMaskCacheChosenRoot(settings, meta_path),
              std::filesystem::absolute(cache_root).lexically_normal());
    EXPECT_EQ(settings.retention, ProjectMaskCacheRetention::DeleteOnProjectClose);
    EXPECT_EQ(settings.settings_revision, revision);
    EXPECT_FALSE(settings.close_cleanup_pending);
  }

  const auto unpack_dir = root / "unpacked";
  std::filesystem::create_directories(unpack_dir);
  std::filesystem::path unpacked_db;
  std::filesystem::path unpacked_meta;
  QString               unpack_error;
  ASSERT_TRUE(ProjectPackageService{}.UnpackProjectToWorkspace(
      root / "project.alcd", unpack_dir, QStringLiteral("reopen_pack"), &unpacked_db, &unpacked_meta,
      &unpack_error))
      << unpack_error.toStdString();
  {
    ProjectService project(unpacked_db, unpacked_meta, ProjectOpenMode::kLoadExisting);
    EXPECT_EQ(project.GetProjectUUID(), uuid);
    const auto settings = project.GetMaskCacheSettings();
    EXPECT_EQ(settings.retention, ProjectMaskCacheRetention::DeleteOnProjectClose);
    EXPECT_EQ(ResolveProjectMaskCacheChosenRoot(settings, unpacked_meta),
              std::filesystem::absolute(cache_root).lexically_normal());
  }
}

TEST(ProjectMaskCacheServiceTest, ThousandStrokesKeepOneRasterSlot) {
  const auto root = TestRoot("thousand_strokes");
  const auto uuid = "11111111-1111-1111-1111-111111111111";
  ProjectMaskCacheService service(uuid, root);
  const auto identity = ProjectMaskCacheSlotIdentity{7, NodeId{"grade.primary"}};
  std::string error;
  for (int i = 0; i < 1000; ++i) {
    auto record = MakeRecord(identity.image_id, "grade.primary", static_cast<std::uint8_t>(i % 250),
                             "fp-" + std::to_string(i));
    ASSERT_TRUE(service.EnqueueSettledWrite(std::move(record), &error)) << error;
  }
  ASSERT_TRUE(service.FlushPendingWrites(&error)) << error;
  EXPECT_EQ(service.CountPublishedSlots(), 1u);
  const auto reader = service.TryAcquireReader(identity, "fp-999", "host-test", &error);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Get().pixels, (std::vector<std::uint8_t>{249, 249, 249, 249}));
  const auto ns = ProjectMaskCacheNamespaceDirectory(root, uuid);
  std::size_t cache_files = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(ns)) {
    if (entry.is_regular_file() && entry.path().extension() == kProjectMaskCacheFileExtension) {
      ++cache_files;
    }
  }
  EXPECT_EQ(cache_files, 1u);
}

TEST(ProjectMaskCacheServiceTest, CacheClearCannotDeleteAnotherProjectsFiles) {
  const auto root   = TestRoot("clear_isolation");
  const auto uuid_a = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
  const auto uuid_b = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
  ProjectMaskCacheService service_a(uuid_a, root);
  ProjectMaskCacheService service_b(uuid_b, root);
  std::string             error;
  ASSERT_TRUE(service_a.EnqueueSettledWrite(MakeRecord(1, "grade.primary", 11, "fp-a"), &error))
      << error;
  ASSERT_TRUE(service_b.EnqueueSettledWrite(MakeRecord(1, "grade.primary", 22, "fp-b"), &error))
      << error;
  ASSERT_TRUE(service_a.FlushPendingWrites(&error)) << error;
  ASSERT_TRUE(service_b.FlushPendingWrites(&error)) << error;

  const auto foreign_mask = root / "legacy.r8mask";
  WriteText(foreign_mask, "ALCR8MSK-not-cache");
  const auto owned_mask =
      ProjectMaskCacheNamespaceDirectory(root, uuid_a) / "1" / "legacy.r8mask";
  WriteText(owned_mask, "ALCR8MSK-inside-namespace");
  const auto outside = root / "outside-secret.bin";
  WriteText(outside, "do-not-delete");
  const auto escape_link = ProjectMaskCacheNamespaceDirectory(root, uuid_a) / "escape.lnk";
  std::error_code link_error;
  std::filesystem::create_symlink(outside, escape_link, link_error);

  ASSERT_TRUE(service_a.ClearOwnedCache(&error)) << error;
  EXPECT_EQ(service_a.CountPublishedSlots(), 0u);
  EXPECT_EQ(service_b.CountPublishedSlots(), 1u);
  EXPECT_TRUE(std::filesystem::exists(foreign_mask));
  EXPECT_TRUE(std::filesystem::exists(owned_mask));
  EXPECT_TRUE(std::filesystem::exists(outside));
  EXPECT_EQ(ReadText(outside), "do-not-delete");
  const auto b_reader =
      service_b.TryAcquireReader({1, NodeId{"grade.primary"}}, "fp-b", "host-test", &error);
  ASSERT_TRUE(b_reader.has_value());
  EXPECT_EQ(b_reader->Get().pixels[0], 22);
}

TEST(ProjectMaskCacheServiceTest, OldWriterCannotRecreateClearedCache) {
  const auto root = TestRoot("old_writer");
  const auto uuid = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  ProjectMaskCacheService service(uuid, root);
  const auto identity = ProjectMaskCacheSlotIdentity{3, NodeId{"grade.primary"}};
  const auto gen      = service.StorageGeneration();
  std::mutex              mutex;
  std::condition_variable ready;
  bool                    entered = false;
  service.SetPublishHookForTesting([&](const std::filesystem::path&, const std::filesystem::path&) {
    {
      std::lock_guard lock(mutex);
      entered = true;
      ready.notify_all();
    }
    while (service.StorageGeneration() == gen) {
      std::this_thread::yield();
    }
    return true;
  });
  std::string error;
  ASSERT_TRUE(service.EnqueueSettledWrite(MakeRecord(3, "grade.primary", 33, "fp-old"), &error))
      << error;
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }))
        << "writer never reached the publish hook";
  }
  ASSERT_TRUE(service.ClearOwnedCache(&error)) << error;
  EXPECT_EQ(service.CountPublishedSlots(), 0u);
  EXPECT_FALSE(std::filesystem::exists(service.PublishedSlotPath(identity)));
  service.SetPublishHookForTesting({});
  std::this_thread::yield();
  EXPECT_EQ(service.CountPublishedSlots(), 0u);
}

TEST(ProjectMaskCacheServiceTest, RootChangeFailurePreservesOldSetting) {
  const auto root      = TestRoot("root_change_failure");
  const auto db_path   = root / "project.db";
  const auto meta_path = root / "project.json";
  const auto cache_a   = root / "cache-a";
  std::filesystem::create_directories(cache_a);
  ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
  project.SaveProject(meta_path);
  std::string error;
  ASSERT_TRUE(project.SetMaskCacheRoot(cache_a, 0, &error)) << error;
  auto* cache = project.GetMaskCacheService();
  ASSERT_NE(cache, nullptr);
  ASSERT_TRUE(cache->EnqueueSettledWrite(MakeRecord(4, "grade.primary", 44, "fp-a"), &error))
      << error;
  ASSERT_TRUE(cache->FlushPendingWrites(&error)) << error;
  const auto published = cache->PublishedSlotPath({4, NodeId{"grade.primary"}});
  ASSERT_TRUE(std::filesystem::exists(published));

  const auto blocker = root / "not-a-directory";
  WriteText(blocker, "file");
  const auto bad_root = blocker / "cache-b";
  const auto before   = project.GetMaskCacheSettings();
  EXPECT_FALSE(project.SetMaskCacheRoot(bad_root, before.settings_revision, &error));
  EXPECT_FALSE(error.empty());
  const auto after = project.GetMaskCacheSettings();
  EXPECT_EQ(after.settings_revision, before.settings_revision);
  EXPECT_EQ(ResolveProjectMaskCacheChosenRoot(after, meta_path),
            std::filesystem::absolute(cache_a).lexically_normal());
  EXPECT_TRUE(std::filesystem::exists(published));
  EXPECT_EQ(project.GetMaskCacheService()->CountPublishedSlots(), 1u);
}

TEST(ProjectMaskCacheServiceTest, CacheWriteFailureDoesNotLoseStrokeHistory) {
  const auto root      = TestRoot("write_failure_history");
  const auto db_path   = root / "project.db";
  const auto meta_path = root / "project.json";
  const auto strokes   = root / "strokes.json";
  WriteText(strokes, kStrokeHistory);
  ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
  project.SaveProject(meta_path);
  auto* cache = project.GetMaskCacheService();
  ASSERT_NE(cache, nullptr);
  cache->SetPublishHookForTesting(
      [](const std::filesystem::path&, const std::filesystem::path&) { return false; });
  std::string error;
  ASSERT_TRUE(cache->EnqueueSettledWrite(MakeRecord(5, "grade.primary", 55, "fp-fail"), &error))
      << error;
  EXPECT_FALSE(cache->FlushPendingWrites(&error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(ReadText(strokes), kStrokeHistory);
  EXPECT_TRUE(std::filesystem::exists(meta_path));
  std::ifstream meta(meta_path);
  nlohmann::json metadata;
  meta >> metadata;
  EXPECT_EQ(metadata.at("project_uuid").get<std::string>(), project.GetProjectUUID());
  EXPECT_TRUE(metadata.contains("mask_cache"));
  EXPECT_TRUE(cache->Usage().has_dirty_slot);
  EXPECT_EQ(cache->CountPublishedSlots(), 0u);
  EXPECT_EQ(ReadText(strokes), kStrokeHistory);
}

TEST(ProjectMaskCacheServiceTest, CloseCleanupRunsAfterParameterSaveAndReadersFinish) {
  const auto root      = TestRoot("close_cleanup");
  const auto db_path   = root / "project.db";
  const auto meta_path = root / "project.json";
  const auto strokes   = root / "strokes.json";
  WriteText(strokes, kStrokeHistory);
  ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
  project.SaveProject(meta_path);
  std::string error;
  ASSERT_TRUE(project.SetMaskCacheRetention(ProjectMaskCacheRetention::DeleteOnProjectClose, 0,
                                            &error))
      << error;
  auto* cache = project.GetMaskCacheService();
  ASSERT_NE(cache, nullptr);
  const auto identity = ProjectMaskCacheSlotIdentity{9, NodeId{"grade.primary"}};
  ASSERT_TRUE(cache->EnqueueSettledWrite(MakeRecord(9, "grade.primary", 99, "fp-close"), &error))
      << error;
  ASSERT_TRUE(cache->FlushPendingWrites(&error)) << error;
  const auto published = cache->PublishedSlotPath(identity);
  ASSERT_TRUE(std::filesystem::exists(published));
  auto reader = cache->TryAcquireReader(identity, "fp-close", "host-test", &error);
  ASSERT_TRUE(reader.has_value());
  const auto generation = cache->StorageGeneration();
  std::string close_error;
  std::atomic<bool> close_done{false};
  std::thread closer([&] {
    (void)project.CloseAfterSuccessfulSave(&close_error);
    close_done = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (cache->StorageGeneration() == generation) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "close did not fence writers";
    std::this_thread::yield();
  }
  EXPECT_FALSE(close_done.load());
  EXPECT_TRUE(std::filesystem::exists(published));
  EXPECT_EQ(ReadText(strokes), kStrokeHistory);
  reader.reset();
  closer.join();
  EXPECT_TRUE(close_error.empty()) << close_error;
  EXPECT_FALSE(std::filesystem::exists(published));
  EXPECT_EQ(cache->CountPublishedSlots(), 0u);
  EXPECT_EQ(ReadText(strokes), kStrokeHistory);
  EXPECT_EQ(project.GetMaskCacheSettings().retention,
            ProjectMaskCacheRetention::DeleteOnProjectClose);
  EXPECT_FALSE(project.GetMaskCacheSettings().close_cleanup_pending);
}

}  // namespace alcedo

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
