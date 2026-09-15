//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_drt_pass.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#include "edit/runtime/pass_encoder.hpp"

namespace alcedo {

template <>
struct PassEncoder<MetalBackend, GpuPassKind::UploadRaw> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document) {
    ExecuteMetalDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::UploadRgb> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document) {
    ExecuteMetalDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::GeometryResample> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&) {
    ExecuteMetalGeometryResample(device, plan);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::CameraToAp1> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document) {
    ExecuteMetalCameraColor(device, plan, document);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::MaskEvaluate> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                     const CompiledMaskSource& source) {
    (void)ExecuteMetalMask(device, plan, document, compiled_grade, source);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::MaskUnion> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document,
                     const CompiledGradeNode& compiled_grade) {
    (void)ExecuteMetalMaskUnion(device, plan, document, compiled_grade);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::PrimaryColorGrade> {
  static auto Encode(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document,
                     const CompiledGradeNode& compiled_grade, const FrameSceneBinding& scene)
      -> FrameSceneBinding {
    return ExecuteMetalPrimaryGrade(device, plan, input, document, compiled_grade, scene)
        .output_binding;
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::Drt> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, const FrameSceneBinding& scene) {
    (void)ExecuteMetalDrt(device, plan, document, scene);
  }
};

}  // namespace alcedo

#endif
