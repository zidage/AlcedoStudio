//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/document_transfer_planner.hpp"

#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app/document_transfer.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
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
 * The materialized node starts from the values of its target base Grade (see
 * @ref RemapGradeEntry), then applies only the selected source values, so
 * unselected fields keep the base values.
 */
struct RemappedGradeEntry {
  TransferColorGradeValue value;
  nlohmann::json          node;
};

/// The default Color Grade of @p root on its image backbone, or null.
auto RootDefaultGrade(const PipelineDocument& root) -> const ColorGradeNodeModel* {
  for (const auto* grade : ColorGradesOnImageBackbone(root)) {
    if (grade->Id() == root.DefaultGradeId()) {
      return grade;
    }
  }
  return nullptr;
}

/**
 * @brief Materialize one selected source Grade.
 *
 * Paste replaces the Color Grades of the target root. The entry that becomes the
 * target default Grade replaces the root default Grade, so it starts from the
 * enabled, mix, and adjustment values of @p base (that root Grade, for example
 * the product Default look of an imported photo). Every other entry has no
 * target counterpart and starts from clean catalog values, like a Grade added in
 * the editor. The selected source values then overwrite the start values; an
 * unselected item keeps them.
 *
 * @p becomes_default marks the entry that establishes the target default Grade.
 * That Grade receives the required default deletion protection; every other
 * Grade keeps its transferred protection value.
 */
