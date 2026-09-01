//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/opencl/opencl_local_tone_pass.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/result_content_key.hpp"
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

enum class GradeOpKind : std::uint8_t { Fused, Detail, LlfBarrier };

struct GradeOp {
  GradeOpKind                kind = GradeOpKind::Fused;
  std::vector<std::uint32_t> offsets;
  GradeNeighborParams        neighbor{};
};

auto AcquireRgba(OpenClRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<OpenClBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto AcquireScratch(OpenClRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
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

auto NeighborVerticalRadius(const GradeNeighborParams& params) -> std::uint32_t {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::Halation) {
    return std::clamp(static_cast<std::uint32_t>(std::ceil(params.sigma_y * 3.0f)), 1U,
                      kGradeNeighborMaxTapCount - 1U);
  }
  return params.radius;
}

void SetImageKernelArgs(cl_kernel kernel, cl_mem src, cl_mem dst) {
  CheckOpenCl(clSetKernelArg(kernel, 0, sizeof(cl_mem), &src),
              "OpenCL Primary Grade source argument");
  CheckOpenCl(clSetKernelArg(kernel, 1, sizeof(cl_mem), &dst),
              "OpenCL Primary Grade destination argument");
}

void DispatchPointwise(OpenClRenderDevice& device, const OpenClBackend::Texture2D& src,
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

void DispatchNeighbor(OpenClRenderDevice& device, const OpenClBackend::Texture2D& src,
                      OpenClBackend::Texture2D& blur_horizontal, OpenClBackend::Texture2D& dst,
                      const GradeNeighborParams& params, std::uint32_t width,
                      std::uint32_t height) {
  auto blur_kernel =
      OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kPrimaryGradeProgramName,
                                              OpenCL::GpuDag::kPrimaryGradeNeighborBlurKernelName);
  auto apply_kernel =
      OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kPrimaryGradeProgramName,
                                              OpenCL::GpuDag::kPrimaryGradeNeighborApplyKernelName);

  const auto src_mem  = src.Native();
  const auto blur_mem = blur_horizontal.Native();
  const auto dst_mem  = dst.Native();
  CheckOpenCl(clSetKernelArg(blur_kernel, 0, sizeof(cl_mem), &src_mem),
              "OpenCL Primary Grade neighbor source argument");
  CheckOpenCl(clSetKernelArg(blur_kernel, 1, sizeof(cl_mem), &blur_mem),
              "OpenCL Primary Grade neighbor blur argument");
  CheckOpenCl(clSetKernelArg(blur_kernel, 2, sizeof(params), &params),
              "OpenCL Primary Grade neighbor parameters");
  constexpr std::size_t kLocalEdge = 8;
  DispatchKernel(device, blur_kernel, width, height, kLocalEdge);

  CheckOpenCl(clSetKernelArg(apply_kernel, 0, sizeof(cl_mem), &src_mem),
              "OpenCL Primary Grade neighbor apply source argument");
  CheckOpenCl(clSetKernelArg(apply_kernel, 1, sizeof(cl_mem), &blur_mem),
              "OpenCL Primary Grade neighbor apply blur argument");
  CheckOpenCl(clSetKernelArg(apply_kernel, 2, sizeof(cl_mem), &dst_mem),
              "OpenCL Primary Grade neighbor apply destination argument");
  CheckOpenCl(clSetKernelArg(apply_kernel, 3, sizeof(params), &params),
              "OpenCL Primary Grade neighbor apply parameters");
  const auto radius      = NeighborVerticalRadius(params);
  const auto local_bytes = kLocalEdge * (kLocalEdge + 2U * radius) * 4U * sizeof(float);
  CheckOpenCl(clSetKernelArg(apply_kernel, 4, local_bytes, nullptr),
              "OpenCL Primary Grade neighbor local tile argument");
  DispatchKernel(device, apply_kernel, width, height, kLocalEdge);
}

