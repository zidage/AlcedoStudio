//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Packed project save and open (#709): streaming of database payloads larger than 2 GiB, the
// header size check, the byte layout of existing packages, the project temp sweep, and the
// recovery of a failed DB snapshot.

#include "app/project_package_backend.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>
#include <xxhash.h>

#include <QString>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <json.hpp>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "app/project_service.hpp"
#include "../support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h and winioctl.h need the Windows types above.
#include <psapi.h>
#include <winioctl.h>
#else
#include <sys/resource.h>
#endif

namespace alcedo {
namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kGiB = 1024ULL * kMiB;

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

auto ReadAll(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

auto CurrentMetadataJson() -> std::string {
  nlohmann::json metadata;
  metadata["project_file_version"] = std::string(project_pack::kProjectFileVersion);
  return metadata.dump();
}

void AppendU32Le(std::string* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<char>((value >> (i * 8)) & 0xFFU));
  }
}

void AppendU64Le(std::string* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((value >> (i * 8)) & 0xFFULL));
  }
}

/// The packed project byte layout of project_package_backend.hpp, built in memory with one
/// XXH3 call over the concatenated bytes, as the code before #709 computed it.
auto BuildPackageBytes(const std::string& meta, const std::string& db) -> std::string {
  std::string hashed(project_pack::kPackedProjectMagic.begin(),
                     project_pack::kPackedProjectMagic.end());
  AppendU32Le(&hashed, project_pack::kPackedProjectVersion);
  AppendU64Le(&hashed, meta.size());
  AppendU64Le(&hashed, db.size());
  const std::string payload = meta + db;
  const uint64_t checksum = XXH3_64bits((hashed + payload).data(), hashed.size() + payload.size());
  std::string    package  = hashed;
  AppendU64Le(&package, checksum);
  return package + payload;
}

void SetModifiedHoursAgo(const std::filesystem::path& path, int hours) {
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now() - std::chrono::hours(hours));
}

auto PeakProcessMemoryBytes() -> uint64_t {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return 0;
  }
  return static_cast<uint64_t>(counters.PeakWorkingSetSize);
#else
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return static_cast<uint64_t>(usage.ru_maxrss);  // bytes on macOS
#endif
}

/// Creates @p path with @p size bytes and no allocated data (NTFS sparse file, APFS hole).
auto CreateSparseFile(const std::filesystem::path& path, uint64_t size) -> bool {
#ifdef _WIN32
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD      returned = 0;
  const bool sparse =
      DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
  CloseHandle(handle);
  if (!sparse) {
    return false;
  }
#else
  {
    std::ofstream create(path, std::ios::binary | std::ios::trunc);
  }
#endif
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  return !ec;
}

void WriteBytesAt(const std::filesystem::path& path, uint64_t offset, const std::string& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

auto ReadBytesAt(const std::filesystem::path& path, uint64_t offset, size_t count) -> std::string {
  std::ifstream file(path, std::ios::binary);
  file.seekg(static_cast<std::streamoff>(offset));
  std::string bytes(count, '\0');
  file.read(bytes.data(), static_cast<std::streamsize>(count));
  return bytes;
}

class ProjectPackageBackendTest : public ::testing::Test {
 protected:
  void SetUp() override { root_ = temp_dir_.Path(); }

  test::TestTempDirectory temp_dir_;
  std::filesystem::path   root_;
};

// ── Streaming of large payloads ─────────────────────────────────────────────

/// A database payload past 2 GiB (the old open limit) packs and unpacks byte for byte, and the
/// copy goes through fixed chunks: the process peak memory stays below half the payload size.
TEST_F(ProjectPackageBackendTest, DatabaseLargerThan2GiBRoundTripsWithBoundedMemory) {
  const uint64_t db_size = 2 * kGiB + 3 * kMiB + 7;
  // The package and the unpacked database are written in full (only the source is sparse).
  const auto     space   = std::filesystem::space(root_);
  if (space.available < 2 * db_size + kGiB) {
    GTEST_SKIP() << "Needs " << (2 * db_size + kGiB) / kMiB << " MiB free in " << root_ << ", has "
                 << space.available / kMiB << " MiB.";
  }

  const auto meta_path = root_ / "large.json";
  const auto db_path   = root_ / "large.db";
  WriteText(meta_path, CurrentMetadataJson());
  ASSERT_TRUE(CreateSparseFile(db_path, db_size));
  // Markers at the start, just past 2 GiB, and at the end; the rest of the file reads as zero.
  WriteBytesAt(db_path, 0, "head-marker");
  WriteBytesAt(db_path, 2 * kGiB + 1, "past-2GiB-marker");
  WriteBytesAt(db_path, db_size - 10, "end-marker");

  const auto packed_path = root_ / "large.alcd";
  QString    error;
  ASSERT_TRUE(project_pack::WritePackedProject(packed_path, meta_path, db_path, &error))
      << error.toStdString();
  EXPECT_EQ(
      std::filesystem::file_size(packed_path),
      project_pack::kPackedProjectHeaderBytes + std::filesystem::file_size(meta_path) + db_size);
  EXPECT_FALSE(std::filesystem::exists(root_ / "large.alcd.tmp"));
  std::error_code ec;
  std::filesystem::remove(db_path, ec);

  const auto            workspace = root_ / "workspace";
  std::filesystem::path unpacked_db;
  std::filesystem::path unpacked_meta;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(packed_path, workspace, "large", &unpacked_db,
                                                     &unpacked_meta, &error))
      << error.toStdString();
  EXPECT_EQ(std::filesystem::file_size(unpacked_db), db_size);
  EXPECT_EQ(ReadBytesAt(unpacked_db, 0, 11), "head-marker");
  EXPECT_EQ(ReadBytesAt(unpacked_db, 2 * kGiB + 1, 16), "past-2GiB-marker");
  EXPECT_EQ(ReadBytesAt(unpacked_db, db_size - 10, 10), "end-marker");
  EXPECT_EQ(ReadBytesAt(unpacked_db, kGiB, 4), std::string(4, '\0'));

  const uint64_t peak = PeakProcessMemoryBytes();
  EXPECT_GT(peak, 0U);
  EXPECT_LT(peak, db_size / 2) << "peak process memory " << peak / kMiB << " MiB";
}

