//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The one translation unit with the implementation of the VulkanMemoryAllocator library.
// volk.h defines VK_NO_PROTOTYPES, so VMA uses no static Vulkan function. VulkanContext gives VMA
// the functions of volk (vmaImportVulkanFunctionsFromVolk).

#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
