//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/runtime_invalidation.hpp"

#include <algorithm>
#include <cstdint>
#include <set>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/runtime/result_content_key.hpp"

namespace alcedo {
namespace {

enum class GradeSegment : std::uint8_t { PreLlf, Llf, PostLlf };

auto SensorDirtyMask() -> DirtyFieldMask {
  return DirtyFieldMask{static_cast<std::uint64_t>(DevelopDirty::Demosaic) |
                        static_cast<std::uint64_t>(DevelopDirty::Highlights) |
                        static_cast<std::uint64_t>(DevelopDirty::Lens)};
}

auto SegmentForIndex(const CompiledGradeNode& grade, std::size_t index) -> GradeSegment {
  std::optional<std::uint32_t> llf_begin;
  std::optional<std::uint32_t> llf_end;
  for (const auto& stage : grade.stages) {
    if (stage.kind == CompiledGradeStageKind::LocalLaplacian) {
      llf_begin = stage.begin;
      llf_end   = stage.begin + stage.count;
      break;
    }
  }
  if (!llf_begin.has_value()) {
    return GradeSegment::PostLlf;
  }
  const auto adj = static_cast<std::uint32_t>(index);
  if (adj < *llf_begin) {
    return GradeSegment::PreLlf;
  }
  if (adj < *llf_end) {
    return GradeSegment::Llf;
  }
  return GradeSegment::PostLlf;
}

auto PackedRasterRevision(const ActiveRasterMaskInput& input) -> std::uint64_t {
  return (input.session_generation << 1) ^ (input.content_revision * 0x9E3779B97F4A7C15ull);
}

auto IsLocalTonePort(const GraphValueId& id) -> bool {
  const auto port = id.output_port.Value();
  return port == "local_tone.source.0" || port == "local_tone.result.0";
}

}  // namespace

void RuntimeInvalidationState::AddEdge(const GraphValueId& from, const GraphValueId& to) {
  if (from == to) {
    return;
  }
  auto& dests = outgoing_[from];
  if (std::find(dests.begin(), dests.end(), to) == dests.end()) {
    dests.push_back(to);
  }
}

auto RuntimeInvalidationState::Ensure(const GraphValueId& id) -> Record& { return records_[id]; }

void RuntimeInvalidationState::BindCompiledPlan(const ExecutionPlan& plan) {
  if (bound_plan_ == plan.static_key && !outgoing_.empty()) {
    sensor_output_   = plan.sensor_linear_output;
    geometry_output_ = plan.geometry_output;
    develop_output_  = plan.develop_output;
    display_output_  = plan.display_output;
    return;
  }
  outgoing_.clear();
  bound_plan_      = plan.static_key;
  sensor_output_   = plan.sensor_linear_output;
  geometry_output_ = plan.geometry_output;
  develop_output_  = plan.develop_output;
  display_output_  = plan.display_output;

  for (const auto& pass : plan.passes) {
    for (const auto& input : pass.inputs) {
      for (const auto& output : pass.outputs) {
        AddEdge(input.source, output.value);
      }
    }
  }

  for (const auto& grade : plan.grade_nodes) {
    const auto llf_source = LocalToneSourceId(grade.node_id);
    const auto llf_result = LocalToneResultId(grade.node_id);
    AddEdge(grade.scene_input, llf_source);
    AddEdge(llf_source, llf_result);
    AddEdge(llf_result, grade.scene_output);
    Ensure(llf_source);
    Ensure(llf_result);
    Ensure(grade.scene_output);
    if (!grade.mask_stack.has_value()) {
      continue;
    }
    for (const auto& source : grade.mask_stack->sources) {
      AddEdge(plan.geometry_output, source.effective_output);
      AddEdge(grade.scene_input, source.effective_output);
      AddEdge(source.effective_output, grade.mask_output);
      Ensure(source.effective_output);
    }
    AddEdge(grade.mask_output, grade.scene_output);
    Ensure(grade.mask_output);
  }
  Ensure(plan.sensor_linear_output);
  Ensure(plan.geometry_output);
  Ensure(plan.develop_output);
  Ensure(plan.display_output);
}

void RuntimeInvalidationState::InvalidateFrom(const GraphValueId& id) {
  std::vector<GraphValueId> stack{id};
  while (!stack.empty()) {
    const auto current = stack.back();
    stack.pop_back();
    auto& record = Ensure(current);
    if (record.required == change_version_) {
      continue;
    }
    record.required = change_version_;
    const auto it   = outgoing_.find(current);
    if (it == outgoing_.end()) {
      continue;
    }
    for (const auto& dest : it->second) {
      stack.push_back(dest);
    }
  }
}

void RuntimeInvalidationState::CollectDevelopChanges(const ExecutionPlan& plan,
                                                     const PipelineDocument& document,
                                                     std::vector<GraphValueId>& origins) {
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    return;
  }
  const auto dirty = develop->Params().DirtyFields();
  if ((dirty & SensorDirtyMask()).Any()) {
    origins.push_back(plan.sensor_linear_output);
  }
  if ((dirty & DirtyFieldMask{DevelopDirty::WhiteBalance}).Any()) {
    origins.push_back(plan.develop_output);
  }
}

