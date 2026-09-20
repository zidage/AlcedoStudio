//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/document_transfer_planner.hpp"

#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "app/document_transfer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

TransferIdentitySource* g_identity_for_testing = nullptr;

[[noreturn]] void       Fail(std::string message) { throw std::runtime_error(std::move(message)); }

auto                    ImagePort() -> PortId { return PortId{"image"}; }

auto OccupiedIdentities(const PipelineDocument& document) -> std::set<std::string> {
  std::set<std::string> occupied;
  for (const auto& node : document.Graph().Nodes()) {
    occupied.insert(std::string{node->Id().Value()});
    if (const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node.get())) {
      for (std::size_t index = 0; index < grade->AdjustmentCount(); ++index) {
        occupied.insert(std::string{grade->AdjustmentIdAt(index).Value()});
      }
      for (const auto& mask : grade->Masks()) {
        occupied.insert(std::string{mask.id.Value()});
#ifdef ALCEDO_ENABLE_BRUSH_MASK
        if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
          for (const auto& stroke : brush->strokes) {
            occupied.insert(std::string{stroke.id.Value()});
          }
        }
#endif
      }
    }
    if (const auto* drt = dynamic_cast<const DrtNodeModel*>(node.get())) {
      for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
        occupied.insert(std::string{drt->AdjustmentIdAt(index).Value()});
      }
    }
  }
  return occupied;
}

void CollectSourceIdentities(const AdjustmentTransferPackage& package,
                             std::set<std::string>*           occupied) {
  for (const auto& grade : package.color_grades_) {
    occupied->insert(std::string{grade.source_node_id.Value()});
    for (const auto& adjustment : grade.adjustments) {
      occupied->insert(std::string{adjustment.source_id.Value()});
    }
    if (grade.masks.has_value()) {
      for (const auto& mask : *grade.masks) {
        occupied->insert(std::string{mask.id.Value()});
#ifdef ALCEDO_ENABLE_BRUSH_MASK
        if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
          for (const auto& stroke : brush->strokes) {
            occupied->insert(std::string{stroke.id.Value()});
          }
        }
#endif
      }
    }
  }
  for (const auto& adjustment : package.drt_post_.adjustments) {
    occupied->insert(std::string{adjustment.source_id.Value()});
  }
}

void RejectCollision(const std::string& id, const std::set<std::string>& occupied,
                     const char* kind) {
  if (id.empty()) {
    Fail(std::string{kind} + " identity must not be empty");
  }
  if (occupied.contains(id)) {
    Fail(std::string{"Paste identity collision: "} + kind + " '" + id + "'");
  }
}

class DefaultTransferIdentitySource final : public TransferIdentitySource {
 public:
  auto NextNodeId() -> NodeId override { return NodeId{"grade." + Token()}; }
  auto NextAdjustmentInstanceId(const NodeId& node_id, const OperatorTypeId& type)
      -> AdjustmentInstanceId override {
    auto id = MakeAdjustmentInstanceId(node_id, type);
    if (used_.insert(std::string{id.Value()}).second) {
      return id;
    }
    return AdjustmentInstanceId{std::string{id.Value()} + "." + Token()};
  }
  auto NextMaskId() -> MaskId override { return MaskId{"mask." + Token()}; }
#ifdef ALCEDO_ENABLE_BRUSH_MASK
  auto NextStrokeId() -> StrokeId override { return StrokeId{"stroke." + Token()}; }
#endif

 private:
  auto Token() -> std::string {
    const auto now =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t payload[2] = {now, seq_++};
    return Hash128::Compute(payload, sizeof(payload)).ToString().substr(0, 12);
  }

  std::uint64_t         seq_ = 1;
  std::set<std::string> used_;
};

/**
 * @brief One remapped sparse Grade entry plus its materialized node JSON.
 *
 * The materialized node starts from clean catalog defaults, then applies only
 * the selected source values, so unselected fields carry clean values.
 */
