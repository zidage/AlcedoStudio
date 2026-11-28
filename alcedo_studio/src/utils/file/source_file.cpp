//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/file/source_file.hpp"

#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace alcedo {
namespace {

#ifndef _WIN32
auto AttributesFromStat(const struct stat& info) -> std::optional<SourceFileAttributes> {
  if (!S_ISREG(info.st_mode) || info.st_size < 0) {
    return std::nullopt;
  }
#ifdef __APPLE__
  const auto& time = info.st_mtimespec;
#else
  const auto& time = info.st_mtim;
#endif
  return SourceFileAttributes{
      .byte_size   = static_cast<std::uint64_t>(info.st_size),
      .modified_ns = static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000LL + time.tv_nsec};
}
#endif

}  // namespace

auto ReadSourceFileAttributes(const std::filesystem::path& path)
    -> std::optional<SourceFileAttributes> {
#ifdef _WIN32
  std::error_code                        ec;
  const std::filesystem::directory_entry entry(path, ec);
  if (ec || !entry.is_regular_file(ec)) {
    return std::nullopt;
  }
  return ReadSourceFileAttributes(entry);
#else
  struct stat info{};
  return ::stat(path.c_str(), &info) == 0 ? AttributesFromStat(info) : std::nullopt;
#endif
}

auto ReadSourceFileAttributes(const std::filesystem::directory_entry& entry)
    -> std::optional<SourceFileAttributes> {
#ifdef _WIN32
  std::error_code ec;
  const auto      byte_size = entry.file_size(ec);
  if (ec) {
    return std::nullopt;
  }
  const auto write_time = entry.last_write_time(ec);
  if (ec) {
    return std::nullopt;
  }
  // The MSVC file clock counts 100 ns ticks from January 1, 1601.
  constexpr std::int64_t kTicksFrom1601To1970 = 116444736000000000LL;
  return SourceFileAttributes{
      .byte_size   = byte_size,
      .modified_ns = (write_time.time_since_epoch().count() - kTicksFrom1601To1970) * 100};
#else
  struct stat info{};
  return ::lstat(entry.path().c_str(), &info) == 0 ? AttributesFromStat(info) : std::nullopt;
#endif
}

auto SourcePathKey(const std::filesystem::path& path) -> std::filesystem::path::string_type {
  auto key = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
#ifdef _WIN32
  const auto   length        = static_cast<int>(key.size());
  const auto   folded_length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, key.data(),
                                             length, nullptr, 0, nullptr, nullptr, 0);
  std::wstring folded(folded_length, L'\0');
  if (folded_length == 0 ||
      LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, key.data(), length, folded.data(),
                    folded_length, nullptr, nullptr, 0) == 0) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "Cannot normalize source path case");
  }
  return folded;
#else
  return key;
#endif
}

}  // namespace alcedo
