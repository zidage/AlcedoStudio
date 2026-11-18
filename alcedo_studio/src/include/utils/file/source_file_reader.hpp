// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "type/type.hpp"
#include "utils/import/import_error_code.hpp"

namespace alcedo {
class SourceReadError : public std::runtime_error {
 public:
  SourceReadError(ImportErrorCode code, const std::string& message)
      : std::runtime_error(message), code(code) {}
  ImportErrorCode code;
};

/// Holds a readable source through metadata extraction and persistence without reading its
/// contents. Windows denies writes/replacement; other platforms recheck file identity, size,
/// and timestamps. Same-size changes between imports are deliberately not detected.
class SourceFileReader {
 public:
  explicit SourceFileReader(const image_path_t& path);
  ~SourceFileReader();
  SourceFileReader(const SourceFileReader&)                    = delete;
  auto operator=(const SourceFileReader&) -> SourceFileReader& = delete;
  auto Size() const -> uint64_t { return size_; }
  /// Throws SOURCE_CHANGED if the source changed while this reader was held. No content read.
  void Validate() const;

 private:
  image_path_t path_;
  std::FILE*   file_                 = nullptr;
  uint64_t     size_                 = 0;
  uint64_t     device_               = 0;
  uint64_t     inode_                = 0;
  int64_t      modified_seconds_     = 0;
  int64_t      modified_nanoseconds_ = 0;
  int64_t      changed_seconds_      = 0;
  int64_t      changed_nanoseconds_  = 0;
};
}  // namespace alcedo
