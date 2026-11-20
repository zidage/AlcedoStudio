//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/file/mount_directory_listing.hpp"

#include <QString>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "type/supported_file_type.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace alcedo {
namespace {

namespace stdfs = std::filesystem;

/// @p name in Unicode normalization form C. An ASCII name is returned unchanged without a copy
/// into a QString.
auto NfcName(std::wstring name) -> std::wstring {
  const bool ascii = std::all_of(name.begin(), name.end(), [](wchar_t c) { return c < 0x80; });
  if (ascii) return name;
  return QString::fromStdWString(name).normalized(QString::NormalizationForm_C).toStdWString();
}

auto JoinRelative(const std::wstring& parent, const std::wstring& name) -> std::wstring {
  return parent.empty() ? name : parent + L'/' + name;
}

struct FileAttributes {
  std::uint64_t byte_size   = 0;
  std::int64_t  modified_ns = 0;
};

/// Byte size and modification time of a regular file entry.
///
/// Windows: the directory entry holds both values from the directory read
/// (`FindNextFileW`), so no further file system call runs. The MSVC file clock counts 100 ns
/// ticks from 1601-01-01.
/// POSIX: one `lstat` reads both values. The directory entry would run one `stat` for each
/// value.
auto ReadFileAttributes(const stdfs::directory_entry& entry) -> std::optional<FileAttributes> {
#ifdef _WIN32
  std::error_code ec;
  const auto      byte_size = entry.file_size(ec);
  if (ec) return std::nullopt;
  const auto write_time = entry.last_write_time(ec);
  if (ec) return std::nullopt;
  constexpr std::int64_t kTicksFrom1601To1970 = 116444736000000000LL;
  const std::int64_t     ticks                = write_time.time_since_epoch().count();
  return FileAttributes{.byte_size   = byte_size,
                        .modified_ns = (ticks - kTicksFrom1601To1970) * 100};
#else
  struct stat info{};
  if (::lstat(entry.path().c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
    return std::nullopt;
  }
#ifdef __APPLE__
  const auto& time = info.st_mtimespec;
#else
  const auto& time = info.st_mtim;
#endif
  return FileAttributes{
      .byte_size   = static_cast<std::uint64_t>(info.st_size),
      .modified_ns = static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000LL + time.tv_nsec};
#endif
}

auto StopRequested(const std::atomic<bool>* stop_requested) -> bool {
  return stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed);
}

auto StoppedListing() -> MountDirectoryListing {
  MountDirectoryListing listing;
  listing.status = MountDirectoryListingStatus::kStopped;
  return listing;
}

}  // namespace

auto ListMountDirectory(const stdfs::path& root, const std::atomic<bool>* stop_requested)
    -> MountDirectoryListing {
  MountDirectoryListing listing;
  std::error_code       ec;
  if (root.empty() || !stdfs::is_directory(root, ec) || ec) {
    listing.status = MountDirectoryListingStatus::kRootUnreachable;
    return listing;
  }

  struct PendingDirectory {
    stdfs::path  path;
    std::wstring relative_path;
  };
  // An explicit stack instead of recursive_directory_iterator: the walk can name each directory
  // that fails, and it decides itself which entries to enter.
  std::vector<PendingDirectory> pending;
  pending.push_back({root, {}});

  while (!pending.empty()) {
    if (StopRequested(stop_requested)) return StoppedListing();
    PendingDirectory directory = std::move(pending.back());
    pending.pop_back();
    const bool                is_root = directory.relative_path.empty();

    stdfs::directory_iterator it(directory.path, ec);
    if (ec) {
      if (is_root) {
        listing.status = MountDirectoryListingStatus::kRootUnreachable;
        return listing;
      }
      listing.unreadable_directories.push_back(std::move(directory.relative_path));
      ec.clear();
      continue;
    }

    // True when an entry of this directory could not be read, or the read stopped before the end.
    bool incomplete = false;
    for (const stdfs::directory_iterator end; it != end; it.increment(ec)) {
      const stdfs::directory_entry& entry = *it;
      std::wstring                  name  = entry.path().filename().wstring();
      if (name.empty() || name.front() == L'.') continue;

      // The type without following a link: a symbolic link reports symlink, and an NTFS
      // junction reports the MSVC type junction. Both are skipped, so a link cycle ends. Other
      // reparse points (cloud placeholder files, deduplicated files) report their target type.
      std::error_code type_ec;
      const auto      type = entry.symlink_status(type_ec).type();
      if (type_ec) {
        incomplete = true;
        continue;
      }

      if (type == stdfs::file_type::directory) {
        auto relative = JoinRelative(directory.relative_path, NfcName(std::move(name)));
        listing.directories.push_back(relative);
        pending.push_back({entry.path(), std::move(relative)});
        continue;
      }
      if (type != stdfs::file_type::regular ||
          CategoryForPath(entry.path()) == ImportFileCategory::Other) {
        continue;
      }
      const auto attributes = ReadFileAttributes(entry);
      if (!attributes) {
        incomplete = true;
        continue;
      }
      listing.files.push_back(MountDirectoryFile{
          .relative_path = JoinRelative(directory.relative_path, NfcName(std::move(name))),
          .byte_size     = attributes->byte_size,
          .modified_ns   = attributes->modified_ns});
    }
    if (ec || incomplete) {
      listing.unreadable_directories.push_back(std::move(directory.relative_path));
      ec.clear();
    }
  }

  std::sort(listing.directories.begin(), listing.directories.end());
  std::sort(listing.files.begin(), listing.files.end(),
            [](const MountDirectoryFile& lhs, const MountDirectoryFile& rhs) {
              return lhs.relative_path < rhs.relative_path;
            });
  std::sort(listing.unreadable_directories.begin(), listing.unreadable_directories.end());
  return listing;
}

}  // namespace alcedo
