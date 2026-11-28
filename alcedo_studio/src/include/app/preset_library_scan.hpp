//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "app/preset_file.hpp"

namespace alcedo {

/// Most preset files that one scan reads (edit presets plan 3.5).
inline constexpr std::size_t kPresetLibraryMaxFiles = 2000;

/**
 * @brief One preset file of the preset folder, as the last scan read it.
 *
 * @p id is the path relative to the folder with `/` separators, such as `Film/Warm.alcpreset`.
 * @p name is the file name without the extension. @p group is the name of the direct subfolder,
 * empty for a file in the folder itself. @p fingerprint is set when the file is available.
 * A file that fails a rule of ReadPresetFile has @p available false, the failed @p rule, and the
 * @p reason message.
 */
struct PresetEntry {
  std::string                     id;
  std::string                     name;
  std::string                     group;
  std::uintmax_t                  file_size = 0;
  std::filesystem::file_time_type write_time{};
  std::string                     fingerprint;
  bool                            available = false;
  PresetFileRule                  rule      = PresetFileRule::kNone;
  std::string                     reason;
};

/**
 * @brief Scan result of one preset folder: the entries in id order, and the scan error.
 *
 * @p error is empty when the scan read the complete folder. It is set when the folder is not a
 * folder or cannot be listed, and when the folder holds more than kPresetLibraryMaxFiles preset
 * files; the entries then hold the files that the scan read before it stopped.
 */
struct PresetLibraryInventory {
  std::vector<PresetEntry> entries;
  std::string              error;
};

/**
 * @brief Read the preset files of @p root and of its direct subfolders.
 *
 * Lists files with the extension kPresetFileExtension (any case). Deeper folders are not read,
 * and no link to a folder is followed. A link to a file is listed and is not available
 * (ReadPresetFile refuses it). Each file is read with ReadPresetFile. Runs on the calling thread
 * and changes no file.
 */
[[nodiscard]] auto ScanPresetLibraryRoot(const std::filesystem::path& root)
    -> PresetLibraryInventory;

/// The entry of @p inventory whose id is @p id, or nullptr.
[[nodiscard]] auto FindPresetEntry(const PresetLibraryInventory& inventory, std::string_view id)
    -> const PresetEntry*;

}  // namespace alcedo
