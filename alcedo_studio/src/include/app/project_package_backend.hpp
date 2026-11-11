//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "edit/history/pipeline_history_format.hpp"

class QString;
namespace alcedo {
class ProjectService;
}

namespace alcedo::project_pack {

constexpr std::wstring_view kPackedProjectExtension = L".alcd";
constexpr std::array<char, 8> kPackedProjectMagic{
    {'P', 'U', 'E', 'R', 'H', 'P', 'K', '1'}};
constexpr uint32_t kPackedProjectVersion = kPackedProjectFormatVersion;
// Byte layout of a packed project: magic (8B) || version (u32 LE) || meta_size (u64 LE) ||
// db_size (u64 LE) || checksum (u64 LE) || meta bytes || db bytes. The file size is exactly
// kPackedProjectHeaderBytes + meta_size + db_size.
constexpr uint64_t kPackedProjectHeaderBytes    = 8 + 4 + 8 + 8 + 8;
constexpr uint64_t kPackedProjectChecksumOffset = 8 + 4 + 8 + 8;
// The metadata payload is JSON that the open parses in memory. The database payload has no
// size limit: save and open copy it in chunks.
constexpr uint64_t kMaxPackedMetadataBytes = 256ULL * 1024ULL * 1024ULL;
// 0.10.0 is a destructive cutover: project data stores DNG color profile
// fingerprints instead of the profile tables, and 0.9.0 or older project
// metadata is rejected at the versioned read boundary with no migration. 0.11.0 adds the
// library mark columns; 0.10.0 opens and gains them (see pipeline_history_format.hpp).
constexpr std::string_view kProjectFileVersion = ::alcedo::kProjectFileVersion;
constexpr std::string_view kMinSupportedProjectFileVersion =
    ::alcedo::kMinSupportedProjectFileVersion;
constexpr std::string_view kMaxSupportedProjectFileVersion =
    ::alcedo::kMaxSupportedProjectFileVersion;

auto IsMetadataJsonPath(const std::filesystem::path& path) -> bool;
auto IsPackedProjectPath(const std::filesystem::path& path) -> bool;
auto IsPackedProjectFile(const std::filesystem::path& path) -> bool;
auto IsSupportedProjectFile(const std::filesystem::path& path) -> bool;

auto ReadFileBytes(const std::filesystem::path& path, std::string* out) -> bool;
auto WriteFileBytes(const std::filesystem::path& path, const std::string& data) -> bool;
auto ComputeFileChecksum(const std::filesystem::path& path, uint64_t* checksumOut) -> bool;
auto FormatChecksum(uint64_t checksum) -> std::string;
auto ProjectVersionIsSupported(std::string_view version) -> bool;

auto BuildUniquePackedProjectPath(const std::filesystem::path& folder,
                                  const QString& projectName,
                                  QString* errorOut) -> std::optional<std::filesystem::path>;
auto BuildBundlePathFromMetaPath(const std::filesystem::path& metaPath) -> std::filesystem::path;

/// Exclusive hold on `<workspace>/owner.lock` while a project uses the workspace. Several
/// processes share the project temp root (the GUI, the headless host of alcedo-cli, test
/// hosts); RemoveAbandonedProjectTempFiles removes a workspace only when no process holds its
/// lock. Windows opens the file with share mode 0, macOS holds an flock(LOCK_EX).
class ProjectWorkspaceLock {
 public:
  ProjectWorkspaceLock() = default;
  ~ProjectWorkspaceLock();
  ProjectWorkspaceLock(ProjectWorkspaceLock&& other) noexcept;
  auto operator=(ProjectWorkspaceLock&& other) noexcept -> ProjectWorkspaceLock&;
  ProjectWorkspaceLock(const ProjectWorkspaceLock&)                    = delete;
  auto operator=(const ProjectWorkspaceLock&) -> ProjectWorkspaceLock& = delete;

