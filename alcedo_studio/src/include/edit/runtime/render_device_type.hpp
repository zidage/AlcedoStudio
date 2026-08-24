//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

namespace alcedo {

template <class Backend>
class BasicRenderDevice;

/**
 * @brief Maps a render backend to its session device type.
 *
 * The primary type is @ref BasicRenderDevice. CUDA specializes this to
 * @ref CudaRenderDevice so Neural and DRT session objects stay on that device.
 */
template <class Backend>
struct RenderDeviceType {
  using Type = BasicRenderDevice<Backend>;
};

}  // namespace alcedo
