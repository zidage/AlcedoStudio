//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

enum class ImportErrorCode : uint8_t {
  UNKNOWN = 0,
  FILE_NOT_FOUND,
  UNSUPPORTED_FORMAT,
  READ_FAILED,
  METADATA_EXTRACTION_FAILED,
  SLEEVE_CREATE_FAILED,
  DB_WRITE_FAILED,
  CANCELED,
  UNSUPPORTED_NIKON_HE_RAW,
  /// The content is a supported type, but its category is not allowed by ImportOptions.
  EXCLUDED_TYPE,
};

}  // namespace alcedo