struct RemappedGradeEntry {
  TransferColorGradeValue value;
  nlohmann::json          node;
};

/**
 * @brief Materialize one selected source Grade against clean defaults.
 *
 * @p becomes_default marks the entry that establishes the target default Grade.
 * That Grade receives the required default deletion protection; every other
 * Grade keeps its transferred protection value.
 */
auto RemapGradeEntry(const TransferColorGradeValue& grade, TransferIdentitySource& identity,
                     std::set<std::string>* occupied, bool becomes_default) -> RemappedGradeEntry {
  const auto new_node = identity.NextNodeId();
  RejectCollision(std::string{new_node.Value()}, *occupied, "NodeId");
  occupied->insert(std::string{new_node.Value()});

  auto clean = ColorGradeNodeModel::MakeClean(new_node);
  clean->SetDisplayName(grade.display_name);
  clean->SetDeletionProtected(becomes_default || grade.deletion_protected);
  if (grade.enabled.has_value()) {
    clean->SetEnabled(*grade.enabled);
  }
  if (grade.mix.has_value()) {
    clean->SetMix(*grade.mix);
  }

  RemappedGradeEntry remapped;
  remapped.value                    = grade;
  remapped.value.source_node_id     = new_node;
  remapped.value.deletion_protected = clean->DeletionProtected();
  remapped.value.adjustments.clear();
  for (const auto& adjustment : grade.adjustments) {
    auto* model = clean->FindAdjustmentByType(adjustment.type);
    if (model == nullptr) {
      Fail("clean Color Grade cannot place transfer adjustment type '" +
           std::string{adjustment.type.Text()} + "'");
    }
    try {
      model->LoadJson(adjustment.params);
    } catch (const std::exception& e) {
      Fail("transfer adjustment params failed to load for '" + std::string{adjustment.type.Text()} +
           "': " + e.what());
    }
    const auto* new_id = clean->FindAdjustmentIdByType(adjustment.type);
    occupied->insert(std::string{new_id->Value()});
    remapped.value.adjustments.push_back({*new_id, adjustment.type, adjustment.params});
  }

  if (grade.masks.has_value()) {
    remapped.value.masks = std::vector<MaskModel>{};
    std::size_t index    = 0;
    for (auto mask : *grade.masks) {
      const auto new_mask = identity.NextMaskId();
      RejectCollision(std::string{new_mask.Value()}, *occupied, "MaskId");
      occupied->insert(std::string{new_mask.Value()});
      mask.id = new_mask;
#ifdef ALCEDO_ENABLE_BRUSH_MASK
      if (auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
        for (auto& stroke : brush->strokes) {
          const auto new_stroke = identity.NextStrokeId();
          RejectCollision(std::string{new_stroke.Value()}, *occupied, "StrokeId");
          occupied->insert(std::string{new_stroke.Value()});
          stroke.id = new_stroke;
        }
      }
#endif
      clean->AddMask(mask, index++);
      remapped.value.masks->push_back(std::move(mask));
    }
  }
  remapped.node = clean->ToJson();
  return remapped;
}

auto DrtFieldKeyForType(const OperatorTypeId& type) -> std::string {
  if (type == type_ids::Clarity()) {
    return "clarity";
  }
  if (type == type_ids::Sharpen()) {
    return "sharpen";
  }
  if (type == type_ids::Halation()) {
    return "halation";
  }
  if (type == type_ids::FilmGrain()) {
    return "film_grain";
  }
  Fail("Unsupported DRT/Post adjustment type: " + std::string{type.Text()});
}

