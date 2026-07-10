//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace alcedo {

// Product-facing demosaic selection. "Default" resolves from the CFA at decode time:
// Bayer -> Legacy, X-Trans -> Neural Engine. Reduced-resolution decodes always resolve to Legacy.
enum class RawDemosaicMethod {
  Default,
  Legacy,
  NeuralEngine,
};

inline auto RawDemosaicMethodToString(const RawDemosaicMethod method) -> std::string_view {
  switch (method) {
    case RawDemosaicMethod::Default:
      return "default";
    case RawDemosaicMethod::Legacy:
      return "legacy";
    case RawDemosaicMethod::NeuralEngine:
      return "neural_engine";
  }
  return "default";
}

inline auto RawDemosaicMethodFromString(const std::string_view value) -> RawDemosaicMethod {
  if (value == "default") {
    return RawDemosaicMethod::Default;
  }
  if (value == "legacy" || value == "classical") {
    return RawDemosaicMethod::Legacy;
  }
  if (value == "neural_engine" || value == "demosaicnet") {
    return RawDemosaicMethod::NeuralEngine;
  }
  throw std::runtime_error("Unknown RAW demosaic method " + std::string(value));
}

}  // namespace alcedo
