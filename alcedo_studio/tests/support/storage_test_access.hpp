//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "sleeve/storage.hpp"
#include "storage/store/database.hpp"

namespace alcedo {

/**
 * @brief Database of a Storage for test fixtures.
 *
 * Fixtures use it to write rows that no store writes (an older schema, a broken row, a dropped
 * table) and to count rows directly. Storage declares this type a friend; production code does
 * not define it, and scripts/check_storage_boundary.py rejects the name in alcedo_studio/src.
 */
struct StorageTestAccess {
  static auto GetDatabase(Storage& storage) -> Database& { return storage.database_; }
};

}  // namespace alcedo
