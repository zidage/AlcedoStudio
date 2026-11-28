//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>

#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {

/// The stored values of `SourceFile.state`.
enum class SourceFileState : std::uint8_t {
  kPresent = 0,
  kMissing = 1,
};

/// The last recorded measurement of the source at `Image.image_path` for one file element.
struct SourceFileRecord {
  sl_element_id_t            element_id          = 0;
  std::uint64_t              byte_size           = 0;
  /// Modification time in nanoseconds since the Unix epoch.
  std::int64_t               modified_ns         = 0;
  Hash128                    fingerprint         = {};
  std::uint32_t              fingerprint_version = 0;
  SourceFileState            state               = SourceFileState::kPresent;
  /// UTC seconds. Set exactly when the source is missing.
  std::optional<std::time_t> missing_since;

  auto                       operator==(const SourceFileRecord&) const -> bool = default;
};

/// The location of a file element with a stored Image binding.
struct FileLocationRecord {
  sl_element_id_t       element_id = 0;
  image_id_t            image_id   = 0;
  std::filesystem::path image_path;

  auto                  operator==(const FileLocationRecord&) const -> bool = default;
};

}  // namespace alcedo
