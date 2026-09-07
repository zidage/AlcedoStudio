//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/grade_executor.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_local_tone_pass.hpp"
#include "edit/runtime/opencl/opencl_neighbor_dispatch.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

struct PrimaryGradeDispatchParams {
  std::uint32_t command_count   = 0;
  std::uint32_t command_offset  = 0;
  std::uint32_t lut_edge        = 0;
  float         local_reference = 0.0f;
  std::uint32_t width           = 0;
  std::uint32_t pad[3]          = {};
};

static_assert(sizeof(PrimaryGradeDispatchParams) == 32);

auto AcquireRgba(OpenClRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<OpenClBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto AcquireOpenClScratch(OpenClRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
    -> ResourceLease<OpenClBackend> {
  return workspace.Textures().Acquire({width, height, TextureFormat::Rgba32f});
}

auto EnsureBuffer(OpenClRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> OpenClBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: command buffer store failed");
  }
  return *stored;
}

void DispatchKernel(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
                    std::uint32_t height, std::size_t local_edge = 16) {
  const std::size_t local[2]  = {local_edge, local_edge};
  const std::size_t global[2] = {
      ((static_cast<std::size_t>(width) + local_edge - 1) / local_edge) * local_edge,
      ((static_cast<std::size_t>(height) + local_edge - 1) / local_edge) * local_edge};
  cl_event event = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "OpenCL Primary Grade enqueue");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

void SetImageKernelArgs(cl_kernel kernel, cl_mem src, cl_mem dst) {
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src),
              "OpenCL Primary Grade source argument");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst),
              "OpenCL Primary Grade destination argument");
}

void EnqueueGradePointwise(OpenClRenderDevice& device, const OpenClBackend::Texture2D& src,
                       OpenClBackend::Texture2D& dst, const OpenClBackend::Buffer& params,
                       const OpenClBackend::Buffer& commands, std::uint32_t command_offset,
                       std::uint32_t command_count, const OpenClLutBinding& lut,
                       std::uint32_t width, std::uint32_t height) {
  const auto kernel_name = OpenCL::GpuDag::kPrimaryGradePointwiseKernelName;
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kPrimaryGradeProgramName,
                                                        kernel_name);
  SetImageKernelArgs(kernel, src.Native(), dst.Native());

  const auto params_mem   = params.Native();
  const auto commands_mem = commands.Native();
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &params_mem),
              "OpenCL Primary Grade parameter argument");
  CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_mem), &commands_mem),
              "OpenCL Primary Grade command argument");

  PrimaryGradeDispatchParams dispatch;
  dispatch.command_count   = command_count;
  dispatch.command_offset  = command_offset;
  dispatch.lut_edge        = lut.edge_size;
  dispatch.local_reference = local_tone_mapping::kAcesccMiddleGray;
  dispatch.width           = width;
  CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(dispatch), &dispatch),
              "OpenCL Primary Grade dispatch argument");

  const auto lut_mem = lut.native;
  if (lut_mem == nullptr) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: LUT resource is missing");
  }
  CheckOpenCl(clSetKernelArg(kernel, 5, sizeof(cl_mem), &lut_mem),
              "OpenCL Primary Grade LUT argument");
  DispatchKernel(device, kernel, width, height);
}

void EnqueueGradeMix(OpenClRenderDevice& device, const OpenClBackend::Texture2D& source,
                 const OpenClBackend::Texture2D& adjusted, OpenClBackend::Texture2D& dst, float mix,
                 const OpenClBackend::Texture2D* mask, std::uint32_t width, std::uint32_t height) {
  const auto kernel_name = mask == nullptr ? OpenCL::GpuDag::kPrimaryGradeMixKernelName
                                           : OpenCL::GpuDag::kPrimaryGradeMixMaskedKernelName;
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kPrimaryGradeProgramName,
                                                        kernel_name);
  const auto source_mem   = source.Native();
  const auto adjusted_mem = adjusted.Native();
  const auto dst_mem      = dst.Native();
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &source_mem),
              "OpenCL Primary Grade mix source argument");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &adjusted_mem),
              "OpenCL Primary Grade mix adjusted argument");
  CheckOpenCl(clSetKernelArg(kernel, 2, sizeof(cl_mem), &dst_mem),
              "OpenCL Primary Grade mix destination argument");
  if (mask != nullptr) {
    const auto mask_mem = mask->Native();
    CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(cl_mem), &mask_mem),
                "OpenCL Primary Grade mix mask argument");
    CheckOpenCl(clSetKernelArg(kernel, 4, sizeof(float), &mix),
                "OpenCL Primary Grade mix value argument");
  } else {
    CheckOpenCl(clSetKernelArg(kernel, 3, sizeof(float), &mix),
                "OpenCL Primary Grade mix value argument");
  }
  DispatchKernel(device, kernel, width, height);
}