// ── Header size check ───────────────────────────────────────────────────────

/// The header sizes must add up to the file size. A trailing byte, a database size one byte
/// too large, and a size that would need more memory than exists are rejected without a
/// database file left behind.
TEST_F(ProjectPackageBackendTest, HeaderSizesThatDoNotAddUpToTheFileSizeAreRejected) {
  const std::string meta            = CurrentMetadataJson();
  const std::string db              = "database payload";
  const std::string package         = BuildPackageBytes(meta, db);
  const auto        db_out          = root_ / "out.db";

  auto              expect_rejected = [&](const std::string& bytes, const char* expected_error) {
    const auto packed_path = root_ / "case.alcd";
    WriteText(packed_path, bytes);
    std::string meta_out;
    QString     error;
    EXPECT_FALSE(project_pack::ReadPackedProject(packed_path, &meta_out, db_out, &error));
    EXPECT_EQ(error.toStdString(), expected_error);
    EXPECT_FALSE(std::filesystem::exists(db_out));
  };

  expect_rejected(package + "x", "Packed project contains unexpected trailing data.");

  auto with_db_size = [&](uint64_t db_size) {
    std::string bytes = package;
    std::string field;
    AppendU64Le(&field, db_size);
    bytes.replace(8 + 4 + 8, 8, field);
    return bytes;
  };
  expect_rejected(with_db_size(db.size() + 1), "Packed project header is corrupted.");
  expect_rejected(with_db_size(1ULL << 62), "Packed project header is corrupted.");
  expect_rejected(package.substr(0, package.size() - 1), "Packed project header is corrupted.");

  std::string meta_out;
  QString     error;
  const auto  packed_path = root_ / "valid.alcd";
  WriteText(packed_path, package);
  ASSERT_TRUE(project_pack::ReadPackedProject(packed_path, &meta_out, db_out, &error))
      << error.toStdString();
  EXPECT_EQ(meta_out, meta);
  EXPECT_EQ(ReadAll(db_out), db);
}

// ── Byte layout of existing packages ────────────────────────────────────────

