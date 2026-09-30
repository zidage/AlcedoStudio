//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_library_service.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "lut_library_test_support.hpp"

namespace alcedo::test {
namespace {

namespace fs  = std::filesystem;
using Service = LutLibraryService;
using LutLibraryServiceTest = LutLibraryRootFixture;

TEST_F(LutLibraryServiceTest, RecursiveScanFindsUnicodeAndUppercaseCubeFiles) {
  const fs::path unicode = library_root_ / fs::path(u8"nested/中文 目录/胶片 look.CUBE");
  WriteBytes(unicode, UserCube("unicode"));
  WriteBytes(library_root_ / "a b" / "space name.cube", UserCube("space"));
  WriteBytes(library_root_ / "top.cube", UserCube("top"));
  WriteBytes(library_root_ / ".downloads" / "partial.cube", UserCube("partial"));
  WriteBytes(library_root_ / "notes.txt", "not a LUT");

  const auto service = StartService();

  EXPECT_EQ(EntryPaths(*service),
            (std::vector<std::string>{"a b/space name.cube",
                                      U8(u8"nested/中文 目录/胶片 look.CUBE"), "top.cube"}));
  EXPECT_TRUE(service->inventory_complete());
  EXPECT_EQ(service->LocateEntry(U8(u8"nested/中文 目录/胶片 look.CUBE")).absolute_path, unicode);
  EXPECT_TRUE(fs::exists(library_root_ / std::string(kLutLibraryInventoryFileName)));
}

TEST_F(LutLibraryServiceTest, EqualBasenamesRemainSeparateEntries) {
  WriteBytes(library_root_ / "kodak" / "look.cube", UserCube("kodak"));
  WriteBytes(library_root_ / "fuji" / "look.cube", UserCube("fuji"));
  const auto service = StartService();

  EXPECT_EQ(EntryPaths(*service), (std::vector<std::string>{"fuji/look.cube", "kodak/look.cube"}));
  EXPECT_EQ(FindEntry(*service, "kodak/look.cube")->header.title, "kodak");
  EXPECT_EQ(FindEntry(*service, "fuji/look.cube")->header.title, "fuji");
  EXPECT_EQ(service->LocateEntry("kodak/look.cube").absolute_path,
            library_root_ / "kodak" / "look.cube");

  // A lookup for a third folder with the same file name must not resolve to either file.
  const Service::Location other = service->LocateEntry("agfa/look.cube");
  EXPECT_EQ(other.status, Service::LocateStatus::kNotInInventory);
  ASSERT_TRUE(WaitUntilIdle(*service));
  ASSERT_TRUE(service->SetFavorite("kodak/look.cube", true) == Service::Status::kOk);
  EXPECT_TRUE(service->IsFavorite("kodak/look.cube"));
  EXPECT_FALSE(service->IsFavorite("fuji/look.cube"));
}

TEST_F(LutLibraryServiceTest, HeaderClaimDoesNotGrantPackageOwnership) {
  const std::string content = "packages/spectral_film_lut/content/abc";
  WriteBytes(library_root_ / "kodak_vision3_250d_5207.cube", OfficialCube());
  WriteBytes(library_root_ / fs::path(content) / "kodak_vision3_250d_5207.cube", OfficialCube());
  WriteBytes(library_root_ / "packages/spectral_film_lut/content/old/kodak_vision3_250d_5207.cube",
             OfficialCube());
  WriteBytes(library_root_ / "packages/spectral_film_lut/installed.json",
             R"({"schema":1,"kind":"alcedo-lut-package-receipt","package_id":"spectral_film_lut",)"
             R"("content_directory":"packages/spectral_film_lut/content/abc"})");
  WriteBytes(library_root_ / "packages/broken/installed.json", "{not json");
  WriteBytes(library_root_ / "packages/broken/content/x/look.cube", UserCube("broken"));

  const auto service = StartService();

  const auto loose   = FindEntry(*service, "kodak_vision3_250d_5207.cube");
  ASSERT_TRUE(loose.has_value());
  EXPECT_TRUE(loose->IsOfficial());
  EXPECT_TRUE(loose->managed_package_id.empty());
  const auto owned = FindEntry(*service, content + "/kodak_vision3_250d_5207.cube");
  ASSERT_TRUE(owned.has_value());
  EXPECT_EQ(owned->managed_package_id, "spectral_film_lut");
  EXPECT_FALSE(
      FindEntry(*service, "packages/spectral_film_lut/content/old/kodak_vision3_250d_5207.cube"));
  EXPECT_FALSE(FindEntry(*service, "packages/broken/content/x/look.cube"));

  std::vector<std::string> replaceable;
  service->ForEachPackageEntry("spectral_film_lut", [&](const LutLibraryEntry& entry) {
    replaceable.push_back(entry.relative_path);
  });
  EXPECT_EQ(replaceable, (std::vector<std::string>{content + "/kodak_vision3_250d_5207.cube"}));
  // The invalid receipt makes the scan incomplete, so no package equality can be claimed.
  EXPECT_FALSE(service->inventory_complete());
  ASSERT_EQ(service->Diagnostics().size(), 1u);
  EXPECT_EQ(service->Diagnostics()[0].kind, LutScanDiagnosticKind::kInvalidPackageReceipt);
}

TEST_F(LutLibraryServiceTest, RefreshDetectsChangedBytesWithUnchangedStamp) {
  const fs::path official = library_root_ / "kodak_vision3_250d_5207.cube";
  WriteBytes(official, OfficialCube());
  WriteBytes(library_root_ / "user.cube", UserCube("user"));
  const auto service = StartService();
  const auto before  = FindEntry(*service, "kodak_vision3_250d_5207.cube");
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(before->sha256.size(), 64u);

  const auto  stamp   = fs::last_write_time(official);
  std::string changed = OfficialCube();
  changed.replace(changed.rfind("1 1 1"), 5, "1 1 0");
  ASSERT_EQ(changed.size(), OfficialCube().size());
  WriteBytes(official, changed);
  fs::last_write_time(official, stamp);

  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  // A second request while the scan runs is merged into it.
  EXPECT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));

