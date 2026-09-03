//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/pipeline_history_applier.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"

#include <set>

namespace alcedo {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto JoinValidation(const std::vector<GraphValidationError>& errors) -> std::string {
  std::string text;
  for (const auto& item : errors) {
    if (!text.empty()) {
      text += "; ";
    }
    text += item.message;
  }
  return text;
}

auto JsonEqual(const nlohmann::json& left, const nlohmann::json& right) -> bool {
  return left.dump() == right.dump();
}

auto EdgesEqual(const GraphEdge& left, const PipelineSceneEdge& right) -> bool {
  return left.from_node == right.from_node && left.from_port == right.from_port &&
         left.to_node == right.to_node && left.to_port == right.to_port;
}

auto RequireColorGrade(PipelineDocument& document, const NodeId& node_id, std::string* error)
    -> ColorGradeNodeModel* {
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string{node_id.Value()});
    return nullptr;
  }
  return grade;
}

auto RequireColorGrade(const PipelineDocument& document, const NodeId& node_id, std::string* error)
    -> const ColorGradeNodeModel* {
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(node_id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string{node_id.Value()});
    return nullptr;
  }
  return grade;
}

auto ApplyGraph(PipelineDocument& document, std::vector<GraphValidationError> errors,
                std::string* error) -> bool {
  if (!errors.empty()) {
    return SetError(error, JoinValidation(errors));
  }
  return true;
}

auto ExpectedParameterJson(const PipelineDocument& document, const SetParameterChange& change,
                           PipelineEditApplyDirection direction, nlohmann::json* json,
                           std::string* error) -> bool {
  const auto target = ToEditorParameterTarget(change.target);
  return ReadEditorParameterJson(document, target, json, error) &&
         JsonEqual(*json, direction == PipelineEditApplyDirection::Forward ? change.before_value
                                                                          : change.after_value);
}

auto ApplySetParameter(PipelineDocument& document, const SetParameterChange& change,
                       PipelineEditApplyDirection direction, std::string* error) -> bool {
  nlohmann::json current;
  if (!ExpectedParameterJson(document, change, direction, &current, error)) {
    if (error != nullptr && error->empty()) {
      *error = "SetParameter expected current side does not match stored values";
    } else if (error != nullptr) {
      *error = "SetParameter expected current side does not match stored values: " + *error;
    }
    return false;
  }
  const auto target = ToEditorParameterTarget(change.target);
  const auto& value = direction == PipelineEditApplyDirection::Forward ? change.after_value
                                                                      : change.before_value;
  return ApplyEditorParameterPatch(document, target, value, error);
}

auto ApplySetNodeEnabled(PipelineDocument& document, const SetNodeEnabledChange& change,
                         PipelineEditApplyDirection direction, std::string* error) -> bool {
  if (change.node_kind != PipelineEditNodeKind::ColorGrade) {
    return SetError(error, "SetNodeEnabled supports Color Grade nodes only");
  }
  const auto* grade = RequireColorGrade(document, change.node_id, error);
  if (grade == nullptr) {
    return false;
  }
  const bool expected = direction == PipelineEditApplyDirection::Forward ? change.before_enabled
                                                                        : change.after_enabled;
  if (grade->Enabled() != expected) {
    return SetError(error, "SetNodeEnabled expected current side does not match stored values");
  }
  const bool next = direction == PipelineEditApplyDirection::Forward ? change.after_enabled
                                                                    : change.before_enabled;
  return ApplyGraph(document, SetColorGradeEnabled(document, change.node_id, next), error);
}

auto ApplySetNodeMix(PipelineDocument& document, const SetNodeMixChange& change,
                     PipelineEditApplyDirection direction, std::string* error) -> bool {
  const auto* grade = RequireColorGrade(document, change.node_id, error);
  if (grade == nullptr) {
    return false;
  }
  const float expected =
      direction == PipelineEditApplyDirection::Forward ? change.before_mix : change.after_mix;
  if (grade->Mix() != expected) {
    return SetError(error, "SetNodeMix expected current side does not match stored values");
  }
  const float next =
      direction == PipelineEditApplyDirection::Forward ? change.after_mix : change.before_mix;
  return ApplyGraph(document, SetColorGradeMix(document, change.node_id, next), error);
}

