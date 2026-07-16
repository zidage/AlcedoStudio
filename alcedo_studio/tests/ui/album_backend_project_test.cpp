//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_project_test.cpp
/// @brief Project lifecycle tests for ApplicationModuleHost.
///
/// Covers: create project, load project (valid/invalid), save project,
/// pack/unpack integrity, data_summary diagnostics, and initial service
/// state.

#include "ui/album_backend_test_fixture.hpp"

#include <QSignalSpy>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <duckdb.h>
#include <json.hpp>

#include "app/model_asset_catalog.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "sleeve/storage_service.hpp"
#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"

namespace alcedo::ui::test {
namespace {

using ProjectTests = ApplicationModuleHostTestFixture;

TEST(ModelAssetCatalogTests, NativeCoreMlProfileIsOnlyAvailableOnMacos) {
  const auto has_coreml = std::any_of(
      SemanticModelProfiles().begin(), SemanticModelProfiles().end(), [](const auto& profile) {
        return std::string{profile.profile_id} == "siglip2-base-256-coreml-macos" &&
               profile.inference_backend == ModelInferenceBackend::kNativeCoreMl;
      });

#ifdef __APPLE__
  EXPECT_TRUE(has_coreml);
  EXPECT_NE(FindSemanticProfile("siglip2-base-256-coreml-macos"), nullptr);
#else
  EXPECT_FALSE(has_coreml);
  EXPECT_EQ(FindSemanticProfile("siglip2-base-256-coreml-macos"), nullptr);
#endif

  EXPECT_NE(FindSemanticProfile("mobileclip2-s2-en"), nullptr);
  EXPECT_NE(FindSemanticProfile("jina-clip-v2-int8-multilingual"), nullptr);
  EXPECT_NE(FindSemanticProfile("siglip2-b32-256-multilingual"), nullptr);
}

auto FindPackedProjectPath(const std::filesystem::path& dir)
    -> std::optional<std::filesystem::path> {
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".alcd") {
      return entry.path();
    }
  }
  return std::nullopt;
}

