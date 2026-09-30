//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/adjustment_transfer_catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "app/pipeline_history_applier.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

constexpr std::string_view kDrtPostRowDisplayName = "DRT and Post Processing";
constexpr std::string_view kDevelopRowDisplayName = "RAW and Geometry";

auto Fail(std::string* error, std::string message) -> std::nullopt_t {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return std::nullopt;
}

/// Product item label. The UI vocabulary names the catalog types the way the
/// Adjustment Stack and the Copy dialog do; unknown types fall back to the
/// registered catalog name.
auto AdjustmentDisplayName(const OperatorTypeId& type) -> std::string {
  if (type == type_ids::Cat02WhiteBalance()) return "Grade White Balance";
  if (type == type_ids::White()) return "Whites";
  if (type == type_ids::Black()) return "Blacks";
  if (type == type_ids::Curve()) return "Tone Curve";
  if (type == type_ids::ColorWheel()) return "Color Wheels";
  if (type == type_ids::Lmt()) return "Look LUT";
  const auto* definition = BuiltinAdjustmentCatalog::Instance().Find(type);
  if (definition != nullptr) {
    return std::string{definition->display_name};
  }
  return std::string{type.Text()};
}

auto ColorGradeSectionFor(const OperatorTypeId& type) -> AdjustmentTransferItemSection {
  if (type == type_ids::Hls() || type == type_ids::Saturation() ||
      type == type_ids::Vibrance() || type == type_ids::ColorWheel()) {
    return AdjustmentTransferItemSection::Look;
  }
  if (type == type_ids::Lmt()) {
    return AdjustmentTransferItemSection::Lut;
  }
  return AdjustmentTransferItemSection::Tone;
}

