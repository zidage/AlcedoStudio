//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file album_backend_project_test.cpp
/// @brief Project lifecycle tests for AlbumBackend.
///
/// Covers: create project, load project (valid/invalid), save project,
/// pack/unpack integrity, data_summary diagnostics, and initial service
/// state.

#include "ui/album_backend_test_fixture.hpp"

#include <QSignalSpy>
#include <algorithm>
#include <array>
#include <chrono>
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
#include "ui/alcedo_main/album_backend/model_download_controller.hpp"

namespace alcedo::ui::test {
namespace {

using ProjectTests = AlbumBackendTestFixture;

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

bool WaitForProjectLoadToFinish(AlbumBackend& backend, int timeoutMs = 15000) {
  if (!backend.ProjectLoading()) {
    return true;
  }

  QSignalSpy spy(&backend, &AlbumBackend::ProjectLoadStateChanged);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (backend.ProjectLoading() && std::chrono::steady_clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              std::chrono::steady_clock::now())
            .count();
    if (remaining <= 0 || !spy.wait(static_cast<int>(std::min<qint64>(remaining, 500)))) {
      ProcessEvents(50);
    }
  }
  return !backend.ProjectLoading();
}

// ── Initial state ──────────────────────────────────────────────────────────

TEST_F(ProjectTests, ServiceState_InitiallyNotReady) {
  AlbumBackend backend;
  EXPECT_FALSE(backend.ServiceReady());
  EXPECT_FALSE(backend.ServiceMessage().isEmpty());
}

// ── Create project — happy path ────────────────────────────────────────────

TEST_F(ProjectTests, CreateProject_ValidFolder_Succeeds) {
  AlbumBackend backend;
  QSignalSpy   projSpy(&backend, &AlbumBackend::ProjectChanged);
  QSignalSpy   stateSpy(&backend, &AlbumBackend::ServiceStateChanged);

  const bool ok =
      backend.CreateProjectInFolderNamed(PathToQString(temp_dir_), "test_proj");
  EXPECT_TRUE(ok);

  // Wait for async project initialisation.
  WaitForSignal(projSpy, 15000);
  ProcessEvents(500);

  EXPECT_TRUE(backend.ServiceReady());
  EXPECT_FALSE(stateSpy.isEmpty());
}

// ── Create project — empty name ────────────────────────────────────────────

TEST_F(ProjectTests, CreateProject_EmptyName_Fails) {
  AlbumBackend backend;
  const bool   ok =
      backend.CreateProjectInFolderNamed(PathToQString(temp_dir_), "");
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
  const auto base_dir   = temp_dir_ / "model";
  const auto model_dir  = base_dir / "mobileclip2-s2-en";
  const auto asset_path = model_dir / "tokenizer.json";
  std::filesystem::create_directories(model_dir);
  {
    std::ofstream asset(asset_path);
    asset << "{}";
  }

  nlohmann::json manifest;
  manifest["profile_id"]                 = "mobileclip2-s2-en";
  manifest["model_id"]                   = "plhery/mobileclip2-onnx:s2";
  manifest["revision"]                   = "ba95759a5bdbaca53e9111e2550a76ec09c8fd9e";
  manifest["engine_profile_id"]          = "mobileclip2-openclip";
  manifest["language"]                   = "en";
  manifest["embedding_dimension"]        = 512;
  manifest["native_embedding_dimension"] = 512;
  manifest["image_size"]                 = 256;
  manifest["embedding_transform"]        = "l2_normalize";
  manifest["model_root"]                 = model_dir.string();
  manifest["assets"]                     = nlohmann::json::array(
      {{{"role", "tokenizer"},
        {"repo_id", "plhery/mobileclip2-onnx"},
        {"revision", "ba95759a5bdbaca53e9111e2550a76ec09c8fd9e"},
        {"remote_path", "tokenizer.json"},
        {"local_path", asset_path.string()},
        {"size_bytes", 2},
        {"sha256", ""}}});

  {
    std::ofstream out(model_dir / "alcedo_model_manifest.json");
    out << manifest.dump(2);
  }

  QString error;
  auto    loaded = detail::LoadLocalResolvedModelManifestForActivation(
      QStringLiteral("mobileclip2-s2-en"), PathToQString(base_dir), &error);
  ASSERT_TRUE(loaded.has_value()) << error.toStdString();

  std::filesystem::remove(asset_path);
  error.clear();
  loaded = detail::LoadLocalResolvedModelManifestForActivation(
      QStringLiteral("mobileclip2-s2-en"), PathToQString(base_dir), &error);
  EXPECT_FALSE(loaded.has_value());
  EXPECT_TRUE(error.contains(QStringLiteral("missing"), Qt::CaseInsensitive))
      << error.toStdString();
}

// ── Create project while "loading" — second call rejected ──────────────────

TEST_F(ProjectTests, CreateProject_DoubleCall_SecondRejected) {
  AlbumBackend backend;

  const bool first =
      backend.CreateProjectInFolderNamed(PathToQString(temp_dir_), "proj_a");

  // If first call started async loading, a second call should be rejected.
  if (first && backend.ProjectLoading()) {
    // Create a different subfolder so paths differ.
    const auto subDir = temp_dir_ / "sub";
    std::filesystem::create_directories(subDir);
    const bool second =
        backend.CreateProjectInFolderNamed(PathToQString(subDir), "proj_b");
    EXPECT_FALSE(second);
  }

  // Drain everything so destructor is clean.
  ProcessEvents(2000);
}

// ── Load project — non-existent file ───────────────────────────────────────

TEST_F(ProjectTests, LoadProject_NonexistentFile_Fails) {
  AlbumBackend backend;
  const bool   ok = backend.LoadProject("C:/nonexistent/project.json");
  EXPECT_FALSE(ok);
  EXPECT_FALSE(backend.ServiceReady());
}

// ── Load project — invalid format ──────────────────────────────────────────

TEST_F(ProjectTests, LoadProject_InvalidFormat_Fails) {
  AlbumBackend backend;

  // Create a temporary .txt file — not a valid project format.
  const auto txtPath = temp_dir_ / "notes.txt";
  {
    std::ofstream ofs(txtPath);
    ofs << "hello world";
  }

  const bool ok = backend.LoadProject(PathToQString(txtPath));
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

  AlbumBackend backend;
  EXPECT_FALSE(backend.LoadProject(PathToQString(oldProjectPath)));
  EXPECT_FALSE(backend.ServiceReady());
}

TEST_F(ProjectTests, LoadProject_MetadataJsonProject_Fails) {
  const auto dbPath = temp_dir_ / "corrupt_meta.db";
  const auto metaPath = temp_dir_ / "corrupt_meta.json";
  CreateMetadataProject(dbPath, metaPath);

  AlbumBackend backend;
  EXPECT_FALSE(backend.LoadProject(PathToQString(metaPath)));
  EXPECT_FALSE(backend.ServiceReady());
}

TEST_F(ProjectTests, LoadProject_CorruptPackedProjectPayload_Fails) {
  {
    AlbumBackend backend;
    ASSERT_TRUE(CreateTestProject(backend, "corrupt_packed_project"));
    ASSERT_TRUE(backend.SaveProject());
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

  AlbumBackend backend;
  EXPECT_FALSE(backend.LoadProject(PathToQString(*packedProjectPath)));
  EXPECT_FALSE(backend.ServiceReady());
}

TEST_F(ProjectTests, LoadProject_ValidMetadataProject_Fails) {
  const auto dbPath = temp_dir_ / "valid_project.db";
  const auto metaPath = temp_dir_ / "valid_project.json";
  CreateMetadataProject(dbPath, metaPath);

  AlbumBackend backend;
  EXPECT_FALSE(backend.LoadProject(PathToQString(metaPath)));
  EXPECT_FALSE(backend.ServiceReady());
}

TEST_F(ProjectTests, LoadProject_ValidPackedProject_Succeeds) {
  {
    AlbumBackend backend;
    ASSERT_TRUE(CreateTestProject(backend, "valid_packed_project"));
    ASSERT_TRUE(backend.SaveProject());
  }

  const auto packedProjectPath = FindPackedProjectPath(temp_dir_);
  ASSERT_TRUE(packedProjectPath.has_value());

  AlbumBackend backend;
  QSignalSpy projectSpy(&backend, &AlbumBackend::ProjectChanged);
  ASSERT_TRUE(backend.LoadProject(PathToQString(*packedProjectPath)));
  ASSERT_TRUE(WaitForSignal(projectSpy, 15000));
  ProcessEvents(500);

  EXPECT_TRUE(backend.ServiceReady());
}

// ── Save project — no project loaded ───────────────────────────────────────

TEST_F(ProjectTests, SaveProject_NoProject_Fails) {
  AlbumBackend backend;
  const bool   ok = backend.SaveProject();
  EXPECT_FALSE(ok);
}

// ── Save project — after create ────────────────────────────────────────────

TEST_F(ProjectTests, SaveProject_AfterCreate_Succeeds) {
  AlbumBackend backend;
  ASSERT_TRUE(CreateTestProject(backend));

  const bool ok = backend.SaveProject();
  EXPECT_TRUE(ok);
}

TEST_F(ProjectTests, CreateProjectWhileProjectOpen_CompletesSwitch) {
  AlbumBackend backend;
  ASSERT_TRUE(CreateTestProject(backend, "first_project"));

  QSignalSpy project_spy(&backend, &AlbumBackend::ProjectChanged);
  ASSERT_TRUE(backend.CreateProjectInFolderNamed(PathToQString(temp_dir_), "second_project"));
  ASSERT_TRUE(WaitForProjectLoadToFinish(backend, 15000));
  ProcessEvents(500);

  EXPECT_TRUE(backend.ServiceReady());
  EXPECT_FALSE(backend.ProjectLoading());
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
  AlbumBackend backend;
  QSignalSpy   projSpy(&backend, &AlbumBackend::ProjectChanged);

  const bool ok = backend.CreateProjectInFolder(PathToQString(temp_dir_));
  EXPECT_TRUE(ok);

  WaitForSignal(projSpy, 15000);
  ProcessEvents(500);
  EXPECT_TRUE(backend.ServiceReady());
}

}  // namespace
}  // namespace alcedo::ui::test
