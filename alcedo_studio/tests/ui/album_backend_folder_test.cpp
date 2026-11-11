//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_folder_test.cpp
/// @brief Folder CRUD & selection tests for ApplicationModuleHost.
///
/// Covers: folder creation, deletion, selection (valid/invalid IDs), and
/// signal emission.

#include <QSignalSpy>

#include "app/album_browse_service.hpp"
#include "ui/album_backend_populated_project_fixture.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/saved_project_invariants.hpp"

namespace alcedo::ui::test {
namespace {

class FolderTests : public ApplicationModuleHostTestFixture {
 protected:
  /// Checks the storage invariants of each project package that the test saved (#805).
  void TearDown() override {
    ExpectSavedProjectInvariants(temp_dir_);
    ApplicationModuleHostTestFixture::TearDown();
  }
};

// ── Create folder — signal emitted, folder visible ─────────────────────────

TEST_F(FolderTests, CreateFolder_ValidName_EmitsFoldersChanged) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  QSignalSpy foldersChangedSpy(backend.folders(), &FolderController::FoldersChanged);

  backend.folders()->CreateFolder("TestSubFolder");
  ProcessEvents(500);

  EXPECT_FALSE(foldersChangedSpy.isEmpty())
      << "FoldersChanged should fire after CreateFolder";

  // Verify the folder appears in the folder list.
  const QVariantList folders = backend.folders()->Folders();
  bool found = false;
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == "TestSubFolder") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Created folder should appear in Folders() list. "
                     << "Got " << folders.size() << " folders.";
}

// ── Select folder — valid ID ───────────────────────────────────────────────

TEST_F(FolderTests, SelectFolder_ValidId_EmitsFolderSelectionChanged) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  // Create a subfolder first.
  backend.folders()->CreateFolder("SubFolderA");
  ProcessEvents(500);

  const QVariantList folders = backend.folders()->Folders();
  ASSERT_FALSE(folders.isEmpty());

  // Find the subfolder ID.
  uint subFolderId = 0;
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == "SubFolderA") {
      subFolderId = map.value("folderId").toUInt();
      break;
    }
  }
  ASSERT_NE(subFolderId, 0u) << "SubFolderA must have a valid ID";

  QSignalSpy selSpy(backend.folders(), &FolderController::FolderSelectionChanged);
  backend.folders()->SelectFolder(subFolderId);
  ProcessEvents(200);

  EXPECT_FALSE(selSpy.isEmpty());
  EXPECT_EQ(backend.folders()->CurrentFolderId(), subFolderId);
}

TEST_F(FolderTests, SelectNestedFolder_LazyPathExpansionUpdatesCurrentPath) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("ParentFolder");
  ProcessEvents(500);

  uint parentId = 0;
  for (const auto& v : backend.folders()->Folders()) {
    const auto map = v.toMap();
    if (map.value("name").toString() == "ParentFolder") {
      parentId = map.value("folderId").toUInt();
      break;
    }
  }
  ASSERT_NE(parentId, 0u);

  backend.folders()->SelectFolder(parentId);
  ProcessEvents(300);
  backend.folders()->CreateFolder("ChildFolder");
  ProcessEvents(500);

  uint childId = 0;
  for (const auto& v : backend.folders()->Folders()) {
    const auto map = v.toMap();
    if (map.value("name").toString() == "ChildFolder") {
      childId = map.value("folderId").toUInt();
      break;
    }
  }
  ASSERT_NE(childId, 0u);

  backend.folders()->SelectFolder(childId);
  ProcessEvents(300);

  EXPECT_EQ(backend.folders()->CurrentFolderPath(), "\\ParentFolder\\ChildFolder");
}

// ── Select folder — invalid ID ─────────────────────────────────────────────

TEST_F(FolderTests, SelectFolder_InvalidId_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  // Select a folder ID that cannot exist.
  backend.folders()->SelectFolder(999999);
  ProcessEvents(200);

  // No crash is the assertion.
  SUCCEED();
}

