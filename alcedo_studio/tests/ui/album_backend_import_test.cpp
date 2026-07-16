//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_import_test.cpp
/// @brief Import-robustness tests for ApplicationModuleHost.
///
/// Focus: JPEG/TIFF graceful handling (pipeline does not support them as raw
/// decode input), CTD prevention, mixed-format import, cancellation, and
/// edge-case inputs.  All tests run headlessly via QCoreApplication.

#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"

#include <QSignalSpy>
#include <chrono>
#include <filesystem>

namespace alcedo::ui::test {
namespace {

using ImportTests = ApplicationModuleHostTestFixture;

auto FindPackedProjectPath(const std::filesystem::path& dir)
    -> std::optional<std::filesystem::path> {
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".alcd") {
      return entry.path();
    }
  }
  return std::nullopt;
}

auto FindFolderId(const QVariantList& folders, const QString& name) -> uint {
  for (const auto& v : folders) {
    const auto map = v.toMap();
    if (map.value("name").toString() == name) {
      return map.value("folderId").toUInt();
    }
  }
  return 0;
}

// ── Helper: wait until importRunning becomes false ─────────────────────────

void WaitForImportFinished(ApplicationModuleHost& backend, int timeoutMs = 30000) {
  QSignalSpy spy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  const int step = 200;
  int       elapsed = 0;
  while (backend.import_export()->ImportRunning() && elapsed < timeoutMs) {
    spy.wait(step);
    elapsed += step;
  }
  // Drain remaining queued events (FinishImport is posted via
  // Qt::QueuedConnection).
  ProcessEvents(500);
}

auto WaitForSearchPreviewThumbnail(QSignalSpy& spy, uint element_id, int timeout_ms)
    -> QVariantList {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    while (!spy.isEmpty()) {
      const QVariantList args = spy.takeFirst();
      if (args.size() < 5 || args[0].toUInt() != element_id) {
        continue;
      }
      if (!args[2].toBool()) {
        return args;
      }
    }
    spy.wait(200);
  }
  return {};
}

// ── Single RAW import ──────────────────────────────────────────────────────

TEST_F(ImportTests, Import_SingleRawFile_Succeeds) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    GTEST_SKIP() << "No test RAW images found in raw/airplane/";
  }

  QSignalSpy importSpy(backend.import_export(), &ImportExportHandler::ImportStateChanged);
  backend.import_export()->StartImport(PathsToQStringList(images));

  WaitForImportFinished(backend);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
  EXPECT_GE(backend.import_export()->ImportCompleted(), 1);
}

// ── JPEG-only import — must not crash ──────────────────────────────────────

TEST_F(ImportTests, Import_JpegOnly_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  // Collect .jpg/.jpeg files from the batch folder (they exist alongside ARWs).
  std::vector<std::filesystem::path> jpegPaths;
  const std::filesystem::path batchDir{std::string(TEST_IMG_PATH) + "/raw/batch"};
  if (std::filesystem::exists(batchDir)) {
    for (const auto& entry : std::filesystem::directory_iterator(batchDir)) {
      if (!entry.is_regular_file()) continue;
      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".jpg" || ext == ".jpeg") {
        jpegPaths.push_back(entry.path());
      }
    }
  }
  // Also check jpeg/tile_tests
  const std::filesystem::path jpegDir{std::string(TEST_IMG_PATH) + "/jpeg/tile_tests"};
  if (std::filesystem::exists(jpegDir)) {
    for (const auto& entry : std::filesystem::directory_iterator(jpegDir)) {
      if (entry.is_regular_file()) {
        jpegPaths.push_back(entry.path());
      }
    }
  }

  if (jpegPaths.empty()) {
    GTEST_SKIP() << "No JPEG test images available";
  }

  // This should NOT crash — even if pipeline cannot process JPEGs.
  backend.import_export()->StartImport(PathsToQStringList(jpegPaths));
  WaitForImportFinished(backend);

  // The test passes as long as we reach here without crashing.
  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

// ── TIFF-only import — must not crash ──────────────────────────────────────

TEST_F(ImportTests, Import_TiffOnly_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  std::vector<std::filesystem::path> tiffPaths;
  const std::filesystem::path imgRoot{std::string(TEST_IMG_PATH)};
  if (std::filesystem::exists(imgRoot)) {
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(imgRoot)) {
      if (!entry.is_regular_file()) continue;
      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".tiff" || ext == ".tif") {
        tiffPaths.push_back(entry.path());
      }
    }
  }

  if (tiffPaths.empty()) {
    GTEST_SKIP() << "No TIFF test images available";
  }

  backend.import_export()->StartImport(PathsToQStringList(tiffPaths));
  WaitForImportFinished(backend);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

