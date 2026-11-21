//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <string>

namespace alcedo {

/**
 * @brief Read a stable identity of the volume that holds @p directory.
 *
 * The identity stays the same when the volume gets a new drive letter or mount point, so a mount
 * can find its volume again (Mounted Folder Plan, MF8). The source of the value:
 *
 * - Windows, local volume: the volume GUID path of `GetVolumeNameForVolumeMountPointW`, for
 *   example `\\?\Volume{0b6c...}\`.
 * - Windows, network share (a UNC path or a mapped drive): `\\server\share` in lower case.
 * - macOS: `NSURLVolumeUUIDStringKey`.
 * - Linux: the name of the `/dev/disk/by-uuid` entry whose block device holds the directory.
 *
 * Thread safety: the function holds no state; any thread can call it.
 *
 * @return The identity as UTF-8 text, or an empty string when the volume has no readable
 *         identity or the directory does not exist. The function does not throw.
 */
[[nodiscard]] auto ReadVolumeIdentity(const std::filesystem::path& directory) -> std::string;

}  // namespace alcedo
