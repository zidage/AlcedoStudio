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

#include <duckdb.h>

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
constexpr uint64_t kMaxPackedComponentBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
// 0.4.0 is a destructive cutover: full PipelineDocument root and checkpoint replace
// CPU-parameter snapshots. Older project packages are rejected with no migration.
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

auto CreateProjectWorkspace(const QString& projectName,
                            std::filesystem::path* workspaceOut,
                            QString* errorOut) -> bool;
auto BuildRuntimeProjectPair(const std::filesystem::path& workspace,
                             const QString& projectName)
    -> std::pair<std::filesystem::path, std::filesystem::path>;

auto RunDuckDbQuery(duckdb_connection conn, const std::string& sql,
                    const char* stage, QString* errorOut) -> bool;
auto QueryCurrentCatalog(duckdb_connection conn, std::string* catalogOut,
                         QString* errorOut) -> bool;

// Best-effort load of the DuckDB vss extension when the project DB already has
// an HNSW index (the semantic embedding tables use HNSW). Without vss loaded,
// any write to such a table fails with "Cannot bind index '...', unknown index
// type 'HNSW'" — which silently breaks semantic-generation embedding persists
// on a normal project open (snapshot import already loads vss). Returns true if
// no HNSW index is present OR vss loaded successfully; false (with errorOut set)
// only when an HNSW index exists but vss could not be loaded. Callers should
// treat false as non-fatal (log a warning) — the project is still usable for
// non-semantic work, but embedding writes will fail until vss is available.
auto EnsureVssExtensionForExistingHnswIndexes(duckdb_connection conn, QString* errorOut) -> bool;

auto BuildTempDbSnapshotPath(std::filesystem::path* snapshotPathOut,
                             QString* errorOut) -> bool;
auto CreateLiveDbSnapshot(const std::shared_ptr<ProjectService>& project,
                          const std::filesystem::path& snapshotPath,
                          QString* errorOut) -> bool;

auto WritePackedProject(const std::filesystem::path& packedPath,
                        const std::filesystem::path& metaPath,
                        const std::filesystem::path& dbPath,
                        QString* errorOut) -> bool;
auto ReadPackedProject(const std::filesystem::path& packedPath,
                       std::string* metaBytes, std::string* dbBytes,
                       QString* errorOut) -> bool;

auto UnpackProjectToWorkspace(const std::filesystem::path& packedPath,
                              const std::filesystem::path& workspaceDir,
                              const QString& projectName,
                              std::filesystem::path* dbPathOut,
                              std::filesystem::path* metaPathOut,
                              QString* errorOut) -> bool;

}  // namespace alcedo::project_pack