auto FixedNumber(double value, int precision = 2) -> std::string {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

/// Fixed-2 text of the single numeric member in @p params, or nullopt when the
/// object does not hold exactly one numeric member.
auto SingleNumberText(const nlohmann::json& params) -> std::optional<std::string> {
  if (!params.is_object() || params.size() != 1 || !params.begin()->is_number()) {
    return std::nullopt;
  }
  return FixedNumber(params.begin()->get<double>());
}

auto BoolText(bool value) -> std::string { return value ? "On" : "Off"; }

auto PathTail(const std::string& path) -> std::string {
  if (path.empty()) {
    return "None";
  }
  return std::filesystem::path(path).filename().string();
}

/// Compact value text for one adjustment Model. Focused per type where a plain
/// scalar does not describe the value; a single-member numeric payload uses the
/// scalar itself; anything else reports Default/Adjusted.
auto AdjustmentDisplayValue(const IOperatorModel& model) -> std::string {
  const auto params = model.ToJson();
  const auto type   = model.Type();
  if (type == type_ids::Cat02WhiteBalance()) {
    if (!params.value("enabled", true)) {
      return BoolText(false);
    }
    return FixedNumber(params.value("temperature", 0.0), 0) + " K, tint " +
           FixedNumber(params.value("tint", 0.0), 0);
  }
  if (type == type_ids::Curve()) {
    if (params.contains("points") && params["points"].is_array()) {
      return std::to_string(params["points"].size()) + " points";
    }
    return "Default";
  }
  if (type == type_ids::Hls() || type == type_ids::ColorWheel()) {
    return model.IsDefault() ? "Default" : "Adjusted";
  }
  if (type == type_ids::Lmt()) {
    const auto* lmt = dynamic_cast<const LmtModel*>(&model);
    if (lmt == nullptr || IsEmptyLutReference(lmt->Reference())) {
      return "None";
    }
    std::string text = lmt->DisplayName().empty() ? PathTail(lmt->CubePath()) : lmt->DisplayName();
    if (text.empty() || text == "None") {
      text = DescribeLutReference(lmt->Reference());
    }
    if (lmt->Strength() != kDefaultLutStrength) {
      text += ", " + FixedNumber(static_cast<double>(lmt->Strength()) * 100.0, 0) + "%";
    }
    return text;
  }
  if (type == type_ids::Sharpen()) {
    return FixedNumber(params.value("amount", 0.0));
  }
  if (const auto scalar = SingleNumberText(params); scalar.has_value()) {
    return *scalar;
  }
  return model.IsDefault() ? "Default" : "Adjusted";
}

auto JsonString(const nlohmann::json& json, const char* key) -> std::string {
  if (json.contains(key) && json.at(key).is_string()) {
    return json.at(key).get<std::string>();
  }
  return {};
}

auto JsonNumber(const nlohmann::json& json, const char* key) -> double {
  if (json.contains(key) && json.at(key).is_number()) {
    return json.at(key).get<double>();
  }
  return 0.0;
}

auto JsonBool(const nlohmann::json& json, const char* key) -> bool {
  return json.contains(key) && json.at(key).is_boolean() && json.at(key).get<bool>();
}

auto DevelopItemDescriptor(AdjustmentTransferItemKind kind, AdjustmentTransferItemSection section,
                           std::string display_name, std::string display_value,
                           std::uint32_t order) -> AdjustmentTransferItemDescriptor {
  AdjustmentTransferItemDescriptor item;
  item.kind          = kind;
  item.section       = section;
  item.display_name  = std::move(display_name);
  item.display_value = std::move(display_value);
  item.source_order  = order;
  return item;
}

/// RAW Decode, White Balance, Lens Correction, and Geometry rows of the Develop
/// endpoint. Values describe the user-set fields only; the camera profile and
/// as-shot white balance are image data and are never transferred.
auto BuildDevelopItems(const PipelineDocument& document, const DevelopNodeModel& develop)
    -> std::vector<AdjustmentTransferItemDescriptor> {
  const auto params = develop.Params().ToJson();
  std::vector<AdjustmentTransferItemDescriptor> items;
  items.reserve(4);

  auto demosaic = JsonString(params, "demosaic_method");
  if (demosaic.empty()) {
    demosaic = "default";
  }
  items.push_back(DevelopItemDescriptor(
      AdjustmentTransferItemKind::RawDecode, AdjustmentTransferItemSection::Raw, "RAW Decode",
      demosaic + ", highlights " + BoolText(JsonBool(params, "highlights_reconstruct")),
      static_cast<std::uint32_t>(items.size())));

  const auto wb_mode = JsonString(params, "wb_mode");
  items.push_back(DevelopItemDescriptor(
      AdjustmentTransferItemKind::WhiteBalance, AdjustmentTransferItemSection::Raw,
      "White Balance",
      wb_mode == "custom" ? FixedNumber(JsonNumber(params, "custom_cct"), 0) + " K, tint " +
                                FixedNumber(JsonNumber(params, "custom_tint"), 0)
                          : std::string{"As Shot"},
      static_cast<std::uint32_t>(items.size())));

  std::string lens_value = BoolText(JsonBool(params, "lens_enabled"));
  if (JsonBool(params, "lens_enabled")) {
    const auto model = JsonString(params, "lens_model");
    lens_value       = model.empty() ? std::string{"Auto"} : model;
  }
  items.push_back(DevelopItemDescriptor(AdjustmentTransferItemKind::LensCalibration,
                                        AdjustmentTransferItemSection::Raw, "Lens Correction",
                                        std::move(lens_value),
                                        static_cast<std::uint32_t>(items.size())));

  const auto& geometry = document.Geometry();
  const auto  crop     = geometry.CropRect();
  const bool  cropped  = crop.x != 0.0f || crop.y != 0.0f || crop.w != 1.0f || crop.h != 1.0f;
  std::string geometry_value =
      FixedNumber(static_cast<double>(geometry.RotationDegrees()), 1) + " deg";
  if (cropped) {
    geometry_value += ", cropped";
  }
  items.push_back(DevelopItemDescriptor(AdjustmentTransferItemKind::Geometry,
                                        AdjustmentTransferItemSection::Geometry, "Crop and Rotate",
                                        std::move(geometry_value),
                                        static_cast<std::uint32_t>(items.size())));
  return items;
}

auto DrtMethodText(DrtMethod method) -> std::string {
  return method == DrtMethod::Aces20 ? "ACES 2.0" : "OpenDRT";
}

auto AdjustmentItemDescriptor(const AdjustmentInstanceId& instance_id,
                              const IOperatorModel& model,
                              AdjustmentTransferItemSection section,
                              std::uint32_t order) -> AdjustmentTransferItemDescriptor {
  AdjustmentTransferItemDescriptor item;
  item.kind          = AdjustmentTransferItemKind::Adjustment;
  item.section       = section;
  item.adjustment_id = instance_id;
  item.type          = model.Type();
  item.display_name  = AdjustmentDisplayName(item.type);
  item.display_value = AdjustmentDisplayValue(model);
  item.source_order  = order;
  return item;
}

/// Enabled, Mix, owned adjustments in document order, then one all-or-none
/// Masks row. Fails closed when a stored adjustment is not Color Grade-owned.
auto BuildColorGradeItems(const ColorGradeNodeModel& grade, std::string* error)
    -> std::optional<std::vector<AdjustmentTransferItemDescriptor>> {
  std::vector<AdjustmentTransferItemDescriptor> items;
  items.reserve(grade.AdjustmentCount() + 3);

  AdjustmentTransferItemDescriptor enabled;
  enabled.kind          = AdjustmentTransferItemKind::NodeEnabled;
  enabled.display_name  = "Enabled";
  enabled.display_value = BoolText(grade.Enabled());
  enabled.source_order  = 0;
  items.push_back(std::move(enabled));

  AdjustmentTransferItemDescriptor mix;
  mix.kind          = AdjustmentTransferItemKind::NodeMix;
  mix.display_name  = "Mix";
  mix.display_value = FixedNumber(grade.Mix());
  mix.source_order  = 1;
  items.push_back(std::move(mix));

  for (std::size_t index = 0; index < grade.AdjustmentCount(); ++index) {
    const auto& model = grade.AdjustmentAt(index);
    if (OwnerOfAdjustment(model.Type()) != AdjustmentParameterOwner::ColorGrade) {
      return Fail(error, "AdjustmentTransferCatalog: Color Grade adjustment " +
                             std::string{grade.AdjustmentIdAt(index).Value()} +
                             " is not Color Grade-owned");
    }
    items.push_back(AdjustmentItemDescriptor(grade.AdjustmentIdAt(index), model,
                                             ColorGradeSectionFor(model.Type()),
                                             static_cast<std::uint32_t>(items.size())));
  }

  AdjustmentTransferItemDescriptor masks;
  masks.kind         = AdjustmentTransferItemKind::Masks;
  masks.section      = AdjustmentTransferItemSection::Masks;
  masks.display_name = "Masks";
  masks.enabled      = grade.MaskCount() > 0;
  if (masks.enabled) {
    masks.display_value = std::to_string(grade.MaskCount());
  }
  masks.source_order = static_cast<std::uint32_t>(items.size());
  items.push_back(std::move(masks));
  return items;
}

/// Display Transform plus the DRT node's owned adjustments in document order.
/// The endpoint must match the current ownership model exactly.
auto BuildDrtPostItems(const DrtNodeModel& drt, std::string* error)
    -> std::optional<std::vector<AdjustmentTransferItemDescriptor>> {
  std::vector<OperatorTypeId> present;
  present.reserve(drt.AdjustmentCount());
  for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
    present.push_back(drt.AdjustmentAt(index).Type());
  }
  try {
    RequireCompleteDrtPostTypes(present, "AdjustmentTransferCatalog DRT/Post");
  } catch (const std::exception& ex) {
    return Fail(error, ex.what());
  }

  std::vector<AdjustmentTransferItemDescriptor> items;
  items.reserve(drt.AdjustmentCount() + 1);

  AdjustmentTransferItemDescriptor params;
  params.kind          = AdjustmentTransferItemKind::DrtParameters;
  params.section       = AdjustmentTransferItemSection::DisplayTransform;
  params.display_name  = "Display Transform";
  params.display_value = DrtMethodText(drt.Params().Method());
  params.source_order  = 0;
  items.push_back(std::move(params));

  for (std::size_t index = 0; index < drt.AdjustmentCount(); ++index) {
    items.push_back(AdjustmentItemDescriptor(drt.AdjustmentIdAt(index),
                                             drt.AdjustmentAt(index),
                                             AdjustmentTransferItemSection::Look,
                                             static_cast<std::uint32_t>(items.size())));
  }
  return items;
}