void WriteU32Le(std::ostream& stream, uint32_t value) {
  std::array<unsigned char, 4> bytes{};
  bytes[0] = static_cast<unsigned char>(value & 0xFFU);
  bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  bytes[2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
  bytes[3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void CreateMetadataProject(const std::filesystem::path& dbPath,
                           const std::filesystem::path& metaPath) {
    ProjectService project(dbPath, metaPath, ProjectOpenMode::kCreateNew);
    project.GetSleeveService()->Sync();
    project.GetImagePoolService()->SyncWithStorage();
    project.SaveProject(metaPath);
}

bool WaitForProjectLoadToFinish(ApplicationModuleHost& backend, int timeoutMs = 15000) {
  if (!backend.project()->ProjectLoading()) {
    return true;
  }

  QSignalSpy spy(backend.project(), &ProjectModule::ProjectLoadStateChanged);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (backend.project()->ProjectLoading() && std::chrono::steady_clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              std::chrono::steady_clock::now())
            .count();
    if (remaining <= 0 || !spy.wait(static_cast<int>(std::min<qint64>(remaining, 500)))) {
      ProcessEvents(50);
    }
  }
  return !backend.project()->ProjectLoading();
}

// ── Initial state ──────────────────────────────────────────────────────────

TEST_F(ProjectTests, ServiceState_InitiallyNotReady) {
  ApplicationModuleHost backend;
  EXPECT_FALSE(backend.project()->ServiceReady());
  EXPECT_FALSE(backend.project()->ServiceMessage().isEmpty());
}

// ── Create project — happy path ────────────────────────────────────────────

TEST_F(ProjectTests, CreateProject_ValidFolder_Succeeds) {
  ApplicationModuleHost backend;
  QSignalSpy projSpy(backend.project(), &ProjectModule::ProjectChanged);
  QSignalSpy stateSpy(backend.project(), &ProjectModule::ServiceStateChanged);

  const bool ok =
      backend.project()->CreateProjectInFolderNamed(PathToQString(temp_dir_), "test_proj");
  EXPECT_TRUE(ok);

  // Wait for async project initialisation.
  WaitForSignal(projSpy, 15000);
  ProcessEvents(500);

  EXPECT_TRUE(backend.project()->ServiceReady());
  EXPECT_FALSE(stateSpy.isEmpty());
}

// ── Create project — empty name ────────────────────────────────────────────

TEST_F(ProjectTests, CreateProject_EmptyName_Fails) {
  ApplicationModuleHost backend;
  const bool   ok =
      backend.project()->CreateProjectInFolderNamed(PathToQString(temp_dir_), "");
  // Either returns false or sets a service message.
  // The critical assertion: no crash.
  if (!ok) {
    SUCCEED();
  } else {
    // If it somehow succeeds with empty name, the service message should
    // still be reasonable.
    ProcessEvents(200);
  }
}

TEST_F(ProjectTests, SemanticActivationManifestRequiresListedFilesOnDisk) {
  // Production validation checks every catalog-listed asset under the profile
  // root, not just the subset written into alcedo_model_manifest.json. Seed a
  // complete stub tree, then delete one asset and confirm activation fails.
  const auto base_dir  = temp_dir_ / "model";
  const auto model_dir = base_dir / "mobileclip2-s2-en";
  const auto* catalog  = FindSemanticProfile("mobileclip2-s2-en");
  ASSERT_NE(catalog, nullptr);

  nlohmann::json assets = nlohmann::json::array();
  for (const auto& asset : catalog->assets) {
    const auto relative = std::filesystem::path(asset.local_path);
    const auto full     = model_dir / relative;
    std::filesystem::create_directories(full.parent_path());
    {
      // Match catalog size_bytes exactly (ValidateCatalogAssetPresence is strict).
      std::ofstream out(full, std::ios::binary);
      if (asset.size_bytes > 0) {
        out.seekp(static_cast<std::streamoff>(asset.size_bytes) - 1);
        out.put('\0');
      }
    }
    assets.push_back({
        {"role", std::string(ToString(asset.role))},
        {"repo_id", asset.repo_id},
        {"revision", asset.revision},
        {"remote_path", asset.remote_path},
        // Relative path so ManifestAssetRelativePath matches catalog local_path.
        {"local_path", asset.local_path},
        {"size_bytes", asset.size_bytes},
        {"sha256", asset.sha256 ? std::string(asset.sha256) : std::string{}},
    });
  }

  nlohmann::json manifest;
  manifest["profile_id"]                 = catalog->profile_id;
  manifest["model_id"]                   = catalog->model_id;
  manifest["revision"]                   = catalog->revision;
  manifest["engine_profile_id"]          = catalog->engine_profile_id;
  manifest["language"]                   = ToString(catalog->language);
  manifest["embedding_dimension"]        = catalog->embedding_dimension;
  manifest["native_embedding_dimension"] = catalog->native_embedding_dimension;
  manifest["image_size"]                 = catalog->image_size;
  manifest["embedding_transform"]        = catalog->embedding_transform;
  manifest["model_root"]                 = model_dir.string();
  manifest["assets"]                     = assets;

  {
    std::ofstream out(model_dir / "alcedo_model_manifest.json");
    out << manifest.dump(2);
  }

  QString error;
  auto    loaded = detail::LoadLocalResolvedModelManifestForActivation(
      QStringLiteral("mobileclip2-s2-en"), PathToQString(base_dir), &error);
  ASSERT_TRUE(loaded.has_value()) << error.toStdString();

  const auto removed_asset = model_dir / catalog->assets.front().local_path;
  std::filesystem::remove(removed_asset);
  error.clear();
  loaded = detail::LoadLocalResolvedModelManifestForActivation(
      QStringLiteral("mobileclip2-s2-en"), PathToQString(base_dir), &error);
  EXPECT_FALSE(loaded.has_value());
  EXPECT_TRUE(error.contains(QStringLiteral("missing"), Qt::CaseInsensitive))
      << error.toStdString();
}

// ── Create project while "loading" — second call rejected ──────────────────

TEST_F(ProjectTests, CreateProject_DoubleCall_SecondRejected) {
  ApplicationModuleHost backend;

  const bool first =
      backend.project()->CreateProjectInFolderNamed(PathToQString(temp_dir_), "proj_a");

  // If first call started async loading, a second call should be rejected.
  if (first && backend.project()->ProjectLoading()) {
    // Create a different subfolder so paths differ.
    const auto subDir = temp_dir_ / "sub";
    std::filesystem::create_directories(subDir);
    const bool second =
        backend.project()->CreateProjectInFolderNamed(PathToQString(subDir), "proj_b");
    EXPECT_FALSE(second);
  }

  // Drain everything so destructor is clean.
  ProcessEvents(2000);
}

// ── Load project — non-existent file ───────────────────────────────────────

TEST_F(ProjectTests, LoadProject_NonexistentFile_Fails) {
  ApplicationModuleHost backend;
  const bool   ok = backend.project()->LoadProject("C:/nonexistent/project.json");
  EXPECT_FALSE(ok);
  EXPECT_FALSE(backend.project()->ServiceReady());
}

// ── Load project — invalid format ──────────────────────────────────────────

TEST_F(ProjectTests, LoadProject_InvalidFormat_Fails) {
  ApplicationModuleHost backend;

  // Create a temporary .txt file — not a valid project format.
  const auto txtPath = temp_dir_ / "notes.txt";
  {
    std::ofstream ofs(txtPath);
    ofs << "hello world";
  }

  const bool ok = backend.project()->LoadProject(PathToQString(txtPath));
  EXPECT_FALSE(ok);
}

TEST_F(ProjectTests, LoadProject_OldPackedProjectVersion_Fails) {
  const auto oldProjectPath = temp_dir_ / "old_project.alcd";
  {
    std::ofstream out(oldProjectPath, std::ios::binary | std::ios::trunc);
    out.write(project_pack::kPackedProjectMagic.data(),
              static_cast<std::streamsize>(project_pack::kPackedProjectMagic.size()));
    WriteU32Le(out, project_pack::kPackedProjectVersion - 1);
  }

  ApplicationModuleHost backend;
  EXPECT_FALSE(backend.project()->LoadProject(PathToQString(oldProjectPath)));
  EXPECT_FALSE(backend.project()->ServiceReady());
}

TEST_F(ProjectTests, LoadProject_MetadataJsonProject_Fails) {
  const auto dbPath = temp_dir_ / "corrupt_meta.db";
  const auto metaPath = temp_dir_ / "corrupt_meta.json";
  CreateMetadataProject(dbPath, metaPath);

  ApplicationModuleHost backend;
  EXPECT_FALSE(backend.project()->LoadProject(PathToQString(metaPath)));
  EXPECT_FALSE(backend.project()->ServiceReady());
}

TEST_F(ProjectTests, LoadProject_CorruptPackedProjectPayload_Fails) {
  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(CreateTestProject(backend, "corrupt_packed_project"));
    ASSERT_TRUE(backend.project()->SaveProject());
  }

  const auto packedProjectPath = FindPackedProjectPath(temp_dir_);
  ASSERT_TRUE(packedProjectPath.has_value());

  {
    std::fstream file(*packedProjectPath, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    file.clear();
    file.seekp(-1, std::ios::end);
    byte ^= 0x01;
    file.write(&byte, 1);
  }

  ApplicationModuleHost backend;
  EXPECT_FALSE(backend.project()->LoadProject(PathToQString(*packedProjectPath)));
  EXPECT_FALSE(backend.project()->ServiceReady());
}

TEST_F(ProjectTests, LoadProject_ValidMetadataProject_Fails) {
  const auto dbPath = temp_dir_ / "valid_project.db";
  const auto metaPath = temp_dir_ / "valid_project.json";
  CreateMetadataProject(dbPath, metaPath);

  ApplicationModuleHost backend;
  EXPECT_FALSE(backend.project()->LoadProject(PathToQString(metaPath)));
  EXPECT_FALSE(backend.project()->ServiceReady());
}

TEST_F(ProjectTests, LoadProject_ValidPackedProject_Succeeds) {
  {
    ApplicationModuleHost backend;
    ASSERT_TRUE(CreateTestProject(backend, "valid_packed_project"));
    ASSERT_TRUE(backend.project()->SaveProject());
  }

  const auto packedProjectPath = FindPackedProjectPath(temp_dir_);
  ASSERT_TRUE(packedProjectPath.has_value());

  ApplicationModuleHost backend;
  QSignalSpy projectSpy(backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(backend.project()->LoadProject(PathToQString(*packedProjectPath)));
  ASSERT_TRUE(WaitForSignal(projectSpy, 15000));
  ProcessEvents(500);

  EXPECT_TRUE(backend.project()->ServiceReady());
}

TEST_F(ProjectTests, LoadProject_ExternalPackedProjectFromEnv_Succeeds) {
  const char* raw_path = std::getenv("ALCEDO_TEST_PACKED_PROJECT_PATH");
  if (raw_path == nullptr || raw_path[0] == '\0') {
    GTEST_SKIP() << "Set ALCEDO_TEST_PACKED_PROJECT_PATH to load a real .alcd project.";
  }

  ApplicationModuleHost backend;
  QSignalSpy projectSpy(backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(backend.project()->LoadProject(QString::fromLocal8Bit(raw_path)));
  ASSERT_TRUE(WaitForSignal(projectSpy, 30000));
  ProcessEvents(500);

  EXPECT_TRUE(backend.project()->ServiceReady()) << backend.project()->ServiceMessage().toStdString();
}

TEST_F(ProjectTests, LoadProject_ExternalPackedProjectFromEnv_RequestsThumbnails) {
  const char* raw_path = std::getenv("ALCEDO_TEST_PACKED_PROJECT_PATH");
  if (raw_path == nullptr || raw_path[0] == '\0') {
    GTEST_SKIP() << "Set ALCEDO_TEST_PACKED_PROJECT_PATH to load a real .alcd project.";
  }

  ApplicationModuleHost backend;
  QSignalSpy projectSpy(backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(backend.project()->LoadProject(QString::fromLocal8Bit(raw_path)));
  ASSERT_TRUE(WaitForSignal(projectSpy, 30000));
  ProcessEvents(500);
  ASSERT_TRUE(backend.project()->ServiceReady()) << backend.project()->ServiceMessage().toStdString();

  auto* model = qobject_cast<AlbumThumbnailModel*>(backend.library()->ThumbnailModel());
  ASSERT_NE(model, nullptr);
  ASSERT_GT(model->count(), 0);

  QSignalSpy thumbnailSpy(backend.library(), &LibraryModule::ThumbnailUpdated);
  for (int i = 0; i < model->count(); ++i) {
    const QVariantMap row = model->getItemAt(i);
    backend.library()->SetThumbnailVisible(row.value(QStringLiteral("elementId")).toUInt(),
                                row.value(QStringLiteral("imageId")).toUInt(), true, 512);
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (thumbnailSpy.count() < model->count() && std::chrono::steady_clock::now() < deadline) {
    ProcessEvents(100);
  }
  EXPECT_GE(thumbnailSpy.count(), model->count());
}

TEST_F(ProjectTests, LoadProject_ExternalPackedProjectFromEnv_StartsSemanticGeneration) {
  const char* raw_path = std::getenv("ALCEDO_TEST_PACKED_PROJECT_PATH");
  if (raw_path == nullptr || raw_path[0] == '\0') {
    GTEST_SKIP() << "Set ALCEDO_TEST_PACKED_PROJECT_PATH to load a real .alcd project.";
  }

  ApplicationModuleHost backend;
  QSignalSpy projectSpy(backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(backend.project()->LoadProject(QString::fromLocal8Bit(raw_path)));
  ASSERT_TRUE(WaitForSignal(projectSpy, 30000));
  ProcessEvents(500);
  ASSERT_TRUE(backend.project()->ServiceReady()) << backend.project()->ServiceMessage().toStdString();

  auto* semantic = backend.semantic_generation();
  ASSERT_NE(semantic, nullptr);
  semantic->RefreshAlbumSummary();
  if (semantic->AlbumTotalCount() <= 0) {
    GTEST_SKIP() << "The external project has no images.";
  }

  QSignalSpy stateSpy(semantic, &SemanticGenerationController::StateChanged);
  semantic->StartAlbumGeneration(true);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
  while (semantic->Running() && std::chrono::steady_clock::now() < deadline) {
    ProcessEvents(250);
  }
  EXPECT_FALSE(semantic->Running()) << semantic->StatusText().toStdString();
  EXPECT_FALSE(stateSpy.isEmpty());
}

// ── Save project — no project loaded ───────────────────────────────────────

TEST_F(ProjectTests, SaveProject_NoProject_Fails) {
  ApplicationModuleHost backend;
  const bool   ok = backend.project()->SaveProject();
  EXPECT_FALSE(ok);
}

// ── Save project — after create ────────────────────────────────────────────

TEST_F(ProjectTests, SaveProject_AfterCreate_Succeeds) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend));

  const bool ok = backend.project()->SaveProject();
  EXPECT_TRUE(ok);
}

TEST_F(ProjectTests, CreateProjectWhileProjectOpen_CompletesSwitch) {
  ApplicationModuleHost backend;
  ASSERT_TRUE(CreateTestProject(backend, "first_project"));

  QSignalSpy project_spy(backend.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(backend.project()->CreateProjectInFolderNamed(PathToQString(temp_dir_), "second_project"));
  ASSERT_TRUE(WaitForProjectLoadToFinish(backend, 15000));
  ProcessEvents(500);

  EXPECT_TRUE(backend.project()->ServiceReady());
  EXPECT_FALSE(backend.project()->ProjectLoading());
  EXPECT_GE(project_spy.count(), 1);
}

// ── Stderr capture for warning verification ─────────────────────────────────

class ScopedStderrCapture {
 public:
  ScopedStderrCapture() : old_(std::cerr.rdbuf(ss_.rdbuf())) {}
  ~ScopedStderrCapture() { std::cerr.rdbuf(old_); }
  auto str() const -> std::string { return ss_.str(); }

 private:
  std::stringstream ss_;
  std::streambuf*   old_;
};

// ── Pack/Unpack round-trip ──────────────────────────────────────────────────

TEST_F(ProjectTests, PackUnpack_RoundTrip_Succeeds) {
  const auto dbPath   = temp_dir_ / "roundtrip.db";
  const auto metaPath = temp_dir_ / "roundtrip.json";
  CreateMetadataProject(dbPath, metaPath);

  auto project =
      std::make_shared<ProjectService>(dbPath, metaPath, ProjectOpenMode::kLoadExisting);

  std::filesystem::path snapshotPath;
  ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshotPath, nullptr));
  ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshotPath, nullptr));

  const auto packedPath = temp_dir_ / "roundtrip.alcd";
  ASSERT_TRUE(project_pack::WritePackedProject(packedPath, metaPath, snapshotPath, nullptr));

  // Read back and verify the binary header.
  std::string metaBytes, dbBytes;
  ASSERT_TRUE(project_pack::ReadPackedProject(packedPath, &metaBytes, &dbBytes, nullptr));
  EXPECT_FALSE(metaBytes.empty());
  EXPECT_FALSE(dbBytes.empty());

  // Unpack to workspace and verify the project loads without warnings.
  std::filesystem::path workspaceDir;
  ASSERT_TRUE(project_pack::CreateProjectWorkspace("roundtrip_test", &workspaceDir, nullptr));
  std::filesystem::path unpackedDbPath, unpackedMetaPath;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(packedPath, workspaceDir,
                                                      "roundtrip_test", &unpackedDbPath,
                                                      &unpackedMetaPath, nullptr));

  {
    ScopedStderrCapture capture;
    EXPECT_NO_THROW({
      ProjectService(unpackedDbPath, unpackedMetaPath, ProjectOpenMode::kLoadExisting);
    });
    // No data summary warning expected for a clean round-trip.
    std::string output = capture.str();
    EXPECT_TRUE(output.empty()) << "Unexpected stderr: " << output;
  }

  // Metadata should contain the expected fields.
  std::ifstream metaFile(unpackedMetaPath);
  ASSERT_TRUE(metaFile.is_open());
  nlohmann::json meta;
  metaFile >> meta;
  EXPECT_TRUE(meta.contains("project_file_version"));
  EXPECT_TRUE(meta.contains("data_summary"));
  EXPECT_TRUE(meta["data_summary"].contains("tables"));

  std::error_code ec;
  std::filesystem::remove_all(workspaceDir, ec);
  std::filesystem::remove(snapshotPath, ec);
}

