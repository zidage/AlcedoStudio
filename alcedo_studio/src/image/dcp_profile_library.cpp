// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dcp_profile_library.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "image/dcp_source_tables.hpp"
#include "metadata/camera_name_match.hpp"
#include "utils/logging/log.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;
using metadata_detail::CompactCameraName;

/// Make names that `UniqueCameraModel` uses, for the compact first word of an Exif or LibRaw
/// make. Built from the camera folders of the Lightroom Classic DCP library (359 cameras) and the
/// camera names of camera_color_matrix_db.cpp. A make without a row uses its own first word.
struct MakeAlias {
  std::string_view                make_word;
  std::array<std::string_view, 2> profile_makes;
};
constexpr std::array kMakeAliases{
    // OLYMPUS CORPORATION, OLYMPUS IMAGING CORP.: OM System bodies keep some Olympus names.
    MakeAlias{"olympus", {"Olympus", "OM Digital Solutions"}},
    // OM Digital Solutions: E-M and TG bodies made before the rename keep the Olympus name.
    MakeAlias{"om", {"OM Digital Solutions", "Olympus"}},
    // RICOH IMAGING COMPANY, LTD. makes the Ricoh GR and the Pentax bodies.
    MakeAlias{"ricoh", {"Ricoh", "Pentax"}},
    // PENTAX Corporation, PENTAX RICOH IMAGING: older GR bodies.
    MakeAlias{"pentax", {"Pentax", "Ricoh"}},
    MakeAlias{"phase", {"Phase One", {}}},
    MakeAlias{"konica", {"Konica Minolta", {}}},
    MakeAlias{"eastman", {"Kodak", {}}},
};

auto FirstWord(const std::string& text) -> std::string {
  const auto begin = std::find_if(text.begin(), text.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  });
  const auto end   = std::find_if(
      begin, text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) == 0; });
  return {begin, end};
}

auto HasDcpExtension(const fs::path& path) -> bool {
  const auto extension = path.extension().string();
  if (extension.size() != 4) return false;
  std::string lower;
  for (char c : extension)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lower == ".dcp";
}

auto PathText(const fs::path& path) -> std::string {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

auto CacheKey(const fs::path& path) -> std::wstring {
  return path.lexically_normal().generic_wstring();
}

}  // namespace

auto DcpUniqueCameraModelKey(const std::string& unique_camera_model) -> std::string {
  return CompactCameraName(unique_camera_model);
}

auto DcpCameraMatchKeys(const std::string& camera_make, const std::string& camera_model)
    -> std::vector<std::string> {
  std::vector<std::string> keys;
  const auto               add = [&keys](std::string key) {
    if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) {
      keys.push_back(std::move(key));
    }
  };
  if (CompactCameraName(camera_model).empty()) return keys;
  add(CompactCameraName(camera_model));

  const std::string make_word = CompactCameraName(FirstWord(camera_make));
  if (make_word.empty()) return keys;
  const auto alias = std::find_if(kMakeAliases.begin(), kMakeAliases.end(),
                                  [&](const MakeAlias& row) { return row.make_word == make_word; });
  if (alias == kMakeAliases.end()) {
    add(CompactCameraName(make_word + " " + camera_model));
    return keys;
  }
  for (const auto profile_make : alias->profile_makes) {
    if (!profile_make.empty())
      add(CompactCameraName(std::string(profile_make) + " " + camera_model));
  }
  return keys;
}

auto DcpCameraModelMatches(const std::string& camera_make, const std::string& camera_model,
                           const std::string& unique_camera_model) -> bool {
  const auto profile_key = DcpUniqueCameraModelKey(unique_camera_model);
  if (profile_key.empty()) return false;
  const auto keys = DcpCameraMatchKeys(camera_make, camera_model);
  return std::find(keys.begin(), keys.end(), profile_key) != keys.end();
}

DcpProfileLibrary::DcpProfileLibrary(Loader loader, std::size_t capacity)
    : loader_(std::move(loader)), capacity_(capacity) {
  if (!loader_) throw std::invalid_argument("DcpProfileLibrary: loader is required");
  if (capacity_ == 0) {
    throw std::invalid_argument("DcpProfileLibrary: capacity must be at least 1");
  }
}

auto DcpProfileLibrary::Shared() -> DcpProfileLibrary& {
  static DcpProfileLibrary library;
  return library;
}

auto DcpProfileLibrary::IdentityKey(const std::string& camera_key, const std::string& profile_name)
    -> std::string {
  // The unit separator cannot occur in a compact camera key.
  return camera_key + '\x1f' + profile_name;
}

