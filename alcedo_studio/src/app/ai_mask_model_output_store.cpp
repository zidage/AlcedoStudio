//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_mask_model_output_store.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "json.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kClickLevelCount = 3;

auto                    Failed(std::string message) -> AiMaskStepResult {
  return {AiMaskStepStatus::Failed, std::move(message)};
}

auto CallFailure(const SegmentationCallStatus& status, std::string_view call) -> AiMaskStepResult {
  return Failed(status.error.empty() ? "the segmentation " + std::string{call} + " failed"
                                     : status.error);
}

auto TaskText(AiMaskTaskKind task) -> std::string_view {
  switch (task) {
    case AiMaskTaskKind::Subject:
      return "subject";
    case AiMaskTaskKind::Background:
      return "background";
    case AiMaskTaskKind::Sky:
      return "sky";
    case AiMaskTaskKind::People:
      return "people";
  }
  throw std::logic_error("AI Mask: unknown task kind");
}

auto SidecarTask(AiMaskTaskKind task) -> SegmentationTask {
  switch (task) {
    case AiMaskTaskKind::Subject:
      return SegmentationTask::kSubject;
    case AiMaskTaskKind::Background:
      return SegmentationTask::kBackground;
    case AiMaskTaskKind::Sky:
      return SegmentationTask::kSky;
    case AiMaskTaskKind::People:
      return SegmentationTask::kPeople;
  }
  throw std::logic_error("AI Mask: unknown task kind");
}

auto OrderedBox(const SegmentationBox& box) -> AiMaskBox {
  return AiMaskBox{std::min(box.x0, box.x1), std::min(box.y0, box.y1), std::max(box.x0, box.x1),
                   std::max(box.y0, box.y1)};
}

/// The group made of the first @p count steps of @p group.
auto Prefix(const AiMaskPromptGroup& group, std::size_t count) -> AiMaskPromptGroup {
  AiMaskPromptGroup prefix;
  prefix.steps.assign(group.steps.begin(),
                      group.steps.begin() + static_cast<std::ptrdiff_t>(count));
  return prefix;
}

/// The decoder input of @p prefix: all points in step order and the box of the latest step that
/// has one.
auto DecoderPrompt(const AiMaskPromptGroup& prefix, std::vector<float> mask_input)
    -> SegmentationPrompt {
  SegmentationPrompt prompt;
  for (const auto& step : prefix.steps) {
    for (const auto& point : step.points) {
      prompt.points.push_back(SegmentationPoint{point.x, point.y,
                                                point.label == AiMaskComponentOperation::Add
                                                    ? SegmentationPointLabel::kAdd
                                                    : SegmentationPointLabel::kSubtract});
    }
    if (step.box.has_value()) {
      prompt.box = SegmentationBox{step.box->x0, step.box->y0, step.box->x1, step.box->y1};
    }
  }
  prompt.mask_input = std::move(mask_input);
  return prompt;
}

}  // namespace

AiMaskModelOutputStore::AiMaskModelOutputStore(IAiMaskSegmentationRunner& runner, Extent2D extent,
                                               std::shared_ptr<const AiMaskCoverageGuide> guide)
    : runner_(runner), extent_(extent), guide_(std::move(guide)) {}

auto AiMaskModelOutputStore::TaskKey(AiMaskTaskKind task) -> std::string {
  return nlohmann::json{{"task", TaskText(task)}}.dump();
}

auto AiMaskModelOutputStore::PersonKey(std::uint32_t person_index) -> std::string {
  return nlohmann::json{{"person", person_index}}.dump();
}

auto AiMaskModelOutputStore::PromptKey(const AiMaskPromptGroup& prefix) -> std::string {
  // The decode does not depend on the group operation or on the level of its own step.
  AiMaskPromptGroup normalized  = prefix;
  normalized.steps.back().level = 0;
  AiMaskRecipe recipe;
  recipe.components.push_back(AiMaskComponent{AiMaskComponentOperation::Add, normalized});
  return nlohmann::json{{"prompt", AiMaskRecipeToJson(recipe)}}.dump();
}

auto AiMaskModelOutputStore::LevelCount(const AiMaskPromptGroup& group) -> std::uint32_t {
  std::size_t points  = 0;
  bool        has_box = false;
  for (const auto& step : group.steps) {
    points += step.points.size();
    has_box = has_box || step.box.has_value();
  }
  return points == 1 && !has_box ? kClickLevelCount : 1;
}

