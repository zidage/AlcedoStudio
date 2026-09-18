//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_frame_presenter.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/renderer.hpp"
#include "edit/runtime/detail/renderer.inl.hpp"

namespace alcedo {

using MetalRenderer = Renderer<MetalBackend>;

}  // namespace alcedo