auto DcpProfileLibrary::Scan(const fs::path& root) -> DcpLibraryScanResult {
  const auto                          start = std::chrono::steady_clock::now();
  std::unique_lock<std::shared_mutex> lock(index_mutex_);
  root_.clear();
  entries_.clear();
  by_identity_.clear();
  by_camera_.clear();

  DcpLibraryScanResult result;
  result.root       = root;
  const auto finish = [&] {
    result.profile_count = entries_.size();
    result.camera_count  = by_camera_.size();
    result.elapsed       = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return result;
  };
  if (root.empty()) return finish();

  // Walk the folder first, so that the files are read in path order and the first sorted path
  // of a duplicate identity wins.
  struct FoundFile {
    fs::path                        path;
    std::uintmax_t                  size = 0;
    std::filesystem::file_time_type last_write_time{};
  };
  std::vector<FoundFile>           files;
  std::error_code                  error;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
  for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
    std::error_code entry_error;
    if (!it->is_regular_file(entry_error) || !HasDcpExtension(it->path())) continue;
    FoundFile file{it->path()};
    file.size = it->file_size(entry_error);
    if (!entry_error) file.last_write_time = it->last_write_time(entry_error);
    if (entry_error) {
      ++result.rejected_count;
      ALCEDO_LOG_WARN(Decode) << "DCP library: rejected " << PathText(file.path) << ": "
                              << entry_error.message();
      continue;
    }
    files.push_back(std::move(file));
  }
  if (error) {
    result.error          = error.message();
    result.rejected_count = 0;
    ALCEDO_LOG_WARN(Decode) << "DCP library: cannot read the folder " << PathText(root) << ": "
                            << result.error;
    return finish();
  }
  std::sort(files.begin(), files.end(),
            [](const FoundFile& a, const FoundFile& b) { return a.path < b.path; });

  std::vector<IndexEntry> entries;
  entries.reserve(files.size());
  for (auto& file : files) {
    try {
      auto        identity   = ReadDcpProfileIdentity(file.path);
      std::string camera_key = DcpUniqueCameraModelKey(identity.unique_camera_model);
      if (camera_key.empty()) {
        throw std::runtime_error("DCP " + PathText(file.path) +
                                 ": UniqueCameraModel has no letter or digit");
      }
      auto identity_key = IdentityKey(camera_key, identity.profile_name);
      if (const auto existing = by_identity_.find(identity_key); existing != by_identity_.end()) {
        ALCEDO_LOG_WARN(Decode) << "DCP library: " << PathText(file.path) << " has the identity of "
                                << PathText(entries[existing->second].path) << "; it is ignored";
        continue;
      }
      by_identity_.emplace(std::move(identity_key), entries.size());
      by_camera_[camera_key].push_back(entries.size());
      entries.push_back(IndexEntry{std::move(file.path), file.size, file.last_write_time,
                                   std::move(identity), std::move(camera_key)});
    } catch (const std::exception& read_error) {
      ++result.rejected_count;
      ALCEDO_LOG_WARN(Decode) << "DCP library: rejected " << PathText(file.path) << ": "
                              << read_error.what();
    }
  }
  root_    = root;
  entries_ = std::move(entries);
  result   = finish();
  ALCEDO_LOG_INFO(Decode) << "DCP library: " << PathText(root) << ": " << result.profile_count
                          << " profiles, " << result.camera_count << " cameras, "
                          << result.rejected_count << " rejected, " << result.elapsed.count()
                          << " ms";
  return result;
}

auto DcpProfileLibrary::Match(const std::string& camera_make, const std::string& camera_model) const
    -> std::vector<DcpProfileIdentity> {
  const auto                          keys = DcpCameraMatchKeys(camera_make, camera_model);
  std::shared_lock<std::shared_mutex> lock(index_mutex_);
  std::vector<DcpProfileIdentity>     matches;
  for (const auto& key : keys) {
    const auto it = by_camera_.find(key);
    if (it == by_camera_.end()) continue;
    for (const auto position : it->second) matches.push_back(entries_[position].identity);
  }
  std::sort(matches.begin(), matches.end(),
            [](const DcpProfileIdentity& a, const DcpProfileIdentity& b) {
              if (a.profile_name != b.profile_name) return a.profile_name < b.profile_name;
              return a.unique_camera_model < b.unique_camera_model;
            });
  return matches;
}

