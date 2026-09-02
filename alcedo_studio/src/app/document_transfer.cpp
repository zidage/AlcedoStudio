//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/document_transfer.hpp"

#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

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
    occupied->insert(grade.at("id").get<std::string>());
    for (const auto& adjustment : grade.at("adjustments")) {
      occupied->insert(adjustment.at("id").get<std::string>());
    }
    for (const auto& mask : grade.at("masks")) {
      occupied->insert(mask.at("id").get<std::string>());
    }
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

auto BrushKeysFromGradeJson(const nlohmann::json& grade) -> std::set<std::string> {
  std::set<std::string> keys;
  if (!grade.contains("masks") || !grade.at("masks").is_array()) {
    return keys;
  }
  for (const auto& mask : grade.at("masks")) {
    if (!mask.is_object() || !mask.contains("source") || !mask.at("source").is_object()) {
      continue;
    }
    const auto& source = mask.at("source");
    if (!source.contains("asset_key") || source.at("asset_key").is_null() ||
        !source.at("asset_key").is_string()) {
      continue;
    }
    const auto key = source.at("asset_key").get<std::string>();
    if (!key.empty()) {
      keys.insert(key);
    }
  }
  return keys;
}

auto DescriptorJson(const MaskAssetDescriptor& descriptor) -> nlohmann::json {
  return {{"height", descriptor.extent.height},
          {"reference_bounds",
           nlohmann::json::array({descriptor.reference_bounds.x, descriptor.reference_bounds.y,
                                  descriptor.reference_bounds.w, descriptor.reference_bounds.h})},
          {"width", descriptor.extent.width}};
}

auto DescriptorFromJson(const nlohmann::json& json) -> MaskAssetDescriptor {
  RequireObject(json, "mask asset descriptor");
  RejectUnknownKeys(json, {"height", "reference_bounds", "width"}, "mask asset descriptor");
  if (!json.contains("width") || !json.at("width").is_number_unsigned() ||
      !json.contains("height") || !json.at("height").is_number_unsigned() ||
      !json.contains("reference_bounds") || !json.at("reference_bounds").is_array() ||
      json.at("reference_bounds").size() != 4) {
    Fail("mask asset descriptor requires width, height, and four reference_bounds");
  }
  MaskAssetDescriptor descriptor;
  descriptor.extent.width             = json.at("width").get<std::uint32_t>();
  descriptor.extent.height            = json.at("height").get<std::uint32_t>();
  descriptor.reference_bounds.x       = json.at("reference_bounds").at(0).get<float>();
  descriptor.reference_bounds.y       = json.at("reference_bounds").at(1).get<float>();
  descriptor.reference_bounds.w       = json.at("reference_bounds").at(2).get<float>();
  descriptor.reference_bounds.h       = json.at("reference_bounds").at(3).get<float>();
  return descriptor;
}

auto DrtPostJson(const DrtNodeModel& drt) -> nlohmann::json {
  nlohmann::json adjustments = nlohmann::json::array();
  for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
    adjustments.push_back({{"id", std::string{drt.AdjustmentIdAt(index).Value()}},
                           {"params", drt.AdjustmentAt(index).ToJson()},
                           {"type", std::string{drt.AdjustmentAt(index).Type().Text()}}});
  }
  return {{"adjustments", std::move(adjustments)}, {"params", drt.Params().ToJson()}};
}

auto CanonicalBody(const AdjustmentTransferPackage& package) -> nlohmann::json {
  nlohmann::json grades = nlohmann::json::array();
  for (const auto& grade : package.color_grades_) {
    grades.push_back(grade);
  }
  nlohmann::json assets = nlohmann::json::array();
  for (const auto& asset : package.mask_assets_) {
    assets.push_back({{"descriptor", DescriptorJson(asset.descriptor)},
                      {"key", std::string{asset.key.Value()}}});
  }
  return {{"color_grades", std::move(grades)},
          {"document_format_version", package.document_format_version_},
          {"drt_post", package.drt_post_},
          {"mask_assets", std::move(assets)},
          {"schema", package.schema_.empty() ? std::string{kAdjustmentTransferSchema}
                                             : package.schema_}};
}

