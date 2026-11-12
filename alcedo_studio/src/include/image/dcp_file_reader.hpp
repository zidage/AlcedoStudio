// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "image/dcp_camera_profile.hpp"

namespace alcedo {

/// Largest DCP file that the reader opens. A larger file fails before any payload read.
inline constexpr std::uintmax_t kMaxDcpFileBytes = 64ull * 1024 * 1024;

/// The two identity strings of a DCP file.
struct DcpProfileIdentity {
  std::string unique_camera_model;  ///< Tag 50708.
  std::string profile_name;         ///< Tag 50936.
  auto        operator==(const DcpProfileIdentity&) const -> bool = default;
};

/**
 * @brief Read only the identity of a DCP file.
 *
 * Reads the TIFF header, the entry table of the first IFD, and the two string payloads. It does
 * not read or check the matrix and table payloads, so a library scan stays short.
 *
 * Thread safety: no shared state; callable from any thread.
 *
 * @throws std::runtime_error with the path and the tag number or the reason when the file cannot
 *         be read, the header or IFD is malformed, or an identity tag is missing or malformed.
 */
auto ReadDcpProfileIdentity(const std::filesystem::path& path) -> DcpProfileIdentity;

/**
 * @brief Read a complete DCP file into an immutable profile.
 *
 * Accepts byte orders `II` and `MM` with magic `0x4352` (DCP) or `42` (TIFF), and reads the first
 * IFD only. `ProfileToneCurve` becomes DcpCameraProfile::profile_tone_curve; it leaves the
 * profile fingerprint unchanged. The other rendering hints are read past. A profile with a third
 * illuminant, `RGBTables`, or a `ReductionMatrix` fails, because the render path has two
 * illuminants and three camera channels.
 *
 * Thread safety: no shared state; callable from any thread.
 *
 * @throws std::runtime_error with the path and the tag number or the reason for any malformed,
 *         missing required, or unsupported tag. No partial profile is returned.
 */
auto ReadDcpCameraProfile(const std::filesystem::path& path) -> DcpCameraProfilePtr;

}  // namespace alcedo
