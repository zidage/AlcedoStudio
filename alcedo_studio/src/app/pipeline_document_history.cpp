//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_document_history.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {
namespace {

[[noreturn]] void Fail(std::string message) { throw std::runtime_error(std::move(message)); }

auto OwnerKindFromEditor(EditorParameterOwnerKind kind) -> PipelineParameterOwnerKind {
  switch (kind) {
    case EditorParameterOwnerKind::Document:
      return PipelineParameterOwnerKind::Document;
    case EditorParameterOwnerKind::Develop:
      return PipelineParameterOwnerKind::Develop;
    case EditorParameterOwnerKind::ColorGrade:
      return PipelineParameterOwnerKind::ColorGrade;
    case EditorParameterOwnerKind::ColorGradeMask:
      return PipelineParameterOwnerKind::ColorGradeMask;
    case EditorParameterOwnerKind::DrtPost:
      return PipelineParameterOwnerKind::DrtPost;
    case EditorParameterOwnerKind::Unspecified:
      break;
  }
  Fail("Editor parameter target requires owner_kind");
}

auto OwnerKindToEditor(PipelineParameterOwnerKind kind) -> EditorParameterOwnerKind {
  switch (kind) {
    case PipelineParameterOwnerKind::Document:
      return EditorParameterOwnerKind::Document;
    case PipelineParameterOwnerKind::Develop:
      return EditorParameterOwnerKind::Develop;
    case PipelineParameterOwnerKind::ColorGrade:
      return EditorParameterOwnerKind::ColorGrade;
    case PipelineParameterOwnerKind::ColorGradeMask:
      return EditorParameterOwnerKind::ColorGradeMask;
    case PipelineParameterOwnerKind::DrtPost:
      return EditorParameterOwnerKind::DrtPost;
  }
  Fail("Pipeline parameter owner_kind is not supported");
}

auto ImagePort() -> PortId { return PortId{"image"}; }

}  // namespace

auto ToPipelineParameterTarget(const EditorParameterTarget& target) -> PipelineParameterTarget {
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty() && target.owner_kind != EditorParameterOwnerKind::ColorGradeMask) {
    Fail(target_error);
  }
  if (target.owner_kind == EditorParameterOwnerKind::Unspecified) {
    Fail("Editor parameter target requires owner_kind");
  }
  PipelineParameterTarget stored;
  stored.owner_kind             = OwnerKindFromEditor(target.owner_kind);
  stored.node_id                = target.node_id;
  stored.adjustment_instance_id = target.adjustment_instance_id;
  stored.mask_id                = MaskId{target.mask_id};
  stored.field_key              = target.field_key;
  return stored;
}

auto ToEditorParameterTarget(const PipelineParameterTarget& target) -> EditorParameterTarget {
  EditorParameterTarget editor;
  editor.owner_kind             = OwnerKindToEditor(target.owner_kind);
  editor.node_id                = target.node_id;
  editor.adjustment_instance_id = target.adjustment_instance_id;
  editor.mask_id                = std::string{target.mask_id.Value()};
  editor.field_key              = target.field_key;
  return editor;
}

auto ToGraphEdge(const PipelineSceneEdge& edge) -> GraphEdge {
  return GraphEdge{edge.from_node, edge.from_port, edge.to_node, edge.to_port};
}

auto ToPipelineSceneEdge(const GraphEdge& edge) -> PipelineSceneEdge {
  return PipelineSceneEdge{edge.from_node, edge.from_port, edge.to_node, edge.to_port};
}

auto PresentationKeyForOperation(PipelineEditOperationKind kind) -> std::string {
  return "history.operation." + std::string{PipelineEditOperationKindText(kind)};
}

auto RenderReasonForBatch(const PipelineEditBatch& batch) -> std::optional<EditorRenderReason> {
  switch (batch.operation_kind) {
    case PipelineEditOperationKind::RenameColorGrade:
      return std::nullopt;
    case PipelineEditOperationKind::AddColorGrade:
    case PipelineEditOperationKind::RemoveColorGrade:
    case PipelineEditOperationKind::ReconnectColorGrade:
      return EditorRenderReason::GraphTopologyChanged;
    case PipelineEditOperationKind::AddMask:
    case PipelineEditOperationKind::RemoveMask:
    case PipelineEditOperationKind::ReplaceMaskSource:
    case PipelineEditOperationKind::ReplaceMaskAsset:
    case PipelineEditOperationKind::SetMaskField:
      return EditorRenderReason::SettledMaskEdit;
    case PipelineEditOperationKind::SetParameter:
    case PipelineEditOperationKind::SetNodeEnabled:
    case PipelineEditOperationKind::SetNodeMix:
      return EditorRenderReason::SettledAdjustment;
    case PipelineEditOperationKind::Paste:
      return EditorRenderReason::PastedPipelineDocument;
  }
  return EditorRenderReason::SettledAdjustment;
}

