//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include "edit/runtime/pass_encoder.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"
#include "edit/runtime/vulkan/vulkan_display_to_ap1_pass.hpp"
#include "edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp"
#include "edit/runtime/vulkan/vulkan_mask_pass.hpp"
#include "edit/runtime/vulkan/vulkan_primary_grade_pass.hpp"

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

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::GeometryResample> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     const PipelineDocument&) {
    ExecuteVulkanGeometryResample(device, plan);
  }
};

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::CameraToAp1> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     const PipelineDocument& document) {
    ExecuteVulkanCameraColor(device, plan, document);
  }
};

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::DisplayToAp1> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, const PipelineDocument& document) {
    ExecuteVulkanDisplayToAp1(device, plan, input, document);
  }
};

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::MaskEvaluate> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                     const CompiledMaskSource& source, const FrameSceneBinding& scene) {
    (void)ExecuteVulkanMask(device, plan, document, compiled_grade, source, scene);
  }
};

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::MaskUnion> {
  static void Encode(VulkanRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     const PipelineDocument& document, const CompiledGradeNode& compiled_grade) {
    (void)ExecuteVulkanMaskUnion(device, plan, document, compiled_grade);
  }
};

template <>
struct PassEncoder<VulkanBackend, GpuPassKind::PrimaryColorGrade> {
  static auto Encode(VulkanRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& prepared, const PipelineDocument& document,
                     const CompiledGradeNode& compiled_grade, const FrameSceneBinding& scene,
                     CurveInputHistogramRecord* histogram = nullptr) -> FrameSceneBinding {
    return ExecuteVulkanPrimaryGrade(device, plan, prepared, document, compiled_grade, scene,
                                     histogram)
        .output_binding;
  }
};

}  // namespace alcedo

#endif  // HAVE_VULKAN
