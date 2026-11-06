//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string_view>

#include "color/color_encoding_math.h"
#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_grain_plan.hpp"

namespace alcedo {

/// The display encoding that the post grain decodes and encodes again.
struct FilmGrainDisplayEncoding {
  int   transfer     = 0;     ///< CE_TF_* id of the display encoding
  float decode_scale = 1.0f;  ///< See `MakeFilmGrainPostParams`
};

/**
 * @brief The grain plan of the DRT/Post Film Grain of @p drt for one render: the 5207 data
 *        (@ref kFilmGrainPostFilm), the film format of @p drt, and the strength of @p model.
 *
 * Reads the shared film stock bundle (`SharedFilmPrintTables`). The plan is a pure function of
 * these inputs and costs a few microseconds, so the render makes it for each frame and keeps no
 * state between frames.
 *
 * @throws std::runtime_error that names the file when the bundle is missing or damaged, and
 *         std::invalid_argument from `MakeFilmGrainPlan`. No fallback grain exists.
 */
[[nodiscard]] inline auto MakeDrtPostFilmGrainPlan(const DrtNodeModel&           drt,
                                                   const IOperatorModel&         model,
                                                   const ResolvedRenderGeometry& geometry)
    -> FilmGrainPlan {
  const auto* grain = dynamic_cast<const FilmGrainModel*>(&model);
  if (grain == nullptr) {
    throw std::runtime_error("DRT/Post Film Grain: the Model is not a Film Grain Model");
  }
  const float strength =
      grain->Read([](const ScalarFloatPayload& payload) { return payload.value; });
  const auto& bundle = SharedFilmPrintTables().Bundle();
  return MakeFilmGrainPlan(geometry, drt.Params().FilmFormat(), bundle.Grain(kFilmGrainPostFilm),
                           FilmGrainTarget::Post, strength);
}

/**
 * @brief The display encoding of the DRT output of @p plan: the EOTF of the export override or of
 *        @p drt, and for ST 2084 the decode scale of the reference white `ColorUtils::ref_lum`.
 *
 * @throws std::runtime_error that starts with @p caller for an unknown EOTF.
 */
[[nodiscard]] inline auto MakeDrtPostFilmGrainEncoding(const DrtNodeModel&  drt,
                                                       const ExecutionPlan& plan,
                                                       std::string_view     caller)
    -> FilmGrainDisplayEncoding {
  const auto eotf = DrtOutputResolver::ResolveNodeEotf(drt, plan.output_color_override, caller);
  FilmGrainDisplayEncoding encoding;
  encoding.transfer = static_cast<int>(eotf);
  encoding.decode_scale =
      eotf == ColorUtils::EOTF::ST2084 ? CE_PQ_PEAK_NITS / ColorUtils::ref_lum : 1.0f;
  return encoding;
}

}  // namespace alcedo