// ── Delete folder ──────────────────────────────────────────────────────────

TEST_F(FolderTests, DeleteFolder_ExistingId_Removes) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("ToBeDeleted");
  ProcessEvents(500);

  const QVariantList before = backend.folders()->Folders();
  uint               targetId = 0;
  for (const auto& v : before) {
    const auto map = v.toMap();
    if (map.value("name").toString() == "ToBeDeleted") {
      targetId = map.value("folderId").toUInt();
      break;
    }
  }
  ASSERT_NE(targetId, 0u);

  QSignalSpy foldersChangedSpy(backend.folders(), &FolderController::FoldersChanged);
  backend.folders()->DeleteFolder(targetId);
  ProcessEvents(500);

  EXPECT_FALSE(foldersChangedSpy.isEmpty());

  const QVariantList after = backend.folders()->Folders();
  for (const auto& v : after) {
    const auto map = v.toMap();
    EXPECT_NE(map.value("folderId").toUInt(), targetId)
        << "Deleted folder should no longer appear";
  }
}

// ── Delete folder — invalid ID ─────────────────────────────────────────────

TEST_F(FolderTests, DeleteFolder_InvalidId_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->DeleteFolder(0);
  backend.folders()->DeleteFolder(888888);
  ProcessEvents(200);

  SUCCEED();
}

// ── Collections tree rows, nested create, rename, and move (issue #732) ────

auto FolderRow(const QVariantList& folders, const QString& name) -> QVariantMap {
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == name) {
      return map;
    }
  }
  return {};
}

auto FolderNames(const QVariantList& folders) -> QStringList {
  QStringList names;
  for (const auto& v : folders) {
    names.push_back(v.toMap().value("name").toString());
  }
  return names;
}

TEST_F(FolderTests, Folders_ListWholeTreeWithParentAndChildFields) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("Parent");
  ProcessEvents(300);
  const uint parent_id = FolderRow(backend.folders()->Folders(), "Parent").value("folderId").toUInt();
  ASSERT_NE(parent_id, 0u);
  backend.folders()->SelectFolder(parent_id);
  ProcessEvents(300);
  backend.folders()->CreateFolder("Child");
  ProcessEvents(300);

  // Back at the root, a create reloads the tree; the nested folder must stay listed.
  backend.folders()->SelectFolder(0);
  ProcessEvents(300);
  backend.folders()->CreateFolder("Other");
  ProcessEvents(300);

  const auto folders = backend.folders()->Folders();
  const auto parent  = FolderRow(folders, "Parent");
  const auto child   = FolderRow(folders, "Child");
  const auto other   = FolderRow(folders, "Other");
  ASSERT_FALSE(parent.isEmpty());
  ASSERT_FALSE(child.isEmpty()) << FolderNames(folders).join(", ").toStdString();
  ASSERT_FALSE(other.isEmpty());

  EXPECT_EQ(child.value("parentFolderId").toUInt(), parent.value("folderId").toUInt());
  EXPECT_EQ(child.value("parentElementId").toUInt(), parent.value("elementId").toUInt());
  EXPECT_EQ(parent.value("parentFolderId").toUInt(), 0u);
  EXPECT_TRUE(parent.value("hasChildren").toBool());
  EXPECT_EQ(parent.value("childCount").toInt(), 1);
  EXPECT_FALSE(child.value("hasChildren").toBool());
  EXPECT_FALSE(other.value("hasChildren").toBool());
}

