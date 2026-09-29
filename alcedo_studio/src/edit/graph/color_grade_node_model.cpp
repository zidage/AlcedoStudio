//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/color_grade_node_model.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/copy_on_write.hpp"
#ifdef ALCEDO_ENABLE_BRUSH_MASK
#include "edit/mask/brush_mask_commands.hpp"
#endif
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
  // One empty list serves every node that has no Masks; the first Mask write copies it.
  static const auto kEmptyMaskEntries =
      std::shared_ptr<const MaskEntryList>(std::make_shared<MaskEntryList>());
  mask_entries_ = kEmptyMaskEntries;
  inputs_[0]  = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
  outputs_[0] = PortDescriptor{PortId{"image"}, PortDataType::SceneImage, true};
}

auto ColorGradeNodeModel::Clone() const -> std::shared_ptr<INodeModel> {
  return std::shared_ptr<INodeModel>(new ColorGradeNodeModel(*this));
}

auto ColorGradeNodeModel::MutableMaskEntries() -> MaskEntryList& {
  return UnshareForWrite(mask_entries_, [](const MaskEntryList& entries) {
    return std::make_shared<MaskEntryList>(entries);
  });
}

auto ColorGradeNodeModel::MaskAtEntry(const void* entries, std::size_t index) -> const MaskModel& {
  return *(*static_cast<const MaskEntryList*>(entries))[index].mask;
}

auto ColorGradeNodeModel::Masks() const -> MaskListView {
  return {mask_entries_.get(), mask_entries_->size(), &MaskAtEntry};
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
  for (const auto& entry : *mask_entries_) {
    masks.push_back(MaskModelToJson(*entry.mask));
  }
  return {{"id", std::string{id_.Value()}},
          {"type", std::string{Type().Text()}},
          {"display_name", display_name_},
          {"enabled", enabled_},
          {"deletion_protected", deletion_protected_},
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
  if (!json.contains("deletion_protected") || !json.at("deletion_protected").is_boolean()) {
    throw std::runtime_error("ColorGrade FromJson: deletion_protected must be boolean");
  }
  node->deletion_protected_ = json.at("deletion_protected").get<bool>();
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
  auto entries = std::make_shared<MaskEntryList>();
  entries->reserve(masks.size());
  for (auto& mask : masks) {
    entries->push_back(
        {std::make_shared<const MaskModel>(std::move(mask)), NextParameterRevision()});
  }
  node->mask_entries_ = std::move(entries);
  return node;
}

void ColorGradeNodeModel::SetEnabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }
  enabled_      = enabled;
  mix_revision_ = NextParameterRevision();
}

void ColorGradeNodeModel::SetDisplayName(std::string name) { display_name_ = std::move(name); }

void ColorGradeNodeModel::SetMix(float mix) {
  const auto clamped = std::clamp(mix, 0.0f, 1.0f);
  if (mix_ == clamped) {
    return;
  }
  mix_          = clamped;
  mix_revision_ = NextParameterRevision();
}

void ColorGradeNodeModel::CopyRevisionsFrom(const ColorGradeNodeModel& source) {
  mix_revision_ = source.mix_revision_;
  for (auto& entry : adjustments_) {
    if (const auto* model = source.FindAdjustment(entry.instance_id); model != nullptr) {
      MutableAdjustmentModel(entry).CopyRevisionsFrom(*model);
    }
  }
  if (mask_entries_->empty()) {
    return;
  }
  for (auto& entry : MutableMaskEntries()) {
    if (const auto revision = source.MaskContentRevision(entry.mask->id);
        revision != kNoParameterRevision) {
      entry.content_revision = revision;
    }
  }
}

auto ColorGradeNodeModel::AdjustmentIdAt(std::size_t index) const -> const AdjustmentInstanceId& {
  return adjustments_.at(index).instance_id;
}

auto ColorGradeNodeModel::AdjustmentAt(std::size_t index) -> IOperatorModel& {
  return MutableAdjustmentModel(adjustments_.at(index));
}

auto ColorGradeNodeModel::AdjustmentAt(std::size_t index) const -> const IOperatorModel& {
  return *adjustments_.at(index).model;
}

auto ColorGradeNodeModel::FindAdjustment(const AdjustmentInstanceId& id) -> IOperatorModel* {
  for (auto& entry : adjustments_) {
    if (entry.instance_id == id) {
      return &MutableAdjustmentModel(entry);
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
      return &MutableAdjustmentModel(entry);
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

}  // namespace

auto ColorGradeNodeModel::FindMaskIndex(const MaskId& mask_id) const
    -> std::optional<std::size_t> {
  const auto& entries = *mask_entries_;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].mask->id == mask_id) {
      return index;
    }
  }
  return std::nullopt;
}

