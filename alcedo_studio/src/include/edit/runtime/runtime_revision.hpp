//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Monotonic session revision for cached GPU results.
 *
 * Assigned by @ref RuntimeInvalidationState. Not a content hash and not a
 * persisted history number. Zero means unpublished.
 */
using RuntimeRevision = std::uint64_t;

}  // namespace alcedo
