//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace alcedo {

/// Why a user operation on the sleeve tree was refused (Mounted Folder Plan, MF5).
enum class SleeveError : std::uint8_t {
  /// The target folder is a mounted folder: the disk directory is the only source of its
  /// structure (D6, D7). Covers a new collection, rename, move, delete, import, link, duplicate,
  /// and the removal of the home link of a mounted file.
  kMountedFolderStructureIsReadOnly,
  /// Remove from Library of a mounted photo whose source file is present: the next scan would
  /// find the file again (P7). The Delete mark stays available.
  kMountedPhotoIsPresent,
  /// Remove from Library of a mounted photo whose source file is missing: only Remove Missing
  /// Photos of the mount removes it (MF8).
  kMountedPhotoIsMissing,
};

/// Stable snake_case name of @p code, for example `mounted_folder_structure_is_read_only`.
auto SleeveErrorName(SleeveError code) -> const char*;

/// The error of a refused sleeve operation (SleeveServiceImpl). The refused operation changed
/// nothing.
class SleeveOperationError : public std::runtime_error {
 public:
  SleeveOperationError(SleeveError code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  [[nodiscard]] auto Code() const -> SleeveError { return code_; }

 private:
  SleeveError code_;
};

}  // namespace alcedo