// ── Mixed RAW + JPEG — only RAWs should survive pipeline without crash ─────

TEST_F(ImportTests, Import_MixedRawAndJpeg_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  // Collect everything from raw/batch (NEFs, ARWs, JPGs).
  std::vector<std::filesystem::path> allPaths;
  const std::filesystem::path batchDir{std::string(TEST_IMG_PATH) + "/raw/batch"};
  if (std::filesystem::exists(batchDir)) {
    for (const auto& entry : std::filesystem::directory_iterator(batchDir)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        allPaths.push_back(entry.path());
      }
    }
  }
  // Limit to a reasonable count so the test finishes quickly.
  std::sort(allPaths.begin(), allPaths.end());
  if (allPaths.size() > 6) allPaths.resize(6);

  if (allPaths.empty()) {
    GTEST_SKIP() << "No mixed test images available in raw/batch/";
  }

  backend.import_export()->StartImport(PathsToQStringList(allPaths));
  WaitForImportFinished(backend);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
  // At least the RAW files should succeed (if JPEGs fail, that's OK —
  // the critical thing is no crash).
}

TEST_F(ImportTests, SearchPreview_ReturnsPagedResultsAndTotalCount) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  auto* search = backend.search();
  ASSERT_NE(search, nullptr);

  auto images = CollectRawTestImages("batch", 8);
  if (images.size() < 6u) {
    GTEST_SKIP() << "Need several RAW fixtures in raw/batch/";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend, 60000);
  ASSERT_FALSE(backend.import_export()->ImportRunning());
  ASSERT_GE(backend.import_export()->ImportCompleted(), 6);

  const QString query = PathToQString(images.front().stem()).left(4);
  ASSERT_FALSE(query.isEmpty());

  const QVariantMap first_page = search->SearchPreview(query, 0, 3);
  const auto        first_rows = first_page.value("rows").toList();
  ASSERT_EQ(first_rows.size(), 3);
  EXPECT_GE(first_page.value("total").toInt(), backend.import_export()->ImportCompleted());
  EXPECT_TRUE(first_page.value("hasMore").toBool());

  const QVariantMap second_page = search->SearchPreview(query, 3, 3);
  const auto        second_rows = second_page.value("rows").toList();
  ASSERT_EQ(second_rows.size(), 3);
  EXPECT_EQ(second_page.value("offset").toInt(), 3);

  const auto first_first_id =
      first_rows.front().toMap().value("elementId").toUInt();
  const auto second_first_id =
      second_rows.front().toMap().value("elementId").toUInt();
  EXPECT_NE(first_first_id, second_first_id);
}

TEST_F(ImportTests, SearchPreviewThumbnail_LoadsForPagedVisibleResult) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));
  auto* search = backend.search();
  ASSERT_NE(search, nullptr);

  auto images = CollectRawTestImages("batch", 8);
  if (images.size() < 6u) {
    GTEST_SKIP() << "Need several RAW fixtures in raw/batch/";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend, 60000);
  ASSERT_FALSE(backend.import_export()->ImportRunning());

  const QString query = PathToQString(images.front().stem()).left(4);
  ASSERT_FALSE(query.isEmpty());

  const QVariantMap second_page = search->SearchPreview(query, 3, 3);
  const auto        second_rows = second_page.value("rows").toList();
  ASSERT_FALSE(second_rows.empty());

  const QVariantMap row = second_rows.front().toMap();
  const auto        element_id = row.value("elementId").toUInt();
  const auto        image_id   = row.value("imageId").toUInt();
  ASSERT_NE(element_id, 0u);
  ASSERT_NE(image_id, 0u);

  QSignalSpy thumb_spy(search, &SearchController::SearchPreviewThumbnailUpdated);
  search->SetSearchPreviewThumbnailVisible(element_id, image_id, true, 256);

  const QVariantList final_update = WaitForSearchPreviewThumbnail(thumb_spy, element_id, 30000);
  ASSERT_FALSE(final_update.empty())
      << "Timed out waiting for a paged search preview thumbnail to finish loading.";
  EXPECT_FALSE(final_update[1].toString().isEmpty());
  EXPECT_FALSE(final_update[2].toBool());
  EXPECT_FALSE(final_update[3].toBool());
  EXPECT_TRUE(final_update[4].toString().isEmpty());

  search->SetSearchPreviewThumbnailVisible(element_id, image_id, false, 256);
}

