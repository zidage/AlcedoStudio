//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "io/image/export_color_profile_config.hpp"

namespace alcedo {

/**
 * @brief Resolves DRT node parameters into the display output transform used by all GPU
 * backends.
 *
 * The resolver reads only the supplied `DrtPayload`. It owns no state apart from the
 * process-wide ACES and OpenDRT precompute caches in `odt_cpu`, which are keyed by the resolved
 * inputs and guarded by their own locks. Backends call it when the DRT parameters are dirty or an
 * export overrides the encoding, not per frame.
 */
class DrtOutputResolver {
 public:
  /**
   * @brief Resolve @p payload into @p output.
   *
   * @param payload         DRT parameters read under the Model lock by the caller.
   * @param export_encoding Optional export encoding (space, EOTF, peak luminance) that replaces
   *                        the payload encoding fields. The live Model is not changed.
   * @param output          Receives the resolved transform. Unchanged on failure.
   * @param error           Receives the failure message. May be null.
   * @return false when the method, a color space, or the EOTF is not a known enumerator, the
   *         peak luminance is not positive, or OpenDRT does not support the encoding space and
   *         EOTF pair. No default transform is substituted.
   */
  [[nodiscard]] static auto Resolve(const DrtPayload&               payload,
                                    const ExportColorProfileConfig* export_encoding,
                                    ColorUtils::TO_OUTPUT_Params* output, std::string* error)
      -> bool;

  /**
   * @brief Resolve the parameters of @p drt for one DRT pass.
   *
   * Copies the payload through the Model API so the ACES and OpenDRT precompute does not run
   * under the Model lock.
   *
   * @param caller Prefix for the error message, for example `ExecuteCudaDrt`.
   * @throws std::runtime_error with the `Resolve` message when resolution fails.
   */
  [[nodiscard]] static auto ResolveNode(
      const DrtNodeModel& drt, const std::optional<ExportColorProfileConfig>& export_encoding,
      std::string_view caller) -> ColorUtils::TO_OUTPUT_Params;
};

}  // namespace alcedo