auto ApplyRename(PipelineDocument& document, const RenameColorGradeChange& change,
                 PipelineEditApplyDirection direction, std::string* error) -> bool {
  const auto* grade = RequireColorGrade(document, change.node_id, error);
  if (grade == nullptr) {
    return false;
  }
  const auto& expected = direction == PipelineEditApplyDirection::Forward
                             ? change.before_display_name
                             : change.after_display_name;
  if (std::string{grade->DisplayName()} != expected) {
    return SetError(error, "RenameColorGrade expected current side does not match stored values");
  }
  const auto& next = direction == PipelineEditApplyDirection::Forward ? change.after_display_name
                                                                      : change.before_display_name;
  return ApplyGraph(document, RenameColorGrade(document, change.node_id, next), error);
}

auto ApplyAddColorGrade(PipelineDocument& document, const AddColorGradeChange& change,
                        PipelineEditApplyDirection direction, std::string* error) -> bool {
  if (direction == PipelineEditApplyDirection::Forward) {
    if (document.NextColorGradeNameNumber() != change.before_next_color_grade_name_number) {
      return SetError(error, "AddColorGrade expected the before name-counter value");
    }
    if (document.Graph().FindNode(change.node_id) != nullptr) {
      return SetError(error, "AddColorGrade expected the stored node to be absent");
    }
    const auto* split =
        FindSceneImageEdge(document.Graph(), change.predecessor_id, change.successor_id);
    if (split == nullptr) {
      return SetError(error, "AddColorGrade expected predecessor-to-successor edge is missing");
    }
    if (!ApplyGraph(document,
                    InsertColorGradeFromJson(document, change.node,
                                             ToGraphEdge(change.incoming_edge),
                                             ToGraphEdge(change.outgoing_edge)),
                    error)) {
      return false;
    }
    document.SetNextColorGradeNameNumber(change.after_next_color_grade_name_number);
    return true;
  }
  if (document.NextColorGradeNameNumber() != change.after_next_color_grade_name_number) {
    return SetError(error, "AddColorGrade inverse expected the after name-counter value");
  }
  if (document.Graph().FindNode(change.node_id) == nullptr) {
    return SetError(error, "AddColorGrade inverse expected the stored node to be present");
  }
  if (!ApplyGraph(document, RemoveColorGradeAndBridge(document, change.node_id), error)) {
    return false;
  }
  document.SetNextColorGradeNameNumber(change.before_next_color_grade_name_number);
  return true;
}

auto ApplyRemoveColorGrade(PipelineDocument& document, const RemoveColorGradeChange& change,
                           PipelineEditApplyDirection direction, std::string* error) -> bool {
  if (direction == PipelineEditApplyDirection::Forward) {
    const auto* incoming = FindSceneImagePredecessor(document.Graph(), change.node_id);
    const auto* outgoing = FindSceneImageSuccessor(document.Graph(), change.node_id);
    if (incoming == nullptr || outgoing == nullptr ||
        !EdgesEqual(*incoming, change.removed_incoming_edge) ||
        !EdgesEqual(*outgoing, change.removed_outgoing_edge)) {
      return SetError(error, "RemoveColorGrade expected current edges do not match stored values");
    }
    return ApplyGraph(document, RemoveColorGradeAndBridge(document, change.node_id), error);
  }
  if (document.Graph().FindNode(change.node_id) != nullptr) {
    return SetError(error, "RemoveColorGrade inverse expected the stored node to be absent");
  }
  return ApplyGraph(document,
                    InsertColorGradeFromJson(document, change.node,
                                             ToGraphEdge(change.removed_incoming_edge),
                                             ToGraphEdge(change.removed_outgoing_edge)),
                    error);
}

auto ApplyReconnect(PipelineDocument& document, const ReconnectColorGradeChange& change,
                    PipelineEditApplyDirection direction, std::string* error) -> bool {
  const auto* incoming = FindSceneImagePredecessor(document.Graph(), change.node_id);
  const auto* outgoing = FindSceneImageSuccessor(document.Graph(), change.node_id);
  const auto& expected_in = direction == PipelineEditApplyDirection::Forward
                                ? change.before_incoming_edge
                                : change.after_incoming_edge;
  const auto& expected_out = direction == PipelineEditApplyDirection::Forward
                                 ? change.before_outgoing_edge
                                 : change.after_outgoing_edge;
  if (incoming == nullptr || outgoing == nullptr || !EdgesEqual(*incoming, expected_in) ||
      !EdgesEqual(*outgoing, expected_out)) {
    return SetError(error, "ReconnectColorGrade expected current edges do not match stored values");
  }
  const auto& pred = direction == PipelineEditApplyDirection::Forward
                         ? change.after_predecessor_id
                         : change.before_predecessor_id;
  const auto& succ = direction == PipelineEditApplyDirection::Forward
                         ? change.after_successor_id
                         : change.before_successor_id;
  return ApplyGraph(document, ReconnectColorGrade(document, change.node_id, pred, succ), error);
}