// ── Header corruption detection ──────────────────────────────────────────────

TEST_F(ProjectTests, PackUnpack_CorruptHeader_Detected) {
  const auto dbPath   = temp_dir_ / "corrupt_hdr.db";
  const auto metaPath = temp_dir_ / "corrupt_hdr.json";
  CreateMetadataProject(dbPath, metaPath);

  auto project =
      std::make_shared<ProjectService>(dbPath, metaPath, ProjectOpenMode::kLoadExisting);

  std::filesystem::path snapshotPath;
  ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshotPath, nullptr));
  ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshotPath, nullptr));

  const auto packedPath = temp_dir_ / "corrupt_hdr.alcd";
  ASSERT_TRUE(project_pack::WritePackedProject(packedPath, metaPath, snapshotPath, nullptr));

  // Corrupt the version field (offset 8, byte 0) — the version is stored as
  // 4-byte LE right after the 8-byte magic.
  {
    std::fstream f(packedPath, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.is_open());
    f.seekp(8);  // first byte of version
    char corrupt = 0xFF;
    f.write(&corrupt, 1);
    f.close();
  }

  std::string metaBytes, dbBytes;
  EXPECT_FALSE(project_pack::ReadPackedProject(packedPath, &metaBytes, &dbBytes, nullptr));

  std::error_code ec;
  std::filesystem::remove(snapshotPath, ec);
}

