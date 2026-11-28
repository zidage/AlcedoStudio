//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace alcedo {

/// The size and Unix modification time of a regular file.
struct SourceFileAttributes {
  std::uint64_t byte_size                                             = 0;
  std::int64_t  modified_ns                                           = 0;

  auto          operator==(const SourceFileAttributes&) const -> bool = default;
};

/// Read file attributes, following symbolic links to the source.
[[nodiscard]] auto ReadSourceFileAttributes(const std::filesystem::path& path)
    -> std::optional<SourceFileAttributes>;

/// Read a regular directory entry's attributes, using the directory listing cache on Windows.
/// The caller has checked the type and excluded symbolic links and junctions.
[[nodiscard]] auto ReadSourceFileAttributes(const std::filesystem::directory_entry& entry)
    -> std::optional<SourceFileAttributes>;

/// Normalize an absolute visible path without resolving links. Windows keys ignore case.
[[nodiscard]] auto SourcePathKey(const std::filesystem::path& path)
    -> std::filesystem::path::string_type;

}  // namespace alcedo
