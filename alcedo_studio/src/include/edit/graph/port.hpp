//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/graph_ids.hpp"

namespace alcedo {

enum class PortDataType {
  SceneImage,
  DisplayImage,
  Mask,
};

/**
 * @brief Static description of one node port.
 *
 * @param required Incoming required ports must have exactly one edge. Optional
 *        ports (ColorGrade mask) may have zero or one edge.
 */
struct PortDescriptor {
  PortId       id;
  PortDataType data_type  = PortDataType::SceneImage;
  bool         required   = true;
};

}  // namespace alcedo