  const auto after = FindEntry(*service, "kodak_vision3_250d_5207.cube");
  EXPECT_EQ(after->size, before->size);
  EXPECT_NE(after->sha256, before->sha256);
  EXPECT_EQ(service->LastResult().affected_paths,
            (std::vector<std::string>{"kodak_vision3_250d_5207.cube"}));
  const auto persisted = ReadLutLibraryInventoryFile(library_root_);
  ASSERT_TRUE(persisted);
  EXPECT_EQ(FindLutLibraryEntry(*persisted.inventory, "kodak_vision3_250d_5207.cube")->sha256,
            after->sha256);
}

TEST_F(LutLibraryServiceTest, PersistedInventoryLoadsWithoutRescanAndDamagedFileIsRebuilt) {
  WriteBytes(library_root_ / "one.cube", UserCube("one"));
  StartService().reset();
  WriteBytes(library_root_ / "two.cube", UserCube("two"));

  // Startup reads the persisted inventory; only a user-requested refresh rescans.
  EXPECT_EQ(EntryPaths(*StartService()), (std::vector<std::string>{"one.cube"}));

  WriteBytes(library_root_ / std::string(kLutLibraryInventoryFileName), "{damaged");
  EXPECT_EQ(EntryPaths(*StartService()), (std::vector<std::string>{"one.cube", "two.cube"}));
  EXPECT_TRUE(ReadLutLibraryInventoryFile(library_root_));
}

