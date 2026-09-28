//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief The kind of work one renderer does. Each renderer has exactly one role.
 *
 * - Interactive: one session device that keeps prepared sources, compiled plans, and published
 *   GPU results between frames of the same binding (@ref RenderBindingKey). Frames may present
 *   to a frame sink. Used by the editor preview.
 * - Batch: one device with a dedicated submission queue. Every render prepares its source and
 *   compiles its plan again, and releases every result resource when it completes. The device is
 *   kept. Used by thumbnails, analysis renditions, and export.
 */
enum class ExecutorRole : std::uint8_t {
  Interactive,
  Batch,
};

/**
 * @brief Identity of the image history that an interactive renderer's GPU resources belong to.
 *
 * A render whose snapshot has a different key releases every resource of the previous binding
 * (results, transients, local tone caches, compiled plans, prepared sources, neural demosaic
 * workspace) before it runs. The device and its queue are kept. Snapshots with the same key are
 * successive states of one loaded history; parameter and topology changes between them are found
 * by revision and by static plan key, not by a release.
 */
struct RenderBindingKey {
  PipelineLineageId lineage;
  sl_element_id_t   element_id = 0;

  [[nodiscard]] static auto Of(const PipelineGraphSnapshot& snapshot) -> RenderBindingKey {
    return RenderBindingKey{.lineage = snapshot.Lineage(), .element_id = snapshot.ElementId()};
  }

  friend auto operator==(const RenderBindingKey&, const RenderBindingKey&) -> bool = default;
};

}  // namespace alcedo
