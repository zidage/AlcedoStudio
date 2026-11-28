// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "utils/file/source_image_status.hpp"

#include <fstream>

#include "utils/file/source_file.hpp"

namespace alcedo {
auto ReadMaintenanceFingerprint(const image_path_t& path) -> std::optional<SourceFileFingerprint> {
  const auto before = ReadSourceFileAttributes(path);
  if (!before) return std::nullopt;
  const auto result = ComputeSourceFileFingerprint(path);
  if (!result.HasFingerprint() || before != ReadSourceFileAttributes(path)) return std::nullopt;
  return result.fingerprint;
}

auto CheckSourceImageStatus(const image_path_t& path) -> SourceImageStatusResult {
  std::error_code error;
  if (!path.is_absolute()) return {SourceImageStatus::Error, "Source path is not absolute"};
  auto volume_path = path.root_path();
#ifdef _WIN32
  if (path.root_name().wstring().starts_with(L"\\\\") && !path.relative_path().empty()) {
    volume_path /= *path.relative_path().begin();
  }
#elif defined(__APPLE__)
  if (path.generic_string().starts_with("/Volumes/")) {
    auto part = path.begin();
    ++part;
    ++part;
    if (part != path.end()) volume_path = std::filesystem::path("/Volumes") / *part;
  }
#endif
  const auto volume = std::filesystem::status(volume_path, error);
  if (error || !std::filesystem::is_directory(volume)) {
    return {SourceImageStatus::Error, "Source volume is unavailable: " + error.message()};
  }
  const auto status = std::filesystem::status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) {
    return {SourceImageStatus::Missing, "Source file does not exist on an available volume"};
  }
  if (error) return {SourceImageStatus::Error, "Source status: " + error.message()};
  if (!std::filesystem::is_regular_file(status))
    return {SourceImageStatus::Error, "Source is not a regular file"};
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {SourceImageStatus::Error, "Source cannot be opened for reading"};
  char byte;
  stream.read(&byte, 1);
  if (stream.bad()) return {SourceImageStatus::Error, "Source read failed"};
  return {SourceImageStatus::Readable, "Source can be read"};
}
}  // namespace alcedo