auto AiMaskModelOutputStore::Ensure(const AiMaskComponent& component) -> AiMaskStepResult {
  if (const auto* group = std::get_if<AiMaskPromptGroup>(&component.content)) {
    return EnsurePromptGroup(*group);
  }
  const auto& task   = std::get<AiMaskTaskComponent>(component.content);
  auto        result = EnsureTask(task.task);
  for (const auto& toggle : task.person_toggles) {
    if (result.status != AiMaskStepStatus::Applied) {
      break;
    }
    result = EnsurePerson(toggle.index);
  }
  return result;
}

auto AiMaskModelOutputStore::Has(const AiMaskComponent& component) const -> bool {
  if (const auto* group = std::get_if<AiMaskPromptGroup>(&component.content)) {
    for (std::size_t count = 1; count <= group->steps.size(); ++count) {
      if (FindDecode(Prefix(*group, count)) == nullptr) {
        return false;
      }
    }
    return true;
  }
  const auto& task = std::get<AiMaskTaskComponent>(component.content);
  if (!task_outputs_.contains(TaskKey(task.task))) {
    return false;
  }
  return std::all_of(task.person_toggles.begin(), task.person_toggles.end(),
                     [this](const AiMaskPersonToggle& toggle) {
                       return task_outputs_.contains(PersonKey(toggle.index));
                     });
}

auto AiMaskModelOutputStore::HasPeople() const -> bool {
  return task_outputs_.contains(TaskKey(AiMaskTaskKind::People));
}

auto AiMaskModelOutputStore::EnsureTask(AiMaskTaskKind task) -> AiMaskStepResult {
  const auto key = TaskKey(task);
  if (task_outputs_.contains(key)) {
    return {};
  }
  const auto result = runner_.RunTask(SidecarTask(task), std::nullopt);
  if (!result.status.ok) {
    return CallFailure(result.status, "task");
  }
  if (!result.found) {
    return {AiMaskStepStatus::NotFound,
            result.message.empty() ? "The task found nothing." : result.message};
  }
  // The Sky alpha of the sky head keeps its partial values (SM3.E decision S9); the S3OD alpha
  // of the other tasks keeps the D23 gain.
  auto stored = StoreTaskResult(
      key, result,
      task == AiMaskTaskKind::Sky ? AiMaskAlphaGain::kOne : AiMaskAlphaGain::kHalfUpscaleFactor);
  if (stored.status == AiMaskStepStatus::Applied && task == AiMaskTaskKind::People) {
    people_ = result.people;
  }
  return stored;
}

auto AiMaskModelOutputStore::EnsurePerson(std::uint32_t person_index) -> AiMaskStepResult {
  const auto key = PersonKey(person_index);
  if (task_outputs_.contains(key)) {
    return {};
  }
  const auto result = runner_.RunTask(SegmentationTask::kPerson, person_index);
  if (!result.status.ok) {
    return CallFailure(result.status, "person task");
  }
  if (!result.found) {
    return {AiMaskStepStatus::NotFound,
            result.message.empty() ? "The person was not found." : result.message};
  }
  if (result.mask_kind != SegmentationTaskMaskKind::kAlpha) {
    return Failed("the segmentation person task returned no alpha grid");
  }
  return StoreTaskResult(key, result, AiMaskAlphaGain::kHalfUpscaleFactor);
}

auto AiMaskModelOutputStore::StoreTaskResult(const std::string&            key,
                                             const SegmentationTaskResult& result,
                                             AiMaskAlphaGain alpha_gain) -> AiMaskStepResult {
  AiMaskModelOutput output;
  switch (result.mask_kind) {
    case SegmentationTaskMaskKind::kAlpha:
      output = AiMaskAlphaGrid{result.mask_width, result.mask_height, result.alpha_values,
                               OrderedBox(result.mask_box), alpha_gain};
      break;
    case SegmentationTaskMaskKind::kNone:
      return Failed("the segmentation task returned no mask");
  }
  try {
    ValidateAiMaskModelOutput(output, extent_);
  } catch (const std::invalid_argument& error) {
    return Failed(error.what());
  }
  task_outputs_.emplace(key, std::move(output));
  return {};
}