/// Version-column order shared by ListVersions and ReadVersion: creation time,
/// then Version id. Deterministic despite unordered_map storage.
auto SortedVersionRefs(const CommitGraph& graph) -> std::vector<const VersionRef*> {
  std::vector<const VersionRef*> refs;
  refs.reserve(graph.GetAllVersionRefs().size());
  for (const auto& [version_id, ref] : graph.GetAllVersionRefs()) {
    (void)version_id;
    refs.push_back(&ref);
  }
  std::ranges::sort(refs, [](const VersionRef* left, const VersionRef* right) {
    if (left->created_at != right->created_at) {
      return left->created_at < right->created_at;
    }
    return left->version_id.ToString() < right->version_id.ToString();
  });
  return refs;
}

auto VersionDescriptor(const VersionRef& ref, const CommitGraph& graph,
                       std::uint32_t order) -> AdjustmentTransferVersionDescriptor {
  AdjustmentTransferVersionDescriptor version;
  version.version_id   = ref.version_id;
  version.display_name = ref.display_name;
  version.created_at   = ref.created_at;
  version.updated_at   = ref.updated_at;
  version.active       = ref.version_id == graph.GetActiveVersionId();
  version.source_order = order;
  return version;
}

}  // namespace

auto AdjustmentTransferCatalogService::ListVersions(const CommitGraph& graph)
    -> std::vector<AdjustmentTransferVersionDescriptor> {
  const auto                                   refs = SortedVersionRefs(graph);
  std::vector<AdjustmentTransferVersionDescriptor> versions;
  versions.reserve(refs.size());
  for (const auto* ref : refs) {
    versions.push_back(
        VersionDescriptor(*ref, graph, static_cast<std::uint32_t>(versions.size())));
  }
  return versions;
}

