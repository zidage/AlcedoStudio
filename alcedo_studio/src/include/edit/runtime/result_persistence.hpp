//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/frame_presentation_types.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo {

/**
 * @brief Which published GPU results a render may look up or replace.
 *
 * Derived from @ref FrameRole, never from long-edge size. Interactive and Detail
 * keep the current result of every graph value. QualityBase may reuse and publish
 * only `develop:sensor_linear`; Geometry and every downstream result stay
 * submission-local so Interactive 2560px outputs are not replaced.
 *
 * Owner: render workspace for one BeginRender/EndRender. Thread: render thread.
 * Failure: a miss executes the pass; it does not clear unrelated published results.
 */
enum class ResultPersistenceScope : std::uint8_t {
  AllCurrentResults,
  SensorDevelopOnly,
};

/**
 * @brief Persistence used by an editor frame role.
 *
 * Thumbnail/export @ref RenderCachePolicy::BypassSessionCache uses a one-shot
 * workspace instead of this scope.
 */
[[nodiscard]] inline auto ResultPersistenceScopeForRole(FrameRole role) -> ResultPersistenceScope {
  return role == FrameRole::QualityBase ? ResultPersistenceScope::SensorDevelopOnly
                                        : ResultPersistenceScope::AllCurrentResults;
}

/**
 * @brief True when @p id may be read from or written to the persistent result cache.
 *
 * @p sensor_linear is the compiled Develop sensor output for this plan.
 */
[[nodiscard]] inline auto PersistsGraphValue(ResultPersistenceScope scope, const GraphValueId& id,
                                             const GraphValueId& sensor_linear) -> bool {
  switch (scope) {
    case ResultPersistenceScope::AllCurrentResults:
      return true;
    case ResultPersistenceScope::SensorDevelopOnly:
      return id == sensor_linear;
  }
  return true;
}

}  // namespace alcedo