auto RenderReasonForHeadMove(const std::vector<EditCommit>& commits)
    -> std::optional<EditorRenderReason> {
  bool pixel_changing = false;
  for (const auto& commit : commits) {
    if (!IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
      pixel_changing = true;
      break;
    }
    const auto batch = PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
    if (RenderReasonForBatch(batch).has_value()) {
      pixel_changing = true;
      break;
    }
  }
  if (!pixel_changing) {
    return std::nullopt;
  }
  return EditorRenderReason::UndoRedo;
}

auto MakeSetParameterBatch(const EditorParameterTarget& target, nlohmann::json before_value,
                           nlohmann::json after_value, bool before_enabled, bool after_enabled,
                           std::string node_display_name) -> PipelineEditBatch {
  SetParameterChange change;
  change.target         = ToPipelineParameterTarget(target);
  change.before_value   = std::move(before_value);
  change.after_value    = std::move(after_value);
  change.before_enabled = before_enabled;
  change.after_enabled  = after_enabled;
  nlohmann::json args{{"field_key", target.field_key},
                      {"node_display_name", std::move(node_display_name)},
                      {"node_id", std::string{target.node_id.Value()}}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::SetParameter),
                                 std::move(args));
}

auto CaptureAddColorGradeChange(const PipelineDocument& document, const NodeId& before_node_id,
                                const NodeId& new_id) -> AddColorGradeChange {
  if (new_id.Empty()) {
    Fail("AddColorGrade requires a NodeId");
  }
  if (document.Graph().FindNode(new_id) != nullptr) {
    Fail("AddColorGrade duplicate node id: " + std::string{new_id.Value()});
  }
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), before_node_id);
  if (incoming == nullptr) {
    Fail("AddColorGrade insert point has no scene-image predecessor");
  }
  const auto next_name_number = document.NextColorGradeNameNumber();
  if (next_name_number == std::numeric_limits<std::uint64_t>::max()) {
    Fail("AddColorGrade display-name number is exhausted");
  }
  auto node = CreateCleanColorGradeNode(new_id);
  node->SetDisplayName(DefaultColorGradeDisplayName(next_name_number));
  AddColorGradeChange change;
  change.node_id         = new_id;
  change.node            = node->ToJson();
  change.before_next_color_grade_name_number = next_name_number;
  change.after_next_color_grade_name_number  = next_name_number + 1;
  change.predecessor_id  = incoming->from_node;
  change.successor_id    = before_node_id;
  change.incoming_edge   = PipelineSceneEdge{incoming->from_node, incoming->from_port, new_id,
                                             ImagePort()};
  change.outgoing_edge   = PipelineSceneEdge{new_id, ImagePort(), before_node_id, incoming->to_port};
  return change;
}

auto MakeAddColorGradeBatch(AddColorGradeChange change) -> PipelineEditBatch {
  nlohmann::json args{{"node_id", std::string{change.node_id.Value()}}};
  if (change.node.contains("display_name") && change.node.at("display_name").is_string()) {
    args["node_display_name"] = change.node.at("display_name").get<std::string>();
  }
  return PipelineEditBatch::Make(PipelineEditOperationKind::AddColorGrade, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::AddColorGrade),
                                 std::move(args));
}

auto CaptureRemoveColorGradeChange(const PipelineDocument& document, const NodeId& node_id)
    -> RemoveColorGradeChange {
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  if (grade == nullptr) {
    Fail("RemoveColorGrade requires a Color Grade");
  }
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), node_id);
  const auto* outgoing = FindSceneImageSuccessor(document.Graph(), node_id);
  if (incoming == nullptr || outgoing == nullptr) {
    Fail("RemoveColorGrade requires a scene-image edge pair");
  }
  RemoveColorGradeChange change;
  change.node_id                = node_id;
  change.node                   = grade->ToJson();
  change.predecessor_id         = incoming->from_node;
  change.successor_id           = outgoing->to_node;
  change.removed_incoming_edge  = ToPipelineSceneEdge(*incoming);
  change.removed_outgoing_edge  = ToPipelineSceneEdge(*outgoing);
  change.bridge_edge = PipelineSceneEdge{incoming->from_node, incoming->from_port, outgoing->to_node,
                                         outgoing->to_port};
  return change;
}