auto AdjustmentTransferCatalogService::ReadVersion(const CommitGraph&      graph,
                                                   const PipelineDocument& root_document,
                                                   const version_ref_id_t& version_id,
                                                   std::string*            error)
    -> std::optional<AdjustmentTransferCatalogRead> {
  try {
    const auto& ref     = graph.GetVersionRef(version_id);
    const auto  commits = FirstParentCommitsForHead(graph, ref.head_commit_hash);
    auto        replayed =
        ReplayPipelineDocumentFromRoot(root_document, commits, error);
    if (!replayed.has_value()) {
      if (error != nullptr && error->empty()) {
        *error = "AdjustmentTransferCatalog: Version replay failed";
      }
      return std::nullopt;
    }
    auto nodes = BuildNodeDescriptors(*replayed, error);
    if (!nodes.has_value()) {
      return std::nullopt;
    }

    const auto refs = SortedVersionRefs(graph);
    std::uint32_t order = 0;
    for (const auto* entry : refs) {
      if (entry->version_id == version_id) {
        break;
      }
      ++order;
    }

    AdjustmentTransferCatalogRead read;
    read.version  = VersionDescriptor(ref, graph, order);
    read.document = std::move(*replayed);
    read.nodes    = std::move(*nodes);
    return read;
  } catch (const std::exception& ex) {
    return Fail(error, ex.what());
  }
}

auto AdjustmentTransferCatalogService::BuildNodeDescriptors(const PipelineDocument& document,
                                                            std::string*            error)
    -> std::optional<std::vector<AdjustmentTransferNodeDescriptor>> {
  try {
    std::vector<AdjustmentTransferNodeDescriptor> nodes;
    const auto* develop = document.Develop();
    if (develop == nullptr) {
      return Fail(error, "AdjustmentTransferCatalog: document has no Develop node");
    }
    AdjustmentTransferNodeDescriptor develop_row;
    develop_row.kind         = AdjustmentTransferNodeKind::Develop;
    develop_row.node_id      = develop->Id();
    develop_row.display_name = std::string{kDevelopRowDisplayName};
    develop_row.source_order = 0;
    develop_row.items        = BuildDevelopItems(document, *develop);
    nodes.push_back(std::move(develop_row));

    for (const auto* grade : ColorGradesOnImageBackbone(document)) {
      if (grade == nullptr) {
        continue;
      }
      auto items = BuildColorGradeItems(*grade, error);
      if (!items.has_value()) {
        return std::nullopt;
      }
      AdjustmentTransferNodeDescriptor node;
      node.kind             = AdjustmentTransferNodeKind::ColorGrade;
      node.node_id          = grade->Id();
      node.display_name     = grade->DisplayName();
      node.is_default_grade = grade->Id() == document.DefaultGradeId();
      node.source_order     = static_cast<std::uint32_t>(nodes.size());
      node.items            = std::move(*items);
      nodes.push_back(std::move(node));
    }

    const auto* drt = document.Drt();
    if (drt == nullptr) {
      return Fail(error, "AdjustmentTransferCatalog: document has no DRT node");
    }
    auto drt_items = BuildDrtPostItems(*drt, error);
    if (!drt_items.has_value()) {
      return std::nullopt;
    }
    AdjustmentTransferNodeDescriptor endpoint;
    endpoint.kind         = AdjustmentTransferNodeKind::DrtPost;
    endpoint.node_id      = drt->Id();
    endpoint.display_name = std::string{kDrtPostRowDisplayName};
    endpoint.source_order = static_cast<std::uint32_t>(nodes.size());
    endpoint.items        = std::move(*drt_items);
    nodes.push_back(std::move(endpoint));
    return nodes;
  } catch (const std::exception& ex) {
    return Fail(error, ex.what());
  }
}

}  // namespace alcedo