void AppendDrtParameterChanges(const PipelineDocument& root, const TransferDrtPostValue& drt_post,
                               std::vector<PipelineEditChange>* changes) {
  if (drt_post.Empty()) {
    return;
  }
  const auto* drt = root.Drt();
  if (drt == nullptr) {
    Fail("Target root is missing DRT");
  }
  std::string error;
  if (drt_post.params.has_value()) {
    nlohmann::json        before_params;
    EditorParameterTarget odt_target;
    odt_target.owner_kind = EditorParameterOwnerKind::DrtPost;
    odt_target.node_id    = drt->Id();
    odt_target.field_key  = "odt";
    if (!ReadEditorParameterJson(root, odt_target, &before_params, &error)) {
      Fail(error.empty() ? "Failed to read target DRT params" : error);
    }
    if (before_params.dump() != drt_post.params->dump()) {
      SetParameterChange change;
      change.target         = ToPipelineParameterTarget(odt_target);
      change.before_value   = before_params;
      change.after_value    = *drt_post.params;
      change.before_enabled = true;
      change.after_enabled  = true;
      changes->push_back(std::move(change));
    }
  }

  std::map<std::string, const nlohmann::json*> incoming;
  for (const auto& item : drt_post.adjustments) {
    incoming.emplace(std::string{item.type.Text()}, &item.params);
  }
  for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
    const auto& model = drt->AdjustmentAt(index);
    const auto  found = incoming.find(std::string{model.Type().Text()});
    if (found == incoming.end()) {
      continue;
    }
    EditorParameterTarget target;
    target.owner_kind             = EditorParameterOwnerKind::DrtPost;
    target.node_id                = drt->Id();
    target.adjustment_instance_id = drt->AdjustmentIdAt(index);
    target.field_key              = DrtFieldKeyForType(model.Type());
    nlohmann::json before;
    if (!ReadEditorParameterJson(root, target, &before, &error)) {
      Fail(error.empty() ? "Failed to read target DRT/Post adjustment" : error);
    }
    if (before.dump() == found->second->dump()) {
      continue;
    }
    SetParameterChange change;
    change.target         = ToPipelineParameterTarget(target);
    change.before_value   = std::move(before);
    change.after_value    = *found->second;
    change.before_enabled = true;
    change.after_enabled  = true;
    changes->push_back(std::move(change));
  }
}

auto BuildPasteBatch(const PipelineDocument&            root,
                     const std::vector<nlohmann::json>& materialized_grades,
                     const TransferDrtPostValue& drt_post, const NodeId& default_grade_id)
    -> PipelineEditBatch {
  auto                            working = ClonePipelineDocument(root);
  std::vector<PipelineEditChange> changes;
  if (!materialized_grades.empty()) {
    if (default_grade_id.Empty()) {
      Fail("selective paste requires one default Grade identity");
    }
    std::vector<NodeId> existing;
    for (const auto* grade : ColorGradesOnImageBackbone(working)) {
      existing.push_back(grade->Id());
    }
    if (existing.empty()) {
      Fail("Target root has no Color Grade to replace");
    }
    for (auto it = existing.rbegin(); it != existing.rend(); ++it) {
      auto       change = CaptureRemoveColorGradeChange(working, *it);
      const auto errors = RemoveColorGradeAndBridge(working, *it);
      if (!errors.empty()) {
        Fail(errors.front().message);
      }
      changes.emplace_back(std::move(change));
    }
    if (!working.DefaultGradeId().Empty()) {
      Fail("Target root default Grade is not on the image backbone");
    }

    const auto* develop = working.Develop();
    const auto* drt     = working.Drt();
    if (develop == nullptr || drt == nullptr) {
      Fail("Target root is missing Develop or DRT");
    }
    NodeId       predecessor = develop->Id();
    const NodeId successor   = drt->Id();
    for (const auto& node : materialized_grades) {
      const NodeId        new_id{node.at("id").get<std::string>()};
      AddColorGradeChange change;
      change.node_id                   = new_id;
      change.establishes_default_grade = new_id == default_grade_id;
      change.node                      = node;
      change.predecessor_id            = predecessor;
      change.successor_id              = successor;
      change.incoming_edge = PipelineSceneEdge{predecessor, ImagePort(), new_id, ImagePort()};
      change.before_next_color_grade_name_number = working.NextColorGradeNameNumber();
      change.after_next_color_grade_name_number  = working.NextColorGradeNameNumber();
      change.outgoing_edge = PipelineSceneEdge{new_id, ImagePort(), successor, ImagePort()};
      const auto errors =
          InsertColorGradeFromJson(working, change.node, ToGraphEdge(change.incoming_edge),
                                   ToGraphEdge(change.outgoing_edge));
      if (!errors.empty()) {
        Fail(errors.front().message);
      }
      if (change.establishes_default_grade) {
        working.SetDefaultGradeId(new_id);
      }
      changes.emplace_back(std::move(change));
      predecessor = new_id;
    }
  }
  AppendDrtParameterChanges(root, drt_post, &changes);
  if (changes.empty()) {
    Fail("transfer package produces no changes on the target document");
  }
  return MakePasteBatch(std::move(changes));
}

}  // namespace

