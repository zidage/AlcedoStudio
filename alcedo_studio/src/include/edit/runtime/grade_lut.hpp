//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/runtime/content_key.hpp"
#include "utils/lut/cube_lut.hpp"

namespace alcedo {

/**
 * @brief Device-uploadable form of a parsed .cube file plus its content identity.
 *
 * Produced once per cube file version by @ref TryPackGradeLut and shared
 * immutably across callers and backends. @ref key is the @ref ContentKey the
 * backend LUT caches index on; it is computed at pack time so an unchanged
 * file never pays the byte-wise hash again.
 */
struct PackedGradeLut {
  std::vector<std::byte> rgba;
  std::uint32_t          edge = 0;
  ContentKey             key{};
};

/**
 * @brief Pack a parsed 3D cube as tightly packed RGBA32F voxels, X varying fastest.
 */
[[nodiscard]] auto PackCubeLutRgba(const CubeLut& lut) -> std::vector<std::byte>;

/**
 * @brief Load the ColorGrade LMT cube when a path is set.
 *
 * The packed result is memoized per normalized cube path and file stamp
 * (size + last write time): repeated calls for an unchanged file return the
 * same immutable instance instead of re-reading, re-parsing, and re-hashing
 * the .cube text on every pipeline execute. A changed file is parsed again
 * and replaces the entry; the old instance stays alive until its in-flight
 * callers release it.
 *
 * @return nullptr when the node has no LMT model or the path is empty.
 * @throws std::runtime_error when a path is set but the cube cannot be parsed as 3D.
 */
[[nodiscard]] auto TryPackGradeLut(const ColorGradeNodeModel& grade)
    -> std::shared_ptr<const PackedGradeLut>;

}  // namespace alcedo