auto AiMaskModelOutputStore::EnsurePromptGroup(const AiMaskPromptGroup& group) -> AiMaskStepResult {
  for (std::size_t count = 1; count <= group.steps.size(); ++count) {
    const auto prefix = Prefix(group, count);
    if (FindDecode(prefix) != nullptr) {
      continue;
    }
    std::vector<float> mask_input;
    if (count > 1) {
      const auto  previous = Prefix(group, count - 1);
      const auto* output   = FindDecode(previous);
      const auto  level    = previous.steps.back().level;
      if (level >= output->levels.size()) {
        return Failed("the prompt step level " + std::to_string(level) +
                      " is not a level of its decode");
      }
      mask_input = output->levels[level];
    }
    auto decoded = Decode(prefix, std::move(mask_input));
    if (decoded.status != AiMaskStepStatus::Applied) {
      return decoded;
    }
  }
  return {};
}

auto AiMaskModelOutputStore::Decode(const AiMaskPromptGroup& prefix, std::vector<float> mask_input)
    -> AiMaskStepResult {
  auto result = runner_.Decode(DecoderPrompt(prefix, std::move(mask_input)));
  if (!result.status.ok) {
    return CallFailure(result.status, "decode");
  }
  const auto expected_levels = LevelCount(prefix);
  if (result.masks.size() != expected_levels || result.selected_index >= result.masks.size()) {
    return Failed("the segmentation decode returned " + std::to_string(result.masks.size()) +
                  " masks for a prompt with " + std::to_string(expected_levels));
  }
  DecodeOutput output;
  output.logit_size     = result.logit_size;
  output.selected_index = result.selected_index;
  for (auto& mask : result.masks) {
    try {
      ValidateAiMaskModelOutput(AiMaskSamLogits{result.logit_size, mask.logits}, extent_);
    } catch (const std::invalid_argument& error) {
      return Failed(error.what());
    }
    output.levels.push_back(std::move(mask.logits));
  }
  decode_outputs_.emplace(PromptKey(prefix), std::move(output));
  return {};
}

auto AiMaskModelOutputStore::FindDecode(const AiMaskPromptGroup& prefix) const
    -> const DecodeOutput* {
  const auto found = decode_outputs_.find(PromptKey(prefix));
  return found == decode_outputs_.end() ? nullptr : &found->second;
}

auto AiMaskModelOutputStore::SelectedLogits(const AiMaskPromptGroup& prefix) const
    -> const std::vector<float>& {
  const auto* output = FindDecode(prefix);
  if (output == nullptr) {
    throw std::logic_error("AI Mask: the decode of a prompt step is missing");
  }
  const auto level = prefix.steps.back().level;
  if (level >= output->levels.size()) {
    throw std::logic_error("AI Mask: the prompt step level is not a level of its decode");
  }
  return output->levels[level];
}

auto AiMaskModelOutputStore::SelectedLevel(const AiMaskPromptGroup& group) const -> std::uint32_t {
  const auto* output = FindDecode(group);
  if (output == nullptr) {
    throw std::logic_error("AI Mask: the decode of a prompt step is missing");
  }
  return output->selected_index;
}

auto AiMaskModelOutputStore::ComponentCoverage(const AiMaskComponent& component) const
    -> std::vector<std::uint8_t> {
  if (const auto* group = std::get_if<AiMaskPromptGroup>(&component.content)) {
    const auto* output = FindDecode(*group);
    if (output == nullptr) {
      throw std::logic_error("AI Mask: the decode of a prompt step is missing");
    }
    return ConvertModelOutputToCoverage(AiMaskSamLogits{output->logit_size, SelectedLogits(*group)},
                                        extent_);
  }
  const auto& task  = std::get<AiMaskTaskComponent>(component.content);
  const auto  found = task_outputs_.find(TaskKey(task.task));
  if (found == task_outputs_.end()) {
    throw std::logic_error("AI Mask: a task output is missing");
  }
  auto coverage = ConvertModelOutputToCoverage(found->second, extent_, guide_.get());
  for (const auto& toggle : task.person_toggles) {
    const auto person = task_outputs_.find(PersonKey(toggle.index));
    if (person == task_outputs_.end()) {
      throw std::logic_error("AI Mask: a person output is missing");
    }
    FoldAiMaskCoverage(coverage,
                       ConvertModelOutputToCoverage(person->second, extent_, guide_.get()),
                       toggle.operation);
  }
  return coverage;
}

}  // namespace alcedo