TEST_F(FolderTests, CreateFolderIn_UsesTheGivenParentAndKeepsTheSelection) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("A");
  ProcessEvents(300);
  const auto a = FolderRow(backend.folders()->Folders(), "A");
  ASSERT_FALSE(a.isEmpty());

  const uint created =
      backend.folders()->CreateFolderIn(a.value("folderId").toUInt(), QStringLiteral("Inner"));
  ProcessEvents(300);

  ASSERT_NE(created, 0u);
  const auto inner = FolderRow(backend.folders()->Folders(), "Inner");
  ASSERT_FALSE(inner.isEmpty());
  EXPECT_EQ(inner.value("elementId").toUInt(), created);
  EXPECT_EQ(inner.value("parentElementId").toUInt(), a.value("elementId").toUInt());
  EXPECT_EQ(backend.folders()->CurrentFolderId(), 0u);

  EXPECT_EQ(backend.folders()->CreateFolderIn(a.value("folderId").toUInt(), QStringLiteral("  ")),
            0u);
  EXPECT_EQ(backend.folders()->CreateFolderIn(777777u, QStringLiteral("Lost")), 0u);
}

TEST_F(FolderTests, RenameFolder_KeepsElementIdChildrenAndCurrentSelection) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("Trip");
  ProcessEvents(300);
  const auto trip = FolderRow(backend.folders()->Folders(), "Trip");
  ASSERT_FALSE(trip.isEmpty());
  backend.folders()->SelectFolder(trip.value("folderId").toUInt());
  ProcessEvents(300);
  backend.folders()->CreateFolder("Day 1");
  ProcessEvents(300);

  ASSERT_TRUE(backend.folders()->RenameFolder(
      FolderRow(backend.folders()->Folders(), "Trip").value("folderId").toUInt(),
      QStringLiteral("Iceland")));
  ProcessEvents(300);

  const auto folders = backend.folders()->Folders();
  EXPECT_TRUE(FolderRow(folders, "Trip").isEmpty());
  const auto iceland = FolderRow(folders, "Iceland");
  const auto day     = FolderRow(folders, "Day 1");
  ASSERT_FALSE(iceland.isEmpty());
  ASSERT_FALSE(day.isEmpty());
  EXPECT_EQ(iceland.value("elementId").toUInt(), trip.value("elementId").toUInt());
  EXPECT_EQ(day.value("parentElementId").toUInt(), trip.value("elementId").toUInt());
  EXPECT_EQ(backend.folders()->CurrentFolderPath(), "\\Iceland");
  EXPECT_EQ(backend.folders()->CurrentFolderId(), iceland.value("folderId").toUInt());
}

TEST_F(FolderTests, RenameFolder_RefusesTakenNameRootAndEmptyName) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("A");
  ProcessEvents(300);
  backend.folders()->CreateFolder("B");
  ProcessEvents(300);
  const uint b_id = FolderRow(backend.folders()->Folders(), "B").value("folderId").toUInt();
  ASSERT_NE(b_id, 0u);

  EXPECT_FALSE(backend.folders()->RenameFolder(b_id, QStringLiteral("A")));
  EXPECT_FALSE(backend.folders()->RenameFolder(b_id, QStringLiteral("   ")));
  EXPECT_FALSE(backend.folders()->RenameFolder(b_id, QStringLiteral("x/y")));
  EXPECT_FALSE(backend.folders()->RenameFolder(0, QStringLiteral("Root")));
  ProcessEvents(200);

  EXPECT_FALSE(FolderRow(backend.folders()->Folders(), "B").isEmpty());
}

