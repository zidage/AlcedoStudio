//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"
#include "edit/runtime/pass_encoder.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

inline void CopyOpenClGraphImage(OpenClRenderDevice& device, const GraphValueId& src_id,
                                 const GraphValueId& dst_id) {
  auto& workspace = device.Workspace();
  auto* source    = workspace.Images().Find(src_id);
  if (source == nullptr || source->Empty()) {
    throw std::runtime_error("OpenCL identity copy: missing source image");
  }
  const auto width  = source->Texture().Width();
  const auto height = source->Texture().Height();
  auto& dest =
      workspace.AcquireImageForWrite(dst_id, {width, height, source->Texture().Format()});
  source = workspace.Images().Find(src_id);
  if (source == nullptr) {
    throw std::runtime_error("OpenCL identity copy: source lost during acquire");
  }
  workspace.Device().CopyTexture2D(source->Texture(), dest.Texture(), device.CommandContext());
}

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::UploadRaw> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*) {
    ExecuteOpenClDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::UploadRgb> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document, MaskStore*) {
    ExecuteOpenClDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::GeometryResample> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    ExecuteOpenClGeometryResample(device, plan);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::CameraToAp1> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, MaskStore*) {
    ExecuteOpenClCameraColor(device, plan, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::MaskEvaluate> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    auto& workspace = device.Workspace();
    auto* source    = workspace.Images().Find(plan.geometry_output);
    if (source == nullptr || source->Empty()) {
      throw std::runtime_error("OpenCL mask identity: missing geometry.scene_source");
    }
    (void)workspace.AcquireImageForWrite(
        plan.mask_output,
        {source->Texture().Width(), source->Texture().Height(), TextureFormat::R8});
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::PrimaryColorGrade> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& prepared, PipelineDocument& document, MaskStore*) {
    (void)ExecuteOpenClPrimaryGrade(device, plan, prepared, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::Drt> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&, MaskStore*) {
    CopyOpenClGraphImage(device, plan.primary_grade_output, plan.display_output);
  }
};

}  // namespace alcedo

#endif
