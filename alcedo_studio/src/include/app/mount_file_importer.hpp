//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "type/type.hpp"

namespace alcedo {

/// One new file of a mount scan: the file, the mounted folder that becomes its home link, and
/// the mount.
struct MountFileImportRequest {
  image_path_t    path;
  sl_element_id_t home_folder_id = 0;
  mount_id_t      mount_id       = 0;
};

/// The result of one MountFileImportRequest, in the order of the requests.
struct MountFileImportOutcome {
  /// The new file element; empty when the file was not imported.
  std::optional<sl_element_id_t> element_id;
  /// Why the file was not imported; empty when it was.
  std::string                    error;
};

/**
 * @brief Imports the new files of a mount scan by reference (Mounted Folder Plan, MF4 step 4).
 *
 * The port keeps MountService free of the import service library, which depends on the project
 * service that owns MountService. ImportServiceImpl implements it with the import of a folder
 * import: the metadata read, the history root with the import defaults, and the Image row.
 */
class MountFileImporter {
 public:
  virtual ~MountFileImporter() = default;

  /**
   * @brief Import each file of @p requests, and return when each one is stored or failed.
   *
   * An imported file is an element of the library with the mount id of its request, a link to
   * its home folder, an Image row, and a history root, all synced to storage before the call
   * returns. A file that is not imported leaves no element and no Image row.
   *
   * Thread safety: call from a worker thread; the call blocks until the import ends.
   * @throws std::runtime_error when the storage write of the batch fails.
   */
  virtual auto ImportMountedFiles(std::span<const MountFileImportRequest> requests)
      -> std::vector<MountFileImportOutcome> = 0;
};

}  // namespace alcedo
