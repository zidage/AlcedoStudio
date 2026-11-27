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
#include "utils/file/source_file.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
      const auto attributes = ReadSourceFileAttributes(entry);
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

namespace {

/// The elements of @p path after `lexically_normal`, without the empty last element of a path
/// that ends with a separator.
auto NormalElements(const stdfs::path& path) -> std::vector<stdfs::path> {
  std::vector<stdfs::path> elements;
  for (const auto& element : path.lexically_normal()) {
    if (!element.empty()) {
      elements.push_back(element);
    }
  }
  return elements;
}

auto SamePathElement(const stdfs::path& lhs, const stdfs::path& rhs) -> bool {
#ifdef _WIN32
  const auto& left  = lhs.native();
  const auto& right = rhs.native();
  return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
  return lhs == rhs;
#endif
}

/// The number of elements of @p root when @p path starts with them, else empty.
auto RootElementCount(const std::vector<stdfs::path>& path, const std::vector<stdfs::path>& root)
    -> std::optional<std::size_t> {
  if (root.empty() || path.size() < root.size()) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < root.size(); ++index) {
    if (!SamePathElement(path[index], root[index])) {
      return std::nullopt;
    }
  }
  return root.size();
}

}  // namespace

auto IsPathAtOrBelow(const stdfs::path& path, const stdfs::path& root) -> bool {
  return RootElementCount(NormalElements(path), NormalElements(root)).has_value();
}

auto MountRelativePathOf(const stdfs::path& root, const stdfs::path& path)
    -> std::optional<std::wstring> {
  const auto path_elements = NormalElements(path);
  const auto count         = RootElementCount(path_elements, NormalElements(root));
  if (!count.has_value() || *count == path_elements.size()) {
    return std::nullopt;
  }
  std::wstring relative;
  for (std::size_t index = *count; index < path_elements.size(); ++index) {
    relative = JoinRelative(relative, NfcName(path_elements[index].wstring()));
  }
  return relative;
}

auto ReadMountDirectoryFile(const stdfs::path& root, const stdfs::path& path)
    -> std::optional<MountDirectoryFile> {
  const auto path_elements = NormalElements(path);
  const auto count         = RootElementCount(path_elements, NormalElements(root));
  if (!count.has_value() || *count == path_elements.size()) {
    return std::nullopt;
  }

  // Walk from the root as the listing does: each directory on the way is a real directory, and
  // no name starts with a period.
  stdfs::path     current = root;
  std::error_code ec;
  for (std::size_t index = *count; index < path_elements.size(); ++index) {
    const auto name = path_elements[index].wstring();
    if (name.empty() || name.front() == L'.') {
      return std::nullopt;
    }
    current /= path_elements[index];
    const auto type = stdfs::symlink_status(current, ec).type();
    if (ec) {
      return std::nullopt;
    }
    const bool last = index + 1 == path_elements.size();
    if (last ? type != stdfs::file_type::regular : type != stdfs::file_type::directory) {
      return std::nullopt;
    }
  }
  if (CategoryForPath(current) == ImportFileCategory::Other) {
    return std::nullopt;
  }
  const stdfs::directory_entry entry(current, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto attributes = ReadSourceFileAttributes(entry);
  if (!attributes) {
    return std::nullopt;
  }
  return MountDirectoryFile{.relative_path = *MountRelativePathOf(root, current),
                            .byte_size     = attributes->byte_size,
                            .modified_ns   = attributes->modified_ns};
}

}  // namespace alcedo
