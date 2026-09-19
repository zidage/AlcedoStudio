//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/document_transfer.hpp"

#include <set>
#include <stdexcept>
#include <utility>

#include "app/adjustment_transfer_package_builder.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "type/hash_type.hpp"

namespace alcedo {
namespace {

[[noreturn]] void Fail(std::string message) { throw std::runtime_error(std::move(message)); }

auto              RequireObject(const nlohmann::json& json, std::string_view context) -> void {
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
  if (!json.contains(key) || !json.at(key).is_string() || json.at(key).get<std::string>().empty()) {
    Fail(std::string{context} + " requires a non-empty string field '" + key + "'");
  }
  return json.at(key).get<std::string>();
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
          {"schema",
           package.schema_.empty() ? std::string{kAdjustmentTransferSchema} : package.schema_}};
}

auto ComputeFingerprint(const AdjustmentTransferPackage& package) -> std::string {
  const auto dumped = CanonicalBody(package).dump();
  return Hash128::Compute(dumped.data(), dumped.size()).ToString();
}

void ValidateAdjustmentParams(const TransferAdjustmentValue& adjustment, std::string_view context) {
  auto model = BuiltinAdjustmentCatalog::Instance().CreateDefault(adjustment.type);
  if (model == nullptr) {
    Fail(std::string{context} + " has unknown adjustment type '" +
         std::string{adjustment.type.Text()} + "'");
  }
  try {
    model->LoadJson(adjustment.params);
  } catch (const std::exception& e) {
    Fail(std::string{context} + " has invalid params for '" + std::string{adjustment.type.Text()} +
         "': " + e.what());
  }
}

}  // namespace

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
    RequireAdjustmentOwner(adjustment.type, AdjustmentParameterOwner::DrtPost, "transfer DRT/Post");
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
  auto json = CanonicalBody(package);
  json["fingerprint"] =
      package.fingerprint_.empty() ? ComputeFingerprint(package) : package.fingerprint_;
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
                    {"color_grades", "default_grade_id", "document_format_version", "drt_post",
                     "fingerprint", "schema"},
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

}  // namespace alcedo
