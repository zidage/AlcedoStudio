//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_drt_pass.hpp"
#include "edit/runtime/opencl/opencl_mask_pass.hpp"
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
                     const PreparedRawInput& input, PipelineDocument& document) {
    ExecuteOpenClDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::UploadRgb> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& input, PipelineDocument& document) {
    ExecuteOpenClDevelop(device, plan, input, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::GeometryResample> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument&) {
    ExecuteOpenClGeometryResample(device, plan);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::CameraToAp1> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document) {
    ExecuteOpenClCameraColor(device, plan, document);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::MaskEvaluate> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                     const CompiledMaskSource& source) {
    (void)ExecuteOpenClMask(device, plan, document, compiled_grade, source);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::MaskUnion> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document,
                     const CompiledGradeNode& compiled_grade) {
    (void)ExecuteOpenClMaskUnion(device, plan, document, compiled_grade);
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::PrimaryColorGrade> {
  static auto Encode(OpenClRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput& prepared, PipelineDocument& document,
                     const CompiledGradeNode& compiled_grade, const FrameSceneBinding& scene)
      -> FrameSceneBinding {
    return ExecuteOpenClPrimaryGrade(device, plan, prepared, document, compiled_grade, scene)
        .output_binding;
  }
};

template <>
struct PassEncoder<OpenClBackend, GpuPassKind::Drt> {
  static void Encode(OpenClRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                     PipelineDocument& document, const FrameSceneBinding& scene) {
    (void)ExecuteOpenClDrt(device, plan, document, scene);
  }
};

}  // namespace alcedo

#endif