TEST_F(LutLibraryServiceTest, MissingFileLookupRequestsOneRefreshWithoutRetries) {
  WriteBytes(library_root_ / "kept.cube", UserCube("kept"));
  WriteBytes(library_root_ / "gone.cube", UserCube("gone"));
  const auto service   = StartService();
  int        refreshes = 0;
  QObject::connect(service.get(), &Service::OperationFinished,
                   [&](Service::Operation operation, Service::Status) {
                     if (operation == Service::Operation::kRefresh) ++refreshes;
                   });

  const std::string saved = ReadBytes(library_root_ / "gone.cube");
  fs::remove(library_root_ / "gone.cube");
  EXPECT_EQ(service->LocateEntry("gone.cube").status, Service::LocateStatus::kMissing);
  EXPECT_EQ(service->CurrentOperation(), Service::Operation::kRefresh);
  EXPECT_EQ(service->LocateEntry("gone.cube").status, Service::LocateStatus::kMissing);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(refreshes, 1);

  // Still unresolved after the refresh: no further automatic refresh.
  EXPECT_EQ(service->LocateEntry("gone.cube").status, Service::LocateStatus::kNotInInventory);
  EXPECT_FALSE(service->busy());
  EXPECT_EQ(refreshes, 1);

  WriteBytes(library_root_ / "gone.cube", saved);
  ASSERT_EQ(service->RefreshInventory(), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(refreshes, 2);
  EXPECT_EQ(service->LocateEntry("gone.cube").status, Service::LocateStatus::kFound);
}

TEST_F(LutLibraryServiceTest, ImportRejectsConflictsBeforeCopyingAndKeepsUserFiles) {
  WriteBytes(library_root_ / "user" / "a.cube", UserCube("library a"));
  WriteBytes(library_root_ / "mine" / "keep.cube", UserCube("keep"));
  WriteBytes(base_ / "incoming" / "x.cube", UserCube("x"));
  WriteBytes(base_ / "incoming" / "a.cube", UserCube("incoming a"));
  WriteBytes(base_ / "incoming" / "y.CUBE", UserCube("y"));
  WriteBytes(base_ / "incoming" / "notes.txt", "text");
  const auto service = StartService();

  ASSERT_EQ(service->ImportFiles({base_ / "incoming" / "x.cube", base_ / "incoming" / "a.cube",
                                  base_ / "incoming" / "notes.txt"}),
            Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kConflict);
  EXPECT_FALSE(fs::exists(library_root_ / "user" / "x.cube"));
  EXPECT_EQ(ReadBytes(library_root_ / "user" / "a.cube"), UserCube("library a"));

  ASSERT_EQ(service->ImportFiles({base_ / "incoming" / "x.cube", base_ / "incoming" / "y.CUBE"}),
            Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kOk);
  EXPECT_EQ(service->LastResult().affected_paths,
            (std::vector<std::string>{"user/x.cube", "user/y.CUBE"}));
  EXPECT_EQ(EntryPaths(*service), (std::vector<std::string>{"mine/keep.cube", "user/a.cube",
                                                            "user/x.cube", "user/y.CUBE"}));
  EXPECT_EQ(ReadBytes(library_root_ / "user" / "x.cube"), UserCube("x"));
  EXPECT_EQ(ReadBytes(library_root_ / "mine" / "keep.cube"), UserCube("keep"));
  const auto persisted = ReadLutLibraryInventoryFile(library_root_);
  ASSERT_TRUE(persisted);
  EXPECT_EQ(persisted.inventory->entries.size(), 4u);
}

TEST_F(LutLibraryServiceTest, UseRootIndexesExistingLibraryAndFailuresKeepCurrentRoot) {
  WriteBytes(library_root_ / "first.cube", UserCube("first"));
  const fs::path other = base_ / "other library";
  WriteBytes(other / "deep" / "second.cube", UserCube("second"));
  const auto service = StartService();

  ASSERT_EQ(service->UseRoot(base_ / "does not exist"), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kRootUnavailable);
  EXPECT_EQ(service->Root(), library_root_);

  preferences_->fail_save = true;
  ASSERT_EQ(service->UseRoot(other), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kPreferenceError);
  EXPECT_EQ(service->Root(), library_root_);
  EXPECT_EQ(EntryPaths(*service), (std::vector<std::string>{"first.cube"}));

  preferences_->fail_save = false;
  QSignalSpy root_spy(service.get(), &Service::RootChanged);
  ASSERT_EQ(service->UseRoot(other), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->LastResult().status, Service::Status::kOk);
  EXPECT_EQ(root_spy.count(), 1);
  EXPECT_EQ(service->Root(), other);
  EXPECT_EQ(preferences_->root, other);
  EXPECT_EQ(EntryPaths(*service), (std::vector<std::string>{"deep/second.cube"}));
  EXPECT_EQ(ReadBytes(other / "deep" / "second.cube"), UserCube("second"));
  EXPECT_TRUE(fs::exists(library_root_ / "first.cube"));
}

TEST_F(LutLibraryServiceTest, LegacyFavoritesConvertThroughExactPaths) {
  WriteBytes(library_root_ / "films" / "portra.cube", UserCube("portra"));
  const QString inside =
      QString::fromStdU16String((library_root_ / "films" / "portra.cube").u16string());
  const QString outside          = QStringLiteral("C:/Program Files/Alcedo/LUTs/portra.cube");
  preferences_->legacy_favorites = {inside, outside};

  const auto service             = StartService();

  EXPECT_EQ(service->FavoritePaths(), (std::vector<std::string>{"films/portra.cube"}));
  EXPECT_EQ(preferences_->legacy_favorites, QStringList{outside});
  EXPECT_EQ(ReadLutLibraryUserStateFile(library_root_).state->favorite_paths,
            (std::vector<std::string>{"films/portra.cube"}));
}

TEST_F(LutLibraryServiceTest, MigrationPreservesEntryIdsAndFavorites) {
  WriteBytes(library_root_ / fs::path(u8"nested/片/look.cube"), UserCube("look"));
  WriteBytes(library_root_ / "kodak_vision3_250d_5207.cube", OfficialCube());
  WriteBytes(library_root_ / "notes" / "readme.txt", "user notes");
  WriteBytes(library_root_ / ".downloads" / "partial.7z", "partial");
  const auto service = StartService();
  ASSERT_EQ(service->SetFavorite(U8(u8"nested/片/look.cube"), true), Service::Status::kOk);
  const std::vector<std::string> before = EntryPaths(*service);
  const std::string              hash = FindEntry(*service, "kodak_vision3_250d_5207.cube")->sha256;

  const fs::path                 destination = base_ / U8(u8"new root 新");
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_EQ(service->Root(), destination);
  EXPECT_EQ(preferences_->root, destination);
  EXPECT_EQ(EntryPaths(*service), before);
  EXPECT_EQ(FindEntry(*service, "kodak_vision3_250d_5207.cube")->sha256, hash);
  EXPECT_EQ(service->FavoritePaths(), (std::vector<std::string>{U8(u8"nested/片/look.cube")}));
  EXPECT_EQ(service->PreviousRoots(), (std::vector<std::string>{LutPathToUtf8(library_root_)}));
  EXPECT_EQ(service->LocateEntry(U8(u8"nested/片/look.cube")).status,
            Service::LocateStatus::kFound);
  EXPECT_EQ(ReadBytes(destination / "notes" / "readme.txt"), "user notes");
  EXPECT_FALSE(fs::exists(destination / ".downloads"));

  // After the commit, verified source files are deleted and the record is removed.
  EXPECT_FALSE(fs::exists(library_root_ / "notes" / "readme.txt"));
  EXPECT_FALSE(fs::exists(library_root_ / "nested"));
  EXPECT_FALSE(fs::exists(destination / std::string(kLutMigrationCleanupFileName)));

  // A restarted service reads the same library from the new root.
  auto restarted = StartService();
  EXPECT_EQ(restarted->Root(), destination);
  EXPECT_EQ(EntryPaths(*restarted), before);
  EXPECT_TRUE(restarted->IsFavorite(U8(u8"nested/片/look.cube")));
}

TEST_F(LutLibraryServiceTest, MigrationFailureKeepsPreviousRoot) {
  WriteBytes(library_root_ / "a.cube", UserCube("a"));
  WriteBytes(library_root_ / "sub" / "b.cube", UserCube("b"));
  bool fail_copy      = false;
  bool fail_inventory = false;
  auto io             = LutLibraryFileOperations::Default();
  auto copy           = io.copy_file;
  io.copy_file        = [&](const fs::path& from, const fs::path& to) -> std::string {
    if (fail_copy && from.filename() == "b.cube") return "simulated disk full";
    return copy(from, to);
  };
  io.write_inventory = [&](const fs::path& root, const LutLibraryInventory& inventory) {
    return fail_inventory ? std::string("simulated write failure")
                          : WriteLutLibraryInventoryFile(root, inventory);
  };
  const auto     service          = StartService(io);
  const fs::path destination      = base_ / "destination";

  const auto     expect_unchanged = [&](Service::Status status) {
    ASSERT_TRUE(WaitUntilIdle(*service));
    EXPECT_EQ(service->LastResult().status, status);
    EXPECT_FALSE(service->last_error().isEmpty());
    EXPECT_EQ(service->Root(), library_root_);
    EXPECT_FALSE(preferences_->root.has_value());
    EXPECT_EQ(ReadBytes(library_root_ / "a.cube"), UserCube("a"));
    EXPECT_EQ(ReadBytes(library_root_ / "sub" / "b.cube"), UserCube("b"));
    EXPECT_TRUE(!fs::exists(destination) || fs::is_empty(destination));
    EXPECT_FALSE(fs::exists(base_ / ".destination.alcedo-migration"));
  };

  fail_copy = true;
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  expect_unchanged(Service::Status::kIoError);

  fail_copy      = false;
  fail_inventory = true;
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  expect_unchanged(Service::Status::kIoError);

  fail_inventory          = false;
  preferences_->fail_save = true;
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  expect_unchanged(Service::Status::kPreferenceError);

  // Rejected destinations fail before any copy.
  preferences_->fail_save = false;
  ASSERT_EQ(service->MigrateRoot(library_root_ / "sub" / "inside"), Service::Status::kOk);
  expect_unchanged(Service::Status::kIoError);
  WriteBytes(base_ / "occupied" / "other.txt", "other");
  ASSERT_EQ(service->MigrateRoot(base_ / "occupied"), Service::Status::kOk);
  expect_unchanged(Service::Status::kIoError);
  EXPECT_EQ(ReadBytes(base_ / "occupied" / "other.txt"), "other");

  // The same owner can retry after the failures.
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));
  EXPECT_EQ(service->Root(), destination);
  EXPECT_EQ(ReadBytes(destination / "sub" / "b.cube"), UserCube("b"));
}