auto MakeRemoveColorGradeBatch(RemoveColorGradeChange change) -> PipelineEditBatch {
  nlohmann::json args{{"node_id", std::string{change.node_id.Value()}}};
  if (change.node.contains("display_name") && change.node.at("display_name").is_string()) {
    args["node_display_name"] = change.node.at("display_name").get<std::string>();
  }
  return PipelineEditBatch::Make(PipelineEditOperationKind::RemoveColorGrade, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::RemoveColorGrade),
                                 std::move(args));
}

auto CaptureReconnectColorGradeChange(const PipelineDocument& document, const NodeId& node_id,
                                      const NodeId& new_predecessor_id,
                                      const NodeId& new_successor_id) -> ReconnectColorGradeChange {
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), node_id);
  const auto* outgoing = FindSceneImageSuccessor(document.Graph(), node_id);
  if (incoming == nullptr || outgoing == nullptr) {
    Fail("ReconnectColorGrade requires a scene-image edge pair");
  }
  ReconnectColorGradeChange change;
  change.node_id               = node_id;
  change.before_predecessor_id = incoming->from_node;
  change.before_successor_id   = outgoing->to_node;
  change.after_predecessor_id  = new_predecessor_id;
  change.after_successor_id    = new_successor_id;
  change.before_incoming_edge  = ToPipelineSceneEdge(*incoming);
  change.before_outgoing_edge  = ToPipelineSceneEdge(*outgoing);
  change.after_incoming_edge   = PipelineSceneEdge{new_predecessor_id, ImagePort(), node_id,
                                                   ImagePort()};
  change.after_outgoing_edge   = PipelineSceneEdge{node_id, ImagePort(), new_successor_id,
                                                   ImagePort()};
  return change;
}

auto MakeReconnectColorGradeBatch(ReconnectColorGradeChange change) -> PipelineEditBatch {
  nlohmann::json args{{"node_id", std::string{change.node_id.Value()}}};
  return PipelineEditBatch::Make(
      PipelineEditOperationKind::ReconnectColorGrade, {std::move(change)},
      PresentationKeyForOperation(PipelineEditOperationKind::ReconnectColorGrade), std::move(args));
}

auto MakeRenameColorGradeBatch(const NodeId& node_id, std::string before_name, std::string after_name)
    -> PipelineEditBatch {
  RenameColorGradeChange change;
  change.node_id             = node_id;
  change.before_display_name = std::move(before_name);
  change.after_display_name  = std::move(after_name);
  nlohmann::json args{{"node_display_name", change.after_display_name},
                      {"node_id", std::string{node_id.Value()}}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::RenameColorGrade, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::RenameColorGrade),
                                 std::move(args));
}

auto MakeSetNodeEnabledBatch(const NodeId& node_id, PipelineEditNodeKind node_kind,
                             bool before_enabled, bool after_enabled) -> PipelineEditBatch {
  SetNodeEnabledChange change;
  change.node_id        = node_id;
  change.node_kind      = node_kind;
  change.before_enabled = before_enabled;
  change.after_enabled  = after_enabled;
  nlohmann::json args{{"node_id", std::string{node_id.Value()}}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::SetNodeEnabled, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::SetNodeEnabled),
                                 std::move(args));
}

auto MakeSetNodeMixBatch(const NodeId& node_id, float before_mix, float after_mix)
    -> PipelineEditBatch {
  SetNodeMixChange change;
  change.node_id    = node_id;
  change.before_mix = before_mix;
  change.after_mix  = after_mix;
  nlohmann::json args{{"node_id", std::string{node_id.Value()}}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::SetNodeMix, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::SetNodeMix),
                                 std::move(args));
}

