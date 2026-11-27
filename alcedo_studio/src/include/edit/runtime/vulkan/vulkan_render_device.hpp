//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/basic_render_workspace.hpp"
#include "edit/runtime/render_backend.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"

namespace alcedo {

static_assert(RenderBackend<VulkanBackend>);

/// The render device of the Vulkan backend: workspace, command context, and pass statistics.
using VulkanRenderDevice    = BasicRenderDevice<VulkanBackend>;
using VulkanRenderWorkspace = BasicRenderWorkspace<VulkanBackend>;

}  // namespace alcedo

#endif  // HAVE_VULKAN
