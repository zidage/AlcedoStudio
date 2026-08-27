//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "decoders/processor/raw_demosaic_method.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/graph/develop_node_model.hpp"

namespace alcedo {

/**
 * @brief Resolve the product demosaic method from Develop params and CFA.
 *
 * Reduced-resolution inputs (downsample_passes != 0) always use Legacy.
 * Default is Legacy on Bayer and Neural Engine on X-Trans.
 */
[[nodiscard]] inline auto ResolveDevelopDemosaicMethod(const DevelopPayload& params,
                                                       RawCfaKind cfa_kind,
                                                       std::uint32_t downsample_passes)
    -> RawDemosaicMethod {
  if (downsample_passes != 0) {
    return RawDemosaicMethod::Legacy;
  }
  const auto parsed = RawDemosaicMethodFromString(params.demosaic_method);
  if (parsed != RawDemosaicMethod::Default) {
    return parsed;
  }
  return cfa_kind == RawCfaKind::XTrans6x6 ? RawDemosaicMethod::NeuralEngine
                                           : RawDemosaicMethod::Legacy;
}

}  // namespace alcedo