auto ColorGradeNodeModel::RequireMaskIndex(const MaskId& mask_id) const -> std::size_t {
  const auto index = FindMaskIndex(mask_id);
  if (!index.has_value()) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  return *index;
}

void ColorGradeNodeModel::StoreMask(std::size_t index, MaskModel mask, MaskWriteKind kind) {
  ValidateMaskModel(mask);
  auto  stored = std::make_shared<const MaskModel>(std::move(mask));
  auto& entry  = MutableMaskEntries()[index];
  entry.mask   = std::move(stored);
  if (kind == MaskWriteKind::Content) {
    entry.content_revision = NextParameterRevision();
  }
}

void ColorGradeNodeModel::SetMaskDeletionProtected(const MaskId& mask_id, bool value) {
  const auto index = RequireMaskIndex(mask_id);
  if (MaskAt(index).deletion_protected == value) {
    return;
  }
  auto candidate               = MaskAt(index);
  candidate.deletion_protected = value;
  StoreMask(index, std::move(candidate), MaskWriteKind::Metadata);
}

void ColorGradeNodeModel::SetMaskDisplayName(const MaskId& mask_id, std::string name) {
  const auto index = RequireMaskIndex(mask_id);
  if (MaskAt(index).display_name == name) {
    return;
  }
  auto candidate         = MaskAt(index);
  candidate.display_name = std::move(name);
  StoreMask(index, std::move(candidate), MaskWriteKind::Metadata);
}

auto ColorGradeNodeModel::MaskContentRevision(const MaskId& mask_id) const -> ParameterRevision {
  const auto index = FindMaskIndex(mask_id);
  return index.has_value() ? (*mask_entries_)[*index].content_revision : kNoParameterRevision;
}