TEST_F(LutLibraryServiceTest, SourceCleanupResumesAfterStopFollowingRootSwitch) {
  WriteBytes(library_root_ / "a.cube", UserCube("a"));
  WriteBytes(library_root_ / "sub" / "b.cube", UserCube("b"));
  auto io                    = LutLibraryFileOperations::Default();
  io.remove_file             = [](const fs::path&) { return std::string("simulated stop"); };
  const fs::path destination = base_ / "destination";
  {
    const auto service = StartService(io);
    ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
    ASSERT_TRUE(WaitUntilIdle(*service));
    EXPECT_EQ(service->LastResult().operation, Service::Operation::kSourceCleanup);
    EXPECT_EQ(service->LastResult().status, Service::Status::kIoError);
    EXPECT_EQ(service->Root(), destination);
    EXPECT_TRUE(fs::exists(library_root_ / "a.cube"));
    EXPECT_TRUE(fs::exists(destination / std::string(kLutMigrationCleanupFileName)));
  }

  const auto restarted = StartService();
  EXPECT_EQ(restarted->Root(), destination);
  EXPECT_EQ(EntryPaths(*restarted), (std::vector<std::string>{"a.cube", "sub/b.cube"}));
  EXPECT_EQ(restarted->LastResult().operation, Service::Operation::kSourceCleanup);
  EXPECT_EQ(restarted->LastResult().status, Service::Status::kOk);
  EXPECT_FALSE(fs::exists(library_root_ / "a.cube"));
  EXPECT_FALSE(fs::exists(library_root_ / "sub"));
  EXPECT_FALSE(fs::exists(destination / std::string(kLutMigrationCleanupFileName)));
  EXPECT_EQ(ReadBytes(destination / "sub" / "b.cube"), UserCube("b"));
}