// ── Empty file list — no crash, sensible feedback ──────────────────────────

TEST_F(ImportTests, Import_EmptyFileList_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  backend.import_export()->StartImport({});

  ProcessEvents(200);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

// ── Non-existent path — no crash ───────────────────────────────────────────

TEST_F(ImportTests, Import_NonexistentPath_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  QStringList fakeFiles;
  fakeFiles << "C:/this/path/does/not/exist/photo.nef";
  fakeFiles << "/tmp/phantom_image.arw";

  backend.import_export()->StartImport(fakeFiles);
  ProcessEvents(200);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

// ── Import without a project loaded — no crash ─────────────────────────────

TEST_F(ImportTests, Import_NoProjectLoaded_NoCrash) {
  ApplicationModuleHost backend;
  // Do NOT create a project — backend is in "not ready" state.

  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    GTEST_SKIP() << "No test RAW images available";
  }

  // Should fail gracefully, not crash.
  backend.import_export()->StartImport(PathsToQStringList(images));
  ProcessEvents(200);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
  EXPECT_FALSE(backend.project()->ServiceReady());
}

// ── Duplicate files in same import call — deduplication ────────────────────

TEST_F(ImportTests, Import_DuplicateFiles_Deduplication) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    GTEST_SKIP() << "No test RAW images available";
  }

  // Duplicate the same file path twice.
  QStringList duped;
  duped << PathToQString(images[0]);
  duped << PathToQString(images[0]);

  backend.import_export()->StartImport(duped);
  WaitForImportFinished(backend);

  EXPECT_FALSE(backend.import_export()->ImportRunning());
  // Only one copy should have been imported.
  EXPECT_EQ(backend.import_export()->ImportCompleted(), 1);
}

// ── Batch DNG import ───────────────────────────────────────────────────────

TEST_F(ImportTests, Import_BatchDng_Succeeds) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("batch_import", 5);
  if (images.empty()) {
    GTEST_SKIP() << "No DNG files in raw/batch_import/";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend, 60000);  // DNG batch can be slow.

  EXPECT_FALSE(backend.import_export()->ImportRunning());
  EXPECT_EQ(backend.import_export()->ImportCompleted(), static_cast<int>(images.size()));
}

// ── Cancel import — no crash ───────────────────────────────────────────────

TEST_F(ImportTests, Import_CancelImmediate_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  auto images = CollectRawTestImages("batch_import", 10);
  if (images.empty()) {
    GTEST_SKIP() << "No DNG files for cancel test";
  }

  backend.import_export()->StartImport(PathsToQStringList(images));
  // Cancel immediately — the import thread may or may not have started.
  ProcessEvents(50);
  backend.import_export()->CancelImport();

  WaitForImportFinished(backend, 15000);
  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

// ── Unsupported extension — silently ignored ───────────────────────────────

TEST_F(ImportTests, Import_UnsupportedExtension_Ignored) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  // Create a temp file with a made-up extension.
  const auto fakePath = temp_dir_ / "image.xyz";
  {
    std::ofstream ofs(fakePath);
    ofs << "not a real image";
  }

  QStringList list;
  list << PathToQString(fakePath);
  backend.import_export()->StartImport(list);
  ProcessEvents(200);

  // The file should be silently skipped (not a supported extension).
  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

// ── Corrupted file with valid extension — no crash ─────────────────────────

TEST_F(ImportTests, Import_CorruptedFileValidExtension_NoCrash) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  // Create a file named .nef but with garbage content.
  const auto corruptPath = temp_dir_ / "corrupt.nef";
  {
    std::ofstream ofs(corruptPath, std::ios::binary);
    ofs << "THIS_IS_NOT_A_REAL_NEF_FILE_JUST_GARBAGE_DATA";
  }

  QStringList list;
  list << PathToQString(corruptPath);
  backend.import_export()->StartImport(list);
  WaitForImportFinished(backend);

  // Must not crash. Import may report 0 succeeded + 1 failed, or skip it.
  EXPECT_FALSE(backend.import_export()->ImportRunning());
}