void ColorGradeNodeModel::AddMask(MaskModel mask, std::size_t index) {
  ValidateMaskModel(mask);
  if (FindMaskIndex(mask.id).has_value()) {
    FailMask("Duplicate MaskId: " + std::string{mask.id.Value()});
  }
  MaskEntry entry{std::make_shared<const MaskModel>(std::move(mask)), NextParameterRevision()};
  auto&     entries = MutableMaskEntries();
  if (index > entries.size()) {
    index = entries.size();
  }
  entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

void ColorGradeNodeModel::RemoveMask(const MaskId& mask_id) {
  const auto index   = RequireMaskIndex(mask_id);
  auto&      entries = MutableMaskEntries();
  entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
}

void ColorGradeNodeModel::ReplaceMaskSource(const MaskId& mask_id, MaskSource source) {
  const auto index     = RequireMaskIndex(mask_id);
  auto       candidate = MaskAt(index);
  candidate.source     = std::move(source);
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::SetMaskEnabled(const MaskId& mask_id, bool enabled) {
  const auto index = RequireMaskIndex(mask_id);
  if (MaskAt(index).enabled == enabled) {
    return;
  }
  auto candidate    = MaskAt(index);
  candidate.enabled = enabled;
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::SetMaskOpacity(const MaskId& mask_id, float opacity) {
  const auto index     = RequireMaskIndex(mask_id);
  auto       candidate = MaskAt(index);
  candidate.opacity    = opacity;
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::SetMaskInvert(const MaskId& mask_id, bool invert) {
  const auto index = RequireMaskIndex(mask_id);
  if (MaskAt(index).invert == invert) {
    return;
  }
  auto candidate   = MaskAt(index);
  candidate.invert = invert;
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::MoveMaskForDisplay(const MaskId& mask_id, std::size_t index) {
  const auto from    = RequireMaskIndex(mask_id);
  auto&      entries = MutableMaskEntries();
  MaskEntry  entry   = std::move(entries[from]);
  entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(from));
  if (index > entries.size()) {
    index = entries.size();
  }
  entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

#ifdef ALCEDO_ENABLE_BRUSH_MASK
auto ColorGradeNodeModel::RequireBrushMaskIndex(const NodeId& node_id, const MaskId& mask_id,
                                                std::uint64_t expected_revision) const
    -> std::size_t {
  if (node_id != id_) {
    FailMask("Brush command NodeId does not match this Color Grade");
  }
  const auto index = RequireMaskIndex(mask_id);
  if ((*mask_entries_)[index].content_revision != expected_revision) {
    FailMask("Brush command revision does not match Mask content revision");
  }
  if (!std::holds_alternative<BrushMaskSource>(MaskAt(index).source)) {
    FailMask("Mask is not a Brush source: " + std::string{mask_id.Value()});
  }
  return index;
}

void ColorGradeNodeModel::AppendBrushStroke(AppendBrushStrokeCommand command) {
  const auto index =
      RequireBrushMaskIndex(command.node_id, command.mask_id, command.expected_revision);
  auto  candidate = MaskAt(index);
  auto& brush     = std::get<BrushMaskSource>(candidate.source);
  ValidateBrushStroke(command.stroke);
  if (FindBrushStrokeIndex(brush.strokes, command.stroke.id) != brush.strokes.size()) {
    FailMask("Duplicate StrokeId: " + std::string{command.stroke.id.Value()});
  }
  brush.strokes.push_back(std::move(command.stroke));
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::RemoveBrushStroke(const RemoveBrushStrokeCommand& command) {
  const auto index =
      RequireBrushMaskIndex(command.node_id, command.mask_id, command.expected_revision);
  auto       candidate    = MaskAt(index);
  auto&      brush        = std::get<BrushMaskSource>(candidate.source);
  const auto stroke_index = FindBrushStrokeIndex(brush.strokes, command.stroke_id);
  if (stroke_index == brush.strokes.size()) {
    FailMask("Unknown StrokeId: " + std::string{command.stroke_id.Value()});
  }
  brush.strokes.erase(brush.strokes.begin() + static_cast<std::ptrdiff_t>(stroke_index));
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::InsertBrushStroke(InsertBrushStrokeCommand command) {
  const auto index =
      RequireBrushMaskIndex(command.node_id, command.mask_id, command.expected_revision);
  auto  candidate = MaskAt(index);
  auto& brush     = std::get<BrushMaskSource>(candidate.source);
  ValidateBrushStroke(command.stroke);
  if (FindBrushStrokeIndex(brush.strokes, command.stroke.id) != brush.strokes.size()) {
    FailMask("Duplicate StrokeId: " + std::string{command.stroke.id.Value()});
  }
  auto stroke_index = command.index;
  if (stroke_index > brush.strokes.size()) {
    stroke_index = brush.strokes.size();
  }
  brush.strokes.insert(brush.strokes.begin() + static_cast<std::ptrdiff_t>(stroke_index),
                       std::move(command.stroke));
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

void ColorGradeNodeModel::SetBrushTranslation(const SetBrushTranslationCommand& command) {
  if (!std::isfinite(command.before.x) || !std::isfinite(command.before.y) ||
      !std::isfinite(command.after.x) || !std::isfinite(command.after.y)) {
    FailMask("Brush translation must be finite");
  }
  const auto index =
      RequireBrushMaskIndex(command.node_id, command.mask_id, command.expected_revision);
  const auto& current = std::get<BrushMaskSource>(MaskAt(index).source);
  if (current.placement_translation != command.before) {
    FailMask("Brush translation before-value does not match the current source");
  }
  if (current.placement_translation == command.after) {
    return;
  }
  auto candidate = MaskAt(index);
  std::get<BrushMaskSource>(candidate.source).placement_translation = command.after;
  StoreMask(index, std::move(candidate), MaskWriteKind::Content);
}

auto ColorGradeNodeModel::BrushStrokes(const MaskId& mask_id) const
    -> std::span<const BrushStroke> {
  const auto* mask = FindMask(mask_id);
  if (mask == nullptr) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  if (brush == nullptr) {
    FailMask("Mask is not a Brush source: " + std::string{mask_id.Value()});
  }
  return brush->strokes;
}

auto ColorGradeNodeModel::BrushPlacementTranslation(const MaskId& mask_id) const -> Vector2 {
  const auto* mask = FindMask(mask_id);
  if (mask == nullptr) {
    FailMask("Unknown MaskId: " + std::string{mask_id.Value()});
  }
  const auto* brush = std::get_if<BrushMaskSource>(&mask->source);
  if (brush == nullptr) {
    FailMask("Mask is not a Brush source: " + std::string{mask_id.Value()});
  }
  return brush->placement_translation;
}
#endif

auto ColorGradeNodeModel::MaskAt(std::size_t index) const -> const MaskModel& {
  return *mask_entries_->at(index).mask;
}

auto ColorGradeNodeModel::FindMask(const MaskId& mask_id) const -> const MaskModel* {
  const auto index = FindMaskIndex(mask_id);
  return index.has_value() ? (*mask_entries_)[*index].mask.get() : nullptr;
}

}  // namespace alcedo
