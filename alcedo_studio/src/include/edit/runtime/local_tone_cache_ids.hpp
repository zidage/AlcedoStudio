//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "edit/graph/graph_ids.hpp"

namespace alcedo {

/**
 * @brief Graph value for the current canonical LLF source plane of @p grade_id.
 *
 * Matches each GPU backend's `local_tone.source.0` storage.
 */
[[nodiscard]] inline auto LocalToneSourceId(const NodeId& grade_id) -> GraphValueId {
  return {grade_id, PortId{"local_tone.source.0"}};
}

/**
 * @brief Graph value for the current canonical LLF result plane of @p grade_id.
 *
 * Matches each GPU backend's `local_tone.result.0` storage.
 */
[[nodiscard]] inline auto LocalToneResultId(const NodeId& grade_id) -> GraphValueId {
  return {grade_id, PortId{"local_tone.result.0"}};
}

[[nodiscard]] inline auto LocalToneLevelId(const NodeId& grade_id, const char* family, int level)
    -> GraphValueId {
  return {grade_id, PortId{std::string{"local_tone."} + family + "." + std::to_string(level)}};
}

}  // namespace alcedo
