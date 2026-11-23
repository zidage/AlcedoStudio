//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

namespace alcedo {

enum class GpuBackendKind {
  None,
  CUDA,
  OpenCL,
  Metal,
  /// In development (#1036). Only a build with ALCEDO_ENABLE_VULKAN resolves to it, and no
  /// renderer accepts it before the phase VK17 of the Vulkan plan.
  Vulkan,
};

}  // namespace alcedo
