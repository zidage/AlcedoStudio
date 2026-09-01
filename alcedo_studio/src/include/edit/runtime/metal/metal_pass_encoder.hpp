//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_drt_pass.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#include "edit/runtime/pass_encoder.hpp"

namespace alcedo {

template <>
struct PassEncoder<MetalBackend, GpuPassKind::UploadRaw> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*) {
    ExecuteMetalDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::UploadRgb> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*) {
    ExecuteMetalDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::GeometryResample> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    ExecuteMetalGeometryResample(device, plan);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::CameraToAp1> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore*) {
    ExecuteMetalCameraColor(device, plan, document);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::MaskEvaluate> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore* mask_store,
                     const CompiledGradeNode& compiled_grade,
                     std::span<const ActiveRasterMaskInput> active_raster_masks = {}) {
    (void)ExecuteMetalMask(device, plan, document, compiled_grade, mask_store, active_raster_masks);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::PrimaryColorGrade> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*,
                     const CompiledGradeNode& compiled_grade) {
    (void)ExecuteMetalPrimaryGrade(device, plan, input, document, compiled_grade);
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::Drt> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore*) {
    (void)ExecuteMetalDrt(device, plan, document);
  }
};

}  // namespace alcedo

#endif