TEST_F(FolderTests, MoveFolder_MovesTheSubtreeAndFollowsTheCurrentFolder) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("A");
  ProcessEvents(300);
  backend.folders()->CreateFolder("B");
  ProcessEvents(300);
  const auto b = FolderRow(backend.folders()->Folders(), "B");
  backend.folders()->SelectFolder(b.value("folderId").toUInt());
  ProcessEvents(300);
  backend.folders()->CreateFolder("C");
  ProcessEvents(300);

  const auto before = backend.folders()->Folders();
  ASSERT_TRUE(backend.folders()->MoveFolder(FolderRow(before, "B").value("folderId").toUInt(),
                                            FolderRow(before, "A").value("folderId").toUInt()));
  ProcessEvents(300);

  const auto after   = backend.folders()->Folders();
  const auto a_after = FolderRow(after, "A");
  const auto b_after = FolderRow(after, "B");
  const auto c_after = FolderRow(after, "C");
  EXPECT_EQ(b_after.value("elementId").toUInt(), b.value("elementId").toUInt());
  EXPECT_EQ(b_after.value("parentElementId").toUInt(), a_after.value("elementId").toUInt());
  EXPECT_EQ(c_after.value("parentElementId").toUInt(), b_after.value("elementId").toUInt());
  EXPECT_EQ(backend.folders()->CurrentFolderPath(), "\\A\\B");

  // Back to the top level.
  ASSERT_TRUE(backend.folders()->MoveFolder(b_after.value("folderId").toUInt(), 0u));
  ProcessEvents(300);
  EXPECT_EQ(FolderRow(backend.folders()->Folders(), "B").value("parentFolderId").toUInt(), 0u);
  EXPECT_EQ(backend.folders()->CurrentFolderPath(), "\\B");
}

TEST_F(FolderTests, MoveFolder_RefusesOwnSubtreeTakenNameAndRoot) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.folders()->CreateFolder("A");
  ProcessEvents(300);
  backend.folders()->CreateFolder("Same");
  ProcessEvents(300);
  const auto a = FolderRow(backend.folders()->Folders(), "A");
  const uint inner =
      backend.folders()->CreateFolderIn(a.value("folderId").toUInt(), QStringLiteral("Inner"));
  ProcessEvents(300);
  ASSERT_NE(inner, 0u);
  ASSERT_NE(backend.folders()->CreateFolderIn(
                FolderRow(backend.folders()->Folders(), "A").value("folderId").toUInt(),
                QStringLiteral("Same")),
            0u);
  ProcessEvents(300);

  const auto folders = backend.folders()->Folders();
  const uint a_id    = FolderRow(folders, "A").value("folderId").toUInt();
  const uint inner_id = FolderRow(folders, "Inner").value("folderId").toUInt();
  uint       top_same_id = 0;
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == "Same" && map.value("parentFolderId").toUInt() == 0u) {
      top_same_id = map.value("folderId").toUInt();
    }
  }
  ASSERT_NE(top_same_id, 0u);

  EXPECT_FALSE(backend.folders()->MoveFolder(a_id, a_id));
  EXPECT_FALSE(backend.folders()->MoveFolder(a_id, inner_id));
  EXPECT_FALSE(backend.folders()->MoveFolder(top_same_id, a_id));
  EXPECT_FALSE(backend.folders()->MoveFolder(0u, a_id));
  ProcessEvents(200);

  const auto after = backend.folders()->Folders();
  EXPECT_EQ(FolderRow(after, "Inner").value("parentElementId").toUInt(),
            FolderRow(after, "A").value("elementId").toUInt());
  EXPECT_EQ(FolderRow(after, "A").value("parentFolderId").toUInt(), 0u);
}

