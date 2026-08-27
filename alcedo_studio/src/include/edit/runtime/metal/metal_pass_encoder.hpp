//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "edit/runtime/metal/metal_develop_pass.hpp"
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
struct PassEncoder<MetalBackend, GpuPassKind::PrimaryColorGrade> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    ExecuteMetalIdentityCopy(device, plan.develop_output, plan.primary_grade_output,
                             ImageExtent{plan.geometry.render_extent.width,
                                         plan.geometry.render_extent.height});
  }
};

template <>
struct PassEncoder<MetalBackend, GpuPassKind::Drt> {
  static void Encode(MetalRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    ExecuteMetalIdentityCopy(device, plan.primary_grade_output, plan.display_output,
                             ImageExtent{plan.geometry.render_extent.width,
                                         plan.geometry.render_extent.height});
  }
};

}  // namespace alcedo

#endif
