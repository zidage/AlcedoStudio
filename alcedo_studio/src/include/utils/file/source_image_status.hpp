// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <optional>
#include <string>

#include "type/type.hpp"
#include "utils/file/source_file_fingerprint.hpp"

namespace alcedo {
enum class SourceImageStatus { Readable, Missing, Error };
struct SourceImageStatusResult {
  SourceImageStatus status = SourceImageStatus::Error;
  std::string       reason;
};
/// Check volume availability and file status, then open/read one byte. No RAW decoding.
/// Errors are report-only; only file-not-found on an available volume is Missing.
auto CheckSourceImageStatus(const image_path_t& path) -> SourceImageStatusResult;
/// Return a fingerprint only when the attributes agree before and after the read.
auto ReadMaintenanceFingerprint(const image_path_t& path) -> std::optional<SourceFileFingerprint>;
}  // namespace alcedo