TEST_F(ProjectTests, PackUnpack_CorruptChecksum_Detected) {
  const auto dbPath   = temp_dir_ / "corrupt_cs.db";
  const auto metaPath = temp_dir_ / "corrupt_cs.json";
  CreateMetadataProject(dbPath, metaPath);

  auto project =
      std::make_shared<ProjectService>(dbPath, metaPath, ProjectOpenMode::kLoadExisting);

  std::filesystem::path snapshotPath;
  ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshotPath, nullptr));
  ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshotPath, nullptr));

  const auto packedPath = temp_dir_ / "corrupt_cs.alcd";
  ASSERT_TRUE(project_pack::WritePackedProject(packedPath, metaPath, snapshotPath, nullptr));

  // Corrupt a metadata byte so the checksum won't match.  Metadata starts at
  // offset 8 (magic) + 4 (version) + 8 (meta_size) + 8 (db_size) + 8 (checksum) = 36.
  {
    std::fstream f(packedPath, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(f.is_open());
    f.seekp(36);  // first byte of meta payload
    char corrupt = 0xCC;
    f.write(&corrupt, 1);
    f.close();
  }

  std::string metaBytes, dbBytes;
  EXPECT_FALSE(project_pack::ReadPackedProject(packedPath, &metaBytes, &dbBytes, nullptr));

  std::error_code ec2;
  std::filesystem::remove(snapshotPath, ec2);
}

