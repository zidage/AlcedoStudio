//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

#include "type/hash_type.hpp"
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

/// The two states of a source record. The stored values are the `SourceFile.state` codes.
enum class SourceFileState : std::uint8_t {
  kPresent = 0,
  kMissing = 1,
};

/**
 * @brief The recorded source of one file element: the values that the last scan read from the
 *        file at `Image.image_path`.
 *
 * The stored form is one `SourceFile` row (MountStore). A mounted file always has a record; an
 * ordinary file has one only after an operation recorded it.
 */
struct SourceFileRecord {
  sl_element_id_t            element_id          = 0;
  std::uint64_t              byte_size           = 0;
  /// Modification time of the file in nanoseconds since the Unix epoch.
  std::int64_t               modified_ns         = 0;
  Hash128                    fingerprint         = {};
  /// Version of the fingerprint definition that produced @ref fingerprint.
  std::uint32_t              fingerprint_version = 0;
  SourceFileState            state               = SourceFileState::kPresent;
  /// Time when the file became missing (UTC, seconds). Set exactly when @ref state is
  /// SourceFileState::kMissing.
  std::optional<std::time_t> missing_since;

  auto                       operator==(const SourceFileRecord&) const -> bool = default;
};

/// The location of one file element: its image and `Image.image_path`. A read result of
/// MountStore; the scan compares it with a listing.
struct FileLocationRecord {
  sl_element_id_t       element_id = 0;
  image_id_t            image_id   = 0;
  std::filesystem::path image_path;

  auto                  operator==(const FileLocationRecord&) const -> bool = default;
};

}  // namespace alcedo