auto CountingTransferIdentitySource::NextNodeId() -> NodeId {
  return NodeId{"grade.t" + std::to_string(next_node_++)};
}

auto CountingTransferIdentitySource::NextAdjustmentInstanceId(const NodeId&         node_id,
                                                              const OperatorTypeId& type)
    -> AdjustmentInstanceId {
  return MakeAdjustmentInstanceId(node_id, type);
}

auto CountingTransferIdentitySource::NextMaskId() -> MaskId {
  return MaskId{"mask.t" + std::to_string(next_mask_++)};
}

#ifdef ALCEDO_ENABLE_BRUSH_MASK
auto CountingTransferIdentitySource::NextStrokeId() -> StrokeId {
  return StrokeId{"stroke.t" + std::to_string(next_stroke_++)};
}
#endif

void SetDocumentTransferIdentitySourceForTesting(TransferIdentitySource* source) {
  g_identity_for_testing = source;
}

auto DocumentTransferPlanner::Plan(const AdjustmentTransferPackage&    package,
                                   const PipelineDocument&             root_document,
                                   const DocumentTransferPasteOptions& options)
    -> PreparedDocumentPaste {
  ValidateDocumentTransfer(package);
  if (root_document.Develop() == nullptr || root_document.Drt() == nullptr) {
    Fail("target root must contain Develop and DRT");
  }

  DefaultTransferIdentitySource owned_identity;
  TransferIdentitySource*       identity = options.identity_source;
  if (identity == nullptr && g_identity_for_testing != nullptr) {
    identity = g_identity_for_testing;
  }
  TransferIdentitySource& source   = identity != nullptr ? *identity : owned_identity;

  auto                    occupied = OccupiedIdentities(root_document);
  CollectSourceIdentities(package, &occupied);

  PreparedDocumentPaste prepared;
  prepared.package = package;
  prepared.package.color_grades_.clear();
  prepared.package.default_grade_id_ = NodeId{};
  std::vector<nlohmann::json> materialized_grades;
  materialized_grades.reserve(package.color_grades_.size());
  for (std::size_t index = 0; index < package.color_grades_.size(); ++index) {
    const auto& grade           = package.color_grades_[index];
    // The remapped source default stays default. When the package omits the
    // source default, the first included Grade becomes the target default so
    // the pasted document keeps one valid default Grade identity.
    const bool  becomes_default = !package.default_grade_id_.Empty()
                                      ? grade.source_node_id == package.default_grade_id_
                                      : index == 0;
    auto        remapped        = RemapGradeEntry(grade, source, &occupied, becomes_default);
    materialized_grades.push_back(remapped.node);
    if (becomes_default) {
      prepared.package.default_grade_id_ = remapped.value.source_node_id;
    }
    prepared.package.color_grades_.push_back(std::move(remapped.value));
  }
  prepared.package.drt_post_    = package.drt_post_;
  prepared.package.fingerprint_ = DocumentTransferFingerprint(prepared.package);
  prepared.batch = BuildPasteBatch(root_document, materialized_grades, prepared.package.drt_post_,
                                   prepared.package.default_grade_id_);
  return prepared;
}

}  // namespace alcedo