TEST_F(LutLibraryServiceTest, MigrationDoesNotDeleteChangedSourceFile) {
  WriteBytes(library_root_ / "stable.cube", UserCube("stable"));
  WriteBytes(library_root_ / "user" / "changed.cube", UserCube("before"));
  auto io      = LutLibraryFileOperations::Default();
  auto copy    = io.copy_file;
  // An external program edits the source right after it was copied.
  io.copy_file = [&, copy](const fs::path& from, const fs::path& to) -> std::string {
    std::string error = copy(from, to);
    if (error.empty() && from.filename() == "changed.cube") WriteBytes(from, UserCube("edited"));
    return error;
  };
  const auto     service     = StartService(io);
  const fs::path destination = base_ / "destination";
  ASSERT_EQ(service->MigrateRoot(destination), Service::Status::kOk);
  ASSERT_TRUE(WaitUntilIdle(*service));

  EXPECT_EQ(service->Root(), destination);
  EXPECT_EQ(service->LastResult().kept_source_paths,
            (std::vector<std::string>{"user/changed.cube"}));
  EXPECT_EQ(ReadBytes(library_root_ / "user" / "changed.cube"), UserCube("edited"));
  EXPECT_EQ(ReadBytes(destination / "user" / "changed.cube"), UserCube("before"));
  EXPECT_FALSE(fs::exists(library_root_ / "stable.cube"));
}

