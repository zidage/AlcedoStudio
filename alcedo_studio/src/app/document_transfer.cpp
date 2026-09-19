//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/document_transfer.hpp"

#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

#include "app/adjustment_transfer_package_builder.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

TransferIdentitySource* g_identity_for_testing = nullptr;

[[noreturn]] void Fail(std::string message) { throw std::runtime_error(std::move(message)); }

auto ImagePort() -> PortId { return PortId{"image"}; }

auto RequireObject(const nlohmann::json& json, std::string_view context) -> void {
  if (!json.is_object()) {
    Fail(std::string{context} + " must be a JSON object");
  }
}

void RejectUnknownKeys(const nlohmann::json& json, std::initializer_list<const char*> allowed,
                       std::string_view context) {
  for (const auto& [key, value] : json.items()) {
    bool ok = false;
    for (const char* allowed_key : allowed) {
      if (key == allowed_key) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      Fail(std::string{context} + " has unknown field '" + key + "'");
    }
    (void)value;
  }
}

auto RequireStringField(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::string {
  if (!json.contains(key) || !json.at(key).is_string() ||
      json.at(key).get<std::string>().empty()) {
    Fail(std::string{context} + " requires a non-empty string field '" + key + "'");
  }
  return json.at(key).get<std::string>();
}

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
                             std::set<std::string>* occupied) {
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

// ---------------------------------------------------------------------------
// Canonical v6 JSON
// ---------------------------------------------------------------------------

auto AdjustmentValueJson(const TransferAdjustmentValue& value) -> nlohmann::json {
  return {{"id", std::string{value.source_id.Value()}},
          {"params", value.params},
          {"type", std::string{value.type.Text()}}};
}

auto ColorGradeEntryJson(const TransferColorGradeValue& grade) -> nlohmann::json {
  nlohmann::json adjustments = nlohmann::json::array();
  for (const auto& adjustment : grade.adjustments) {
    adjustments.push_back(AdjustmentValueJson(adjustment));
  }
  nlohmann::json json{{"id", std::string{grade.source_node_id.Value()}},
                      {"adjustments", std::move(adjustments)},
                      {"deletion_protected", grade.deletion_protected},
                      {"display_name", grade.display_name}};
  if (grade.enabled.has_value()) {
    json["enabled"] = *grade.enabled;
  }
  if (grade.mix.has_value()) {
    json["mix"] = *grade.mix;
  }
  if (grade.masks.has_value()) {
    nlohmann::json masks = nlohmann::json::array();
    for (const auto& mask : *grade.masks) {
      masks.push_back(MaskModelToJson(mask));
    }
    json["masks"] = std::move(masks);
  }
  return json;
}

auto DrtPostEntryJson(const TransferDrtPostValue& drt_post) -> nlohmann::json {
  nlohmann::json adjustments = nlohmann::json::array();
  for (const auto& adjustment : drt_post.adjustments) {
    adjustments.push_back(AdjustmentValueJson(adjustment));
  }
  nlohmann::json json{{"adjustments", std::move(adjustments)}};
  if (drt_post.params.has_value()) {
    json["params"] = *drt_post.params;
  }
  return json;
}

auto AdjustmentValueFromJson(const nlohmann::json& json, std::string_view context)
    -> TransferAdjustmentValue {
  RequireObject(json, context);
  RejectUnknownKeys(json, {"id", "params", "type"}, context);
  TransferAdjustmentValue value;
  value.source_id = AdjustmentInstanceId{RequireStringField(json, "id", context)};
  value.type      = OperatorTypeId{RequireStringField(json, "type", context)};
  if (!json.contains("params") || !json.at("params").is_object()) {
    Fail(std::string{context} + " requires object params");
  }
  value.params = json.at("params");
  return value;
}

auto ColorGradeEntryFromJson(const nlohmann::json& json) -> TransferColorGradeValue {
  RequireObject(json, "color grade");
  RejectUnknownKeys(
      json, {"id", "adjustments", "deletion_protected", "display_name", "enabled", "masks", "mix"},
      "color grade");
  TransferColorGradeValue grade;
  grade.source_node_id = NodeId{RequireStringField(json, "id", "color grade")};
  grade.display_name   = RequireStringField(json, "display_name", "color grade");
  if (!json.contains("deletion_protected") || !json.at("deletion_protected").is_boolean()) {
    Fail("color grade requires boolean deletion_protected");
  }
  grade.deletion_protected = json.at("deletion_protected").get<bool>();
  if (json.contains("enabled")) {
    if (!json.at("enabled").is_boolean()) {
      Fail("color grade enabled must be boolean");
    }
    grade.enabled = json.at("enabled").get<bool>();
  }
  if (json.contains("mix")) {
    if (!json.at("mix").is_number()) {
      Fail("color grade mix must be a number");
    }
    grade.mix = json.at("mix").get<float>();
  }
  if (!json.contains("adjustments") || !json.at("adjustments").is_array()) {
    Fail("color grade requires an adjustments array");
  }
  for (const auto& item : json.at("adjustments")) {
    grade.adjustments.push_back(AdjustmentValueFromJson(item, "color grade adjustment"));
  }
  if (json.contains("masks")) {
    if (!json.at("masks").is_array()) {
      Fail("color grade masks must be an array");
    }
    grade.masks = std::vector<MaskModel>{};
    for (const auto& item : json.at("masks")) {
      grade.masks->push_back(MaskModelFromJson(item));
    }
  }
  return grade;
}

auto DrtPostEntryFromJson(const nlohmann::json& json) -> TransferDrtPostValue {
  RequireObject(json, "drt_post");
  RejectUnknownKeys(json, {"adjustments", "params"}, "drt_post");
  TransferDrtPostValue drt_post;
  if (!json.contains("adjustments") || !json.at("adjustments").is_array()) {
    Fail("drt_post requires an adjustments array");
  }
  for (const auto& item : json.at("adjustments")) {
    drt_post.adjustments.push_back(AdjustmentValueFromJson(item, "drt_post adjustment"));
  }
  if (json.contains("params")) {
    if (!json.at("params").is_object()) {
      Fail("drt_post params must be a JSON object");
    }
    drt_post.params = json.at("params");
  }
  return drt_post;
}

auto CanonicalBody(const AdjustmentTransferPackage& package) -> nlohmann::json {
  nlohmann::json grades = nlohmann::json::array();
  for (const auto& grade : package.color_grades_) {
    grades.push_back(ColorGradeEntryJson(grade));
  }
  return {{"color_grades", std::move(grades)},
          {"document_format_version", package.document_format_version_},
          {"default_grade_id", package.default_grade_id_.Empty()
                                   ? nlohmann::json(nullptr)
                                   : nlohmann::json(package.default_grade_id_.Value())},
          {"drt_post", DrtPostEntryJson(package.drt_post_)},
          {"schema", package.schema_.empty() ? std::string{kAdjustmentTransferSchema}
                                             : package.schema_}};
}

auto ComputeFingerprint(const AdjustmentTransferPackage& package) -> std::string {
  const auto dumped = CanonicalBody(package).dump();
  return Hash128::Compute(dumped.data(), dumped.size()).ToString();
}

void ValidateAdjustmentParams(const TransferAdjustmentValue& adjustment,
                              std::string_view               context) {
  auto model = BuiltinAdjustmentCatalog::Instance().CreateDefault(adjustment.type);
  if (model == nullptr) {
    Fail(std::string{context} + " has unknown adjustment type '" +
         std::string{adjustment.type.Text()} + "'");
  }
  try {
    model->LoadJson(adjustment.params);
  } catch (const std::exception& e) {
    Fail(std::string{context} + " has invalid params for '" +
         std::string{adjustment.type.Text()} + "': " + e.what());
  }
}

// ---------------------------------------------------------------------------
// Paste planning on the typed package
// ---------------------------------------------------------------------------

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
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
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

auto RemapGradeEntry(const TransferColorGradeValue& grade, TransferIdentitySource& identity,
                     std::set<std::string>* occupied) -> RemappedGradeEntry {
  const auto new_node = identity.NextNodeId();
  RejectCollision(std::string{new_node.Value()}, *occupied, "NodeId");
  occupied->insert(std::string{new_node.Value()});

  auto clean = ColorGradeNodeModel::MakeClean(new_node);
  clean->SetDisplayName(grade.display_name);
  clean->SetDeletionProtected(grade.deletion_protected);
  if (grade.enabled.has_value()) {
    clean->SetEnabled(*grade.enabled);
  }
  if (grade.mix.has_value()) {
    clean->SetMix(*grade.mix);
  }

  RemappedGradeEntry remapped;
  remapped.value                = grade;
  remapped.value.source_node_id = new_node;
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
      Fail("transfer adjustment params failed to load for '" +
           std::string{adjustment.type.Text()} + "': " + e.what());
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

auto BuildPasteBatch(const PipelineDocument&               root,
                     const std::vector<nlohmann::json>&    materialized_grades,
                     const TransferDrtPostValue&           drt_post,
                     const NodeId&                         default_grade_id) -> PipelineEditBatch {
  auto                        working = ClonePipelineDocument(root);
  std::vector<PipelineEditChange> changes;
  if (!materialized_grades.empty()) {
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

    const auto* develop = working.Develop();
    const auto* drt     = working.Drt();
    if (develop == nullptr || drt == nullptr) {
      Fail("Target root is missing Develop or DRT");
    }
    NodeId       predecessor = develop->Id();
    const NodeId successor   = drt->Id();
    for (const auto& node : materialized_grades) {
      const NodeId new_id{node.at("id").get<std::string>()};
      AddColorGradeChange change;
      change.node_id                       = new_id;
      change.establishes_default_grade     = new_id == default_grade_id;
      change.node                          = node;
      change.predecessor_id                = predecessor;
      change.successor_id                  = successor;
      change.incoming_edge                 = PipelineSceneEdge{predecessor, ImagePort(), new_id,
                                                               ImagePort()};
      change.outgoing_edge                 = PipelineSceneEdge{new_id, ImagePort(), successor,
                                                               ImagePort()};
      change.before_next_color_grade_name_number = working.NextColorGradeNameNumber();
      change.after_next_color_grade_name_number  = working.NextColorGradeNameNumber();
      const auto errors = InsertColorGradeFromJson(
          working, change.node, ToGraphEdge(change.incoming_edge),
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

void ValidateDocumentTransfer(const AdjustmentTransferPackage& package) {
  if (package.schema_ != kAdjustmentTransferSchema) {
    Fail("unsupported adjustment package schema");
  }
  if (package.document_format_version_ != kPipelineDocumentFormatVersion) {
    Fail("unsupported transfer document_format_version");
  }
  if (package.Empty()) {
    Fail("transfer package requires at least one selected item");
  }

  std::set<std::string> identities;
  const auto            claim = [&identities](const std::string& id, const char* kind) {
    if (id.empty()) {
      Fail(std::string{kind} + " identity must not be empty");
    }
    if (!identities.insert(id).second) {
      Fail(std::string{"duplicate "} + kind + " in transfer package: '" + id + "'");
    }
  };

  std::size_t default_matches = 0;
  for (const auto& grade : package.color_grades_) {
    claim(std::string{grade.source_node_id.Value()}, "NodeId");
    if (grade.display_name.empty()) {
      Fail("transfer Color Grade display_name must be non-empty");
    }
    if (grade.mix.has_value() && (*grade.mix < 0.0f || *grade.mix > 1.0f)) {
      Fail("transfer Color Grade mix is out of range");
    }
    std::set<std::string> grade_types;
    for (const auto& adjustment : grade.adjustments) {
      claim(std::string{adjustment.source_id.Value()}, "AdjustmentInstanceId");
      RequireAdjustmentOwner(adjustment.type, AdjustmentParameterOwner::ColorGrade,
                             "transfer Color Grade");
      if (!grade_types.insert(std::string{adjustment.type.Text()}).second) {
        Fail("transfer Color Grade has duplicate adjustment type '" +
             std::string{adjustment.type.Text()} + "'");
      }
      ValidateAdjustmentParams(adjustment, "transfer Color Grade");
    }
    if (grade.masks.has_value()) {
      if (HasDuplicateOrEmptyMaskId(*grade.masks)) {
        Fail("transfer Color Grade has an empty or duplicate MaskId");
      }
      for (const auto& mask : *grade.masks) {
        ValidateMaskModel(mask);
        claim(std::string{mask.id.Value()}, "MaskId");
#ifdef ALCEDO_ENABLE_BRUSH_MASK
        if (const auto* brush = std::get_if<BrushMaskSource>(&mask.source)) {
          for (const auto& stroke : brush->strokes) {
            claim(std::string{stroke.id.Value()}, "StrokeId");
          }
        }
#endif
      }
    }
    if (grade.source_node_id == package.default_grade_id_) {
      ++default_matches;
    }
  }
  if (!package.default_grade_id_.Empty() && default_matches != 1) {
    Fail("transfer default_grade_id must identify exactly one Color Grade");
  }

  std::set<std::string> drt_types;
  for (const auto& adjustment : package.drt_post_.adjustments) {
    claim(std::string{adjustment.source_id.Value()}, "AdjustmentInstanceId");
    RequireAdjustmentOwner(adjustment.type, AdjustmentParameterOwner::DrtPost,
                           "transfer DRT/Post");
    if (!drt_types.insert(std::string{adjustment.type.Text()}).second) {
      Fail("transfer DRT/Post has duplicate adjustment type '" +
           std::string{adjustment.type.Text()} + "'");
    }
    ValidateAdjustmentParams(adjustment, "transfer DRT/Post");
  }
  if (package.drt_post_.params.has_value()) {
    DrtParamsModel params_model;
    try {
      params_model.LoadJson(*package.drt_post_.params);
    } catch (const std::exception& e) {
      Fail(std::string{"transfer DRT params failed to load: "} + e.what());
    }
  }
}

auto CaptureDocumentTransfer(const PipelineDocument& document) -> AdjustmentTransferPackage {
  return AdjustmentTransferPackageBuilder::Build(document, SelectAllTransferableItems(document));
}

auto ExportDocumentTransfer(const AdjustmentTransferPackage& package) -> nlohmann::json {
  auto json           = CanonicalBody(package);
  json["fingerprint"] = package.fingerprint_.empty() ? ComputeFingerprint(package)
                                                     : package.fingerprint_;
  return json;
}

auto DocumentTransferFingerprint(const AdjustmentTransferPackage& package) -> std::string {
  return ComputeFingerprint(package);
}

auto ImportDocumentTransfer(const nlohmann::json& json) -> AdjustmentTransferPackage {
  RequireObject(json, "transfer package");
  if (json.contains("operators")) {
    Fail("operator-list transfer packages are not accepted");
  }
  RejectUnknownKeys(
      json,
      {"color_grades", "default_grade_id", "document_format_version", "drt_post", "fingerprint",
       "schema"},
      "transfer package");
  if (!json.contains("schema") || !json.at("schema").is_string() ||
      json.at("schema").get<std::string>() != kAdjustmentTransferSchema) {
    Fail("unsupported adjustment package schema");
  }
  AdjustmentTransferPackage package;
  package.schema_ = json.at("schema").get<std::string>();
  if (!json.contains("document_format_version") ||
      !json.at("document_format_version").is_number_unsigned()) {
    Fail("transfer package requires document_format_version");
  }
  package.document_format_version_ = json.at("document_format_version").get<std::uint32_t>();
  if (!json.contains("default_grade_id") ||
      (!json.at("default_grade_id").is_null() &&
       (!json.at("default_grade_id").is_string() ||
        json.at("default_grade_id").get<std::string>().empty()))) {
    Fail("transfer package requires null or nonempty default_grade_id");
  }
  package.default_grade_id_ = json.at("default_grade_id").is_null()
                                  ? NodeId{}
                                  : NodeId{json.at("default_grade_id").get<std::string>()};
  if (!json.contains("color_grades") || !json.at("color_grades").is_array()) {
    Fail("transfer package requires a color_grades array");
  }
  for (const auto& grade : json.at("color_grades")) {
    package.color_grades_.push_back(ColorGradeEntryFromJson(grade));
  }
  if (!json.contains("drt_post")) {
    Fail("transfer package requires drt_post");
  }
  package.drt_post_ = DrtPostEntryFromJson(json.at("drt_post"));
  ValidateDocumentTransfer(package);
  package.fingerprint_ = ComputeFingerprint(package);
  if (json.contains("fingerprint")) {
    if (!json.at("fingerprint").is_string() ||
        json.at("fingerprint").get<std::string>() != package.fingerprint_) {
      Fail("transfer package fingerprint does not match canonical content");
    }
    const auto canonical = ExportDocumentTransfer(package);
    if (json.dump() != canonical.dump()) {
      Fail("transfer package JSON is not canonical");
    }
  }
  return package;
}

auto PrepareDocumentPaste(const AdjustmentTransferPackage&    package,
                          const PipelineDocument&             root_document,
                          const DocumentTransferPasteOptions& options) -> PreparedDocumentPaste {
  ValidateDocumentTransfer(package);
  if (root_document.Develop() == nullptr || root_document.Drt() == nullptr) {
    Fail("target root must contain Develop and DRT");
  }

  DefaultTransferIdentitySource owned_identity;
  TransferIdentitySource*       identity = options.identity_source;
  if (identity == nullptr && g_identity_for_testing != nullptr) {
    identity = g_identity_for_testing;
  }
  TransferIdentitySource& source = identity != nullptr ? *identity : owned_identity;

  auto occupied = OccupiedIdentities(root_document);
  CollectSourceIdentities(package, &occupied);

  PreparedDocumentPaste prepared;
  prepared.package = package;
  prepared.package.color_grades_.clear();
  prepared.package.default_grade_id_ = NodeId{};
  std::vector<nlohmann::json> materialized_grades;
  materialized_grades.reserve(package.color_grades_.size());
  for (const auto& grade : package.color_grades_) {
    auto remapped = RemapGradeEntry(grade, source, &occupied);
    materialized_grades.push_back(remapped.node);
    if (grade.source_node_id == package.default_grade_id_) {
      prepared.package.default_grade_id_ = remapped.value.source_node_id;
    }
    prepared.package.color_grades_.push_back(std::move(remapped.value));
  }
  prepared.package.drt_post_    = package.drt_post_;
  prepared.package.fingerprint_ = ComputeFingerprint(prepared.package);
  prepared.batch                = BuildPasteBatch(root_document, materialized_grades,
                                                  prepared.package.drt_post_,
                                                  prepared.package.default_grade_id_);
  return prepared;
}

}  // namespace alcedo