auto MaskIndex(const ColorGradeNodeModel& grade, const MaskId& mask_id) -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < grade.MaskCount(); ++index) {
    if (grade.MaskAt(index).id == mask_id) {
      return index;
    }
  }
  return std::nullopt;
}

auto ApplyAddMask(PipelineDocument& document, const AddMaskChange& change,
                  PipelineEditApplyDirection direction, std::string* error) -> bool {
  auto* grade = RequireColorGrade(document, change.node_id, error);
  if (grade == nullptr) {
    return false;
  }
  try {
    if (direction == PipelineEditApplyDirection::Forward) {
      if (grade->FindMask(change.mask_id) != nullptr) {
        return SetError(error, "AddMask expected the stored Mask to be absent");
      }
      if (change.display_index > grade->MaskCount()) {
        return SetError(error, "AddMask display_index is past the Mask list");
      }
      grade->AddMask(MaskModelFromJson(change.mask), change.display_index);
      return true;
    }
    const auto index = MaskIndex(*grade, change.mask_id);
    if (!index.has_value() || *index != change.display_index) {
      return SetError(error, "AddMask inverse expected the stored Mask at display_index");
    }
    grade->RemoveMask(change.mask_id);
    return true;
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto ApplyRemoveMask(PipelineDocument& document, const RemoveMaskChange& change,
                     PipelineEditApplyDirection direction, std::string* error) -> bool {
  auto* grade = RequireColorGrade(document, change.node_id, error);
  if (grade == nullptr) {
    return false;
  }
  try {
    if (direction == PipelineEditApplyDirection::Forward) {
      const auto index = MaskIndex(*grade, change.mask_id);
      if (!index.has_value() || *index != change.display_index) {
        return SetError(error, "RemoveMask expected the stored Mask at display_index");
      }
      if (!JsonEqual(MaskModelToJson(grade->MaskAt(*index)), change.mask)) {
        return SetError(error, "RemoveMask expected current Mask JSON does not match stored values");
      }
      grade->RemoveMask(change.mask_id);
      return true;
    }
    if (grade->FindMask(change.mask_id) != nullptr) {
      return SetError(error, "RemoveMask inverse expected the stored Mask to be absent");
    }
    grade->AddMask(MaskModelFromJson(change.mask), change.display_index);
    return true;
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto CurrentMaskSourceJson(const ColorGradeNodeModel& grade, const MaskId& mask_id,
                           std::string* error) -> nlohmann::json {
  const auto* mask = grade.FindMask(mask_id);
  if (mask == nullptr) {
    SetError(error, "Mask is missing: " + std::string{mask_id.Value()});
    return nullptr;
  }
  return MaskModelToJson(*mask).at("source");
}

auto ApplyReplaceMaskSource(PipelineDocument& document, const NodeId& node_id, const MaskId& mask_id,
                            const nlohmann::json& before_source, const nlohmann::json& after_source,
                            PipelineEditApplyDirection direction, std::string* error) -> bool {
  auto* grade = RequireColorGrade(document, node_id, error);
  if (grade == nullptr) {
    return false;
  }
  const auto current = CurrentMaskSourceJson(*grade, mask_id, error);
  if (current.is_null()) {
    return false;
  }
  const auto& expected =
      direction == PipelineEditApplyDirection::Forward ? before_source : after_source;
  if (!JsonEqual(current, expected)) {
    return SetError(error, "Mask source expected current side does not match stored values");
  }
  const auto& next = direction == PipelineEditApplyDirection::Forward ? after_source : before_source;
  try {
    auto parsed = MaskModelFromJson(nlohmann::json{{"id", std::string{mask_id.Value()}},
                                                   {"display_name", ""},
                                                   {"enabled", true},
                                                   {"opacity", 1.0},
                                                   {"invert", false},
                                                   {"source", next},
                                                   {"color_range", nullptr},
                                                   {"luminance_range", nullptr}});
    grade->ReplaceMaskSource(mask_id, std::move(parsed.source));
    return true;
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto AssetKeyFromSource(const nlohmann::json& source) -> MaskAssetKey {
  return MaskAssetKey{source.at("asset_key").get<std::string>()};
}

auto ApplyReplaceMaskAsset(PipelineDocument& document, const ReplaceMaskAssetChange& change,
                           PipelineEditApplyDirection direction, std::string* error,
                           MaskStore* mask_store) -> bool {
  if (mask_store == nullptr) {
    return SetError(error, "ReplaceMaskAsset requires a Mask store");
  }
  try {
    (void)mask_store->Load(AssetKeyFromSource(change.before_source));
    (void)mask_store->Load(AssetKeyFromSource(change.after_source));
  } catch (const std::exception& ex) {
    return SetError(error, std::string{"ReplaceMaskAsset asset load failed: "} + ex.what());
  }
  return ApplyReplaceMaskSource(document, change.node_id, change.mask_id, change.before_source,
                                change.after_source, direction, error);
}

auto CurrentMaskField(const MaskModel& mask, const std::string& field_key) -> nlohmann::json {
  if (field_key == "enabled") {
    return mask.enabled;
  }
  if (field_key == "invert") {
    return mask.invert;
  }
  if (field_key == "opacity") {
    return mask.opacity;
  }
  return mask.display_name;
}

auto ApplySetMaskField(PipelineDocument& document, const SetMaskFieldChange& change,
                       PipelineEditApplyDirection direction, std::string* error) -> bool {
  auto* grade = RequireColorGrade(document, change.node_id, error);
  if (grade == nullptr) {
    return false;
  }
  auto* mask = grade->FindMask(change.mask_id);
  if (mask == nullptr) {
    return SetError(error, "Mask is missing: " + std::string{change.mask_id.Value()});
  }
  const auto& expected =
      direction == PipelineEditApplyDirection::Forward ? change.before_value : change.after_value;
  if (CurrentMaskField(*mask, change.field_key) != expected) {
    return SetError(error, "SetMaskField expected current side does not match stored values");
  }
  const auto& next =
      direction == PipelineEditApplyDirection::Forward ? change.after_value : change.before_value;
  try {
    if (change.field_key == "enabled") {
      grade->SetMaskEnabled(change.mask_id, next.get<bool>());
    } else if (change.field_key == "invert") {
      grade->SetMaskInvert(change.mask_id, next.get<bool>());
    } else if (change.field_key == "opacity") {
      grade->SetMaskOpacity(change.mask_id, next.get<float>());
    } else {
      mask->display_name = next.get<std::string>();
    }
    return true;
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto ApplyOneChange(PipelineDocument& document, const PipelineEditChange& change,
                    PipelineEditApplyDirection direction, std::string* error,
                    const PipelineHistoryApplyContext& context) -> bool {
  return std::visit(
      [&](const auto& typed) -> bool {
        using Typed = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Typed, SetParameterChange>) {
          return ApplySetParameter(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, SetNodeEnabledChange>) {
          return ApplySetNodeEnabled(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, SetNodeMixChange>) {
          return ApplySetNodeMix(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, RenameColorGradeChange>) {
          return ApplyRename(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, AddColorGradeChange>) {
          return ApplyAddColorGrade(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, RemoveColorGradeChange>) {
          return ApplyRemoveColorGrade(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, ReconnectColorGradeChange>) {
          return ApplyReconnect(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, AddMaskChange>) {
          return ApplyAddMask(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, RemoveMaskChange>) {
          return ApplyRemoveMask(document, typed, direction, error);
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskSourceChange>) {
          return ApplyReplaceMaskSource(document, typed.node_id, typed.mask_id, typed.before_source,
                                        typed.after_source, direction, error);
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskAssetChange>) {
          return ApplyReplaceMaskAsset(document, typed, direction, error, context.mask_store);
        } else {
          return ApplySetMaskField(document, typed, direction, error);
        }
      },
      change);
}

auto Opposite(PipelineEditApplyDirection direction) -> PipelineEditApplyDirection {
  return direction == PipelineEditApplyDirection::Forward ? PipelineEditApplyDirection::Inverse
                                                          : PipelineEditApplyDirection::Forward;
}

auto StructuralBatch(const PipelineEditBatch& batch) -> bool {
  switch (batch.operation_kind) {
    case PipelineEditOperationKind::SetParameter:
    case PipelineEditOperationKind::SetNodeEnabled:
    case PipelineEditOperationKind::SetNodeMix:
    case PipelineEditOperationKind::RenameColorGrade:
    case PipelineEditOperationKind::SetMaskField:
      return false;
    default:
      return true;
  }
}

}  // namespace

auto ApplyPipelineEditBatch(PipelineDocument& document, const PipelineEditBatch& batch,
                            PipelineEditApplyDirection direction, std::string* error,
                            const PipelineHistoryApplyContext& context) -> bool {
  try {
    batch.Validate();
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
  auto snapshot = ClonePipelineDocument(document);
  const auto ordered = OrderedChangesForApply(batch, direction);
  std::vector<PipelineEditChange> applied;
  applied.reserve(ordered.size());
  auto restore_pre_call_document = [&] {
    document = std::move(snapshot);
  };
  for (const auto& change : ordered) {
    if (!ApplyOneChange(document, change, direction, error, context)) {
      std::string restore_error;
      bool        inverse_ok = true;
      for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        if (!ApplyOneChange(document, *it, Opposite(direction), &restore_error, context)) {
          inverse_ok = false;
          if (error != nullptr) {
            *error += "; inverse restoration failed: " + restore_error;
          }
          break;
        }
      }
      if (!inverse_ok) {
        restore_pre_call_document();
      }
      return false;
    }
    applied.push_back(change);
    if (context.after_successful_change) {
      try {
        context.after_successful_change(applied.size());
      } catch (const std::exception& ex) {
        std::string       restore_error;
        const std::string injected = ex.what();
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
          if (!ApplyOneChange(document, *it, Opposite(direction), &restore_error, context)) {
            restore_pre_call_document();
            return SetError(error, injected + "; inverse restoration failed: " + restore_error);
          }
        }
        return SetError(error, injected);
      }
    }
  }
  if (StructuralBatch(batch) && !PipelineDocumentPassesValidation(document, error)) {
    restore_pre_call_document();
    return false;
  }
  return true;
}

auto ReplayPipelineDocumentFromRoot(const PipelineDocument&             root_document,
                                    const std::vector<EditCommit>&      first_parent_commits,
                                    std::string*                        error,
                                    const PipelineHistoryApplyContext&  context)
    -> std::optional<PipelineDocument> {
  try {
    auto document = ClonePipelineDocument(root_document);
    for (const auto& commit : first_parent_commits) {
      if (!IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
        SetError(error, "ReplayPipelineDocumentFromRoot: commit payload is not a typed batch");
        return std::nullopt;
      }
      const auto batch = PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
      if (!ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, error,
                                  context)) {
        return std::nullopt;
      }
    }
    if (!PipelineDocumentPassesValidation(document, error)) {
      return std::nullopt;
    }
    return document;
  } catch (const std::exception& ex) {
    SetError(error, ex.what());
    return std::nullopt;
  }
}

auto FirstParentCommitsForHead(const CommitGraph& graph, head_commit_hash_t head)
    -> std::vector<EditCommit> {
  std::vector<EditCommit> commits;
  for (const auto& hash : graph.FirstParentChain(head)) {
    commits.push_back(graph.GetCommit(hash));
  }
  return commits;
}

auto CollectPersistentMaskAssetKeys(const PipelineDocument& document) -> std::vector<MaskAssetKey> {
  std::set<MaskAssetKey> unique;
  for (const auto& node : document.Graph().Nodes()) {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node.get());
    if (grade == nullptr) {
      continue;
    }
    for (const auto& mask : grade->Masks()) {
      const auto* brush = std::get_if<BrushMaskSource>(&mask.source);
      if (brush == nullptr || !brush->asset_key.has_value() || brush->asset_key->Empty()) {
        continue;
      }
      unique.insert(*brush->asset_key);
    }
  }
  return {unique.begin(), unique.end()};
}

auto VerifyPersistentMaskAssets(const PipelineDocument& document, MaskStore* mask_store,
                                std::string* error) -> bool {
  const auto keys = CollectPersistentMaskAssetKeys(document);
  if (keys.empty()) {
    return true;
  }
  if (mask_store == nullptr) {
    return SetError(error, "Mask store is required to verify referenced Mask assets");
  }
  try {
    for (const auto& key : keys) {
      const auto asset = mask_store->Load(key);
      if (!asset) {
        return SetError(error, "Mask asset is missing: " + std::string{key.Value()});
      }
    }
    return true;
  } catch (const std::exception& ex) {
    return SetError(error, std::string{"Mask asset verification failed: "} + ex.what());
  }
}

}  // namespace alcedo
