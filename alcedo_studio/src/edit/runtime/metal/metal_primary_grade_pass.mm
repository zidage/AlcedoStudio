//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_primary_grade_pass.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/metal/metal_local_tone_pass.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

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

enum class GradeOpKind : std::uint8_t { Fused, Detail, LlfBarrier };

struct GradeOp {
  GradeOpKind                kind = GradeOpKind::Fused;
  std::vector<std::uint32_t> offsets;
  ParameterSlotKey           detail_key{};
};

auto AcquireRgba(MetalRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<MetalBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto AcquireScratch(MetalRenderWorkspace& workspace, std::uint32_t width, std::uint32_t height)
    -> MetalBackend::Texture2D& {
  return workspace.Device().AcquireRecordedWorkScratchTexture(width, height,
                                                              TextureFormat::Rgba32f);
}

auto EnsureBuffer(MetalRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> MetalBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: command buffer store failed");
  }
  return *stored;
}

auto Pipeline(const char* function, const char* label) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH
  throw std::runtime_error("Metal primary grade metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH, function, label);
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     std::uint32_t width, std::uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1}, MTL::Size{thread_width, thread_height, 1});
}

void DispatchPointwise(MetalRenderDevice& device, const MetalBackend::Texture2D& src,
                       MetalBackend::Texture2D& dst, const MetalBackend::Buffer& params,
                       const MetalBackend::Buffer& commands, std::uint32_t command_offset,
                       std::uint32_t command_count, const MetalLutBinding& lut, std::uint32_t width,
                       std::uint32_t height) {
  auto  pipeline = Pipeline("primary_grade_pointwise", "Metal PrimaryGrade");
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: compute encoder is missing");
  }
  PrimaryGradeDispatchParams dispatch;
  dispatch.command_count   = command_count;
  dispatch.command_offset  = command_offset;
  dispatch.lut_edge        = lut.edge_size;
  dispatch.local_reference = local_tone_mapping::kAcesccMiddleGray;
  dispatch.width           = width;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(src.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 1);
  encoder->setBuffer(static_cast<MTL::Buffer*>(params.Native()), 0, 0);
  encoder->setBuffer(static_cast<MTL::Buffer*>(commands.Native()), 0, 1);
  encoder->setBytes(&dispatch, sizeof(dispatch), 2);
  encoder->setBuffer(static_cast<MTL::Buffer*>(lut.native), 0, 3);
  DispatchThreads(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

void DispatchMix(MetalRenderDevice& device, const MetalBackend::Texture2D& source,
                 const MetalBackend::Texture2D& adjusted, MetalBackend::Texture2D& dst, float mix,
                 const MetalBackend::Texture2D* mask, std::uint32_t width, std::uint32_t height) {
  auto  pipeline = mask == nullptr
                       ? Pipeline("primary_grade_mix", "Metal PrimaryGrade Mix")
                       : Pipeline("primary_grade_mix_masked", "Metal PrimaryGrade Mix Masked");
  auto* encoder  = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: mix encoder is missing");
  }
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(source.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(adjusted.Native()), 1);
  encoder->setTexture(static_cast<MTL::Texture*>(dst.Native()), 2);
  if (mask != nullptr) {
    encoder->setTexture(static_cast<MTL::Texture*>(mask->Native()), 3);
  }
  encoder->setBytes(&mix, sizeof(mix), 0);
  DispatchThreads(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

auto LoadLut(MetalRenderDevice& device, ColorGradeNodeModel& grade) -> MetalLutBinding {
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

void AppendMetalPrimaryGradeWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_pointwise", "Metal PrimaryGrade"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_mix", "Metal PrimaryGrade Mix"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH,
                                          "primary_grade_mix_masked",
                                          "Metal PrimaryGrade Mix Masked"});
#else
  (void)pipelines;
#endif
  AppendMetalLocalToneWarmup(pipelines);
}

auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                              const PreparedRawInput& prepared, PipelineDocument& document,
                              const CompiledGradeNode& compiled_grade_node)
    -> MetalPrimaryGradeResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: BeginRender has not been called");
  }
  const auto* compiled_grade = &compiled_grade_node;
  auto* grade =
      dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(compiled_grade->node_id));
  if (grade == nullptr) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: compiled Color Grade is missing");
  }
  auto* input = workspace.Images().Find(compiled_grade->scene_input);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: missing Color Grade scene input");
  }
  const float early_mix = grade->Enabled() ? grade->Mix() : 0.0f;
  if (early_mix == 0.0f) {
    workspace.AliasImageFrom(compiled_grade->scene_output, compiled_grade->scene_input);
    MetalPrimaryGradeResult skipped;
    skipped.output = compiled_grade->scene_output;
    return skipped;
  }

  auto&       arena      = workspace.Parameters();
  std::size_t slot_count = 0;
  for (const auto& compiled_node : plan.grade_nodes) {
    slot_count += compiled_node.adjustments.size();
  }
  arena.Reserve(slot_count *
                (kGradeRuntimeParamBytes + ParameterArena<MetalBackend>::kSlotAlignment));
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
          "ExecuteMetalPrimaryGrade: compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      throw std::runtime_error("ExecuteMetalPrimaryGrade: unregistered adjustment type '" +
                               std::string{compiled.type.Text()} + "'");
    }
    if (IsLocalToneBehavior(*behavior) &&
        compiled.algorithm != CompiledAdjustmentAlgorithm::LocalLaplacian) {
      throw std::runtime_error(
          "ExecuteMetalPrimaryGrade: Shadows/Highlights were not compiled for LLF");
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
          "ExecuteMetalPrimaryGrade: compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      throw std::runtime_error("ExecuteMetalPrimaryGrade: unregistered adjustment type '" +
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
      if (runtime_params.values[0] != 0.0f) {
        ops.push_back(GradeOp{GradeOpKind::Detail, {arena.Binding(key).offset}, key});
      }
      continue;
    }
    FlushFused()->offsets.push_back(arena.Binding(key).offset);
  }

  const bool local_tone_active = local_tone_mapping::ShouldRun(shadows_slider * 1.5f / 80.0f,
                                                               -highlights_slider * 1.5f / 100.0f);
  ops                          = CompactOps(std::move(ops), local_tone_active);

  auto& context                = device.CommandContext();
  arena.UploadDirty(context);
  for (auto& patch : pending) {
    patch.Commit();
  }

  std::vector<std::uint32_t> command_offsets;
  command_offsets.reserve(compiled_grade->adjustments.size());
  std::vector<std::uint32_t> fused_starts;
  fused_starts.reserve(ops.size());
  ContentHash topology;
  topology.MixText(grade->Id().Value());
  for (const auto& op : ops) {
    topology.MixU32(static_cast<std::uint32_t>(op.kind));
    topology.MixU32(static_cast<std::uint32_t>(op.offsets.size()));
    fused_starts.push_back(static_cast<std::uint32_t>(command_offsets.size()));
    for (const auto offset : op.offsets) {
      topology.MixU32(offset);
      command_offsets.push_back(offset);
    }
  }
  const auto              topology_hash = topology.Key().hash;

  MetalPrimaryGradeResult result;
  result.output = compiled_grade->scene_output;
  const GraphValueId command_id{grade->Id(), PortId{"runtime.order"}};
  if (!command_offsets.empty()) {
    const auto bytes = command_offsets.size() * sizeof(command_offsets[0]);
    const auto* existing_command_buffer = workspace.Values().Find(command_id);
    const bool command_buffer_needs_upload =
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
        "ExecuteMetalPrimaryGrade: Color Grade scene input lost during parameter bind");
  }
  const auto  width       = input->Texture().Width();
  const auto  height      = input->Texture().Height();
  const float mix         = grade->Enabled() ? grade->Mix() : 0.0f;
  const bool  skip_mix    = mix == 1.0f && !compiled_grade->mask.has_value();
  const auto  write_count = CountGpuWrites(ops, skip_mix);

  std::vector<MetalBackend::Texture2D*> scratches;
  scratches.reserve(write_count + 1);
  auto remaining = write_count;
  enum class ImageSlot : std::uint8_t { Input, Scratch, Output };
  ImageSlot   current_slot    = ImageSlot::Input;
  std::size_t current_scratch = 0;
  ImageSlot   dest_slot       = ImageSlot::Scratch;
  std::size_t dest_scratch    = 0;

  auto        Resolve = [&](ImageSlot slot, std::size_t scratch_index) -> MetalBackend::Texture2D& {
    if (slot == ImageSlot::Input) {
      auto* source = workspace.Images().Find(compiled_grade->scene_input);
      if (source == nullptr) {
        throw std::runtime_error("ExecuteMetalPrimaryGrade: missing Color Grade scene input");
      }
      return source->Texture();
    }
    if (slot == ImageSlot::Output) {
      auto* dest = workspace.Images().Find(compiled_grade->scene_output);
      if (dest == nullptr) {
        throw std::runtime_error("ExecuteMetalPrimaryGrade: missing grade output");
      }
      return dest->Texture();
    }
    if (scratch_index >= scratches.size()) {
      throw std::runtime_error("ExecuteMetalPrimaryGrade: scratch index is invalid");
    }
    return *scratches[scratch_index];
  };

  auto AllocateDest = [&]() {
    if (remaining == 0) {
      throw std::runtime_error("ExecuteMetalPrimaryGrade: destination count underflow");
    }
    --remaining;
    if (remaining == 0) {
      dest_slot = ImageSlot::Output;
      (void)AcquireRgba(workspace, compiled_grade->scene_output, width, height);
      return;
    }
    dest_slot    = ImageSlot::Scratch;
    dest_scratch = scratches.size();
    scratches.push_back(&AcquireScratch(workspace, width, height));
  };

  if (write_count == 0) {
    workspace.AliasImageFrom(compiled_grade->scene_output, compiled_grade->scene_input);
    return result;
  }

  auto* commands_buffer = command_offsets.empty() ? nullptr : workspace.Values().Find(command_id);
  for (std::size_t index = 0; index < ops.size(); ++index) {
    const auto& op = ops[index];
    AllocateDest();
    auto& src  = Resolve(current_slot, current_scratch);
    auto& dest = Resolve(dest_slot, dest_scratch);
    if (op.kind == GradeOpKind::LlfBarrier) {
      const auto tone =
          ExecuteMetalLocalTone(device, src, dest, grade->Id(), shadows_slider, highlights_slider,
                                plan.geometry,
                                HashLlfSourceKey(plan, prepared, document, compiled_grade->node_id),
                                HashLlfReferenceKey(plan, prepared, document,
                                                    compiled_grade->node_id));
      result.local_tone_reference_resource_id       = tone.reference_resource_id;
      result.local_tone_rebuilt_reference           = tone.rebuilt_reference;
      result.local_tone_sampled_canonical_reference = tone.sampled_canonical_reference;
      result.local_tone_transient_bytes             = tone.transient_bytes;
    } else if (op.kind == GradeOpKind::Fused) {
      if (commands_buffer == nullptr) {
        throw std::runtime_error("ExecuteMetalPrimaryGrade: missing fused command buffer");
      }
      DispatchPointwise(device, src, dest, arena.DeviceBuffer(), *commands_buffer,
                        fused_starts[index], static_cast<std::uint32_t>(op.offsets.size()), lut,
                        width, height);
      ++result.pointwise_dispatch_count;
    } else {
      if (commands_buffer == nullptr) {
        throw std::runtime_error("ExecuteMetalPrimaryGrade: missing detail command buffer");
      }
      DispatchPointwise(device, src, dest, arena.DeviceBuffer(), *commands_buffer,
                        fused_starts[index], 1, lut, width, height);
      ++result.detail_pass_count;
    }
    current_slot    = dest_slot;
    current_scratch = dest_scratch;
  }

  if (!skip_mix) {
    AllocateDest();
    auto&                          source       = Resolve(ImageSlot::Input, 0);
    auto&                          adjusted     = Resolve(current_slot, current_scratch);
    auto&                          dest         = Resolve(dest_slot, dest_scratch);
    const MetalBackend::Texture2D* mask_texture = nullptr;
    if (compiled_grade->mask) {
      auto* mask = workspace.Images().Find(compiled_grade->mask_output);
      if (mask == nullptr || mask->Texture().Native() == nullptr ||
          mask->Texture().Format() != TextureFormat::R8 || mask->Texture().Width() != width ||
          mask->Texture().Height() != height) {
        throw std::runtime_error("ExecuteMetalPrimaryGrade: compiled mask output is missing");
      }
      mask_texture = &mask->Texture();
    }
    DispatchMix(device, source, adjusted, dest, mix, mask_texture, width, height);
  }
  return result;
}

auto ExecuteMetalPrimaryGrade(MetalRenderDevice& device, const ExecutionPlan& plan,
                              const PreparedRawInput& prepared, PipelineDocument& document)
    -> MetalPrimaryGradeResult {
  if (plan.grade_nodes.empty()) {
    throw std::runtime_error("ExecuteMetalPrimaryGrade: plan has no Color Grade");
  }
  MetalPrimaryGradeResult last{};
  for (const auto& compiled_grade : plan.grade_nodes) {
    last = ExecuteMetalPrimaryGrade(device, plan, prepared, document, compiled_grade);
  }
  return last;
}

}  // namespace alcedo
