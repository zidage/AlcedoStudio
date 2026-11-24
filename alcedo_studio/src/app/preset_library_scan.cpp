//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/preset_library_scan.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <utility>

namespace alcedo {
namespace {

namespace fs = std::filesystem;

auto Utf8(const fs::path& path) -> std::string {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

auto HasPresetExtension(const fs::path& path) -> bool {
  std::string extension = Utf8(path.extension());
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == kPresetFileExtension;
}

struct PresetFileCandidate {
  fs::path    path;
  std::string group;
};

/// Add the preset files of @p folder to @p candidates. Returns false when the file limit stops
/// the listing. A folder that cannot be listed adds nothing and sets @p error.
auto ListFolder(const fs::path& folder, const std::string& group,
                std::vector<PresetFileCandidate>* candidates, std::vector<fs::path>* subfolders,
                std::string* error) -> bool {
  std::error_code        code;
  fs::directory_iterator iterator(folder, fs::directory_options::skip_permission_denied, code);
  const fs::directory_iterator end;
  if (code) {
    *error = "cannot read the preset folder " + Utf8(folder) + ": " + code.message();
    return true;
  }
  for (; iterator != end; iterator.increment(code)) {
    if (code) {
      *error = "cannot read the preset folder " + Utf8(folder) + ": " + code.message();
      return true;
    }
    const fs::path        path   = iterator->path();
    // symlink_status: a link is never followed, so a link to a folder is not read.
    const fs::file_status status = iterator->symlink_status(code);
    if (code) {
      continue;
    }
    if (fs::is_directory(status)) {
      if (subfolders != nullptr) {
        subfolders->push_back(path);
      }
      continue;
    }
    if ((fs::is_regular_file(status) || fs::is_symlink(status)) && HasPresetExtension(path)) {
      if (candidates->size() >= kPresetLibraryMaxFiles) {
        *error = "the preset folder holds more than " + std::to_string(kPresetLibraryMaxFiles) +
                 " preset files; the scan stopped";
        return false;
      }
      candidates->push_back({path, group});
    }
  }
  return true;
}

}  // namespace

auto ScanPresetLibraryRoot(const fs::path& root) -> PresetLibraryInventory {
  PresetLibraryInventory inventory;
  std::error_code        code;
  if (!fs::is_directory(fs::symlink_status(root, code))) {
    inventory.error = "the preset folder " + Utf8(root) + " is not available";
    return inventory;
  }
  std::vector<PresetFileCandidate> candidates;
  std::vector<fs::path>            subfolders;
  bool complete = ListFolder(root, {}, &candidates, &subfolders, &inventory.error);
  std::sort(subfolders.begin(), subfolders.end());
  for (const auto& folder : subfolders) {
    if (!complete) {
      break;
    }
    complete = ListFolder(folder, Utf8(folder.filename()), &candidates, nullptr, &inventory.error);
  }

  inventory.entries.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    PresetEntry entry;
    entry.group      = candidate.group;
    entry.name       = Utf8(candidate.path.stem());
    entry.id         = entry.group.empty() ? Utf8(candidate.path.filename())
                                           : entry.group + "/" + Utf8(candidate.path.filename());
    entry.file_size  = fs::file_size(candidate.path, code);
    entry.write_time = fs::last_write_time(candidate.path, code);
    const auto read  = ReadPresetFile(candidate.path);
    entry.available  = read.Ok();
    entry.rule       = read.rule;
    entry.reason     = read.message;
    if (read.package.has_value()) {
      entry.fingerprint = read.package->fingerprint_;
    }
    inventory.entries.push_back(std::move(entry));
  }
  std::sort(inventory.entries.begin(), inventory.entries.end(),
            [](const PresetEntry& lhs, const PresetEntry& rhs) { return lhs.id < rhs.id; });
  return inventory;
}

auto FindPresetEntry(const PresetLibraryInventory& inventory, std::string_view id)
    -> const PresetEntry* {
  const auto found = std::find_if(inventory.entries.begin(), inventory.entries.end(),
                                  [id](const PresetEntry& entry) { return entry.id == id; });
  return found == inventory.entries.end() ? nullptr : &*found;
}

}  // namespace alcedo
