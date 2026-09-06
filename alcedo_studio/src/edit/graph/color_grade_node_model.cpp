//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/color_grade_node_model.hpp"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/graph/adjustment_ownership.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"

namespace alcedo {
namespace {

void PopulateAdjustments(ColorGradeNodeModel& node, std::span<const OperatorTypeId> types) {
  const auto& catalog = BuiltinAdjustmentCatalog::Instance();
  for (const auto& type : types) {
    auto model = catalog.CreateDefault(type);
    if (model == nullptr) {
      throw std::logic_error("Missing default adjustment factory for " + std::string{type.Text()});
    }
    node.InsertAdjustment(node.AdjustmentCount(), MakeAdjustmentInstanceId(node.Id(), type),
                          std::move(model));
  }
}

auto PresentTypes(const std::vector<AdjustmentModelEntry>& adjustments)
    -> std::vector<OperatorTypeId> {
  std::vector<OperatorTypeId> types;
  types.reserve(adjustments.size());
  for (const auto& entry : adjustments) {
    types.push_back(entry.model->Type());
  }
  return types;
}

}  // namespace

ColorGradeNodeModel::ColorGradeNodeModel(NodeId id) : id_(std::move(id)) {
  inputs_[0]  = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
  outputs_[0] = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
}

auto ColorGradeNodeModel::InputPorts() const -> std::span<const PortDescriptor> { return inputs_; }

auto ColorGradeNodeModel::OutputPorts() const -> std::span<const PortDescriptor> { return outputs_; }

auto ColorGradeNodeModel::ToJson() const -> nlohmann::json {
  for (const auto& type : PresentTypes(adjustments_)) {
    RequireAdjustmentOwner(type, AdjustmentParameterOwner::ColorGrade, "ColorGrade ToJson");
  }
  nlohmann::json adjustments = nlohmann::json::array();
  for (const auto& entry : adjustments_) {
    adjustments.push_back({{"id", std::string{entry.instance_id.Value()}},
                           {"type", std::string{entry.model->Type().Text()}},
                           {"params", entry.model->ToJson()}});
  }
  nlohmann::json masks = nlohmann::json::array();
  for (const auto& mask : masks_) {
    masks.push_back(MaskModelToJson(mask));
  }
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"display_name", display_name_},
          {"enabled", enabled_},
          {"mix", mix_},
          {"adjustments", std::move(adjustments)},
          {"masks", std::move(masks)}};
}

auto ColorGradeNodeModel::MakeDefault(NodeId id) -> std::unique_ptr<ColorGradeNodeModel> {
  auto       node  = std::make_unique<ColorGradeNodeModel>(std::move(id));
  const auto types = ColorGradeAdjustmentTypes();
  PopulateAdjustments(*node, types);
  return node;
}

auto ColorGradeNodeModel::MakeClean(NodeId id) -> std::unique_ptr<ColorGradeNodeModel> {
  auto       node  = std::make_unique<ColorGradeNodeModel>(std::move(id));
  const auto types = ColorGradeAdjustmentTypes();
  PopulateAdjustments(*node, types);
  return node;
}

auto CreateCleanColorGradeNode(NodeId id) -> std::unique_ptr<ColorGradeNodeModel> {
  return ColorGradeNodeModel::MakeClean(std::move(id));
}

auto ColorGradeNodeModel::FromJson(const nlohmann::json& json)
    -> std::unique_ptr<ColorGradeNodeModel> {
  auto        node    = std::make_unique<ColorGradeNodeModel>(NodeId{json.at("id").get<std::string>()});
  const auto& catalog = BuiltinAdjustmentCatalog::Instance();
  if (!json.contains("display_name") || !json.at("display_name").is_string() ||
      json.at("display_name").get<std::string>().empty()) {
    throw std::runtime_error("ColorGrade FromJson: display_name must be non-empty");
  }
  node->display_name_ = json.at("display_name").get<std::string>();
  node->enabled_      = json.value("enabled", true);
  node->mix_          = json.value("mix", 1.0f);
  if (json.contains("adjustments") && json["adjustments"].is_array()) {
    for (const auto& item : json["adjustments"]) {
      const auto type_text = item.at("type").get<std::string>();
      auto       model     = catalog.CreateDefault(OperatorTypeId{type_text});
      if (model == nullptr) {
        throw std::runtime_error("Unknown adjustment type: " + type_text);
      }
      RequireAdjustmentOwner(model->Type(), AdjustmentParameterOwner::ColorGrade,
                             "ColorGrade FromJson");
      if (item.contains("params") && item["params"].is_object()) {
        model->LoadJson(item["params"]);
      }
      node->InsertAdjustment(node->AdjustmentCount(),
                             AdjustmentInstanceId{item.at("id").get<std::string>()},
                             std::move(model));
    }
  }
  if (!json.contains("masks") || !json["masks"].is_array()) {
    throw std::runtime_error("ColorGrade FromJson: missing masks array");
  }
  std::vector<MaskModel> masks;
  masks.reserve(json["masks"].size());
  for (const auto& item : json["masks"]) {
    masks.push_back(MaskModelFromJson(item));
  }
  if (HasDuplicateOrEmptyMaskId(masks)) {
    throw std::runtime_error("ColorGrade FromJson: empty or duplicate MaskId");
  }
  node->masks_ = std::move(masks);
  for (const auto& mask : node->masks_) {
    node->TouchMask(mask.id);
  }
  return node;
}

