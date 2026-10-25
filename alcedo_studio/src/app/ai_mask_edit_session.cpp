//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_mask_edit_session.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <variant>

namespace alcedo {
namespace {

auto Rejected(std::string message) -> AiMaskStepResult {
  return {AiMaskStepStatus::Rejected, std::move(message)};
}

auto ExtentText(Extent2D extent) -> std::string {
  return std::to_string(extent.width) + " x " + std::to_string(extent.height);
}

auto SessionImage(AiMaskSessionImage image) -> AiMaskSessionImage {
  if (image.extent.Empty() || image.extent.width > kMaximumRasterMaskAxis ||
      image.extent.height > kMaximumRasterMaskAxis) {
    throw std::invalid_argument("AI Mask edit session: the session image size " +
                                ExtentText(image.extent) + " is not in [1, 4096]");
  }
  if (image.guide != nullptr && image.guide->extent != image.extent) {
    throw std::invalid_argument("AI Mask edit session: the guide image size " +
                                ExtentText(image.guide->extent) +
                                " is not the session image size " + ExtentText(image.extent));
  }
  return image;
}

auto PromptGroupOf(AiMaskRecipe& recipe) -> AiMaskPromptGroup* {
  return recipe.components.empty()
             ? nullptr
             : std::get_if<AiMaskPromptGroup>(&recipe.components.back().content);
}

auto PeopleOf(AiMaskRecipe& recipe) -> AiMaskTaskComponent* {
  if (recipe.components.empty()) {
    return nullptr;
  }
  auto* task = std::get_if<AiMaskTaskComponent>(&recipe.components.back().content);
  return task != nullptr && task->task == AiMaskTaskKind::People ? task : nullptr;
}

auto PointCount(const AiMaskPromptGroup& group) -> std::size_t {
  std::size_t count = 0;
  for (const auto& step : group.steps) {
    count += step.points.size();
  }
  return count;
}

/// The People list box clipped to [0, 1] with ordered corners.
auto ToggleBox(const SegmentationBox& box) -> AiMaskBox {
  const auto unit = [](float value) { return std::clamp(value, 0.0f, 1.0f); };
  return AiMaskBox{unit(std::min(box.x0, box.x1)), unit(std::min(box.y0, box.y1)),
                   unit(std::max(box.x0, box.x1)), unit(std::max(box.y0, box.y1))};
}

}  // namespace

AiMaskEditSession::AiMaskEditSession(IAiMaskSegmentationRunner& runner, AiMaskSessionImage image)
    : image_(SessionImage(std::move(image))), outputs_(runner, image_.extent, image_.guide) {
  states_.push_back(SessionState{});
  coverage_ = std::make_shared<const std::vector<std::uint8_t>>(
      static_cast<std::size_t>(image_.extent.width) * image_.extent.height, std::uint8_t{0});
}

AiMaskEditSession::AiMaskEditSession(IAiMaskSegmentationRunner& runner, AiMaskSessionImage image,
                                     const AiSegmentationMaskSource& stored, MaskAsset stored_asset)
    : AiMaskEditSession(runner, std::move(image)) {
  ValidateAiSegmentationMaskSource(stored);
  if (stored.analysis_extent != image_.extent) {
    throw std::runtime_error("AI Mask edit session: the AI Mask was made on a " +
                             ExtentText(stored.analysis_extent) +
                             " analysis image; the session image is " + ExtentText(image_.extent));
  }
  if (stored_asset.key != stored.asset_key || stored_asset.descriptor != stored.descriptor) {
    throw std::runtime_error("AI Mask edit session: the asset is not the asset of the AI Mask");
  }
  StoredMask mask{stored, {}};
  try {
    if (MakeMaskAssetKey(stored_asset.descriptor, stored_asset.pixels) != stored.asset_key) {
      throw std::invalid_argument("its pixels do not match its key");
    }
    mask.coverage = std::make_shared<const std::vector<std::uint8_t>>(
        PlaceAiMaskAssetInCoverage(stored_asset, image_.extent));
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error(std::string{"AI Mask edit session: the stored asset is invalid: "} +
                             error.what());
  }
  auto& first  = states_.front();
  first.recipe = stored.recipe;
  first.edge   = stored.edge;
  first.open_prompt_group =
      std::holds_alternative<AiMaskPromptGroup>(stored.recipe.components.back().content);
  first.input_mode = first.open_prompt_group ? AiMaskInputMode::Prompt : AiMaskInputMode::Task;
  coverage_        = mask.coverage;
  stored_          = std::move(mask);
}

auto AiMaskEditSession::AddPromptStep(AiMaskPromptStep step, AiMaskComponentOperation mode)
    -> AiMaskStepResult {
  SessionState next  = Current();
  auto*        group = next.open_prompt_group ? PromptGroupOf(next.recipe) : nullptr;
  const auto   label = group != nullptr ? mode : AiMaskComponentOperation::Add;
  for (auto& point : step.points) {
    point.label = label;
  }
  step.level = 0;
  if (group != nullptr) {
    group->steps.push_back(std::move(step));
  } else {
    next.recipe.components.push_back(AiMaskComponent{mode, AiMaskPromptGroup{{std::move(step)}}});
    group = PromptGroupOf(next.recipe);
  }
  next.open_prompt_group = true;
  if (PointCount(*group) > kAiMaskMaximumPromptGroupPoints) {
    return Rejected("A prompt group holds at most " +
                    std::to_string(kAiMaskMaximumPromptGroupPoints) + " points.");
  }
  try {
    ValidateAiMaskRecipe(next.recipe);
  } catch (const std::runtime_error& error) {
    return Rejected(error.what());
  }
  auto result = EnsureOutputs(next.recipe);
  if (result.status != AiMaskStepStatus::Applied) {
    return result;
  }
  group->steps.back().level = outputs_.SelectedLevel(*group);
  Push(std::move(next));
  return {};
}

auto AiMaskEditSession::SelectLevel(std::uint32_t level) -> AiMaskStepResult {
  SessionState next  = Current();
  auto*        group = PromptGroupOf(next.recipe);
  if (group == nullptr) {
    return Rejected("A level change needs a prompt group as the last component.");
  }
  if (AiMaskModelOutputStore::LevelCount(*group) <= 1) {
    return Rejected("Only a single click without a box has levels.");
  }
  if (level > kAiMaskMaximumPromptLevel) {
    return Rejected("The level must be 0, 1, or 2.");
  }
  if (group->steps.back().level == level) {
    return Rejected("The level is already selected.");
  }
  group->steps.back().level = level;
  return Apply(std::move(next));
}

auto AiMaskEditSession::AddTask(AiMaskTaskKind task, AiMaskComponentOperation operation)
    -> AiMaskStepResult {
  SessionState next = Current();
  next.recipe.components.push_back(AiMaskComponent{operation, AiMaskTaskComponent{task, {}}});
  next.open_prompt_group = false;
  return Apply(std::move(next));
}

auto AiMaskEditSession::TogglePerson(std::uint32_t person_index) -> AiMaskStepResult {
  SessionState next   = Current();
  auto*        people = PeopleOf(next.recipe);
  if (people == nullptr) {
    return Rejected("A person toggle needs People as the last component.");
  }
  if (!outputs_.HasPeople()) {
    // A session opened from a stored AI Mask reads the People list with the People task first.
    auto result = outputs_.Ensure(AiMaskComponent{AiMaskComponentOperation::Add,
                                                  AiMaskTaskComponent{AiMaskTaskKind::People, {}}});
    if (result.status != AiMaskStepStatus::Applied) {
      return result;
    }
  }
  const auto list = outputs_.People();
  if (person_index >= list.size()) {
    return Rejected("Person " + std::to_string(person_index) + " is not in the People list of " +
                    std::to_string(list.size()) + " people.");
  }
  const auto operation = PersonIncluded(person_index) ? AiMaskComponentOperation::Subtract
                                                      : AiMaskComponentOperation::Add;
  people->person_toggles.push_back(
      AiMaskPersonToggle{person_index, operation, ToggleBox(list[person_index].box)});
  next.open_prompt_group = false;
  return Apply(std::move(next));
}

auto AiMaskEditSession::SetEdgeAdjustment(AiMaskEdgeAdjustment edge) -> AiMaskStepResult {
  try {
    ValidateAiMaskEdgeAdjustment(edge);
  } catch (const std::runtime_error& error) {
    return Rejected(error.what());
  }
  if (edge == Current().edge) {
    return Rejected("The edge adjustment is unchanged.");
  }
  SessionState next = Current();
  next.edge         = edge;
  return Apply(std::move(next));
}

auto AiMaskEditSession::SetInputMode(AiMaskInputMode mode) -> AiMaskStepResult {
  if (mode == Current().input_mode) {
    return Rejected("The input mode is unchanged.");
  }
  SessionState next      = Current();
  next.input_mode        = mode;
  next.open_prompt_group = false;
  return Apply(std::move(next));
}

auto AiMaskEditSession::Undo() -> bool {
  if (!CanUndo()) {
    return false;
  }
  // An edge or input-mode step keeps the recipe, so it keeps the shared coverage too.
  if (states_[current_ - 1].recipe != Recipe()) {
    Compose(states_[current_ - 1].recipe);
  }
  --current_;
  return true;
}

auto AiMaskEditSession::Redo() -> bool {
  if (!CanRedo()) {
    return false;
  }
  if (states_[current_ + 1].recipe != Recipe()) {
    Compose(states_[current_ + 1].recipe);
  }
  ++current_;
  return true;
}

auto AiMaskEditSession::PersonIncluded(std::uint32_t person_index) const -> bool {
  const auto& components = Current().recipe.components;
  for (auto component = components.rbegin(); component != components.rend(); ++component) {
    const auto* task = std::get_if<AiMaskTaskComponent>(&component->content);
    if (task == nullptr || task->task != AiMaskTaskKind::People) {
      continue;
    }
    const auto& toggles = task->person_toggles;
    const auto  toggle  = std::find_if(
        toggles.rbegin(), toggles.rend(),
        [person_index](const AiMaskPersonToggle& value) { return value.index == person_index; });
    if (toggle != toggles.rend()) {
      return toggle->operation == AiMaskComponentOperation::Add;
    }
    break;
  }
  const auto list = outputs_.People();
  return person_index < list.size() && list[person_index].in_subject;
}

auto AiMaskEditSession::PrepareCommit() const -> std::pair<MaskAsset, AiSegmentationMaskSource> {
  auto asset = CropAiMaskCoverage(*coverage_, image_.extent);
  if (!asset.has_value()) {
    throw std::runtime_error("The AI Mask has no coverage.");
  }
  const bool               stored = UsesStoredCoverage(Current().recipe);
  AiSegmentationMaskSource source;
  source.asset_key       = asset->key;
  source.descriptor      = asset->descriptor;
  source.analysis_extent = image_.extent;
  source.recipe          = Current().recipe;
  source.edge            = Current().edge;
  source.model_set_id    = stored ? stored_->source.model_set_id : image_.model_set_id;
  source.geometry_fingerprint =
      stored ? stored_->source.geometry_fingerprint : image_.geometry_fingerprint;
  ValidateAiSegmentationMaskSource(source);
  return {std::move(*asset), std::move(source)};
}

auto AiMaskEditSession::Apply(SessionState state) -> AiMaskStepResult {
  auto result = EnsureOutputs(state.recipe);
  if (result.status != AiMaskStepStatus::Applied) {
    return result;
  }
  Push(std::move(state));
  return {};
}

auto AiMaskEditSession::EnsureOutputs(const AiMaskRecipe& recipe) -> AiMaskStepResult {
  if (UsesStoredCoverage(recipe)) {
    return {};
  }
  for (const auto& component : recipe.components) {
    auto result = outputs_.Ensure(component);
    if (result.status != AiMaskStepStatus::Applied) {
      return result;
    }
  }
  return {};
}

void AiMaskEditSession::Push(SessionState state) {
  if (state.recipe != Current().recipe) {
    Compose(state.recipe);
  }
  states_.resize(current_ + 1);
  states_.push_back(std::move(state));
  current_ = states_.size() - 1;
}

void AiMaskEditSession::Compose(const AiMaskRecipe& recipe) {
  if (UsesStoredCoverage(recipe)) {
    coverage_ = stored_->coverage;
    return;
  }
  // Reuse the coverage of each leading component that did not change.
  std::vector<std::pair<AiMaskComponent, std::vector<std::uint8_t>>> next;
  next.reserve(recipe.components.size());
  try {
    for (std::size_t i = 0; i < recipe.components.size(); ++i) {
      const auto& component = recipe.components[i];
      if (i < component_coverages_.size() && component_coverages_[i].first == component) {
        next.push_back(std::move(component_coverages_[i]));
      } else {
        next.emplace_back(component, outputs_.ComponentCoverage(component));
      }
    }
  } catch (...) {
    component_coverages_.clear();
    throw;
  }
  std::vector<std::uint8_t> total(coverage_->size(), 0);
  for (std::size_t i = 0; i < next.size(); ++i) {
    FoldAiMaskCoverage(total, next[i].second, recipe.components[i].operation);
  }
  component_coverages_ = std::move(next);
  // A new allocation: a read of the previous coverage keeps its bytes.
  coverage_            = std::make_shared<const std::vector<std::uint8_t>>(std::move(total));
}

auto AiMaskEditSession::UsesStoredCoverage(const AiMaskRecipe& recipe) const -> bool {
  return stored_.has_value() && recipe == stored_->source.recipe;
}

}  // namespace alcedo
