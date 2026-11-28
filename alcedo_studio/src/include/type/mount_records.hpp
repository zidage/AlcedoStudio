//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

#include "type/type.hpp"

namespace alcedo {

/**
 * @brief One mounted folder: the binding of one collection to one disk directory.
 *
 * The stored form is one `MountRoot` row (MountStore). Mounted Folder Plan, section 2.5.
 */
struct MountRootRecord {
  mount_id_t                 id             = 0;
  /// The folder element of the mount root. It holds `mount_id == id`.
  sl_element_id_t            root_folder_id = 0;
  /// Identity of the volume that holds the root directory; empty when the volume has none.
  std::string                volume_identity;
  /// Absolute path of the root directory.
  std::filesystem::path      root_path;
  /// End of the last completed scan (UTC, seconds); empty before the first scan completes.
  std::optional<std::time_t> last_scan_at;
  /// The display name of the volume when CreateMount or the last scan that reached it read it
  /// (ReadVolumeLabel). It stays while the volume is offline. Empty when the volume has no name.
  std::string                volume_name;

  auto                       operator==(const MountRootRecord&) const -> bool = default;
};

}  // namespace alcedo