auto MakeAddMaskBatch(const NodeId& node_id, MaskId mask_id, nlohmann::json mask,
                      std::uint32_t display_index) -> PipelineEditBatch {
  AddMaskChange change;
  change.node_id       = node_id;
  change.mask_id       = std::move(mask_id);
  change.mask          = std::move(mask);
  change.display_index = display_index;
  nlohmann::json args{{"node_id", std::string{node_id.Value()}},
                      {"mask_id", std::string{change.mask_id.Value()}}};
  if (change.mask.contains("display_name") && change.mask.at("display_name").is_string()) {
    args["mask_display_name"] = change.mask.at("display_name").get<std::string>();
  }
  return PipelineEditBatch::Make(PipelineEditOperationKind::AddMask, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::AddMask),
                                 std::move(args));
}

auto MakeRemoveMaskBatch(const NodeId& node_id, MaskId mask_id, nlohmann::json mask,
                         std::uint32_t display_index) -> PipelineEditBatch {
  RemoveMaskChange change;
  change.node_id       = node_id;
  change.mask_id       = std::move(mask_id);
  change.mask          = std::move(mask);
  change.display_index = display_index;
  nlohmann::json args{{"node_id", std::string{node_id.Value()}},
                      {"mask_id", std::string{change.mask_id.Value()}}};
  if (change.mask.contains("display_name") && change.mask.at("display_name").is_string()) {
    args["mask_display_name"] = change.mask.at("display_name").get<std::string>();
  }
  return PipelineEditBatch::Make(PipelineEditOperationKind::RemoveMask, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::RemoveMask),
                                 std::move(args));
}

auto MakeReplaceMaskSourceBatch(const NodeId& node_id, const MaskId& mask_id,
                                nlohmann::json before_source, nlohmann::json after_source)
    -> PipelineEditBatch {
  ReplaceMaskSourceChange change;
  change.node_id       = node_id;
  change.mask_id       = mask_id;
  change.before_source = std::move(before_source);
  change.after_source  = std::move(after_source);
  nlohmann::json args{{"node_id", std::string{node_id.Value()}},
                      {"mask_id", std::string{mask_id.Value()}}};
  return PipelineEditBatch::Make(
      PipelineEditOperationKind::ReplaceMaskSource, {std::move(change)},
      PresentationKeyForOperation(PipelineEditOperationKind::ReplaceMaskSource), std::move(args));
}

auto MakeReplaceMaskAssetBatch(const NodeId& node_id, const MaskId& mask_id,
                               nlohmann::json before_source, nlohmann::json after_source)
    -> PipelineEditBatch {
  ReplaceMaskAssetChange change;
  change.node_id       = node_id;
  change.mask_id       = mask_id;
  change.before_source = std::move(before_source);
  change.after_source  = std::move(after_source);
  nlohmann::json args{{"node_id", std::string{node_id.Value()}},
                      {"mask_id", std::string{mask_id.Value()}}};
  return PipelineEditBatch::Make(
      PipelineEditOperationKind::ReplaceMaskAsset, {std::move(change)},
      PresentationKeyForOperation(PipelineEditOperationKind::ReplaceMaskAsset), std::move(args));
}

auto MakeSetMaskFieldBatch(const NodeId& node_id, const MaskId& mask_id, std::string field_key,
                           nlohmann::json before_value, nlohmann::json after_value)
    -> PipelineEditBatch {
  SetMaskFieldChange change;
  change.node_id      = node_id;
  change.mask_id      = mask_id;
  change.field_key    = std::move(field_key);
  change.before_value = std::move(before_value);
  change.after_value  = std::move(after_value);
  nlohmann::json args{{"field_key", change.field_key},
                      {"node_id", std::string{node_id.Value()}},
                      {"mask_id", std::string{mask_id.Value()}}};
  return PipelineEditBatch::Make(PipelineEditOperationKind::SetMaskField, {std::move(change)},
                                 PresentationKeyForOperation(PipelineEditOperationKind::SetMaskField),
                                 std::move(args));
}

auto MakePasteBatch(std::vector<PipelineEditChange> changes) -> PipelineEditBatch {
  return PipelineEditBatch::Make(PipelineEditOperationKind::Paste, std::move(changes),
                                 PresentationKeyForOperation(PipelineEditOperationKind::Paste));
}

auto PublishTypedPipelineEdit(MiniGitWorkingHistory& history, const PipelineEditBatch& batch)
    -> MiniGitEditAppendResult {
  return history.PublishPreparedEdit(history.PrepareAppendEdit(batch));
}

}  // namespace alcedo
