//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

namespace alcedo {

/**
 * @brief How a TransientBufferArena sizes slabs and what it keeps after last use.
 *
 * Selected per render from the task request (session cache vs bypass). Not a
 * pipeline mode flag and not stored on the live document.
 *
 * SessionPacked keeps the editor bump allocator: minimum slabs, quantum growth,
 * and idle capacity for the next interactive frame.
 * ExactRelease allocates only the requested aligned bytes and drops unused slabs
 * after the caller has satisfied the GPU last-use dependency.
 */
enum class TransientAllocationPolicy {
  SessionPacked,
  ExactRelease,
};

}  // namespace alcedo