void DispatchMix(OpenClRenderDevice& device, const OpenClBackend::Texture2D& source,
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

auto LoadLut(OpenClRenderDevice& device, ColorGradeNodeModel& grade) -> OpenClLutBinding {
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

auto CompactOps(std::vector<GradeOp> ops, bool local_tone_active) -> std::vector<GradeOp> {
  std::vector<GradeOp> compacted;
  compacted.reserve(ops.size());
  for (auto& op : ops) {
    if (op.kind == GradeOpKind::LlfBarrier && !local_tone_active) {
      continue;
    }
    if (op.kind == GradeOpKind::Fused && op.offsets.empty()) {
      continue;
    }
    if (op.kind == GradeOpKind::Fused && !compacted.empty() &&
        compacted.back().kind == GradeOpKind::Fused) {
      compacted.back().offsets.insert(compacted.back().offsets.end(), op.offsets.begin(),
                                      op.offsets.end());
      continue;
    }
    compacted.push_back(std::move(op));
  }
  return compacted;
}

auto CountGpuWrites(const std::vector<GradeOp>& ops, bool skip_mix) -> std::size_t {
  std::size_t count = skip_mix ? 0 : 1;
  for (const auto& op : ops) {
    if (op.kind == GradeOpKind::Fused || op.kind == GradeOpKind::Detail ||
        op.kind == GradeOpKind::LlfBarrier) {
      ++count;
    }
  }
  return count;
}

}  // namespace

auto ExecuteOpenClPrimaryGrade(OpenClRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& prepared, PipelineDocument& document,
                               const CompiledGradeNode& compiled_grade_node)
    -> OpenClPrimaryGradeResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: BeginRender has not been called");
  }
  const auto* compiled_grade = &compiled_grade_node;
  auto* grade =
      dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(compiled_grade->node_id));
  if (grade == nullptr) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: compiled Color Grade is missing");
  }
  auto* input = workspace.Images().Find(compiled_grade->scene_input);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing Color Grade scene input");
  }
  const float early_mix = grade->Enabled() ? grade->Mix() : 0.0f;
  if (early_mix == 0.0f) {
    workspace.AliasImageFrom(compiled_grade->scene_output, compiled_grade->scene_input);
    OpenClPrimaryGradeResult skipped;
    skipped.output = compiled_grade->scene_output;
    return skipped;
  }

  auto&       arena      = workspace.Parameters();
  std::size_t slot_count = 0;
  for (const auto& compiled_node : plan.grade_nodes) {
    slot_count += compiled_node.adjustments.size();
  }
  arena.Reserve(slot_count *
                (kGradeRuntimeParamBytes + ParameterArena<OpenClBackend>::kSlotAlignment));
  std::vector<PendingParameterPatch> pending;
  std::vector<GradeOp>               ops;
  ops.reserve(compiled_grade->adjustments.size());
  float                       shadows_slider    = 0.0f;
  float                       highlights_slider = 0.0f;
  const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                    kGradeRuntimeParamBytes};

  auto BindAdjustmentSlot = [&](ColorGradeNodeModel& node, const CompiledAdjustment& compiled) {
    auto* model = node.FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error(
          "ExecuteOpenClPrimaryGrade: compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: unregistered adjustment type '" +
                               std::string{compiled.type.Text()} + "'");
    }
    if (IsLocalToneBehavior(*behavior) &&
        compiled.algorithm != CompiledAdjustmentAlgorithm::LocalLaplacian) {
      throw std::runtime_error(
          "ExecuteOpenClPrimaryGrade: Shadows/Highlights were not compiled for LLF");
    }
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian &&
        !IsLocalToneBehavior(*behavior)) {
      throw std::runtime_error(
          "ExecuteOpenClPrimaryGrade: non-local adjustment was compiled for LLF");
    }
    const ParameterSlotKey key{node.Id(), compiled.instance_id};
    const auto             runtime_params = MakeGradeRuntimeParams(*model, *behavior);
    auto payload = std::make_shared<TypedOperatorParamPayload<GradeAdjustmentParams>>(
        compiled.type, 1, runtime_params);
    if (!arena.Contains(key)) {
      arena.BindSlot(key, kGradeRuntimeParamBytes, std::span{&field, 1});
      arena.InitializeFromFullDto(key, OperatorParamDto{compiled.type, 1, payload});
      if (auto first = TakePendingParameterPatch(*model)) {
        pending.push_back(std::move(*first));
      }
    } else if (auto change = TakePendingParameterPatch(*model)) {
      OperatorParamPatchDto patch{node.Id(), compiled.instance_id, compiled.type,
                                  DirtyFieldMask{kGradeRuntimeParamDirtyBit}, payload};
      arena.ApplyPatch(key, patch);
      pending.push_back(std::move(*change));
    }
  };

  for (const auto& compiled : compiled_grade->adjustments) {
    BindAdjustmentSlot(*grade, compiled);
  }

  auto FlushFused = [&]() -> GradeOp* {
    if (ops.empty() || ops.back().kind != GradeOpKind::Fused) {
      ops.push_back(GradeOp{GradeOpKind::Fused, {}, {}});
    }
    return &ops.back();
  };

  for (const auto& compiled : compiled_grade->adjustments) {
    auto* model = grade->FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      throw std::runtime_error(
          "ExecuteOpenClPrimaryGrade: compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: unregistered adjustment type '" +
                               std::string{compiled.type.Text()} + "'");
    }
    const ParameterSlotKey key{grade->Id(), compiled.instance_id};
    const auto             runtime_params = MakeGradeRuntimeParams(*model, *behavior);

    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian) {
      if (*behavior == AdjustmentBehavior::Shadows) {
        shadows_slider = runtime_params.values[0];
      } else if (*behavior == AdjustmentBehavior::Highlights) {
        highlights_slider = runtime_params.values[0];
      }
      if (ops.empty() || ops.back().kind != GradeOpKind::LlfBarrier) {
        ops.push_back(GradeOp{GradeOpKind::LlfBarrier, {}, {}});
      }
      continue;
    }

    if (compiled.algorithm == CompiledAdjustmentAlgorithm::Neighborhood ||
        IsNeighborhoodBehavior(*behavior)) {
      auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
      if (neighbor.enabled != 0U) {
        ops.push_back(GradeOp{GradeOpKind::Detail, {}, neighbor});
      }
      continue;
    }
    if (compiled.algorithm != CompiledAdjustmentAlgorithm::Pointwise) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: unsupported grade algorithm");
    }
    FlushFused()->offsets.push_back(arena.Binding(key).offset);
  }

  const bool local_tone_active = local_tone_mapping::ShouldRun(
      shadows_slider * local_tone_mapping::kHighlightStrengthScale / 80.0f,
      -highlights_slider * local_tone_mapping::kHighlightStrengthScale / 100.0f);
  ops           = CompactOps(std::move(ops), local_tone_active);
  auto& context = device.CommandContext();
  arena.UploadDirty(context);
  for (auto& patch : pending) {
    patch.Commit();
  }

  std::vector<std::uint32_t> command_offsets;
  command_offsets.reserve(compiled_grade->adjustments.size());
  std::vector<std::uint32_t> command_starts;
  command_starts.reserve(ops.size());
  ContentHash topology;
  topology.MixText(grade->Id().Value());
  for (const auto& op : ops) {
    topology.MixU32(static_cast<std::uint32_t>(op.kind));
    topology.MixU32(static_cast<std::uint32_t>(op.offsets.size()));
    command_starts.push_back(static_cast<std::uint32_t>(command_offsets.size()));
    for (const auto offset : op.offsets) {
      topology.MixU32(offset);
      command_offsets.push_back(offset);
    }
  }
  const auto               topology_hash = topology.Key().hash;

  OpenClPrimaryGradeResult result;
  result.output = compiled_grade->scene_output;
  const GraphValueId command_id{grade->Id(), PortId{"runtime.order"}};
  if (!command_offsets.empty()) {
    const auto  bytes                   = command_offsets.size() * sizeof(command_offsets[0]);
    const auto* existing_command_buffer = workspace.Values().Find(command_id);
    const bool  command_buffer_needs_upload =
        existing_command_buffer == nullptr || existing_command_buffer->Bytes() < bytes;
    auto& command_buffer =
        EnsureBuffer(workspace, command_id, std::max<std::size_t>(bytes, sizeof(std::uint32_t)));
    if (command_buffer_needs_upload ||
        workspace.Device().GradeCommandTopologyHash() != topology_hash) {
      workspace.Device().UploadBufferRange(
          command_buffer, 0,
          std::span<const std::byte>(reinterpret_cast<const std::byte*>(command_offsets.data()),
                                     bytes),
          context);
      workspace.Device().SetGradeCommandTopologyHash(topology_hash);
      result.command_upload_bytes = static_cast<std::uint32_t>(bytes);
    }
  }

  const auto lut         = LoadLut(device, *grade);
  result.lut_resource_id = lut.resource_id;

  input                  = workspace.Images().Find(compiled_grade->scene_input);
  if (input == nullptr) {
    throw std::runtime_error(
        "ExecuteOpenClPrimaryGrade: Color Grade scene input lost during parameter bind");
  }
  const auto  width       = input->Texture().Width();
  const auto  height      = input->Texture().Height();
  const float mix         = grade->Enabled() ? grade->Mix() : 0.0f;
  const bool  skip_mix    = mix == 1.0f && !compiled_grade->mask_stack.has_value();
  const auto  write_count = CountGpuWrites(ops, skip_mix);

  std::vector<ResourceLease<OpenClBackend>> scratches;
  scratches.reserve(write_count + 1);
  auto remaining = write_count;
  enum class ImageSlot : std::uint8_t { Input, Scratch, Output };
  ImageSlot   current_slot    = ImageSlot::Input;
  std::size_t current_scratch = 0;
  ImageSlot   dest_slot       = ImageSlot::Scratch;
  std::size_t dest_scratch    = 0;

  auto Resolve = [&](ImageSlot slot, std::size_t scratch_index) -> OpenClBackend::Texture2D& {
    if (slot == ImageSlot::Input) {
      auto* source = workspace.Images().Find(compiled_grade->scene_input);
      if (source == nullptr) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing Color Grade scene input");
      }
      return source->Texture();
    }
    if (slot == ImageSlot::Output) {
      auto* dest = workspace.Images().Find(compiled_grade->scene_output);
      if (dest == nullptr) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing grade output");
      }
      return dest->Texture();
    }
    if (scratch_index >= scratches.size()) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: scratch index is invalid");
    }
    return scratches[scratch_index].Texture();
  };

  auto AllocateDest = [&]() {
    if (remaining == 0) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: destination count underflow");
    }
    --remaining;
    if (remaining == 0) {
      dest_slot = ImageSlot::Output;
      (void)AcquireRgba(workspace, compiled_grade->scene_output, width, height);
      return;
    }
    dest_slot    = ImageSlot::Scratch;
    dest_scratch = scratches.size();
    scratches.push_back(AcquireScratch(workspace, width, height));
  };

  if (write_count == 0) {
    workspace.AliasImageFrom(compiled_grade->scene_output, compiled_grade->scene_input);
    return result;
  }

  auto* commands_buffer = command_offsets.empty() ? nullptr : workspace.Values().Find(command_id);
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const auto& op = ops[index];
    AllocateDest();
    if (op.kind == GradeOpKind::Detail) {
      auto  blur_horizontal = AcquireScratch(workspace, width, height);
      auto& src             = Resolve(current_slot, current_scratch);
      auto& dest            = Resolve(dest_slot, dest_scratch);
      DispatchNeighbor(device, src, blur_horizontal.Texture(), dest, op.neighbor, width, height);
      ++result.detail_pass_count;
    } else if (op.kind == GradeOpKind::LlfBarrier) {
      auto&      src  = Resolve(current_slot, current_scratch);
      auto&      dest = Resolve(dest_slot, dest_scratch);
      const auto local_tone =
          ExecuteOpenClLocalTone(device, src, dest, grade->Id(), shadows_slider, highlights_slider,
                                 plan.geometry,
                                 HashLlfSourceKey(plan, prepared, document, compiled_grade->node_id),
                                 HashLlfReferenceKey(plan, prepared, document,
                                                     compiled_grade->node_id));
      result.local_tone_reference_resource_id       = local_tone.reference_resource_id;
      result.local_tone_rebuilt_reference           = local_tone.rebuilt_reference;
      result.local_tone_sampled_canonical_reference = local_tone.sampled_canonical_reference;
      result.local_tone_transient_bytes             = local_tone.transient_bytes;
      ++result.local_tone_pass_count;
    } else {
      if (commands_buffer == nullptr) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing fused command buffer");
      }
      auto& src  = Resolve(current_slot, current_scratch);
      auto& dest = Resolve(dest_slot, dest_scratch);
      DispatchPointwise(device, src, dest, arena.DeviceBuffer(), *commands_buffer,
                        command_starts[index], static_cast<std::uint32_t>(op.offsets.size()), lut,
                        width, height);
      ++result.pointwise_dispatch_count;
    }
    current_slot    = dest_slot;
    current_scratch = dest_scratch;
  }

  if (!skip_mix) {
    AllocateDest();
    auto&                           source       = Resolve(ImageSlot::Input, 0);
    auto&                           adjusted     = Resolve(current_slot, current_scratch);
    auto&                           dest         = Resolve(dest_slot, dest_scratch);
    const OpenClBackend::Texture2D* mask_texture = nullptr;
    if (compiled_grade->mask_stack.has_value()) {
      auto* mask = workspace.Images().Find(compiled_grade->mask_output);
      if (mask == nullptr || mask->Texture().Native() == nullptr ||
          mask->Texture().Format() != TextureFormat::R8 || mask->Texture().Width() != width ||
          mask->Texture().Height() != height) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: compiled mask output is missing");
      }
      mask_texture = &mask->Texture();
    }
    DispatchMix(device, source, adjusted, dest, mix, mask_texture, width, height);
  }
  return result;
}

auto ExecuteOpenClPrimaryGrade(OpenClRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& prepared, PipelineDocument& document)
    -> OpenClPrimaryGradeResult {
  if (plan.grade_nodes.empty()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: plan has no Color Grade");
  }
  OpenClPrimaryGradeResult last{};
  for (const auto& compiled_grade : plan.grade_nodes) {
    last = ExecuteOpenClPrimaryGrade(device, plan, prepared, document, compiled_grade);
  }
  return last;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