void ColorGradeNodeModel::SetEnabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }
  enabled_   = enabled;
  mix_dirty_ = true;
}

void ColorGradeNodeModel::SetDisplayName(std::string name) { display_name_ = std::move(name); }

void ColorGradeNodeModel::SetMix(float mix) {
  const auto clamped = std::clamp(mix, 0.0f, 1.0f);
  if (mix_ == clamped) {
    return;
  }
  mix_       = clamped;
  mix_dirty_ = true;
}

auto ColorGradeNodeModel::AdjustmentIdAt(std::size_t index) const -> const AdjustmentInstanceId& {
  return adjustments_.at(index).instance_id;
}

auto ColorGradeNodeModel::AdjustmentAt(std::size_t index) -> IOperatorModel& {
  return *adjustments_.at(index).model;
}

auto ColorGradeNodeModel::AdjustmentAt(std::size_t index) const -> const IOperatorModel& {
  return *adjustments_.at(index).model;
}

auto ColorGradeNodeModel::FindAdjustment(const AdjustmentInstanceId& id) -> IOperatorModel* {
  for (auto& entry : adjustments_) {
    if (entry.instance_id == id) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto ColorGradeNodeModel::FindAdjustment(const AdjustmentInstanceId& id) const
    -> const IOperatorModel* {
  for (const auto& entry : adjustments_) {
    if (entry.instance_id == id) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto ColorGradeNodeModel::FindAdjustmentByType(const OperatorTypeId& type) -> IOperatorModel* {
  for (auto& entry : adjustments_) {
    if (entry.model->Type() == type) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto ColorGradeNodeModel::FindAdjustmentByType(const OperatorTypeId& type) const
    -> const IOperatorModel* {
  for (const auto& entry : adjustments_) {
    if (entry.model->Type() == type) {
      return entry.model.get();
    }
  }
  return nullptr;
}

auto ColorGradeNodeModel::FindAdjustmentIdByType(const OperatorTypeId& type) const
    -> const AdjustmentInstanceId* {
  for (const auto& entry : adjustments_) {
    if (entry.model->Type() == type) {
      return &entry.instance_id;
    }
  }
  return nullptr;
}

void ColorGradeNodeModel::InsertAdjustment(std::size_t index, AdjustmentInstanceId id,
                                           std::unique_ptr<IOperatorModel> model) {
  if (model == nullptr) {
    throw std::invalid_argument("InsertAdjustment requires a Model");
  }
  RequireAdjustmentOwner(model->Type(), AdjustmentParameterOwner::ColorGrade,
                         "ColorGrade InsertAdjustment");
  if (index > adjustments_.size()) {
    index = adjustments_.size();
  }
  AdjustmentModelEntry entry;
  entry.instance_id = std::move(id);
  entry.model       = std::move(model);
  adjustments_.insert(adjustments_.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

void ColorGradeNodeModel::RemoveAdjustment(const AdjustmentInstanceId& id) {
  const auto it = std::find_if(adjustments_.begin(), adjustments_.end(),
                               [&id](const AdjustmentModelEntry& entry) {
                                 return entry.instance_id == id;
                               });
  if (it != adjustments_.end()) {
    adjustments_.erase(it);
  }
}

void ColorGradeNodeModel::MoveAdjustment(const AdjustmentInstanceId& id, std::size_t index) {
  const auto it = std::find_if(adjustments_.begin(), adjustments_.end(),
                               [&id](const AdjustmentModelEntry& entry) {
                                 return entry.instance_id == id;
                               });
  if (it == adjustments_.end()) {
    return;
  }
  AdjustmentModelEntry entry = std::move(*it);
  adjustments_.erase(it);
  if (index > adjustments_.size()) {
    index = adjustments_.size();
  }
  adjustments_.insert(adjustments_.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

namespace {

[[noreturn]] void FailMask(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

auto RequireMaskIterator(std::vector<MaskModel>& masks, const MaskId& mask_id)
    -> std::vector<MaskModel>::iterator {
  const auto it = std::find_if(masks.begin(), masks.end(),
                               [&mask_id](const MaskModel& mask) { return mask.id == mask_id; });
  if (it == masks.end()) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  return it;
}

}  // namespace

void ColorGradeNodeModel::TouchMask(const MaskId& mask_id) {
  mask_content_revision_[mask_id] = next_mask_revision_++;
}

auto ColorGradeNodeModel::MaskContentRevision(const MaskId& mask_id) const -> std::uint64_t {
  const auto it = mask_content_revision_.find(mask_id);
  return it == mask_content_revision_.end() ? 0 : it->second;
}

void ColorGradeNodeModel::AddMask(MaskModel mask, std::size_t index) {
  ValidateMaskModel(mask);
  if (FindMask(mask.id) != nullptr) {
    FailMask("Duplicate MaskId: " + std::string{mask.id.Value()});
  }
  masks_.reserve(masks_.size() + 1);
  if (index > masks_.size()) {
    index = masks_.size();
  }
  masks_.insert(masks_.begin() + static_cast<std::ptrdiff_t>(index), std::move(mask));
  TouchMask(masks_[index].id);
}

void ColorGradeNodeModel::RemoveMask(const MaskId& mask_id) {
  const auto it = RequireMaskIterator(masks_, mask_id);
  mask_content_revision_.erase(mask_id);
  masks_.erase(it);
}

void ColorGradeNodeModel::ReplaceMaskSource(const MaskId& mask_id, MaskSource source) {
  auto* mask = FindMask(mask_id);
  if (mask == nullptr) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  MaskModel candidate = *mask;
  candidate.source    = std::move(source);
  ValidateMaskModel(candidate);
  mask->source = std::move(candidate.source);
  TouchMask(mask_id);
}

void ColorGradeNodeModel::SetMaskEnabled(const MaskId& mask_id, bool enabled) {
  auto* mask = FindMask(mask_id);
  if (mask == nullptr) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  if (mask->enabled == enabled) {
    return;
  }
  mask->enabled = enabled;
  TouchMask(mask_id);
}

void ColorGradeNodeModel::SetMaskOpacity(const MaskId& mask_id, float opacity) {
  auto* mask = FindMask(mask_id);
  if (mask == nullptr) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  MaskModel candidate = *mask;
  candidate.opacity   = opacity;
  ValidateMaskModel(candidate);
  mask->opacity = candidate.opacity;
  TouchMask(mask_id);
}

void ColorGradeNodeModel::SetMaskInvert(const MaskId& mask_id, bool invert) {
  auto* mask = FindMask(mask_id);
  if (mask == nullptr) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  if (mask->invert == invert) {
    return;
  }
  mask->invert = invert;
  TouchMask(mask_id);
}

void ColorGradeNodeModel::MoveMaskForDisplay(const MaskId& mask_id, std::size_t index) {
  auto      it    = RequireMaskIterator(masks_, mask_id);
  MaskModel entry = std::move(*it);
  masks_.erase(it);
  if (index > masks_.size()) {
    index = masks_.size();
  }
  masks_.insert(masks_.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

auto ColorGradeNodeModel::MaskAt(std::size_t index) -> MaskModel& { return masks_.at(index); }

auto ColorGradeNodeModel::MaskAt(std::size_t index) const -> const MaskModel& {
  return masks_.at(index);
}

auto ColorGradeNodeModel::FindMask(const MaskId& mask_id) -> MaskModel* {
  for (auto& mask : masks_) {
    if (mask.id == mask_id) {
      return &mask;
    }
  }
  return nullptr;
}

auto ColorGradeNodeModel::FindMask(const MaskId& mask_id) const -> const MaskModel* {
  for (const auto& mask : masks_) {
    if (mask.id == mask_id) {
      return &mask;
    }
  }
  return nullptr;
}

}  // namespace alcedo