TEST_F(ImportTests, ImportIntoSubfolder_PersistsAcrossFreshProjectLoad) {
  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    GTEST_SKIP() << "No test RAW images available";
  }

  const QString expected_name = PathToQString(images.front().filename());

  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(CreateTestProject(backend, "subfolder_import_reload"));

    backend.folders()->CreateFolder("Imports");
    ProcessEvents(500);

    const uint imports_folder_id = FindFolderId(backend.folders()->Folders(), "Imports");
    ASSERT_NE(imports_folder_id, 0u);

    backend.folders()->SelectFolder(imports_folder_id);
    ProcessEvents(300);
    ASSERT_EQ(backend.folders()->CurrentFolderPath(), "\\Imports");

    backend.import_export()->StartImport(PathsToQStringList(images));
    WaitForImportFinished(backend);

    ASSERT_FALSE(backend.import_export()->ImportRunning());
    ASSERT_EQ(backend.library()->ShownCount(), 1);
    ASSERT_EQ(backend.library()->Thumbnails().size(), 1);

    const QVariantMap imported = backend.library()->Thumbnails().front().toMap();
    EXPECT_EQ(imported.value("fileName").toString(), expected_name);
    ASSERT_TRUE(backend.project()->SaveProject());
  }

  const auto packed_project_path = FindPackedProjectPath(temp_dir_);
  ASSERT_TRUE(packed_project_path.has_value());

  ApplicationModuleHost reloaded_backend;
  QSignalSpy project_spy(reloaded_backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(reloaded_backend.project()->LoadProject(PathToQString(*packed_project_path)));
  ASSERT_TRUE(WaitForSignal(project_spy, 15000));
  ProcessEvents(500);

  const uint imports_folder_id = FindFolderId(reloaded_backend.folders()->Folders(), "Imports");
  ASSERT_NE(imports_folder_id, 0u);

  reloaded_backend.folders()->SelectFolder(imports_folder_id);
  ProcessEvents(500);

  EXPECT_EQ(reloaded_backend.folders()->CurrentFolderPath(), "\\Imports");
  ASSERT_EQ(reloaded_backend.library()->ShownCount(), 1);
  ASSERT_EQ(reloaded_backend.library()->Thumbnails().size(), 1);

  const QVariantMap imported = reloaded_backend.library()->Thumbnails().front().toMap();
  EXPECT_EQ(imported.value("fileName").toString(), expected_name);
}

TEST_F(ImportTests, ImportIntoNestedSubfolder_PersistsAcrossProjectReload) {
  auto images = CollectRawTestImages("airplane", 1);
  if (images.empty()) {
    GTEST_SKIP() << "No test RAW images available";
  }

  const QString expected_name = PathToQString(images.front().filename());

  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend, "nested_subfolder_import_reload"));

  backend.folders()->CreateFolder("ParentFolder");
  ProcessEvents(500);

  const uint parent_folder_id = FindFolderId(backend.folders()->Folders(), "ParentFolder");
  ASSERT_NE(parent_folder_id, 0u);

  backend.folders()->SelectFolder(parent_folder_id);
  ProcessEvents(300);
  backend.folders()->CreateFolder("ChildFolder");
  ProcessEvents(500);

  const uint child_folder_id = FindFolderId(backend.folders()->Folders(), "ChildFolder");
  ASSERT_NE(child_folder_id, 0u);

  backend.folders()->SelectFolder(child_folder_id);
  ProcessEvents(300);
  ASSERT_EQ(backend.folders()->CurrentFolderPath(), "\\ParentFolder\\ChildFolder");

  backend.import_export()->StartImport(PathsToQStringList(images));
  WaitForImportFinished(backend);

  ASSERT_FALSE(backend.import_export()->ImportRunning());
  ASSERT_EQ(backend.library()->ShownCount(), 1);
  ASSERT_EQ(backend.library()->Thumbnails().size(), 1);
  EXPECT_EQ(backend.library()->Thumbnails().front().toMap().value("fileName").toString(), expected_name);
  const auto packed_project_path = FindPackedProjectPath(temp_dir_);
  ASSERT_TRUE(packed_project_path.has_value());

  QSignalSpy project_spy(backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(backend.project()->LoadProject(PathToQString(*packed_project_path)));
  ASSERT_TRUE(WaitForSignal(project_spy, 15000));
  ProcessEvents(500);

  EXPECT_EQ(backend.folders()->CurrentFolderPath(), "\\ParentFolder\\ChildFolder");
  ASSERT_EQ(backend.library()->ShownCount(), 1);
  ASSERT_EQ(backend.library()->Thumbnails().size(), 1);
  EXPECT_EQ(backend.library()->Thumbnails().front().toMap().value("fileName").toString(), expected_name);
}

}  // namespace
}  // namespace alcedo::ui::test