auto DcpProfileLibrary::Find(const std::string& camera_model, const std::string& profile_name)
    -> DcpCameraProfilePtr {
  fs::path           path;
  DcpProfileIdentity identity;
  {
    std::shared_lock<std::shared_mutex> lock(index_mutex_);
    const auto                          it =
        by_identity_.find(IdentityKey(DcpUniqueCameraModelKey(camera_model), profile_name));
    if (it == by_identity_.end()) return nullptr;
    path     = entries_[it->second].path;
    identity = entries_[it->second].identity;
  }

  std::error_code error;
  FileStamp       stamp;
  stamp.size = fs::file_size(path, error);
  if (!error) stamp.last_write_time = fs::last_write_time(path, error);
  if (error) {
    throw std::runtime_error("DCP " + PathText(path) + ": the file is unavailable (" +
                             error.message() + ")");
  }
  const auto key = CacheKey(path);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (const auto it = cache_by_path_.find(key); it != cache_by_path_.end()) {
      if (it->second->stamp == stamp) {
        cache_.splice(cache_.begin(), cache_, it->second);
        return it->second->profile;
      }
      cache_.erase(it->second);
      cache_by_path_.erase(it);
    }
    ++profile_reads_;
  }

  // Read the tables without a lock; a load must not block lookups of other profiles.
  auto profile = loader_(path);
  if (!profile) throw std::runtime_error("DCP " + PathText(path) + ": the loader gave no profile");
  if (DcpUniqueCameraModelKey(profile->unique_camera_model) !=
          DcpUniqueCameraModelKey(identity.unique_camera_model) ||
      profile->profile_name != identity.profile_name) {
    throw std::runtime_error("DCP " + PathText(path) + ": the file now holds \"" +
                             profile->profile_name + "\" for \"" + profile->unique_camera_model +
                             "\"; scan the DCP folder again");
  }

  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (const auto it = cache_by_path_.find(key); it != cache_by_path_.end()) {
    // Another thread loaded the same file meanwhile; keep this newer read.
    cache_.erase(it->second);
    cache_by_path_.erase(it);
  }
  cache_.push_front(CacheEntry{key, stamp, profile});
  cache_by_path_[key] = cache_.begin();
  while (cache_.size() > capacity_) {
    cache_by_path_.erase(cache_.back().key);
    cache_.pop_back();
  }
  return profile;
}

auto DcpProfileLibrary::SourceTables(const DcpCameraProfilePtr& dcp,
                                     const DngColorProfilePtr&  source) -> DngColorProfilePtr {
  if (!dcp || !source) {
    throw std::invalid_argument("DcpProfileLibrary: source tables need a DCP and a DNG profile");
  }
  const auto&           calibration = source->source_calibration;
  const SourceTablesKey key{dcp->fingerprint, source->fingerprint, calibration.signature,
                            calibration.camera_calibration_1, calibration.camera_calibration_2};
  {
    std::lock_guard<std::mutex> lock(source_tables_mutex_);
    if (const auto it = source_tables_.find(key); it != source_tables_.end()) {
      if (auto existing = it->second.lock()) return existing;
    }
    ++source_tables_merges_;
  }
  // Merge without the lock; the fingerprint of the merged tables reads all table entries.
  auto                        merged = MergeDcpTablesWithSourceCalibration(*dcp, *source);

  std::lock_guard<std::mutex> lock(source_tables_mutex_);
  if (const auto it = source_tables_.find(key); it != source_tables_.end()) {
    // Another thread merged the same inputs meanwhile; share its result.
    if (auto existing = it->second.lock()) return existing;
  }
  // Drop entries that no holder keeps alive, so the map stays as small as the live set.
  std::erase_if(source_tables_, [](const auto& entry) { return entry.second.expired(); });
  source_tables_[key] = merged;
  return merged;
}

auto DcpProfileLibrary::SourceTablesMergeCount() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(source_tables_mutex_);
  return source_tables_merges_;
}

auto DcpProfileLibrary::Root() const -> fs::path {
  std::shared_lock<std::shared_mutex> lock(index_mutex_);
  return root_;
}

auto DcpProfileLibrary::ProfileCount() const -> std::size_t {
  std::shared_lock<std::shared_mutex> lock(index_mutex_);
  return entries_.size();
}

auto DcpProfileLibrary::CameraCount() const -> std::size_t {
  std::shared_lock<std::shared_mutex> lock(index_mutex_);
  return by_camera_.size();
}

auto DcpProfileLibrary::ProfileReadCount() const -> std::uint64_t {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  return profile_reads_;
}

}  // namespace alcedo
