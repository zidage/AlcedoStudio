//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mount_scan_change_list.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace alcedo {
namespace {

/// The match key of a fingerprint: the hash words and the version.
using FingerprintKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>;

auto KeyOf(const SourceFileFingerprint& fingerprint) -> FingerprintKey {
  return {fingerprint.hash.high64(), fingerprint.hash.low64(), fingerprint.version};
}

auto StoredFingerprint(const SourceFileRecord& source) -> SourceFileFingerprint {
  return SourceFileFingerprint{source.fingerprint, source.fingerprint_version};
}

/// The last element name of @p relative_path.
auto FileNameOf(const std::wstring& relative_path) -> std::wstring_view {
  const std::wstring_view path{relative_path};
  const auto              slash = path.rfind(L'/');
  return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

/// True when @p relative_path is @p directory or is below it. An empty @p directory is the root.
auto IsAtOrBelow(const std::wstring& relative_path, const std::wstring& directory) -> bool {
  if (directory.empty() || relative_path == directory) return true;
  return relative_path.size() > directory.size() &&
         relative_path.compare(0, directory.size(), directory) == 0 &&
         relative_path[directory.size()] == L'/';
}

/// Insert each directory above @p relative_path into @p directories. The root is not inserted.
void InsertParentDirectories(const std::wstring&     relative_path,
                             std::set<std::wstring>& directories) {
  auto end = relative_path.rfind(L'/');
  while (end != std::wstring::npos) {
    if (!directories.insert(relative_path.substr(0, end)).second) return;
    if (end == 0) return;
    end = relative_path.rfind(L'/', end - 1);
  }
}

/// A lost record and the new paths of one fingerprint.
struct FingerprintGroup {
  std::vector<const MountScanFileRecord*> lost_records;
  std::vector<MountScanNewFile>           new_paths;
};

/// Pair the lost records and the new paths of one fingerprint (Mounted Folder Plan, section 2.4):
/// the same file name first, then the sorted relative path. Both lists are sorted by relative
/// path. Returns the unmatched records; the unmatched paths stay in @p group with no element.
auto MatchGroup(FingerprintGroup& group, std::vector<MountScanFileMove>& moved)
    -> std::vector<const MountScanFileRecord*> {
  std::vector<bool> path_taken(group.new_paths.size(), false);
  std::vector<bool> record_matched(group.lost_records.size(), false);

  const auto        match = [&](std::size_t record_index, std::size_t path_index) {
    const MountScanFileRecord& record = *group.lost_records[record_index];
    MountScanNewFile&          path   = group.new_paths[path_index];
    moved.push_back(MountScanFileMove{record.element_id, record.relative_path,
                                      std::move(path.relative_path), path.values});
    path_taken[path_index]       = true;
    record_matched[record_index] = true;
  };

  for (std::size_t r = 0; r < group.lost_records.size(); ++r) {
    const auto name = FileNameOf(group.lost_records[r]->relative_path);
    for (std::size_t p = 0; p < group.new_paths.size(); ++p) {
      if (!path_taken[p] && FileNameOf(group.new_paths[p].relative_path) == name) {
        match(r, p);
        break;
      }
    }
  }

  std::size_t next_path = 0;
  for (std::size_t r = 0; r < group.lost_records.size(); ++r) {
    if (record_matched[r]) continue;
    while (next_path < path_taken.size() && path_taken[next_path]) ++next_path;
    if (next_path == path_taken.size()) break;
    match(r, next_path);
  }

  std::vector<const MountScanFileRecord*> unmatched;
  for (std::size_t r = 0; r < group.lost_records.size(); ++r) {
    if (!record_matched[r]) unmatched.push_back(group.lost_records[r]);
  }
  std::vector<MountScanNewFile> remaining_paths;
  for (std::size_t p = 0; p < group.new_paths.size(); ++p) {
    if (!path_taken[p]) remaining_paths.push_back(std::move(group.new_paths[p]));
  }
  group.new_paths = std::move(remaining_paths);
  return unmatched;
}

}  // namespace

auto BuildMountScanChangeList(const MountScanRecords& records, const MountDirectoryListing& listing,
                              const MountScanFingerprintFunction& fingerprint,
                              const std::atomic<bool>* stop_requested) -> MountScanChangeList {
  MountScanChangeList changes;
  if (!listing.IsComplete()) return changes;

  const auto stopped = [stop_requested] {
    return stop_requested != nullptr && stop_requested->load();
  };

  const auto below_unreadable_directory = [&](const std::wstring& relative_path) {
    return std::any_of(
        listing.unreadable_directories.begin(), listing.unreadable_directories.end(),
        [&](const std::wstring& directory) { return IsAtOrBelow(relative_path, directory); });
  };
  const auto read_values =
      [&](const MountDirectoryFile& file) -> std::optional<MountScanSourceValues> {
    const SourceFileFingerprintResult result = fingerprint(file);
    if (!result.HasFingerprint()) {
      ++changes.unreadable_count;
      return std::nullopt;
    }
    return MountScanSourceValues{file.byte_size, file.modified_ns, result.fingerprint};
  };

  // Records in the order of their relative path. When two records claim one path, the lower
  // element id keeps it and the other record is lost.
  std::vector<const MountScanFileRecord*> sorted_records;
  sorted_records.reserve(records.files.size());
  for (const auto& record : records.files) sorted_records.push_back(&record);
  std::sort(sorted_records.begin(), sorted_records.end(), [](const auto* a, const auto* b) {
    return std::tie(a->relative_path, a->element_id) < std::tie(b->relative_path, b->element_id);
  });
  std::map<std::wstring, const MountScanFileRecord*> record_at_path;
  std::vector<const MountScanFileRecord*>            lost_records;
  for (const auto* record : sorted_records) {
    if (!record_at_path.emplace(record->relative_path, record).second) {
      lost_records.push_back(record);
    }
  }

  // Step 1: join the listing with the records by relative path.
  std::vector<const MountDirectoryFile*> new_paths;
  for (const auto& file : listing.files) {
    const auto found = record_at_path.find(file.relative_path);
    if (found == record_at_path.end()) {
      new_paths.push_back(&file);
      continue;
    }
    const MountScanFileRecord& record = *found->second;
    record_at_path.erase(found);
    const bool same_values =
        file.byte_size == record.source.byte_size && file.modified_ns == record.source.modified_ns;
    const bool was_missing = record.source.state == SourceFileState::kMissing;
    if (same_values) {
      if (was_missing) {
        changes.returned.push_back(MountScanFileUpdate{
            record.element_id,
            record.relative_path,
            {file.byte_size, file.modified_ns, StoredFingerprint(record.source)}});
      } else {
        ++changes.unchanged_count;
      }
      continue;
    }
    if (stopped()) return {};
    auto values = read_values(file);
    if (!values) continue;
    auto& target = was_missing ? changes.returned : changes.changed_in_place;
    target.push_back(MountScanFileUpdate{record.element_id, record.relative_path, *values});
  }

  // Step 2: the records with no file at their path. A record below an unreadable directory is not
  // verified: its file can still be there.
  for (const auto& [path, record] : record_at_path) lost_records.push_back(record);
  std::sort(lost_records.begin(), lost_records.end(), [](const auto* a, const auto* b) {
    return std::tie(a->relative_path, a->element_id) < std::tie(b->relative_path, b->element_id);
  });
  std::vector<const MountScanFileRecord*>    unverified_records;
  std::vector<const MountScanFileRecord*>    unmatched_records;
  std::map<FingerprintKey, FingerprintGroup> groups;
  std::set<FingerprintKey>                   unverified_fingerprints;
  // The byte sizes of the lost records that a new path can pair with. The fingerprint holds the
  // byte size, so a new path with another size has the fingerprint of none of them.
  std::set<std::uint64_t>                    lost_byte_sizes;
  for (const auto* record : lost_records) {
    const bool current_version =
        record->source.fingerprint_version == kSourceFileFingerprintVersion;
    if (current_version) lost_byte_sizes.insert(record->source.byte_size);
    if (below_unreadable_directory(record->relative_path)) {
      unverified_records.push_back(record);
      if (current_version) unverified_fingerprints.insert(KeyOf(StoredFingerprint(record->source)));
      continue;
    }
    if (current_version) {
      groups[KeyOf(StoredFingerprint(record->source))].lost_records.push_back(record);
    } else {
      unmatched_records.push_back(record);
    }
  }
  changes.unverified_count = unverified_records.size();

  // Step 3: a new path at the image path of an ordinary file is adopted by the lowest element id.
  std::map<std::wstring, sl_element_id_t> ordinary_at_path;
  for (const auto& ordinary : records.ordinary_files) {
    auto [it, inserted] = ordinary_at_path.emplace(ordinary.relative_path, ordinary.element_id);
    if (!inserted) it->second = std::min(it->second, ordinary.element_id);
  }

  // Step 4: fingerprint the new paths that an ordinary file adopts or that a lost record can
  // pair with, and group them with the lost records. The other new paths are not read here.
  for (const auto* file : new_paths) {
    const auto ordinary = ordinary_at_path.find(file->relative_path);
    if (ordinary == ordinary_at_path.end() && !lost_byte_sizes.contains(file->byte_size)) {
      changes.unread_new_files.push_back(*file);
      continue;
    }
    if (stopped()) return {};
    auto values = read_values(*file);
    if (!values) continue;
    if (ordinary != ordinary_at_path.end()) {
      changes.adopted.push_back(
          MountScanFileUpdate{ordinary->second, file->relative_path, *values});
      continue;
    }
    groups[KeyOf(values->fingerprint)].new_paths.push_back(
        MountScanNewFile{file->relative_path, *values});
  }

  // Step 5: moves, new files, and missing records.
  for (auto& [key, group] : groups) {
    auto unmatched = MatchGroup(group, changes.moved);
    unmatched_records.insert(unmatched_records.end(), unmatched.begin(), unmatched.end());
    for (auto& path : group.new_paths) {
      if (unverified_fingerprints.contains(key)) {
        ++changes.deferred_count;
      } else {
        changes.new_files.push_back(std::move(path));
      }
    }
  }
  std::set<std::wstring> protected_directories;
  for (const auto* record : unmatched_records) {
    InsertParentDirectories(record->relative_path, protected_directories);
    if (record->source.state == SourceFileState::kMissing) {
      ++changes.still_missing_count;
    } else {
      changes.missing.push_back(MountScanMissingFile{record->element_id, record->relative_path});
    }
  }
  for (const auto* record : unverified_records) {
    InsertParentDirectories(record->relative_path, protected_directories);
  }

  // Step 6: folders. A directory with a listed file at any depth has a folder. A folder that
  // holds a missing photo or an unverified record, or that is below an unreadable directory,
  // stays.
  std::set<std::wstring> listed_directories;
  for (const auto& file : listing.files) {
    InsertParentDirectories(file.relative_path, listed_directories);
  }
  std::set<std::wstring> recorded_directories;
  for (const auto& folder : records.folders) {
    recorded_directories.insert(folder.relative_path);
    if (below_unreadable_directory(folder.relative_path)) {
      protected_directories.insert(folder.relative_path);
      InsertParentDirectories(folder.relative_path, protected_directories);
    }
  }
  for (const auto& directory : listed_directories) {
    if (!recorded_directories.contains(directory)) changes.folders_to_add.push_back(directory);
  }
  for (const auto& folder : records.folders) {
    if (!listed_directories.contains(folder.relative_path) &&
        !protected_directories.contains(folder.relative_path)) {
      changes.folders_to_remove.push_back(folder);
    }
  }

  const auto by_path = [](const auto& a, const auto& b) {
    return a.relative_path < b.relative_path;
  };
  std::sort(changes.moved.begin(), changes.moved.end(), by_path);
  std::sort(changes.new_files.begin(), changes.new_files.end(), by_path);
  std::sort(changes.unread_new_files.begin(), changes.unread_new_files.end(), by_path);
  std::sort(changes.missing.begin(), changes.missing.end(), [](const auto& a, const auto& b) {
    return std::tie(a.relative_path, a.element_id) < std::tie(b.relative_path, b.element_id);
  });
  std::sort(changes.folders_to_remove.begin(), changes.folders_to_remove.end(),
            [](const auto& a, const auto& b) {
              return std::tie(b.relative_path, b.element_id) <
                     std::tie(a.relative_path, a.element_id);
            });
  return changes;
}

}  // namespace alcedo
