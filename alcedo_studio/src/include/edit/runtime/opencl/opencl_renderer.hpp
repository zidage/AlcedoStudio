//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "edit/runtime/renderer.hpp"
#include "edit/runtime/detail/renderer.inl.hpp"

namespace alcedo {

using OpenClRenderer = Renderer<OpenClBackend>;

}  // namespace alcedo

#endif  // HAVE_OPENCL
