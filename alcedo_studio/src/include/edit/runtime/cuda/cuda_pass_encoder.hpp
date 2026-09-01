//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_drt_pass.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/pass_encoder.hpp"

namespace alcedo {

/**
 * @brief CUDA sensor develop (raw or RGB upload plus the compiled develop sequence).
 */
template <>
struct PassEncoder<CudaBackend, GpuPassKind::UploadRaw> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*) {
    ExecuteCudaDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::UploadRgb> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*) {
    ExecuteCudaDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::GeometryResample> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    ExecuteCudaGeometryResample(device, plan);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::CameraToAp1> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore*) {
    ExecuteCudaCameraColor(device, plan, document);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::MaskEvaluate> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore* mask_store,
                     const CompiledGradeNode& compiled_grade, const CompiledMaskSource& source,
                     std::span<const ActiveRasterMaskInput> active_raster_masks = {}) {
    (void)ExecuteCudaMask(device, plan, document, compiled_grade, source, mask_store,
                          active_raster_masks);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::MaskUnion> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore*,
                     const CompiledGradeNode& compiled_grade) {
    (void)ExecuteCudaMaskUnion(device, plan, document, compiled_grade);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::PrimaryColorGrade> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*,
                     const CompiledGradeNode& compiled_grade) {
    (void)ExecuteCudaPrimaryGrade(device, plan, input, document, compiled_grade);
  }
};

template <>
struct PassEncoder<CudaBackend, GpuPassKind::Drt> {
  static void Encode(CudaRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore*) {
    (void)ExecuteCudaDrt(device, plan, document);
  }
};

}  // namespace alcedo