auto ComputeFingerprint(const AdjustmentTransferPackage& package) -> std::string {
  const auto dumped = CanonicalBody(package).dump();
  return Hash128::Compute(dumped.data(), dumped.size()).ToString();
}

void CopyReferencedAssets(const AdjustmentTransferPackage& package, MaskStore* source,
                          MaskStore* target) {
  if (package.mask_assets_.empty()) {
    return;
  }
  if (source == nullptr) {
    Fail("Paste requires a source Mask store for referenced Brush assets");
  }
  if (target == nullptr) {
    Fail("Paste requires a target Mask store for referenced Brush assets");
  }
  if (source == target) {
    for (const auto& asset : package.mask_assets_) {
      try {
        (void)source->Load(asset.key);
      } catch (const std::exception&) {
        Fail("Mask asset is missing: " + std::string{asset.key.Value()});
      }
    }
    return;
  }
  for (const auto& asset : package.mask_assets_) {
    const auto loaded = source->Load(asset.key);
    if (!loaded) {
      Fail("Mask asset is missing: " + std::string{asset.key.Value()});
    }
    const auto published = target->Put(loaded->descriptor, loaded->pixels);
    if (published != asset.key) {
      Fail("Mask asset copy produced a different content key");
    }
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

auto RemapGrade(nlohmann::json grade, TransferIdentitySource& identity,
                std::set<std::string>* occupied) -> nlohmann::json {
  const auto old_node = grade.at("id").get<std::string>();
  const auto new_node = identity.NextNodeId();
  RejectCollision(std::string{new_node.Value()}, *occupied, "NodeId");
  occupied->insert(std::string{new_node.Value()});
  grade["id"] = std::string{new_node.Value()};
  (void)old_node;

  if (!grade.contains("adjustments") || !grade.at("adjustments").is_array()) {
    Fail("Color Grade is missing adjustments");
  }
  for (auto& adjustment : grade.at("adjustments")) {
    const auto type = OperatorTypeId{adjustment.at("type").get<std::string>()};
    const auto new_id = identity.NextAdjustmentInstanceId(new_node, type);
    RejectCollision(std::string{new_id.Value()}, *occupied, "AdjustmentInstanceId");
    occupied->insert(std::string{new_id.Value()});
    adjustment["id"] = std::string{new_id.Value()};
  }
  if (!grade.contains("masks") || !grade.at("masks").is_array()) {
    Fail("Color Grade is missing masks");
  }
  for (auto& mask : grade.at("masks")) {
    const auto new_id = identity.NextMaskId();
    RejectCollision(std::string{new_id.Value()}, *occupied, "MaskId");
    occupied->insert(std::string{new_id.Value()});
    mask["id"] = std::string{new_id.Value()};
  }
  return grade;
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

void AppendDrtParameterChanges(const PipelineDocument& root, const nlohmann::json& drt_post,
                               std::vector<PipelineEditChange>* changes) {
  const auto* drt = root.Drt();
  if (drt == nullptr) {
    Fail("Target root is missing DRT");
  }
  nlohmann::json before_params;
  EditorParameterTarget odt_target;
  odt_target.owner_kind = EditorParameterOwnerKind::DrtPost;
  odt_target.node_id    = drt->Id();
  odt_target.field_key  = "odt";
  std::string error;
  if (!ReadEditorParameterJson(root, odt_target, &before_params, &error)) {
    Fail(error.empty() ? "Failed to read target DRT params" : error);
  }
  const auto& after_params = drt_post.at("params");
  if (before_params.dump() != after_params.dump()) {
    SetParameterChange change;
    change.target         = ToPipelineParameterTarget(odt_target);
    change.before_value   = before_params;
    change.after_value    = after_params;
    change.before_enabled = true;
    change.after_enabled  = true;
    changes->push_back(std::move(change));
  }

  std::map<std::string, nlohmann::json> incoming;
  for (const auto& item : drt_post.at("adjustments")) {
    incoming.emplace(item.at("type").get<std::string>(), item.at("params"));
  }
  for (std::size_t index = 0; index < drt->AdjustmentCount(); ++index) {
    const auto& model = drt->AdjustmentAt(index);
    const auto  type  = model.Type();
    const auto  found = incoming.find(std::string{type.Text()});
    if (found == incoming.end()) {
      Fail("Transfer DRT/Post is missing " + std::string{type.Text()});
    }
    EditorParameterTarget target;
    target.owner_kind             = EditorParameterOwnerKind::DrtPost;
    target.node_id                = drt->Id();
    target.adjustment_instance_id = drt->AdjustmentIdAt(index);
    target.field_key              = DrtFieldKeyForType(type);
    nlohmann::json before;
    if (!ReadEditorParameterJson(root, target, &before, &error)) {
      Fail(error.empty() ? "Failed to read target DRT/Post adjustment" : error);
    }
    if (before.dump() == found->second.dump()) {
      continue;
    }
    SetParameterChange change;
    change.target         = ToPipelineParameterTarget(target);
    change.before_value   = std::move(before);
    change.after_value    = found->second;
    change.before_enabled = true;
    change.after_enabled  = true;
    changes->push_back(std::move(change));
  }
}

auto BuildPasteBatch(const PipelineDocument& root, const std::vector<nlohmann::json>& remapped_grades,
                     const nlohmann::json& drt_post) -> PipelineEditBatch {
  auto working = ClonePipelineDocument(root);
  std::vector<PipelineEditChange> changes;
  std::vector<NodeId>             existing;
  for (const auto* grade : ColorGradesOnImageBackbone(working)) {
    existing.push_back(grade->Id());
  }
  if (existing.empty()) {
    Fail("Target root has no Color Grade to replace");
  }
  for (auto it = existing.rbegin(); it != existing.rend(); ++it) {
    auto change = CaptureRemoveColorGradeChange(working, *it);
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
  NodeId predecessor = develop->Id();
  const NodeId successor = drt->Id();
  for (const auto& grade : remapped_grades) {
    const NodeId new_id{grade.at("id").get<std::string>()};
    AddColorGradeChange change;
    change.node_id        = new_id;
    change.node           = grade;
    change.predecessor_id = predecessor;
    change.successor_id   = successor;
    change.incoming_edge  = PipelineSceneEdge{predecessor, ImagePort(), new_id, ImagePort()};
    change.outgoing_edge  = PipelineSceneEdge{new_id, ImagePort(), successor, ImagePort()};
    const auto errors     = InsertColorGradeFromJson(
        working, change.node, ToGraphEdge(change.incoming_edge), ToGraphEdge(change.outgoing_edge));
    if (!errors.empty()) {
      Fail(errors.front().message);
    }
    changes.emplace_back(std::move(change));
    predecessor = new_id;
  }
  AppendDrtParameterChanges(root, drt_post, &changes);
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
  if (package.color_grades_.empty()) {
    Fail("transfer package requires at least one Color Grade");
  }
  RequireObject(package.drt_post_, "drt_post");
  RejectUnknownKeys(package.drt_post_, {"adjustments", "params"}, "drt_post");
  if (!package.drt_post_.contains("params") || !package.drt_post_.at("params").is_object() ||
      !package.drt_post_.contains("adjustments") ||
      !package.drt_post_.at("adjustments").is_array()) {
    Fail("drt_post requires object params and an adjustments array");
  }
  std::set<std::string> referenced;
  std::set<std::string> listed;
  for (const auto& grade_json : package.color_grades_) {
    RequireObject(grade_json, "color grade");
    (void)ColorGradeNodeModel::FromJson(grade_json);
    for (const auto& key : BrushKeysFromGradeJson(grade_json)) {
      referenced.insert(key);
    }
  }
  for (const auto& asset : package.mask_assets_) {
    if (asset.key.Empty()) {
      Fail("mask asset key must not be empty");
    }
    if (!listed.insert(std::string{asset.key.Value()}).second) {
      Fail("duplicate mask asset key");
    }
  }
  if (referenced != listed) {
    Fail("mask_assets must list exactly the Brush keys referenced by Color Grades");
  }
}

auto CaptureDocumentTransfer(const PipelineDocument& document, MaskStore* mask_store)
    -> AdjustmentTransferPackage {
  const auto grades = ColorGradesOnImageBackbone(document);
  if (grades.empty()) {
    Fail("document has no Color Grade on the image backbone");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    Fail("document is missing DRT");
  }
  AdjustmentTransferPackage package;
  package.schema_                   = std::string{kAdjustmentTransferSchema};
  package.document_format_version_  = document.FormatVersion();
  std::set<MaskAssetKey> keys;
  for (const auto* grade : grades) {
    const auto json = grade->ToJson();
    for (const auto& key : BrushKeysFromGradeJson(json)) {
      keys.insert(MaskAssetKey{key});
    }
    package.color_grades_.push_back(json);
  }
  package.drt_post_ = DrtPostJson(*drt);
  for (const auto& key : keys) {
    if (mask_store == nullptr) {
      Fail("Mask store is required to capture referenced Brush assets");
    }
    const auto asset = mask_store->Load(key);
    if (!asset) {
      Fail("Mask asset is missing: " + std::string{key.Value()});
    }
    package.mask_assets_.push_back(DocumentTransferMaskAsset{key, asset->descriptor});
  }
  ValidateDocumentTransfer(package);
  package.fingerprint_ = ComputeFingerprint(package);
  return package;
}

auto ExportDocumentTransfer(const AdjustmentTransferPackage& package) -> nlohmann::json {
  auto json            = CanonicalBody(package);
  json["fingerprint"]  = package.fingerprint_.empty() ? ComputeFingerprint(package)
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
  RejectUnknownKeys(json,
                    {"color_grades", "document_format_version", "drt_post", "fingerprint",
                     "mask_assets", "schema"},
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
  if (!json.contains("color_grades") || !json.at("color_grades").is_array()) {
    Fail("transfer package requires a color_grades array");
  }
  for (const auto& grade : json.at("color_grades")) {
    package.color_grades_.push_back(grade);
  }
  if (!json.contains("drt_post")) {
    Fail("transfer package requires drt_post");
  }
  package.drt_post_ = json.at("drt_post");
  if (json.contains("mask_assets")) {
    if (!json.at("mask_assets").is_array()) {
      Fail("mask_assets must be an array");
    }
    for (const auto& item : json.at("mask_assets")) {
      RequireObject(item, "mask asset");
      RejectUnknownKeys(item, {"descriptor", "key"}, "mask asset");
      DocumentTransferMaskAsset asset;
      asset.key        = MaskAssetKey{item.at("key").get<std::string>()};
      asset.descriptor = DescriptorFromJson(item.at("descriptor"));
      package.mask_assets_.push_back(std::move(asset));
    }
  }
  ValidateDocumentTransfer(package);
  package.fingerprint_ = ComputeFingerprint(package);
  if (json.contains("fingerprint")) {
    if (!json.at("fingerprint").is_string() ||
        json.at("fingerprint").get<std::string>() != package.fingerprint_) {
      Fail("transfer package fingerprint does not match canonical content");
    }
  }
  const auto canonical = ExportDocumentTransfer(package);
  if (json.contains("fingerprint")) {
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
  CopyReferencedAssets(package, options.source_mask_store, options.target_mask_store);

  DefaultTransferIdentitySource owned_identity;
  TransferIdentitySource*       identity = options.identity_source;
  if (identity == nullptr && g_identity_for_testing != nullptr) {
    identity = g_identity_for_testing;
  }
  TransferIdentitySource& source =
      identity != nullptr ? *identity : owned_identity;

  auto occupied = OccupiedIdentities(root_document);
  CollectSourceIdentities(package, &occupied);

  PreparedDocumentPaste prepared;
  prepared.package = package;
  prepared.package.color_grades_.clear();
  for (const auto& grade : package.color_grades_) {
    prepared.package.color_grades_.push_back(RemapGrade(grade, source, &occupied));
  }
  prepared.package.drt_post_     = package.drt_post_;
  prepared.package.mask_assets_  = package.mask_assets_;
  prepared.package.fingerprint_  = ComputeFingerprint(prepared.package);
  prepared.batch                 = BuildPasteBatch(root_document, prepared.package.color_grades_,
                                                   prepared.package.drt_post_);
  return prepared;
}

}  // namespace alcedo