TEST_F(LutLibraryServiceTest, OpenRootUsesEncodedLocalFileUrl) {
  library_root_      = base_ / U8(u8"LUT #1 100% 胶片 folder");
  const auto service = StartService();

  ASSERT_TRUE(service->OpenRootDirectory());
  ASSERT_EQ(opened_urls_.size(), 1u);
  const QUrl& url = opened_urls_.front();
  EXPECT_TRUE(url.isLocalFile());
  EXPECT_TRUE(url.fragment().isEmpty());
  EXPECT_EQ(QDir::cleanPath(url.toLocalFile()), QDir::cleanPath(service->root_path()));
  const QString encoded = url.toString(QUrl::FullyEncoded);
  EXPECT_TRUE(encoded.startsWith(QStringLiteral("file:///")) ||
              encoded.startsWith(QStringLiteral("file://")));
  EXPECT_TRUE(encoded.contains(QStringLiteral("LUT%20%231%20100%25%20")));
  EXPECT_TRUE(encoded.contains(QStringLiteral("%E8%83%B6%E7%89%87")));
  EXPECT_EQ(url, LutLibraryDirectoryUrl(library_root_));
}

TEST_F(LutLibraryServiceTest, FolderOpenFailureIsVisible) {
  open_result_       = false;
  const auto service = StartService();
  QSignalSpy state_spy(service.get(), &Service::OperationStateChanged);

  EXPECT_FALSE(service->OpenRootDirectory());
  EXPECT_EQ(opened_urls_.size(), 1u);
  EXPECT_EQ(state_spy.count(), 1);
  EXPECT_TRUE(service->last_error().contains(service->root_path()));

  // An unavailable root is reported without dispatching a URL.
  fs::remove_all(library_root_);
  EXPECT_FALSE(service->OpenRootDirectory());
  EXPECT_EQ(opened_urls_.size(), 1u);
  EXPECT_FALSE(service->last_error().isEmpty());
}

}  // namespace
}  // namespace alcedo::test
