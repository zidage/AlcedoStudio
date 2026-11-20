//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "type/hash_type.hpp"

namespace alcedo {

/// Version of the fingerprint definition that ComputeSourceFileFingerprint implements.
inline constexpr std::uint32_t kSourceFileFingerprintVersion   = 1;

/// Size of the first block and of the last block that the fingerprint reads.
inline constexpr std::size_t   kSourceFileFingerprintBlockSize = 128 * 1024;

/**
 * @brief The match key of one source file (Mounted Folder Plan, P1).
 *
 * Version 1 is XXH3-128 of the 8-byte little-endian byte size, then the first
 * kSourceFileFingerprintBlockSize bytes, then the last kSourceFileFingerprintBlockSize bytes. A
 * file of two blocks or less contributes all its bytes one time. A change in the middle of a
 * larger file does not change the value.
 */
struct SourceFileFingerprint {
  Hash128       hash;
  std::uint32_t version = kSourceFileFingerprintVersion;

  auto          operator==(const SourceFileFingerprint&) const -> bool = default;
};

/// Why ComputeSourceFileFingerprint returned no fingerprint.
enum class SourceFileFingerprintError : std::uint8_t {
  kNone = 0,
  /// The file does not exist, is not a regular file, or another process holds it locked.
  kCannotOpen,
  /// A read returned an error or fewer bytes than the size of the file promised.
  kReadFailed,
  /// The byte size after the read is different from the byte size before the read.
  kSizeChangedDuringRead,
};

/// Result of ComputeSourceFileFingerprint. @ref fingerprint is valid only when @ref error is
/// SourceFileFingerprintError::kNone.
struct SourceFileFingerprintResult {
  SourceFileFingerprintError error = SourceFileFingerprintError::kNone;
  SourceFileFingerprint      fingerprint;

  [[nodiscard]] auto         HasFingerprint() const -> bool {
    return error == SourceFileFingerprintError::kNone;
  }
};

/**
 * @brief Compute the fingerprint (version 1) of the file at @p path.
 *
 * The file is opened for read only, and other processes can read, write, rename, and delete it
 * during the read. The function reads at most two blocks and writes nothing.
 *
 * Thread safety: the function holds no state; any thread can call it.
 *
 * @return The fingerprint, or a named error. The function does not throw for a file error.
 */
[[nodiscard]] auto ComputeSourceFileFingerprint(const std::filesystem::path& path)
    -> SourceFileFingerprintResult;

}  // namespace alcedo
