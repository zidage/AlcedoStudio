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
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"
#include "utils/lut/cube_lut.hpp"

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
                    std::uint32_t height) {
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
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

void DispatchPointwise(OpenClRenderDevice& device, const OpenClBackend::Texture2D& src,
                       OpenClBackend::Texture2D& dst, const OpenClBackend::Buffer& params,
                       const OpenClBackend::Buffer& commands, std::uint32_t command_offset,
                       std::uint32_t command_count, const OpenClLutBinding& lut,
                       std::uint32_t width, std::uint32_t height, bool detail) {
  const auto kernel_name = detail ? OpenCL::GpuDag::kPrimaryGradeDetailKernelName
                                  : OpenCL::GpuDag::kPrimaryGradePointwiseKernelName;
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

void DispatchLocalToneBarrier(OpenClRenderDevice& device, const OpenClBackend::Texture2D& src,
                              OpenClBackend::Texture2D& dst, std::uint32_t width,
                              std::uint32_t height) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kLocalToneProgramName,
                                                        OpenCL::GpuDag::kLocalToneKernelName);
  SetImageKernelArgs(kernel, src.Native(), dst.Native());
  DispatchKernel(device, kernel, width, height);
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

auto PackLutRgba(const CubeLut& lut) -> std::vector<std::byte> {
  const auto             edge   = static_cast<std::size_t>(lut.edge3d_);
  const auto             voxels = edge * edge * edge;
  std::vector<std::byte> packed(voxels * 4 * sizeof(float));
  auto*                  out = reinterpret_cast<float*>(packed.data());
  for (std::size_t i = 0; i < voxels; ++i) {
    out[i * 4 + 0] = lut.lut3d_[i * 3 + 0];
    out[i * 4 + 1] = lut.lut3d_[i * 3 + 1];
    out[i * 4 + 2] = lut.lut3d_[i * 3 + 2];
    out[i * 4 + 3] = 1.0f;
  }
  return packed;
}

auto LoadLut(OpenClRenderDevice& device, ColorGradeNodeModel& grade) -> OpenClLutBinding {
  auto* model = dynamic_cast<LmtModel*>(grade.FindAdjustmentByType(type_ids::Lmt()));
  if (model == nullptr || model->CubePath().empty()) {
    return device.Workspace().Device().DummyLut();
  }

  CubeLut     cube;
  std::string error;
  if (!ParseCubeFile(model->CubePath(), cube, &error) || !cube.Has3D()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: failed to load LMT cube '" +
                             model->CubePath() + "': " + error);
  }
  const auto  packed = PackLutRgba(cube);
  ContentHash hash;
  hash.MixBytes(packed);
  hash.MixU32(static_cast<std::uint32_t>(cube.edge3d_));
  return device.Workspace().Device().AcquireLut(
      hash.Key(), packed, static_cast<std::uint32_t>(cube.edge3d_), device.CommandContext());
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
                               const PreparedRawInput& prepared, PipelineDocument& document)
    -> OpenClPrimaryGradeResult {
  (void)prepared;
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: BeginRender has not been called");
  }
  auto* grade = document.PrimaryGrade();
  if (grade == nullptr) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing primary grade");
  }
  auto* input = workspace.Images().Find(plan.develop_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing Develop output");
  }

  auto& arena = workspace.Parameters();
  arena.Reserve(plan.primary_grade_adjustments.size() *
                (kGradeRuntimeParamBytes + ParameterArena<OpenClBackend>::kSlotAlignment));
  std::vector<PendingParameterPatch> pending;
  std::vector<GradeOp>               ops;
  ops.reserve(plan.primary_grade_adjustments.size());
  float                       shadows_slider    = 0.0f;
  float                       highlights_slider = 0.0f;
  const ParameterFieldBinding field{DirtyFieldMask{kGradeRuntimeParamDirtyBit}, 0, 0,
                                    kGradeRuntimeParamBytes};

  auto                        FlushFused = [&]() -> GradeOp* {
    if (ops.empty() || ops.back().kind != GradeOpKind::Fused) {
      ops.push_back(GradeOp{GradeOpKind::Fused, {}});
    }
    return &ops.back();
  };

  for (const auto& compiled : plan.primary_grade_adjustments) {
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

    const ParameterSlotKey key{grade->Id(), compiled.instance_id};
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
      OperatorParamPatchDto patch{grade->Id(), compiled.instance_id, compiled.type,
                                  DirtyFieldMask{kGradeRuntimeParamDirtyBit}, payload};
      arena.ApplyPatch(key, patch);
      pending.push_back(std::move(*change));
    }

    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian) {
      if (*behavior == AdjustmentBehavior::Shadows) {
        shadows_slider = runtime_params.values[0];
      } else if (*behavior == AdjustmentBehavior::Highlights) {
        highlights_slider = runtime_params.values[0];
      }
      if (ops.empty() || ops.back().kind != GradeOpKind::LlfBarrier) {
        ops.push_back(GradeOp{GradeOpKind::LlfBarrier, {}});
      }
      continue;
    }

    if (compiled.algorithm == CompiledAdjustmentAlgorithm::Neighborhood ||
        IsNeighborhoodBehavior(*behavior)) {
      if (runtime_params.values[0] != 0.0f) {
        ops.push_back(GradeOp{GradeOpKind::Detail, {arena.Binding(key).offset}});
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
  command_offsets.reserve(plan.primary_grade_adjustments.size());
  std::vector<std::uint32_t> command_starts;
  command_starts.reserve(ops.size());
  ContentHash topology;
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
  result.output = plan.primary_grade_output;
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

  input                  = workspace.Images().Find(plan.develop_output);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteOpenClPrimaryGrade: Develop image lost during parameter bind");
  }
  const auto  width       = input->Texture().Width();
  const auto  height      = input->Texture().Height();
  const float mix         = grade->Enabled() ? grade->Mix() : 0.0f;
  const bool  skip_mix    = mix == 1.0f && !plan.primary_grade_mask.has_value();
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
      auto* source = workspace.Images().Find(plan.develop_output);
      if (source == nullptr) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing Develop output");
      }
      return source->Texture();
    }
    if (slot == ImageSlot::Output) {
      auto* dest = workspace.Images().Find(plan.primary_grade_output);
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
      (void)AcquireRgba(workspace, plan.primary_grade_output, width, height);
      return;
    }
    dest_slot    = ImageSlot::Scratch;
    dest_scratch = scratches.size();
    scratches.push_back(AcquireScratch(workspace, width, height));
  };

  if (write_count == 0) {
    auto& dest   = AcquireRgba(workspace, plan.primary_grade_output, width, height);
    auto* source = workspace.Images().Find(plan.develop_output);
    if (source == nullptr) {
      throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing Develop output");
    }
    workspace.Device().CopyTexture2D(source->Texture(), dest.Texture(), context);
    return result;
  }

  auto* commands_buffer = command_offsets.empty() ? nullptr : workspace.Values().Find(command_id);
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const auto& op = ops[index];
    AllocateDest();
    auto& src  = Resolve(current_slot, current_scratch);
    auto& dest = Resolve(dest_slot, dest_scratch);
    if (op.kind == GradeOpKind::LlfBarrier) {
      DispatchLocalToneBarrier(device, src, dest, width, height);
      ++result.local_tone_pass_count;
    } else if (op.kind == GradeOpKind::Fused) {
      if (commands_buffer == nullptr) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing fused command buffer");
      }
      DispatchPointwise(device, src, dest, arena.DeviceBuffer(), *commands_buffer,
                        command_starts[index], static_cast<std::uint32_t>(op.offsets.size()), lut,
                        width, height, false);
      ++result.pointwise_dispatch_count;
    } else {
      if (commands_buffer == nullptr) {
        throw std::runtime_error("ExecuteOpenClPrimaryGrade: missing detail command buffer");
      }
      DispatchPointwise(device, src, dest, arena.DeviceBuffer(), *commands_buffer,
                        command_starts[index], 1, lut, width, height, true);
      ++result.detail_pass_count;
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
    if (plan.primary_grade_mask.has_value()) {
      auto* mask = workspace.Images().Find(plan.mask_output);
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

}  // namespace alcedo

#endif  // HAVE_OPENCL