auto LoadOpenClGradeLut(OpenClRenderDevice& device, ColorGradeNodeModel& grade) -> OpenClLutBinding {
  const auto packed = TryPackGradeLut(grade);
  if (!packed.has_value()) {
    return device.Workspace().Device().DummyLut();
  }
  ContentHash hash;
  hash.MixBytes(packed->rgba);
  hash.MixU32(packed->edge);
  return device.Workspace().Device().AcquireLut(hash.Key(), packed->rgba, packed->edge,
                                                device.CommandContext());
}


struct OpenClGradeOps {
  using Device            = OpenClRenderDevice;
  using Backend           = OpenClBackend;
  using Texture           = OpenClBackend::Texture2D;
  using Scratch           = ResourceLease<OpenClBackend>*;
  using HorizontalScratch = ResourceLease<OpenClBackend>;
  using LutBinding        = OpenClLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteOpenClPrimaryGrade";

  static void AliasOutput(OpenClRenderDevice& device, const GraphValueId& output,
                          const GraphValueId& input) {
    device.Workspace().AliasImageFrom(output, input);
  }

  static auto UploadFusedCommands(OpenClRenderDevice& device, const NodeId& grade_id,
                                  const std::vector<std::uint32_t>& fused_offsets)
      -> std::uint32_t {
    if (fused_offsets.empty()) {
      return 0;
    }
    auto&              workspace = device.Workspace();
    const GraphValueId command_id{grade_id, PortId{"runtime.order"}};
    const auto         bytes = fused_offsets.size() * sizeof(fused_offsets[0]);
    ContentHash        topology;
    topology.MixText(grade_id.Value());
    topology.MixU32(static_cast<std::uint32_t>(fused_offsets.size()));
    for (const auto offset : fused_offsets) {
      topology.MixU32(offset);
    }
    const auto topology_hash = topology.Key().hash;
    const auto* existing     = workspace.Values().Find(command_id);
    const bool needs_upload =
        existing == nullptr || existing->Bytes() < bytes ||
        workspace.Device().GradeCommandTopologyHash() != topology_hash;
    auto& buffer =
        EnsureBuffer(workspace, command_id, std::max<std::size_t>(bytes, sizeof(std::uint32_t)));
    if (needs_upload) {
      workspace.Device().UploadBufferRange(
          buffer, 0,
          std::span<const std::byte>(reinterpret_cast<const std::byte*>(fused_offsets.data()),
                                     bytes),
          device.CommandContext());
      workspace.Device().SetGradeCommandTopologyHash(topology_hash);
      return static_cast<std::uint32_t>(bytes);
    }
    return 0;
  }

  static auto LoadLut(OpenClRenderDevice& device, ColorGradeNodeModel& grade) -> OpenClLutBinding {
    return LoadOpenClGradeLut(device, grade);
  }

  static auto LutResourceId(const OpenClLutBinding& lut) -> std::uint64_t {
    return lut.resource_id;
  }

  static auto AcquireScratch(OpenClRenderDevice& device, std::uint32_t width, std::uint32_t height,
                             const GraphValueId& id) -> Scratch {
    return &AcquireRgba(device.Workspace(), id, width, height);
  }

  static auto ScratchTexture(Scratch scratch) -> Texture& { return scratch->Texture(); }

  static auto AcquireOutput(OpenClRenderDevice& device, const GraphValueId& output,
                            std::uint32_t width, std::uint32_t height) -> Texture& {
    return AcquireRgba(device.Workspace(), output, width, height).Texture();
  }