TEST_F(FolderTests, RenameAndMove_PersistAfterProjectReopen) {
  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(CreateTestProject(backend));

    backend.folders()->CreateFolder("A");
    ProcessEvents(300);
    backend.folders()->CreateFolder("B");
    ProcessEvents(300);
    ASSERT_NE(backend.folders()->CreateFolderIn(
                  FolderRow(backend.folders()->Folders(), "A").value("folderId").toUInt(),
                  QStringLiteral("Inner")),
              0u);
    ProcessEvents(300);
    ASSERT_TRUE(backend.folders()->RenameFolder(
        FolderRow(backend.folders()->Folders(), "B").value("folderId").toUInt(),
        QStringLiteral("Beta")));
    ProcessEvents(300);
    const auto folders = backend.folders()->Folders();
    ASSERT_TRUE(backend.folders()->MoveFolder(FolderRow(folders, "Inner").value("folderId").toUInt(),
                                              FolderRow(folders, "Beta").value("folderId").toUInt()));
    ProcessEvents(300);
    ASSERT_TRUE(backend.project()->SaveProject());
  }

  const auto packed_project_path = FindPackedProjectPath(temp_dir_);
  ASSERT_TRUE(packed_project_path.has_value());

  ApplicationModuleHost reloaded;
  QSignalSpy project_spy(reloaded.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(reloaded.project()->LoadProject(PathToQString(*packed_project_path)));
  ASSERT_TRUE(WaitForSignal(project_spy, 15000));
  ProcessEvents(500);

  const auto folders = reloaded.folders()->Folders();
  EXPECT_TRUE(FolderRow(folders, "B").isEmpty());
  const auto beta  = FolderRow(folders, "Beta");
  const auto inner = FolderRow(folders, "Inner");
  ASSERT_FALSE(beta.isEmpty()) << FolderNames(folders).join(", ").toStdString();
  ASSERT_FALSE(inner.isEmpty());
  EXPECT_EQ(inner.value("parentElementId").toUInt(), beta.value("elementId").toUInt());
  EXPECT_FALSE(FolderRow(folders, "A").value("hasChildren").toBool());
}

// ── Photo count of every tree row (issue #733) ─────────────────────────────

/// `imageCount` of the row named @p name (the root row when empty); -2 when no row matches.
auto RowImageCount(ApplicationModuleHost& backend, const QString& name) -> qint64 {
  for (const auto& v : backend.folders()->Folders()) {
    const auto map     = v.toMap();
    const bool is_root = map.value("folderId").toUInt() == 0u;
    if (name.isEmpty() ? is_root : (!is_root && map.value("name").toString() == name)) {
      return map.value("imageCount").toLongLong();
    }
  }
  return -2;
}

/// Pumps events until the root row and the rows of @p expected show the given counts.
auto WaitForCounts(ApplicationModuleHost& backend, qint64 root,
                   const std::vector<std::pair<QString, qint64>>& expected) -> bool {
  const auto matches = [&] {
    if (RowImageCount(backend, {}) != root) {
      return false;
    }
    return std::ranges::all_of(expected, [&](const auto& entry) {
      return RowImageCount(backend, entry.first) == entry.second;
    });
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!matches() && std::chrono::steady_clock::now() < deadline) {
    ProcessEvents(20);
  }
  return matches();
}

auto CountsText(ApplicationModuleHost& backend) -> std::string {
  QStringList parts;
  for (const auto& v : backend.folders()->Folders()) {
    const auto map = v.toMap();
    parts << QStringLiteral("%1=%2").arg(map.value("name").toString(),
                                         map.value("imageCount").toString());
  }
  return parts.join(", ").toStdString();
}

/// Element and image ids of the photos of @p folder_id (0 = the whole project), by element id.
auto FolderPhotos(ApplicationModuleHost& backend, sl_element_id_t folder_id)
    -> std::vector<std::pair<sl_element_id_t, image_id_t>> {
  std::vector<std::pair<sl_element_id_t, image_id_t>> out;
  const auto browse = backend.project()->handler().project()->GetAlbumBrowseService();
  for (const auto& file : browse->ListFilesInFolderById(folder_id)) {
    out.emplace_back(file.element_id_, file.image_id_);
  }
  std::ranges::sort(out);
  return out;
}

auto TargetEntries(const std::vector<std::pair<sl_element_id_t, image_id_t>>& photos)
    -> QVariantList {
  QVariantList list;
  for (const auto& [element_id, image_id] : photos) {
    list.push_back(QVariantMap{{"elementId", static_cast<uint>(element_id)},
                               {"imageId", static_cast<uint>(image_id)}});
  }
  return list;
}