auto RemapGradeEntry(const TransferColorGradeValue& grade, TransferIdentitySource& identity,
                     std::set<std::string>* occupied, bool becomes_default,
                     const ColorGradeNodeModel* base) -> RemappedGradeEntry {
  const auto new_node = identity.NextNodeId();
  RejectCollision(std::string{new_node.Value()}, *occupied, "NodeId");
  occupied->insert(std::string{new_node.Value()});

  auto clean = ColorGradeNodeModel::MakeClean(new_node);
  clean->SetDisplayName(grade.display_name);
  clean->SetDeletionProtected(becomes_default || grade.deletion_protected);
  if (becomes_default && base != nullptr) {
    clean->SetEnabled(base->Enabled());
    clean->SetMix(base->Mix());
    for (std::size_t index = 0; index < base->AdjustmentCount(); ++index) {
      const auto& base_adjustment = base->AdjustmentAt(index);
      if (auto* model = clean->FindAdjustmentByType(base_adjustment.Type())) {
        model->LoadJson(base_adjustment.ToJson());
      }
    }
  }
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
    // The package holds the stored DRT params: the output transform, the diffusion strength, and
    // the film format. History records them as three editor fields, "odt" (output transform
    // only), "diffusion" ({"strength"}), and "film_format" ({"film_format"}), so each field gets
    // its own change. A package made before the film format reads 35mm.
    DrtParamsModel package_params;
    package_params.LoadJson(*drt_post.params);
    const std::pair<std::string, nlohmann::json> fields[] = {
        {"odt", package_params.OutputTransformJson()},
        {"diffusion", nlohmann::json{{"strength", package_params.DiffusionStrength()}}},
        {"film_format",
         nlohmann::json{{"film_format", std::string{FilmFormatId(package_params.FilmFormat())}}}},
    };
    for (const auto& [field_key, after_value] : fields) {
      EditorParameterTarget target;
      target.owner_kind = EditorParameterOwnerKind::DrtPost;
      target.node_id    = drt->Id();
      target.field_key  = field_key;
      nlohmann::json before_value;
      if (!ReadEditorParameterJson(root, target, &before_value, &error)) {
        Fail(error.empty() ? "Failed to read target DRT " + field_key : error);
      }
      if (before_value.dump() == after_value.dump()) {
        continue;
      }
      SetParameterChange change;
      change.target         = ToPipelineParameterTarget(target);
      change.before_value   = std::move(before_value);
      change.after_value    = after_value;
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

/// One SetParameter change for @p target when the selected value differs from
/// the target root. Develop fields compare and store only owned keys, so the
/// target camera profile and as-shot white balance are never rewritten.
void AppendParameterChange(const PipelineDocument& root, const EditorParameterTarget& target,
                           const nlohmann::json& selected_value,
                           std::vector<PipelineEditChange>* changes) {
  nlohmann::json before;
  std::string    error;
  if (!ReadEditorParameterJson(root, target, &before, &error)) {
    Fail(error.empty() ? "Failed to read target " + target.field_key : error);
  }
  auto before_owned = FieldOwnedParameterJson(target.field_key, before);
  auto after_owned  = FieldOwnedParameterJson(target.field_key, selected_value);
  if (before_owned.dump() == after_owned.dump()) {
    return;
  }
  SetParameterChange change;
  change.target         = ToPipelineParameterTarget(target);
  change.before_value   = std::move(before_owned);
  change.after_value    = std::move(after_owned);
  change.before_enabled = true;
  change.after_enabled  = true;
  changes->push_back(std::move(change));
}

void AppendDevelopParameterChanges(const PipelineDocument& root, const TransferDevelopValue& develop,
                                   std::vector<PipelineEditChange>* changes) {
  if (develop.Empty()) {
    return;
  }
  const auto* node = root.Develop();
  if (node == nullptr) {
    Fail("Target root is missing Develop");
  }
  const std::pair<const char*, const std::optional<nlohmann::json>*> fields[] = {
      {"raw_decode", &develop.raw_decode},
      {"color_temp", &develop.color_temp},
      {"lens_calib", &develop.lens_calib},
  };
  for (const auto& [field_key, value] : fields) {
    if (!value->has_value()) {
      continue;
    }
    EditorParameterTarget target;
    target.owner_kind = EditorParameterOwnerKind::Develop;
    target.node_id    = node->Id();
    target.field_key  = field_key;
    AppendParameterChange(root, target, **value, changes);
  }
  if (develop.geometry.has_value()) {
    EditorParameterTarget target;
    target.owner_kind = EditorParameterOwnerKind::Document;
    target.field_key  = "crop_rotate";
    AppendParameterChange(root, target, *develop.geometry, changes);
  }
}

auto BuildPasteBatch(const PipelineDocument&            root,
                     const std::vector<nlohmann::json>& materialized_grades,
                     const TransferDevelopValue&        develop,
                     const TransferDrtPostValue& drt_post, const NodeId& default_grade_id)
    -> PipelineEditBatch {
  auto                            working = ClonePipelineDocument(root);
  std::vector<PipelineEditChange> changes;
  AppendDevelopParameterChanges(root, develop, &changes);
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

auto DocumentTransferPlanner::PlanOntoCurrent(const AdjustmentTransferPackage&    package,
                                              const PipelineDocument&             document,
                                              const DocumentTransferPasteOptions& options)
    -> std::optional<PipelineEditBatch> {
  ValidateDocumentTransfer(package);
  if (document.Develop() == nullptr || document.Drt() == nullptr) {
    Fail("Paste target must contain Develop and DRT");
  }
  DefaultTransferIdentitySource owned_identity;
  auto* identity = options.identity_source ? options.identity_source : g_identity_for_testing;
  auto& source   = identity ? *identity : owned_identity;
  auto  occupied = OccupiedIdentities(document);
  CollectSourceIdentities(package, &occupied);
  const auto                      grades = ColorGradesOnImageBackbone(document);
  std::set<NodeId>                matched;
  std::vector<PipelineEditChange> changes;
  AppendDevelopParameterChanges(document, package.develop_, &changes);
  NodeId predecessor = grades.empty() ? document.Develop()->Id() : grades.back()->Id();

  for (const auto& incoming : package.color_grades_) {
    const ColorGradeNodeModel* target = nullptr;
    const bool source_default         = incoming.source_node_id == package.default_grade_id_;
    for (const auto* grade : grades) {
      if (!matched.contains(grade->Id()) &&
          (source_default ? grade->Id() == document.DefaultGradeId()
                          : grade->Id() != document.DefaultGradeId() &&
                                grade->Id() == incoming.source_node_id)) {
        target = grade;
        break;
      }
    }
    if (!target && !source_default) {
      for (const auto* grade : grades) {
        if (grade->Id() != document.DefaultGradeId() && !matched.contains(grade->Id()) &&
            grade->DisplayName() == incoming.display_name) {
          target = grade;
          break;
        }
      }
    }
    if (!target) {
      if (source_default) Fail("Paste target has no default Color Grade");
      auto                remapped = RemapGradeEntry(incoming, source, &occupied, false, nullptr);
      const auto          id       = remapped.value.source_node_id;
      AddColorGradeChange change;
      change.node_id        = id;
      change.node           = std::move(remapped.node);
      change.predecessor_id = predecessor;
      change.successor_id   = document.Drt()->Id();
      change.incoming_edge  = {predecessor, ImagePort(), id, ImagePort()};
      change.outgoing_edge  = {id, ImagePort(), document.Drt()->Id(), ImagePort()};
      change.before_next_color_grade_name_number = document.NextColorGradeNameNumber();
      change.after_next_color_grade_name_number  = document.NextColorGradeNameNumber();
      changes.emplace_back(std::move(change));
      predecessor = id;
      continue;
    }
    matched.insert(target->Id());
    if (incoming.enabled && *incoming.enabled != target->Enabled()) {
      changes.emplace_back(SetNodeEnabledChange{target->Id(), PipelineEditNodeKind::ColorGrade,
                                                target->Enabled(), *incoming.enabled});
    }
    if (incoming.mix && *incoming.mix != target->Mix()) {
      changes.emplace_back(SetNodeMixChange{target->Id(), target->Mix(), *incoming.mix});
    }
    for (const auto& adjustment : incoming.adjustments) {
      const auto* id = target->FindAdjustmentIdByType(adjustment.type);
      if (!id) Fail("Paste target is missing adjustment " + std::string{adjustment.type.Text()});
      EditorParameterTarget parameter;
      parameter.owner_kind             = EditorParameterOwnerKind::ColorGrade;
      parameter.node_id                = target->Id();
      parameter.adjustment_instance_id = *id;
      parameter.field_key = adjustment.type == type_ids::Cat02WhiteBalance() ? "grade_white_balance"
                            : adjustment.type == type_ids::Lmt()
                                ? "lut"
                                : AdjustmentInstanceSuffix(adjustment.type);
      AppendParameterChange(document, parameter, adjustment.params, &changes);
    }
    if (incoming.masks) {
      std::uint32_t retained = 0;
      for (std::size_t index = target->Masks().size(); index > 0; --index) {
        const auto& mask = target->MaskAt(index - 1);
        if (GetMaskSourceKind(mask.source) == MaskSourceKind::AiSegmentation) {
          ++retained;
        } else {
          changes.emplace_back(RemoveMaskChange{target->Id(), mask.id, MaskModelToJson(mask),
                                                static_cast<std::uint32_t>(index - 1)});
        }
      }
      // Transferred Masks need independent identities, even when source and target are one image.
      for (auto mask : *incoming.masks) {
        mask.id = source.NextMaskId();
        RejectCollision(std::string{mask.id.Value()}, occupied, "MaskId");
        occupied.insert(std::string{mask.id.Value()});
#ifdef ALCEDO_ENABLE_BRUSH_MASK
        if (auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
          for (auto& stroke : brush->strokes) {
            stroke.id = source.NextStrokeId();
            RejectCollision(std::string{stroke.id.Value()}, occupied, "StrokeId");
            occupied.insert(std::string{stroke.id.Value()});
          }
        }
#endif
        changes.emplace_back(
            AddMaskChange{target->Id(), mask.id, MaskModelToJson(mask), retained++});
      }
    }
  }
  AppendDrtParameterChanges(document, package.drt_post_, &changes);
  if (changes.empty()) return std::nullopt;
  return MakePasteBatch(std::move(changes));
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
    auto        remapped =
        RemapGradeEntry(grade, source, &occupied, becomes_default, RootDefaultGrade(root_document));
    materialized_grades.push_back(remapped.node);
    if (becomes_default) {
      prepared.package.default_grade_id_ = remapped.value.source_node_id;
    }
    prepared.package.color_grades_.push_back(std::move(remapped.value));
  }
  prepared.package.develop_     = package.develop_;
  prepared.package.drt_post_    = package.drt_post_;
  prepared.package.fingerprint_ = DocumentTransferFingerprint(prepared.package);
  prepared.batch                = BuildPasteBatch(root_document, materialized_grades,
                                                  prepared.package.develop_, prepared.package.drt_post_,
                                                  prepared.package.default_grade_id_);
  return prepared;
}

}  // namespace alcedo