void RuntimeInvalidationState::CollectGradeChanges(
    const ExecutionPlan& plan, const PipelineDocument& document,
    std::span<const ActiveRasterMaskInput> active_raster_masks,
    std::vector<GraphValueId>& origins) {
  for (const auto& compiled : plan.grade_nodes) {
    const auto* grade =
        dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(compiled.node_id));
    if (grade == nullptr) {
      continue;
    }
    if (grade->MixDirty()) {
      origins.push_back(compiled.scene_output);
    }
    for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
      if (!grade->AdjustmentAt(index).IsDirty()) {
        continue;
      }
      const auto segment = SegmentForIndex(compiled, index);
      if (segment == GradeSegment::PreLlf) {
        origins.push_back(LocalToneSourceId(compiled.node_id));
      } else if (segment == GradeSegment::Llf) {
        origins.push_back(LocalToneResultId(compiled.node_id));
      } else {
        origins.push_back(compiled.scene_output);
      }
    }
    if (!compiled.mask_stack.has_value()) {
      continue;
    }
    for (const auto& source : compiled.mask_stack->sources) {
      MaskKey key{compiled.node_id, source.mask_id};
      const auto revision = grade->MaskContentRevision(source.mask_id);
      auto& last          = last_mask_revision_[key];
      if (revision != last) {
        origins.push_back(source.effective_output);
        last = revision;
      }
      const auto* active =
          FindActiveRasterMaskInput(active_raster_masks, compiled.node_id, source.mask_id);
      auto& last_raster = last_raster_revision_[key];
      const auto raster_rev = active == nullptr ? 0 : PackedRasterRevision(*active);
      if (raster_rev != last_raster) {
        origins.push_back(source.effective_output);
        last_raster = raster_rev;
      }
    }
  }
}

void RuntimeInvalidationState::CollectDrtChanges(const ExecutionPlan& plan,
                                                 const PipelineDocument& document,
                                                 std::vector<GraphValueId>& origins) {
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    return;
  }
  if (drt->Params().IsDirty()) {
    origins.push_back(plan.display_output);
  }
  for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
    if (drt->AdjustmentAt(index).IsDirty()) {
      origins.push_back(plan.display_output);
      return;
    }
  }
}

void RuntimeInvalidationState::CollectStructureChanges(const ExecutionPlan& plan,
                                                       std::vector<GraphValueId>& origins) {
  // New workspaces have required==0. Assign a revision so the first bind can
  // miss and publish even when operator dirty was already consumed elsewhere.
  auto collect_if_unassigned = [&](const GraphValueId& id) {
    if (Ensure(id).required == 0) {
      origins.push_back(id);
    }
  };
  collect_if_unassigned(plan.sensor_linear_output);
  collect_if_unassigned(plan.geometry_output);
  collect_if_unassigned(plan.develop_output);
  collect_if_unassigned(plan.display_output);

  std::set<NodeId> current_grades;
  for (const auto& grade : plan.grade_nodes) {
    current_grades.insert(grade.node_id);
    collect_if_unassigned(LocalToneSourceId(grade.node_id));
    collect_if_unassigned(LocalToneResultId(grade.node_id));
    collect_if_unassigned(grade.scene_output);
    GradeBindState current;
    current.scene_input = grade.scene_input;
    if (grade.mask_stack.has_value()) {
      current.mask_ids.reserve(grade.mask_stack->sources.size());
      for (const auto& source : grade.mask_stack->sources) {
        current.mask_ids.push_back(source.mask_id);
        collect_if_unassigned(source.effective_output);
      }
      collect_if_unassigned(grade.mask_output);
    }
    const auto it = last_grade_bind_.find(grade.node_id);
    if (it == last_grade_bind_.end()) {
      last_grade_bind_.emplace(grade.node_id, std::move(current));
      continue;
    }
    const bool input_changed = it->second.scene_input != current.scene_input;
    const bool masks_changed = it->second.mask_ids != current.mask_ids;
    it->second               = std::move(current);
    if (input_changed) {
      origins.push_back(LocalToneSourceId(grade.node_id));
    }
    if (masks_changed) {
      if (grade.mask_stack.has_value()) {
        for (const auto& source : grade.mask_stack->sources) {
          origins.push_back(source.effective_output);
        }
        origins.push_back(grade.mask_output);
      }
      origins.push_back(grade.scene_output);
    }
  }
  for (auto it = last_grade_bind_.begin(); it != last_grade_bind_.end();) {
    if (current_grades.contains(it->first)) {
      ++it;
    } else {
      it = last_grade_bind_.erase(it);
    }
  }
  const auto drt_input = plan.SceneInputForDrt();
  if (last_drt_input_ != drt_input) {
    origins.push_back(plan.display_output);
    last_drt_input_ = drt_input;
  }
}

