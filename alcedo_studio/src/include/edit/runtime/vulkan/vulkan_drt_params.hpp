//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <array>
#include <cstdint>

#include "edit/operators/utils/color_utils.hpp"
#include "edit/runtime/vulkan/vulkan_drt_layout.h"

namespace alcedo {

/// The DRT parameter block of the Vulkan display transform: the words of `vulkan_drt_layout.h`.
/// Floats are stored by their bits, ints as two's complement.
using VulkanDrtParamBlock = std::array<std::uint32_t, ALCEDO_VK_DRT_WORDS>;

static_assert(sizeof(VulkanDrtParamBlock) == 11096,
              "the block has the bytes of OpenClToOutputParams");

/**
 * @brief Copy a resolved DRT output transform into the Vulkan DRT parameter block (phase VK12).
 *
 * The bytes equal the bytes of `PackOpenClDrtParams` for the same input: ACES 2.0 copies the four
 * lookup tables (the fourth word of each cusp is 0); OpenDRT leaves them zero. Pure value function.
 *
 * @throws std::runtime_error when an ACES 2.0 transform has no resolved tables.
 */
[[nodiscard]] auto PackVulkanDrtParams(const ColorUtils::TO_OUTPUT_Params& resolved)
    -> VulkanDrtParamBlock;

}  // namespace alcedo

#endif  // HAVE_VULKAN