/// A package of the current version laid out as before #709 (one-shot checksum over the
/// concatenated bytes) opens, and the streaming writer produces the same bytes.
TEST_F(ProjectPackageBackendTest, PackageInTheExistingByteLayoutOpensAndWriterMatchesIt) {
  const std::string meta = CurrentMetadataJson();
  std::string       db(3 * kMiB + 123, '\0');
  for (size_t i = 0; i < db.size(); ++i) {
    db[i] = static_cast<char>((i * 131U) & 0xFFU);
  }
  const std::string expected_package = BuildPackageBytes(meta, db);

  const auto        old_package      = root_ / "old.alcd";
  WriteText(old_package, expected_package);
  const auto            workspace = root_ / "workspace";
  std::filesystem::path unpacked_db;
  std::filesystem::path unpacked_meta;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(old_package, workspace, "old", &unpacked_db,
                                                     &unpacked_meta, &error))
      << error.toStdString();
  EXPECT_EQ(ReadAll(unpacked_db), db);

  const auto meta_path = root_ / "new.json";
  const auto db_path   = root_ / "new.db";
  WriteText(meta_path, meta);
  WriteText(db_path, db);
  const auto new_package = root_ / "new.alcd";
  ASSERT_TRUE(project_pack::WritePackedProject(new_package, meta_path, db_path, &error))
      << error.toStdString();
  EXPECT_EQ(ReadAll(new_package), expected_package);
}