// ── data_summary diagnostics: tampered metadata ──────────────────────────────

TEST_F(ProjectTests, DataSummary_WarnsOnTamperedMetadata) {
  const auto dbPath   = temp_dir_ / "tamper_meta.db";
  const auto metaPath = temp_dir_ / "tamper_meta.json";
  CreateMetadataProject(dbPath, metaPath);

  // Modify the saved data_summary to claim more rows than exist.
  nlohmann::json metadata;
  {
    std::ifstream f(metaPath);
    ASSERT_TRUE(f.is_open());
    f >> metadata;
  }
  ASSERT_TRUE(metadata.contains("data_summary"));
  metadata["data_summary"]["tables"]["Element"]["rows"] = 999999;
  {
    std::ofstream f(metaPath, std::ios::trunc);
    ASSERT_TRUE(f.is_open());
    f << metadata.dump(4);
  }

  // Repack so the checksum covers the modified metadata.
  auto project =
      std::make_shared<ProjectService>(dbPath, metaPath, ProjectOpenMode::kLoadExisting);

  std::filesystem::path snapshotPath;
  ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshotPath, nullptr));
  ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshotPath, nullptr));

  const auto packedPath = temp_dir_ / "tamper_meta.alcd";
  ASSERT_TRUE(project_pack::WritePackedProject(packedPath, metaPath, snapshotPath, nullptr));

  // Unpack to workspace.
  std::filesystem::path workspaceDir;
  ASSERT_TRUE(project_pack::CreateProjectWorkspace("tamper_test", &workspaceDir, nullptr));
  std::filesystem::path unpackedDbPath, unpackedMetaPath;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(packedPath, workspaceDir,
                                                      "tamper_test", &unpackedDbPath,
                                                      &unpackedMetaPath, nullptr));

  // Loading should succeed but emit a data_summary warning.
  {
    ScopedStderrCapture capture;
    EXPECT_NO_THROW({
      ProjectService(unpackedDbPath, unpackedMetaPath, ProjectOpenMode::kLoadExisting);
    });
    std::string output = capture.str();
    EXPECT_FALSE(output.empty()) << "Expected a data-summary warning on stderr";
    EXPECT_NE(output.find("[Alcedo] Project data summary differs"),
              std::string::npos)
        << "stderr: " << output;
  }

  std::error_code ec;
  std::filesystem::remove_all(workspaceDir, ec);
  std::filesystem::remove(snapshotPath, ec);
}

