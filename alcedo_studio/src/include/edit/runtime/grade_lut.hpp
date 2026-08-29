//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "utils/lut/cube_lut.hpp"

namespace alcedo {

class ColorGradeNodeModel;

struct PackedGradeLut {
  std::vector<std::byte> rgba;
  std::uint32_t          edge = 0;
};

/**
 * @brief Pack a parsed 3D cube as tightly packed RGBA32F voxels, X varying fastest.
 */
[[nodiscard]] auto PackCubeLutRgba(const CubeLut& lut) -> std::vector<std::byte>;

/**
 * @brief Load the ColorGrade LMT cube when a path is set.
 * @return nullopt when the node has no LMT model or the path is empty.
 * @throws std::runtime_error when a path is set but the cube cannot be parsed as 3D.
 */
[[nodiscard]] auto TryPackGradeLut(const ColorGradeNodeModel& grade)
    -> std::optional<PackedGradeLut>;

}  // namespace alcedo
