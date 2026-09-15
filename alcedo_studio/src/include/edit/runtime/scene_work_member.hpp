//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/**
 * @brief Index of one RGBA32F member in the render-workspace scene work pair.
 *
 * The pair has exactly two members. Grades and DRT/Post alternate between them.
 * This value does not identify a GraphValueId or a published result.
 */
enum class SceneWorkMember : std::uint8_t { Member0 = 0, Member1 = 1 };

/**
 * @brief Return the other member of the two-image pair.
 *
 * @param member One allocated work member.
 */
[[nodiscard]] constexpr auto PeerOf(SceneWorkMember member) -> SceneWorkMember {
  return member == SceneWorkMember::Member0 ? SceneWorkMember::Member1 : SceneWorkMember::Member0;
}

}  // namespace alcedo
