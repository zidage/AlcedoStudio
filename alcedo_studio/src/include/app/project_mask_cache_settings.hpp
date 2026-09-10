//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

namespace alcedo {

/// Directory name under the user-selected root that owns every project UUID namespace.
inline constexpr std::string_view kProjectMaskCacheDirectoryName = "alcedo-mask-cache";
/// Published Mix-cache file suffix. Distinct from persistent `.r8mask` assets.
inline constexpr std::string_view kProjectMaskCacheFileExtension = ".r8cache";

enum class ProjectMaskCacheRetention : std::uint8_t {
  Keep = 0,
  DeleteOnProjectClose = 1,
};

enum class ProjectMaskCacheRemovalChoice : std::uint8_t {
  KeepFiles = 0,
  DeleteFiles = 1,
};

/**
 * @brief Per-project Mix-cache location and close policy owned by ProjectService.
 *
 * Settings are project metadata, not photo history. An empty @ref chosen_root
 * means the persistent metadata directory. @ref previous_roots records namespaces
 * whose cleanup failed after a successful root change so they stay manageable.
 */
struct ProjectMaskCacheSettings {
  std::filesystem::path              chosen_root;
  ProjectMaskCacheRetention          retention = ProjectMaskCacheRetention::Keep;
  std::uint64_t                      settings_revision = 0;
  std::vector<std::filesystem::path> previous_roots;
  bool                               close_cleanup_pending = false;
};

/**
 * @brief Default user-selected root: the directory that contains @p meta_path.
 *
 * @pre @p meta_path is the persistent project metadata file.
 */
[[nodiscard]] auto DefaultProjectMaskCacheChosenRoot(const std::filesystem::path& meta_path)
    -> std::filesystem::path;

/**
 * @brief Resolve @p settings.chosen_root against @p meta_path.
 *
 * Relative stored roots are interpreted from the metadata directory. Absolute
 * roots are returned as stored. Does not create directories or substitute temp.
 */
[[nodiscard]] auto ResolveProjectMaskCacheChosenRoot(const ProjectMaskCacheSettings& settings,
                                                     const std::filesystem::path& meta_path)
    -> std::filesystem::path;

/**
 * @brief Dedicated namespace `<chosen-root>/alcedo-mask-cache/<project-uuid>`.
 *
 * @throws std::invalid_argument when @p project_uuid is empty or contains a path
 *         separator or parent traversal.
 */
[[nodiscard]] auto ProjectMaskCacheNamespaceDirectory(const std::filesystem::path& chosen_root,
                                                      std::string_view project_uuid)
    -> std::filesystem::path;

/**
 * @brief Write @p settings under `mask_cache` in reconstructed project metadata.
 *
 * Empty roots persist as an empty string so Save cannot drop the key. Paths are
 * stored relative to the metadata directory when they lie inside it.
 */
void ApplyProjectMaskCacheSettingsToMetadata(nlohmann::json& metadata,
                                             const ProjectMaskCacheSettings& settings,
                                             const std::filesystem::path& meta_path);

/**
 * @brief Read `mask_cache` from @p metadata. Missing keys yield defaults.
 *
 * Does not migrate thumbnail cache keys. Unknown retention values throw.
 */
[[nodiscard]] auto ReadProjectMaskCacheSettingsFromMetadata(const nlohmann::json& metadata,
                                                            const std::filesystem::path& meta_path)
    -> ProjectMaskCacheSettings;

}  // namespace alcedo
