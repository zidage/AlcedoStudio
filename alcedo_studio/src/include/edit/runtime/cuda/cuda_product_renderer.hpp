//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_frame_presenter.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/detail/renderer.inl.hpp"
#include "edit/runtime/renderer.hpp"

namespace alcedo {

using CudaRenderer        = Renderer<CudaBackend>;
using CudaProductRenderer = CudaRenderer;

}  // namespace alcedo