void RuntimeInvalidationState::CollectAndPropagate(
    const ExecutionPlan& plan, PipelineDocument& document, const PreparedRawInput& input,
    std::span<const ActiveRasterMaskInput> active_raster_masks) {
  BindCompiledPlan(plan);
  CaptureFrameRepresentations(plan, input);

  std::vector<GraphValueId> origins;
  CollectStructureChanges(plan, origins);
  if (document.TopologyDirty()) {
    document.ClearTopologyDirty();
  }
  CollectDevelopChanges(plan, document, origins);
  CollectGradeChanges(plan, document, active_raster_masks, origins);
  CollectDrtChanges(plan, document, origins);

  for (const auto& compiled : plan.grade_nodes) {
    auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(compiled.node_id));
    if (grade != nullptr) {
      grade->ClearMixDirty();
    }
  }

  if (origins.empty()) {
    return;
  }
  ++change_version_;
  std::set<GraphValueId> unique(origins.begin(), origins.end());
  for (const auto& origin : unique) {
    InvalidateFrom(origin);
  }
}

void RuntimeInvalidationState::CaptureFrameRepresentations(const ExecutionPlan& plan,
                                                           const PreparedRawInput& input) {
  sensor_identity_    = HashSensorSourceIdentity(input).hash;
  canonical_identity_ = HashCanonicalReferenceIdentity(plan, input).hash;
  frame_identity_     = HashFrameImageIdentity(plan, input, document_epoch_).hash;
}

void RuntimeInvalidationState::AdvanceDocumentEpoch() {
  ++document_epoch_;
  ++change_version_;
  for (auto& [id, record] : records_) {
    (void)id;
    record.required = change_version_;
  }
  last_mask_revision_.clear();
  last_raster_revision_.clear();
}

void RuntimeInvalidationState::Clear() {
  outgoing_.clear();
  records_.clear();
  last_mask_revision_.clear();
  last_raster_revision_.clear();
  last_grade_bind_.clear();
  last_drt_input_     = {};
  bound_plan_         = {};
  document_epoch_     = 1;
  change_version_     = 0;
  sensor_identity_    = 0;
  frame_identity_     = 0;
  canonical_identity_ = 0;
}

auto RuntimeInvalidationState::RequiredRevision(const GraphValueId& id) const -> RuntimeRevision {
  const auto it = records_.find(id);
  return it == records_.end() ? 0 : it->second.required;
}

auto RuntimeInvalidationState::CompletedRevision(const GraphValueId& id) const -> RuntimeRevision {
  const auto it = records_.find(id);
  return it == records_.end() ? 0 : it->second.completed;
}

auto RuntimeInvalidationState::PublishedRepresentation(const GraphValueId& id) const
    -> ResultRepresentation {
  const auto it = records_.find(id);
  return it == records_.end() ? ResultRepresentation{} : it->second.published;
}

auto RuntimeInvalidationState::IsSatisfied(const GraphValueId& id,
                                           const ResultRepresentation& needed) const -> bool {
  const auto it = records_.find(id);
  if (it == records_.end()) {
    return false;
  }
  const auto& record = it->second;
  return record.completed != 0 && record.completed == record.required &&
         RepresentationSatisfies(record.published, needed);
}

auto RuntimeInvalidationState::HasCurrentRevision(const GraphValueId& id,
                                                  RuntimeRevision published_revision) const
    -> bool {
  return published_revision != 0 && RequiredRevision(id) == published_revision;
}

auto RuntimeInvalidationState::MakeImageRepresentation(const GraphValueId& id, ImageExtent extent,
                                                       TextureFormat format,
                                                       std::uint32_t source_detail) const
    -> ResultRepresentation {
  ResultRepresentation repr;
  repr.document_epoch = document_epoch_;
  repr.extent         = extent;
  repr.format         = format;
  repr.source_detail  = source_detail;
  if (id == sensor_output_) {
    repr.identity = sensor_identity_;
  } else if (IsLocalTonePort(id)) {
    repr.identity = canonical_identity_;
  } else {
    repr.identity = frame_identity_;
  }
  return repr;
}

void RuntimeInvalidationState::MarkCompleted(const GraphValueId& id,
                                             const ResultRepresentation& representation) {
  auto& record     = Ensure(id);
  record.completed = record.required;
  record.published = representation;
}

auto RuntimeInvalidationState::Downstream(const GraphValueId& id) const
    -> std::vector<GraphValueId> {
  const auto it = outgoing_.find(id);
  return it == outgoing_.end() ? std::vector<GraphValueId>{} : it->second;
}

}  // namespace alcedo
