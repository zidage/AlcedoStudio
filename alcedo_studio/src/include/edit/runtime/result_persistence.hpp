//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string_view>

#include "edit/frame_presentation_types.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo {

/**
 * @brief Output port of the canonical prescan of print material Negative Scan
 * (`FilmScanPrescanResultId`).
 *
 * The prescan is the full frame reduced to a long edge of 96 pixels, so its size does not depend on
 * the render. Rule 'frame_auto_colour' reads it, and a viewport ROI must sample it: an ROI has only
 * the pixels of the viewport, so a prescan of its own gives another auto colour for each zoom or
 * pan.
 */
inline constexpr std::string_view kFilmScanPrescanPort = "film.scan_prescan.0";

/**
 * @brief Which published GPU results a render may look up or replace.
 *
 * Derived from @ref FrameRole, never from long-edge size. Interactive and Detail
 * keep the current result of every graph value. QualityBase may reuse and publish
 * only `develop:sensor_linear` and the prescan of Negative Scan (@ref kFilmScanPrescanPort);
 * Geometry and every other downstream result stay submission-local so Interactive 2560px outputs
 * are not replaced. The prescan has a fixed size, so a QualityBase prescan replaces no detail. A
 * settled edit, an undo, or a redo renders only a QualityBase before the Detail patch, so with no
 * QualityBase prescan the patch has no prescan of the current revision.
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
 * Only interactive renders use this scope. A batch renderer (@ref ExecutorRole::Batch) keeps all
 * current results of one render and releases them when the render completes.
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
      return id == sensor_linear || id.output_port.Value() == kFilmScanPrescanPort;
  }
  return true;
}

}  // namespace alcedo
