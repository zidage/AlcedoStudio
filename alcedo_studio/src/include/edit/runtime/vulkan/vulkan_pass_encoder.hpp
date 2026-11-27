//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/runtime/pass_encoder.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"

namespace alcedo {

// The pass encoders of the Vulkan backend (plan #1036). A kind without a specialization here
// fails in the primary template; the later phases add the other kinds.

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::UploadRaw> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, const PipelineDocument& document) {
    ExecuteVulkanDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::UploadRgb> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, const PipelineDocument& document) {
    ExecuteVulkanDevelop(device, plan, input, document);
  }
};

}  // namespace alcedo

#endif  // HAVE_VULKAN
