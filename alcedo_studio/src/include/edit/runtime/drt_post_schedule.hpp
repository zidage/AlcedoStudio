//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Selected DRT/Post neighborhood launches after disabled adjustments are skipped.
 *
 * Owner: DrtPostExecutor for one compiled DRT encode. Display transform always runs first.
 * Not persisted.
 */
struct DrtPostSchedule {
  std::vector<GradeNeighborParams> enabled;
  bool                             copy_scene_to_post = false;
};

/**
 * @brief Comparable DRT/Post host decisions for equivalent compiled inputs.
 *
 * Backends may differ in kernel APIs; these fields may not.
 */
struct DrtPostDecisionTrace {
  bool        copy_scene_to_post          = false;
  std::size_t enabled_neighborhood_count  = 0;
  bool        display_transform_scheduled = true;
};

/**
 * @brief True when @p neighbor runs in the Post of a display transform of @p method.
 *
 * A disabled adjustment does not run. The film print does not run the Halation operator, because
 * the film model takes this colour from its layers. With @p film_print_grain_in_negative, the film
 * print does not run the Film Grain operator either: the film print adds the grain to the dye
 * amounts of its negative with the value of the same slider (decision U4 of the plan
 * 'film_grain_format_and_stock_plan.md'). Each Model keeps its stored value.
 *
 * @param film_print_grain_in_negative True on a backend whose film print adds the grain to the
 *        negative: CUDA since phase G5. OpenCL and Metal keep the post grain until phase G6.
 */
[[nodiscard]] auto DrtPostNeighborhoodRuns(DrtMethod method, const GradeNeighborParams& neighbor,
                                           bool film_print_grain_in_negative) -> bool;

/**
 * @brief Keep the neighborhood params that @ref DrtPostNeighborhoodRuns selects, in compiler order.
 *
 * An empty enabled list copies the display-transform result to the final display output.
 */
[[nodiscard]] auto MakeDrtPostSchedule(std::span<const GradeNeighborParams> compiled_order,
                                       DrtMethod method, bool film_print_grain_in_negative)
    -> DrtPostSchedule;

/** @brief Record the comparable host decisions from @p schedule. */
[[nodiscard]] auto MakeDrtPostDecisionTrace(const DrtPostSchedule& schedule) -> DrtPostDecisionTrace;

/**
 * @brief Physical destination of one DRT transform or Post neighborhood write.
 *
 * Display is the existing display output. FreeWorkMember is the scene-work peer
 * of the current scene. The last write in @ref DrtWriteSequence is always Display.
 */
enum class DrtWriteTarget : std::uint8_t { Display, FreeWorkMember };

/**
 * @brief Display-transform plus @p neighborhood_count Post writes.
 *
 * Length is @p neighborhood_count + 1. The last target is always Display so
 * frame sink never receives a work member. An even neighborhood count starts
 * on Display; an odd count starts on the free work member.
 */
[[nodiscard]] auto DrtWriteSequence(std::size_t neighborhood_count) -> std::vector<DrtWriteTarget>;

}  // namespace alcedo