/// A save whose database file cannot be read leaves no `.alcd.tmp` and keeps the previous
/// package.
TEST_F(ProjectPackageBackendTest, FailedWriteRemovesTempFileAndKeepsPackage) {
  const auto meta_path   = root_ / "p.json";
  const auto packed_path = root_ / "p.alcd";
  WriteText(meta_path, CurrentMetadataJson());
  WriteText(packed_path, "previous package");

  QString error;
  EXPECT_FALSE(
      project_pack::WritePackedProject(packed_path, meta_path, root_ / "missing.db", &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_FALSE(std::filesystem::exists(root_ / "p.alcd.tmp"));
  EXPECT_EQ(ReadAll(packed_path), "previous package");
}

// ── Project temp sweep ──────────────────────────────────────────────────────

/// The sweep keeps a workspace whose owner lock is held and every entry modified after the
/// process started, and removes workspaces without a holder and stale DB snapshots.
TEST_F(ProjectPackageBackendTest, SweepRemovesOnlyAbandonedWorkspacesAndSnapshots) {
  const auto held = root_ / "runtime_held_1_1";
  std::filesystem::create_directories(held / "editor-journal");
  WriteText(held / "held.db", "db");
  project_pack::ProjectWorkspaceLock held_lock;
  QString                            error;
  ASSERT_TRUE(held_lock.Acquire(held, &error)) << error.toStdString();
  ASSERT_TRUE(held_lock.held());

  const auto released = root_ / "runtime_released_1_2";
  std::filesystem::create_directories(released / "editor-journal");
  WriteText(released / "released.db", "db");
  {
    project_pack::ProjectWorkspaceLock released_lock;
    ASSERT_TRUE(released_lock.Acquire(released, &error)) << error.toStdString();
  }

  // A workspace of an Alcedo version without owner locks.
  const auto unlocked = root_ / "runtime_unlocked_1_3";
  std::filesystem::create_directories(unlocked / "editor-journal");
  WriteText(unlocked / "unlocked.json", "{}");

  // Created after the process started, as a workspace before its lock exists.
  const auto recent = root_ / "runtime_recent_1_4";
  std::filesystem::create_directories(recent);

  const auto snapshot     = root_ / "pack_snapshot_1_5.db";
  const auto snapshot_wal = root_ / "pack_snapshot_1_5.db.wal";
  WriteText(snapshot, "snapshot");
  WriteText(snapshot_wal, "wal");
  const auto recent_snapshot = root_ / "pack_snapshot_1_6.db";
  WriteText(recent_snapshot, "snapshot");
  const auto unrelated = root_ / "unrelated.db";
  WriteText(unrelated, "other");

  for (const auto& path : {held, released, unlocked, snapshot, snapshot_wal, unrelated}) {
    SetModifiedHoursAgo(path, 1);
  }

#ifdef _WIN32
  // A workspace without a lock whose database is open stays. On macOS the check reads the
  // fcntl locks of other processes only, so this process cannot stand in for the holder there.
  const auto open_unlocked = root_ / "runtime_open_unlocked_1_7";
  std::filesystem::create_directories(open_unlocked);
  WriteText(open_unlocked / "open.db", "db");
  std::ifstream open_db(open_unlocked / "open.db", std::ios::binary);
  ASSERT_TRUE(open_db.is_open());
  SetModifiedHoursAgo(open_unlocked, 1);
#endif

  project_pack::RemoveAbandonedProjectTempFiles(root_);

  EXPECT_TRUE(std::filesystem::exists(held / "held.db"));
  EXPECT_TRUE(std::filesystem::exists(held / project_pack::kProjectWorkspaceLockFileName));
  EXPECT_FALSE(std::filesystem::exists(released));
  EXPECT_FALSE(std::filesystem::exists(unlocked));
  EXPECT_TRUE(std::filesystem::exists(recent));
  EXPECT_FALSE(std::filesystem::exists(snapshot));
  EXPECT_FALSE(std::filesystem::exists(snapshot_wal));
  EXPECT_TRUE(std::filesystem::exists(recent_snapshot));
  EXPECT_TRUE(std::filesystem::exists(unrelated));
#ifdef _WIN32
  EXPECT_TRUE(std::filesystem::exists(open_unlocked / "open.db"));
  open_db.close();
#endif

  // Once its holder releases the lock, the next sweep removes the workspace.
  held_lock.Release();
  SetModifiedHoursAgo(held, 1);
  project_pack::RemoveAbandonedProjectTempFiles(root_);
  EXPECT_FALSE(std::filesystem::exists(held));
}

/// CreateProjectWorkspace returns the workspace with its owner lock held.
TEST_F(ProjectPackageBackendTest, CreatedWorkspaceHoldsItsOwnerLock) {
  std::filesystem::path              workspace;
  project_pack::ProjectWorkspaceLock lock;
  QString                            error;
  ASSERT_TRUE(project_pack::CreateProjectWorkspace("owner_lock_test", &workspace, &lock, &error))
      << error.toStdString();
  EXPECT_TRUE(lock.held());
  EXPECT_EQ(workspace.parent_path(), project_pack::ProjectTempRoot());
  EXPECT_TRUE(std::filesystem::exists(workspace / project_pack::kProjectWorkspaceLockFileName));

  // A second holder cannot take the lock while the first one holds it.
  project_pack::ProjectWorkspaceLock second;
  EXPECT_FALSE(second.Acquire(workspace, &error));

  lock.Release();
  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  EXPECT_FALSE(ec) << ec.message();
}

// ── DB snapshot failure ─────────────────────────────────────────────────────

auto RunSql(const std::shared_ptr<ProjectService>& project, const std::string& sql) -> bool {
  auto&         db_ctrl = StorageTestAccess::GetDatabase(*project->GetStorage());
  auto          guard   = db_ctrl.GetConnectionGuard();
  auto          lock    = guard.Lock();
  duckdb_result result;
  const bool    ok = duckdb_query(guard.conn_, sql.c_str(), &result) == DuckDBSuccess;
  duckdb_destroy_result(&result);
  return ok;
}

auto CountAttachedSnapshots(const std::shared_ptr<ProjectService>& project) -> int64_t {
  auto&         db_ctrl = StorageTestAccess::GetDatabase(*project->GetStorage());
  auto          guard   = db_ctrl.GetConnectionGuard();
  auto          lock    = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_,
                   "SELECT COUNT(*) FROM duckdb_databases() WHERE database_name = "
                   "'pack_snapshot';",
                   &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return -1;
  }
  const int64_t count = duckdb_value_int64(&result, 0, 0);
  duckdb_destroy_result(&result);
  return count;
}

/// A snapshot write that fails after the ATTACH (as on a full temp drive) detaches the snapshot
/// and removes its database file, so the next snapshot on the same connection succeeds.
TEST_F(ProjectPackageBackendTest, FailedSnapshotWriteDetachesSoNextSnapshotSucceeds) {
  const auto db_path   = root_ / "snapshot_failure.db";
  const auto meta_path = root_ / "snapshot_failure.json";
  {
    auto project =
        std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
    project->SaveProject(meta_path);
  }
  auto project =
      std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);

  // A non-empty directory where the snapshot WAL must go: the ATTACH succeeds, the first
  // write of the snapshot fails, and the snapshot cannot remove the directory.
  const auto snapshot_path = root_ / "pack_snapshot_failure.db";
  auto       wal_path      = snapshot_path;
  wal_path += ".wal";
  std::filesystem::create_directories(wal_path);
  WriteText(wal_path / "keep", "x");

  QString error;
  EXPECT_FALSE(project_pack::CreateLiveDbSnapshot(project, snapshot_path, &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_EQ(CountAttachedSnapshots(project), 0) << error.toStdString();
  EXPECT_FALSE(std::filesystem::exists(snapshot_path)) << error.toStdString();

  std::filesystem::remove_all(wal_path);
  error.clear();
  EXPECT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshot_path, &error))
      << error.toStdString();
  EXPECT_EQ(CountAttachedSnapshots(project), 0);
  EXPECT_TRUE(std::filesystem::exists(snapshot_path));
  project.reset();
}

}  // namespace
}  // namespace alcedo
