//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SHA-256 (FIPS 180-4) of an in-memory byte sequence.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace alcedo {

/// SHA-256 digest of @p bytes.
auto ComputeSha256(std::span<const std::byte> bytes) -> std::array<uint8_t, 32>;

/// Lower-case hexadecimal SHA-256 digest of @p bytes (64 characters).
auto ComputeSha256Hex(std::span<const std::byte> bytes) -> std::string;

}  // namespace alcedo
