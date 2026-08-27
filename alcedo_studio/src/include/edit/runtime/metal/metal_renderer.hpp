//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/renderer.hpp"

namespace alcedo {

using MetalRenderDevice    = BasicRenderDevice<MetalBackend>;
using MetalRenderWorkspace = BasicRenderWorkspace<MetalBackend>;
using MetalRenderer        = Renderer<MetalBackend>;

}  // namespace alcedo
