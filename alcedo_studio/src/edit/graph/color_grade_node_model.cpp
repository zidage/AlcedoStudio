//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/color_grade_node_model.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

namespace {

auto DefaultAdjustmentTypes() -> std::array<OperatorTypeId, 17> {
  return {type_ids::Cat02WhiteBalance(), type_ids::Exposure(),   type_ids::Contrast(),
          type_ids::White(),             type_ids::Black(),      type_ids::Shadows(),
          type_ids::Highlights(),        type_ids::Curve(),      type_ids::Hls(),
          type_ids::Saturation(),        type_ids::Vibrance(),   type_ids::ColorWheel(),
          type_ids::Lmt(),               type_ids::Clarity(),    type_ids::Sharpen(),
          type_ids::Halation(),          type_ids::FilmGrain()};
}

auto CleanAdjustmentTypes() -> std::array<OperatorTypeId, 13> {
  return {type_ids::Cat02WhiteBalance(), type_ids::Exposure(), type_ids::Contrast(),
          type_ids::White(),             type_ids::Black(),    type_ids::Shadows(),
          type_ids::Highlights(),        type_ids::Curve(),    type_ids::Hls(),
          type_ids::Saturation(),        type_ids::Vibrance(), type_ids::ColorWheel(),
          type_ids::Lmt()};
}

auto InstanceSuffixFor(const OperatorTypeId& type) -> std::string {
  const std::string text{type.Text()};
  const auto        pos = text.rfind('.');
  if (pos == std::string::npos || pos + 1 >= text.size()) {
    return text;
  }
  return text.substr(pos + 1);
}

void PopulateAdjustments(ColorGradeNodeModel& node, std::span<const OperatorTypeId> types) {
  const auto& catalog = BuiltinAdjustmentCatalog::Instance();
  const std::string prefix = std::string{node.Id().Value()} + ".";
  for (const auto& type : types) {
    auto model = catalog.CreateDefault(type);
    if (model == nullptr) {
      throw std::logic_error("Missing default adjustment factory for " + std::string{type.Text()});
    }
    std::string suffix = InstanceSuffixFor(type);
    if (type == type_ids::Cat02WhiteBalance()) {
      suffix = "cat02_wb";
    } else if (type == type_ids::Hls()) {
      suffix = "hls";
    }
    node.InsertAdjustment(node.AdjustmentCount(), AdjustmentInstanceId{prefix + suffix},
                          std::move(model));
  }
}

}  // namespace

ColorGradeNodeModel::ColorGradeNodeModel(NodeId id) : id_(std::move(id)) {
  inputs_[0]  = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
  inputs_[1]  = PortDescriptor{PortId{"mask"}, PortDataType::Mask, false};
  outputs_[0] = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
}

auto ColorGradeNodeModel::InputPorts() const -> std::span<const PortDescriptor> { return inputs_; }

auto ColorGradeNodeModel::OutputPorts() const -> std::span<const PortDescriptor> { return outputs_; }

auto ColorGradeNodeModel::ToJson() const -> nlohmann::json {
  nlohmann::json adjustments = nlohmann::json::array();
  for (const auto& entry : adjustments_) {
    adjustments.push_back({{"id", std::string{entry.instance_id.Value()}},
                           {"type", std::string{entry.model->Type().Text()}},
                           {"params", entry.model->ToJson()}});
  }
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"display_name", display_name_},
          {"enabled", enabled_},
          {"mix", mix_},
          {"adjustments", std::move(adjustments)}};
}

auto ColorGradeNodeModel::MakeDefault(NodeId id) -> std::unique_ptr<ColorGradeNodeModel> {
  auto       node  = std::make_unique<ColorGradeNodeModel>(std::move(id));
  const auto types = DefaultAdjustmentTypes();
  PopulateAdjustments(*node, types);
  return node;
}

auto ColorGradeNodeModel::MakeClean(NodeId id) -> std::unique_ptr<ColorGradeNodeModel> {
  auto       node  = std::make_unique<ColorGradeNodeModel>(std::move(id));
  const auto types = CleanAdjustmentTypes();
  PopulateAdjustments(*node, types);
  return node;
}

auto CreateCleanColorGradeNode(NodeId id) -> std::unique_ptr<ColorGradeNodeModel> {
  return ColorGradeNodeModel::MakeClean(std::move(id));
}

auto ColorGradeNodeModel::FromJson(const nlohmann::json& json) -> std::unique_ptr<ColorGradeNodeModel> {
  auto        node    = std::make_unique<ColorGradeNodeModel>(NodeId{json.at("id").get<std::string>()});
  const auto& catalog = BuiltinAdjustmentCatalog::Instance();
  node->display_name_ = json.value("display_name", std::string{"Color Grade"});
  node->enabled_      = json.value("enabled", true);
  node->mix_          = json.value("mix", 1.0f);
  if (json.contains("adjustments") && json["adjustments"].is_array()) {
    for (const auto& item : json["adjustments"]) {
      const auto type_text = item.at("type").get<std::string>();
      auto       model     = catalog.CreateDefault(OperatorTypeId{type_text});
      if (model == nullptr) {
        throw std::runtime_error("Unknown adjustment type: " + type_text);
      }
      if (item.contains("params") && item["params"].is_object()) {
        model->LoadJson(item["params"]);
      }
      node->InsertAdjustment(node->AdjustmentCount(),
                             AdjustmentInstanceId{item.at("id").get<std::string>()}, std::move(model));
    }
  }
  return node;
}

void ColorGradeNodeModel::SetEnabled(bool enabled) { enabled_ = enabled; }

void ColorGradeNodeModel::SetDisplayName(std::string name) { display_name_ = std::move(name); }

void ColorGradeNodeModel::SetMix(float mix) { mix_ = std::clamp(mix, 0.0f, 1.0f); }

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

void ColorGradeNodeModel::InsertAdjustment(std::size_t index, AdjustmentInstanceId id,
                                           std::unique_ptr<IOperatorModel> model) {
  if (model == nullptr) {
    throw std::invalid_argument("InsertAdjustment requires a Model");
  }
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
                               [&id](const AdjustmentModelEntry& entry) { return entry.instance_id == id; });
  if (it != adjustments_.end()) {
    adjustments_.erase(it);
  }
}

void ColorGradeNodeModel::MoveAdjustment(const AdjustmentInstanceId& id, std::size_t index) {
  const auto it = std::find_if(adjustments_.begin(), adjustments_.end(),
                               [&id](const AdjustmentModelEntry& entry) { return entry.instance_id == id; });
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

}  // namespace alcedo
