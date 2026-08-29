//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/graph_ids.hpp"

namespace alcedo {

inline const AdjustmentInstanceId kDevelopCameraColorSlot{"camera_color"};

/**
 * @brief GPU parameter body for CameraColorPass.
 *
 * CPU interpolation writes @ref camera_to_ap1 here. The kernel only multiplies
 * camera RGB by this matrix. Not serialized in pipeline JSON.
 */
struct alignas(16) CameraColorGpuParams {
  float camera_to_ap1[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float pad[3]           = {};
};

}  // namespace alcedo