TEST_F(FolderTests, ImageCounts_FollowImportAddRemoveAndDelete) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  backend.folders()->CreateFolder("Trip");
  ProcessEvents(200);
  backend.folders()->CreateFolder("Empty");
  ProcessEvents(200);
  // An empty project: every row reads 0, the empty collection included.
  ASSERT_TRUE(WaitForCounts(backend, 0, {{"Trip", 0}, {"Empty", 0}})) << CountsText(backend);

  // Import into the root view.
  const auto photos = WritePngPhotos(temp_dir_ / "count_source", "count-", 4);
  ASSERT_EQ(photos.size(), 4u);
  ASSERT_TRUE(ImportAndWait(backend, photos));
  ASSERT_TRUE(WaitForCounts(backend, 4, {{"Trip", 0}, {"Empty", 0}})) << CountsText(backend);

  // "Add to" Trip.
  const auto all = FolderPhotos(backend, 0);
  ASSERT_EQ(all.size(), 4u);
  const uint trip = FolderRow(backend.folders()->Folders(), "Trip").value("folderId").toUInt();
  const auto added =
      backend.images()->AddImagesToFolder(TargetEntries({all[0], all[1], all[2]}), trip);
  ASSERT_EQ(added.value("addedCount").toInt(), 3)
      << added.value("message").toString().toStdString();
  ASSERT_TRUE(WaitForCounts(backend, 4, {{"Trip", 3}, {"Empty", 0}})) << CountsText(backend);

  // Remove one photo from Trip: it stays in the project.
  backend.folders()->SelectFolder(
      FolderRow(backend.folders()->Folders(), "Trip").value("folderId").toUInt());
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_TRUE(DeleteAndWait(backend, {all[0]}));
  ASSERT_TRUE(WaitForCounts(backend, 4, {{"Trip", 2}, {"Empty", 0}})) << CountsText(backend);

  // Import into Trip: the photos join Trip and the project.
  const auto more = WritePngPhotos(temp_dir_ / "count_source_trip", "count-trip-", 2);
  ASSERT_EQ(more.size(), 2u);
  ASSERT_TRUE(ImportAndWait(backend, more));
  ASSERT_TRUE(WaitForCounts(backend, 6, {{"Trip", 4}, {"Empty", 0}})) << CountsText(backend);

  // Delete a photo of Trip from the project: both counts drop.
  backend.folders()->SelectFolder(0);
  ASSERT_TRUE(WaitForLibraryQuery(backend));
  ASSERT_TRUE(DeleteAndWait(backend, {all[1]}));
  ASSERT_TRUE(WaitForCounts(backend, 5, {{"Trip", 3}, {"Empty", 0}})) << CountsText(backend);
}

TEST_F(FolderTests, ImageCounts_NewCollectionReadsZeroAfterATreeReload) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  const auto photos = WritePngPhotos(temp_dir_ / "count_reload_source", "reload-", 2);
  ASSERT_EQ(photos.size(), 2u);
  ASSERT_TRUE(ImportAndWait(backend, photos));
  ASSERT_TRUE(WaitForCounts(backend, 2, {})) << CountsText(backend);

  // A create reloads the tree; the new collection reads 0 and the root keeps its count.
  backend.folders()->CreateFolder("Fresh");
  ASSERT_TRUE(WaitForCounts(backend, 2, {{"Fresh", 0}})) << CountsText(backend);
}

// ── Folder operations without project — no crash ───────────────────────────

TEST_F(FolderTests, FolderOps_NoProject_NoCrash) {
  ApplicationModuleHost backend;
  // No project created — all folder ops should be no-ops.

  backend.folders()->CreateFolder("ShouldNotWork");
  backend.folders()->SelectFolder(1);
  backend.folders()->DeleteFolder(1);
  ProcessEvents(200);

  EXPECT_FALSE(backend.project()->ServiceReady());
}

}  // namespace
}  // namespace alcedo::ui::test