  /// Creates `owner.lock` in @p workspace and holds it, after releasing a lock this object
  /// held. Returns false and writes @p errorOut when the file cannot be created or locked.
  auto Acquire(const std::filesystem::path& workspace, QString* errorOut) -> bool;
  [[nodiscard]] auto held() const -> bool;
  /// Closes the lock file. Call it before the workspace is removed: Windows refuses to delete
  /// an open file.
  void Release();

 private:
#ifdef _WIN32
  void* handle_ = nullptr;  // HANDLE of owner.lock
#else
  int fd_ = -1;
#endif
};

constexpr std::string_view kProjectWorkspaceLockFileName = "owner.lock";

/// `<temp>/alcedo_main`: the runtime workspaces (`runtime_*`) and the save snapshots
/// (`pack_snapshot_*.db`) of every Alcedo process.
auto ProjectTempRoot() -> std::filesystem::path;

/// Removes the temp data that no live process uses from @p root:
/// - each `runtime_*` directory whose owner.lock no process holds (or that has none);
/// - each `pack_snapshot_*.db` and `pack_snapshot_*.db.wal` file that no process holds open.
/// An entry modified after this process started is left alone, so the entries this process
/// creates (a workspace before its lock exists, a snapshot between its write and its pack)
/// are never removed by its own sweep. A directory that holds a `.db` file that another open
/// holds also stays. Logs each removal and each failure.
void RemoveAbandonedProjectTempFiles(const std::filesystem::path& root);

/// Removes the abandoned temp data of ProjectTempRoot(), then creates a new
/// `runtime_<name>_<ms>_<nonce>` workspace in it and acquires its owner lock into @p lockOut.
/// Keep the lock for as long as the project uses the workspace.
auto CreateProjectWorkspace(const QString& projectName, std::filesystem::path* workspaceOut,
                            ProjectWorkspaceLock* lockOut, QString* errorOut) -> bool;
auto BuildRuntimeProjectPair(const std::filesystem::path& workspace,
                             const QString& projectName)
    -> std::pair<std::filesystem::path, std::filesystem::path>;

auto BuildTempDbSnapshotPath(std::filesystem::path* snapshotPathOut,
                             QString* errorOut) -> bool;
/// Copies the live project database into @p snapshotPath through
/// StorageMaintenance::WriteSnapshot (ATTACH, COPY FROM DATABASE, CHECKPOINT, DETACH). On a
/// failure the snapshot is detached from the live connection, so a later snapshot can attach the
/// alias again, the snapshot file and its `.wal` are removed, and @p errorOut names the failed
/// step.
auto CreateLiveDbSnapshot(const std::shared_ptr<ProjectService>& project,
                          const std::filesystem::path& snapshotPath,
                          QString* errorOut) -> bool;

/// Writes the package to `<packedPath>.tmp` and renames it to @p packedPath. Both payloads are
/// copied from their files in 1 MiB chunks, so the memory use does not depend on the database
/// size. A failure removes the `.tmp` file and leaves @p packedPath as it was.
auto WritePackedProject(const std::filesystem::path& packedPath,
                        const std::filesystem::path& metaPath,
                        const std::filesystem::path& dbPath,
                        QString* errorOut) -> bool;
/// Reads the metadata payload of @p packedPath into @p metaBytes and copies the database
/// payload to @p dbOutPath in 1 MiB chunks, then verifies the checksum. Rejects a header whose
/// sizes do not add up to the file size. A failure removes @p dbOutPath.
auto ReadPackedProject(const std::filesystem::path& packedPath, std::string* metaBytes,
                       const std::filesystem::path& dbOutPath, QString* errorOut) -> bool;

auto UnpackProjectToWorkspace(const std::filesystem::path& packedPath,
                              const std::filesystem::path& workspaceDir,
                              const QString& projectName,
                              std::filesystem::path* dbPathOut,
                              std::filesystem::path* metaPathOut,
                              QString* errorOut) -> bool;

}  // namespace alcedo::project_pack