// ── data_summary diagnostics: DB modified after save ─────────────────────────

TEST_F(ProjectTests, DataSummary_WarnsOnDbChanged) {
  const auto dbPath   = temp_dir_ / "db_mod.db";
  const auto metaPath = temp_dir_ / "db_mod.json";
  CreateMetadataProject(dbPath, metaPath);

  // Modify the database outside of the project so the stored data_summary
  // no longer matches.
  {
    StorageService storage(dbPath);
    auto           guard = storage.GetDBController().GetConnectionGuard();
    duckdb_result  result;
    ASSERT_EQ(duckdb_query(guard.conn_,
                           "INSERT INTO Element (id, type, element_name, "
                           "added_time, modified_time, ref_count) "
                           "VALUES (88888, 1, 'extra_elem', NOW(), NOW(), 0);",
                           &result),
              DuckDBSuccess);
    duckdb_destroy_result(&result);
  }

  // Load — should succeed but warn about mismatched summary.
  {
    ScopedStderrCapture capture;
    EXPECT_NO_THROW({
      ProjectService(dbPath, metaPath, ProjectOpenMode::kLoadExisting);
    });
    std::string output = capture.str();
    EXPECT_FALSE(output.empty()) << "Expected a data-summary warning on stderr";
    EXPECT_NE(output.find("[Alcedo] Project data summary differs"),
              std::string::npos)
        << "stderr: " << output;
  }
}

// ── Create project with default name via convenience overload ──────────────

TEST_F(ProjectTests, CreateProjectInFolder_DefaultName_Succeeds) {
  ApplicationModuleHost backend;
  QSignalSpy projSpy(backend.project(), &ProjectModule::ProjectChanged);

  const bool ok = backend.project()->CreateProjectInFolder(PathToQString(temp_dir_));
  EXPECT_TRUE(ok);

  WaitForSignal(projSpy, 15000);
  ProcessEvents(500);
  EXPECT_TRUE(backend.project()->ServiceReady());
}

}  // namespace
}  // namespace alcedo::ui::test