  static auto SceneTexture(OpenClRenderDevice& device, const GraphValueId& id) -> Texture& {
    auto* image = device.Workspace().Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: stage image is missing");
    }
    return image->Texture();
  }

  static void DispatchPointwise(OpenClRenderDevice& device, const Texture& src, Texture& dst,
                                const OpenClLutBinding& lut, const NodeId& grade_id,
                                std::uint32_t command_start, std::uint32_t command_count,
                                std::uint32_t width, std::uint32_t height) {
    auto& workspace = device.Workspace();
    auto* commands  = workspace.Values().Find(GraphValueId{grade_id, PortId{"runtime.order"}});
    if (commands == nullptr) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing fused command buffer");
    }
    EnqueueGradePointwise(device, src, dst, workspace.Parameters().DeviceBuffer(), *commands,
                          command_start, command_count, lut, width, height);
  }

  static auto AcquireHorizontalScratch(OpenClRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch {
    return AcquireOpenClScratch(device.Workspace(), width, height);
  }

  static auto HorizontalScratchTexture(HorizontalScratch& scratch) -> Texture& {
    return scratch.Texture();
  }

  static void DispatchHorizontal(OpenClRenderDevice& device, const Texture& src, Texture& blur,
                                 const NeighborWork& work, std::uint32_t width,
                                 std::uint32_t height) {
    EnqueueOpenClNeighborHorizontal(device, src, blur, work.params, width, height);
  }

  static void DispatchVerticalApply(OpenClRenderDevice& device, const Texture& src,
                                    const Texture& blur, Texture& dst, const LutBinding&,
                                    const NeighborWork& work, std::uint32_t width,
                                    std::uint32_t height) {
    EnqueueOpenClNeighborVertical(device, src, blur, dst, work.params, width, height);
  }

  static void DispatchMix(OpenClRenderDevice& device, const Texture& source, const Texture& adjusted,
                          Texture& destination, float mix, const Texture* mask, std::uint32_t width,
                          std::uint32_t height) {
    EnqueueGradeMix(device, source, adjusted, destination, mix, mask, width, height);
  }

  static auto ExecuteLocalTone(OpenClRenderDevice& device, const Texture& src, Texture& dst,
                               const NodeId& grade_id, float shadows_slider,
                               float highlights_slider, const ResolvedRenderGeometry& geometry)
      -> OpenClLocalToneResult {
    return ExecuteOpenClLocalTone(device, src, dst, grade_id, shadows_slider, highlights_slider,
                                  geometry);
  }

  static auto MaskTexture(OpenClRenderDevice& device, const GraphValueId& mask_output,
                          std::uint32_t width, std::uint32_t height) -> const Texture* {
    auto* mask = device.Workspace().Images().Find(mask_output);
    if (mask == nullptr || mask->Texture().Native() == nullptr ||
        mask->Texture().Format() != TextureFormat::R8 || mask->Texture().Width() != width ||
        mask->Texture().Height() != height) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: compiled mask output is missing");
    }
    return &mask->Texture();
  }

  static void CheckAfterEncode(OpenClRenderDevice&) {}
};

}  // namespace

auto ExecuteOpenClPrimaryGrade(OpenClRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& prepared, PipelineDocument& document,
                               const CompiledGradeNode& compiled_grade_node)
    -> OpenClPrimaryGradeResult {
  const auto executed = GradeExecutor<OpenClGradeOps>::Execute(device, plan, prepared, document,
                                                              compiled_grade_node);
  OpenClPrimaryGradeResult result;
  result.output                                 = executed.output;
  result.pointwise_dispatch_count               = executed.pointwise_dispatch_count;
  result.detail_pass_count                      = executed.detail_pass_count;
  result.local_tone_pass_count                  = executed.local_tone_pass_count;
  result.local_tone_transient_bytes             = executed.local_tone_transient_bytes;
  result.command_upload_bytes                   = executed.command_upload_bytes;
  result.lut_resource_id                        = executed.lut_resource_id;
  result.local_tone_reference_resource_id       = executed.local_tone_reference_resource_id;
  result.local_tone_rebuilt_reference           = executed.local_tone_rebuilt_reference;
  result.local_tone_sampled_canonical_reference = executed.local_tone_sampled_canonical_reference;
  return result;
}

auto ExecuteOpenClPrimaryGrade(OpenClRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& prepared, PipelineDocument& document)
    -> OpenClPrimaryGradeResult {
  if (plan.grade_nodes.empty()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: plan has no Color Grade");
  }
  device.Workspace().PrepareResultValidity(plan, document, prepared);
  OpenClPrimaryGradeResult last{};
  for (const auto& compiled_grade : plan.grade_nodes) {
    last = ExecuteOpenClPrimaryGrade(device, plan, prepared, document, compiled_grade);
  }
  return last;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
