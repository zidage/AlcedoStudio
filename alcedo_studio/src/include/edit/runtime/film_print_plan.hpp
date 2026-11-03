//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string_view>

#include "edit/graph/graph_ids.hpp"

namespace alcedo {

/**
 * @brief Graph value for the canonical Halation result of the DRT node @p drt_id.
 *
 * Like the scatter image of `DiffusionScatterId`, the result covers the full reference frame.
 * Full-edit renders publish it; viewport ROI renders sample it.
 */
[[nodiscard]] inline auto FilmHalationResultId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"film.halation.0"}};
}

/// Graph value for the canonical Enlarger flare result. See @ref FilmHalationResultId.
[[nodiscard]] inline auto FilmFlareResultId(const NodeId& drt_id) -> GraphValueId {
  return {drt_id, PortId{"film.flare.0"}};
}

/// True when @p id is a canonical film print result.
[[nodiscard]] inline auto IsFilmPrintResultPort(const GraphValueId& id) -> bool {
  const std::string_view port = id.output_port.Value();
  return port == "film.halation.0" || port == "film.flare.0";
}

}  // namespace alcedo
