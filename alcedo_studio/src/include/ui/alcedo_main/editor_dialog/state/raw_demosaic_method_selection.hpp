//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace alcedo::ui {

// UI/domain representation of the persisted RAW Method field. The decoder translates the
// serialized token into its execution enum, keeping editor state independent of decoder details.
enum class RawDemosaicMethodSelection {
  Default,
  Legacy,
  NeuralEngine,
};

inline auto RawDemosaicMethodSelectionToString(const RawDemosaicMethodSelection method)
    -> std::string_view {
  switch (method) {
    case RawDemosaicMethodSelection::Default:
      return "default";
    case RawDemosaicMethodSelection::Legacy:
      return "legacy";
    case RawDemosaicMethodSelection::NeuralEngine:
      return "neural_engine";
  }
  return "default";
}

inline auto RawDemosaicMethodSelectionFromString(const std::string_view value)
    -> RawDemosaicMethodSelection {
  if (value == "default") {
    return RawDemosaicMethodSelection::Default;
  }
  if (value == "legacy" || value == "classical") {
    return RawDemosaicMethodSelection::Legacy;
  }
  if (value == "neural_engine" || value == "demosaicnet") {
    return RawDemosaicMethodSelection::NeuralEngine;
  }
  throw std::runtime_error("Unknown RAW demosaic method selection " + std::string(value));
}

}  // namespace alcedo::ui
