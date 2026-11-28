//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <stdexcept>

#include "decoders/processor/raw_rgb_linearization_params.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/ai_denoise_error.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/develop_compile_source.hpp"

namespace alcedo {

/**
 * @brief The `linearize_rgb` parameters that the Develop passes use for @p input.
 *
 * Decoded RGB uses its stored parameters unchanged (identity without them). The AI Denoise input
 * (raw_norm::BuildDenoisedRgbLinearization) with the highlight recovery off also takes the clamp
 * of the CFA path after the white balance, so each channel is clamped to [0, min(wb, 1)].
 */
[[nodiscard]] inline auto DevelopRgbLinearization(const PreparedRawInput& input,
                                                  bool                    highlights_reconstruct)
    -> RawRgbLinearizationParams {
  auto params = input.rgb_linearization.value_or(RawRgbLinearizationParams{});
  if (input.input_kind == RawInputKind::DenoisedRgb && !highlights_reconstruct) {
    for (float& limit : params.range_max) {
      limit = (std::min)(limit, 1.0f);
    }
  }
  return params;
}

/**
 * @brief Reject a Develop input that does not agree with the AI Denoise switch (D12, 1.4).
 *
 * While the switch is on, a full-size render needs the denoised input; a reduced input
 * (thumbnail, semantic input image) renders the CFA path without the model. A denoised input
 * needs the switch on.
 * @throws AiDenoiseError of the kind `SensorNotSupported` for a photo that is not a Bayer RAW
 *         file (D14); std::runtime_error with the reason for an input that does not agree with
 *         the switch. The caller reports it as the render error.
 */
inline void RequireDevelopInputForAiDenoise(const DevelopPayload&       params,
                                            const DevelopCompileSource& source) {
  const bool denoised = source.kind == DevelopInputKind::DenoisedRgb;
  if (!params.ai_denoise.has_value()) {
    if (denoised) {
      throw std::runtime_error("AI Denoise is off and the prepared input is denoised");
    }
    return;
  }
  if (denoised || source.downsample_passes != 0) {
    return;
  }
  if (source.kind == DevelopInputKind::BayerCfa) {
    throw std::runtime_error("AI Denoise is on and the prepared input is not denoised");
  }
  throw AiDenoiseError(AiDenoiseErrorKind::SensorNotSupported,
                       "AI Denoise supports Bayer sensors only");
}

}  // namespace alcedo
