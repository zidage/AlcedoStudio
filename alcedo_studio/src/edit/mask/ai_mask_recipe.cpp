//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/ai_mask_recipe.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace alcedo {
namespace {

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

void              RequireObjectWithOnlyKeys(const nlohmann::json& json, std::string_view owner,
                                            std::initializer_list<std::string_view> keys) {
  if (!json.is_object()) {
    Fail(std::string{owner} + " must be an object");
  }
  for (const auto& [key, value] : json.items()) {
    bool known = false;
    for (const auto allowed : keys) {
      known = known || key == allowed;
    }
    if (!known) {
      Fail(std::string{owner} + " contains unsupported field " + key);
    }
  }
}

auto RequireKey(const nlohmann::json& json, const char* key, std::string_view owner)
    -> const nlohmann::json& {
  if (!json.contains(key)) {
    Fail(std::string{owner} + " is missing " + key);
  }
  return json.at(key);
}

auto ReadFloat(const nlohmann::json& json, const char* key, std::string_view owner) -> float {
  const auto& value = RequireKey(json, key, owner);
  if (!value.is_number()) {
    Fail(std::string{owner} + " field " + key + " must be a number");
  }
  return value.get<float>();
}

auto ReadUint32(const nlohmann::json& json, const char* key, std::string_view owner)
    -> std::uint32_t {
  const auto& value = RequireKey(json, key, owner);
  if (!value.is_number_integer() || value.get<std::int64_t>() < 0 ||
      value.get<std::int64_t>() > std::numeric_limits<std::uint32_t>::max()) {
    Fail(std::string{owner} + " field " + key + " must be an unsigned integer");
  }
  return static_cast<std::uint32_t>(value.get<std::int64_t>());
}

auto OperationText(AiMaskComponentOperation operation) -> const char* {
  switch (operation) {
    case AiMaskComponentOperation::Add:
      return "add";
    case AiMaskComponentOperation::Subtract:
      return "subtract";
  }
  Fail("AI Mask operation is unknown");
}

auto ReadOperation(const nlohmann::json& json, const char* key, std::string_view owner)
    -> AiMaskComponentOperation {
  const auto& value = RequireKey(json, key, owner);
  if (value == "add") {
    return AiMaskComponentOperation::Add;
  }
  if (value == "subtract") {
    return AiMaskComponentOperation::Subtract;
  }
  Fail(std::string{owner} + " field " + key + " must be add or subtract");
}

auto TaskText(AiMaskTaskKind task) -> const char* {
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
  Fail("AI Mask task is unknown");
}

auto ReadTask(const nlohmann::json& value) -> AiMaskTaskKind {
  if (value == "subject") {
    return AiMaskTaskKind::Subject;
  }
  if (value == "background") {
    return AiMaskTaskKind::Background;
  }
  if (value == "sky") {
    return AiMaskTaskKind::Sky;
  }
  if (value == "people") {
    return AiMaskTaskKind::People;
  }
  Fail("AI Mask component task must be subject, background, sky, or people");
}

void RequireUnitCoordinate(float value, std::string_view name) {
  if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
    Fail(std::string{name} + " must be finite and in [0, 1]");
  }
}

void ValidateBox(const AiMaskBox& box, std::string_view owner) {
  RequireUnitCoordinate(box.x0, std::string{owner} + " x0");
  RequireUnitCoordinate(box.y0, std::string{owner} + " y0");
  RequireUnitCoordinate(box.x1, std::string{owner} + " x1");
  RequireUnitCoordinate(box.y1, std::string{owner} + " y1");
  if (box.x0 > box.x1 || box.y0 > box.y1) {
    Fail(std::string{owner} + " corners must be ordered");
  }
}

void ValidatePromptGroup(const AiMaskPromptGroup& group) {
  if (group.steps.empty()) {
    Fail("AI Mask prompt group must have at least one step");
  }
  std::size_t points = 0;
  for (const auto& step : group.steps) {
    if (step.points.empty() && !step.box.has_value()) {
      Fail("AI Mask prompt step must have points or a box");
    }
    for (const auto& point : step.points) {
      RequireUnitCoordinate(point.x, "AI Mask prompt point x");
      RequireUnitCoordinate(point.y, "AI Mask prompt point y");
      (void)OperationText(point.label);
    }
    if (step.box.has_value()) {
      ValidateBox(*step.box, "AI Mask prompt box");
    }
    if (step.level > kAiMaskMaximumPromptLevel) {
      Fail("AI Mask prompt step level must be 0 to 2");
    }
    if (step.level > 0 && (step.points.size() != 1 || step.box.has_value())) {
      Fail("AI Mask prompt step level above 0 needs one point and no box");
    }
    points += step.points.size();
  }
  if (points > kAiMaskMaximumPromptGroupPoints) {
    Fail("AI Mask prompt group has more than 64 points");
  }
}

void ValidateTask(const AiMaskTaskComponent& task) {
  (void)TaskText(task.task);
  if (task.task != AiMaskTaskKind::People && !task.person_toggles.empty()) {
    Fail("AI Mask person toggles need the people task");
  }
  for (const auto& toggle : task.person_toggles) {
    (void)OperationText(toggle.operation);
    ValidateBox(toggle.box, "AI Mask person box");
  }
}

auto BoxToJson(const AiMaskBox& box) -> nlohmann::json {
  return {{"x0", box.x0}, {"y0", box.y0}, {"x1", box.x1}, {"y1", box.y1}};
}

auto BoxFromJson(const nlohmann::json& json, std::string_view owner) -> AiMaskBox {
  RequireObjectWithOnlyKeys(json, owner, {"x0", "y0", "x1", "y1"});
  return AiMaskBox{ReadFloat(json, "x0", owner), ReadFloat(json, "y0", owner),
                   ReadFloat(json, "x1", owner), ReadFloat(json, "y1", owner)};
}

auto PromptStepToJson(const AiMaskPromptStep& step) -> nlohmann::json {
  auto points = nlohmann::json::array();
  for (const auto& point : step.points) {
    points.push_back({{"x", point.x}, {"y", point.y}, {"label", OperationText(point.label)}});
  }
  return {{"points", std::move(points)},
          {"box", step.box.has_value() ? BoxToJson(*step.box) : nlohmann::json(nullptr)},
          {"level", step.level}};
}

auto PromptStepFromJson(const nlohmann::json& json) -> AiMaskPromptStep {
  constexpr std::string_view kOwner = "AI Mask prompt step";
  RequireObjectWithOnlyKeys(json, kOwner, {"points", "box", "level"});
  AiMaskPromptStep step;
  const auto&      points = RequireKey(json, "points", kOwner);
  if (!points.is_array()) {
    Fail("AI Mask prompt step points must be an array");
  }
  for (const auto& point : points) {
    constexpr std::string_view kPointOwner = "AI Mask prompt point";
    RequireObjectWithOnlyKeys(point, kPointOwner, {"x", "y", "label"});
    step.points.push_back({ReadFloat(point, "x", kPointOwner), ReadFloat(point, "y", kPointOwner),
                           ReadOperation(point, "label", kPointOwner)});
  }
  const auto& box = RequireKey(json, "box", kOwner);
  if (!box.is_null()) {
    step.box = BoxFromJson(box, "AI Mask prompt box");
  }
  step.level = ReadUint32(json, "level", kOwner);
  return step;
}

auto ComponentToJson(const AiMaskComponent& component) -> nlohmann::json {
  nlohmann::json json = {{"operation", OperationText(component.operation)}};
  if (const auto* task = std::get_if<AiMaskTaskComponent>(&component.content)) {
    json["task"] = TaskText(task->task);
    if (task->task == AiMaskTaskKind::People) {
      auto toggles = nlohmann::json::array();
      for (const auto& toggle : task->person_toggles) {
        toggles.push_back({{"index", toggle.index},
                           {"operation", OperationText(toggle.operation)},
                           {"box", BoxToJson(toggle.box)}});
      }
      json["person_toggles"] = std::move(toggles);
    }
    return json;
  }
  auto steps = nlohmann::json::array();
  for (const auto& step : std::get<AiMaskPromptGroup>(component.content).steps) {
    steps.push_back(PromptStepToJson(step));
  }
  json["prompt_steps"] = std::move(steps);
  return json;
}

auto ComponentFromJson(const nlohmann::json& json) -> AiMaskComponent {
  constexpr std::string_view kOwner = "AI Mask component";
  RequireObjectWithOnlyKeys(json, kOwner, {"operation", "task", "person_toggles", "prompt_steps"});
  AiMaskComponent component;
  component.operation = ReadOperation(json, "operation", kOwner);
  if (json.contains("task") == json.contains("prompt_steps")) {
    Fail("AI Mask component must have exactly one of task and prompt_steps");
  }
  if (json.contains("task")) {
    AiMaskTaskComponent task;
    task.task = ReadTask(json.at("task"));
    if (task.task == AiMaskTaskKind::People) {
      const auto& toggles = RequireKey(json, "person_toggles", kOwner);
      if (!toggles.is_array()) {
        Fail("AI Mask person_toggles must be an array");
      }
      for (const auto& toggle : toggles) {
        constexpr std::string_view kToggleOwner = "AI Mask person toggle";
        RequireObjectWithOnlyKeys(toggle, kToggleOwner, {"index", "operation", "box"});
        task.person_toggles.push_back(
            {ReadUint32(toggle, "index", kToggleOwner),
             ReadOperation(toggle, "operation", kToggleOwner),
             BoxFromJson(RequireKey(toggle, "box", kToggleOwner), "AI Mask person box")});
      }
    } else if (json.contains("person_toggles")) {
      Fail("AI Mask person toggles need the people task");
    }
    component.content = std::move(task);
    return component;
  }
  if (json.contains("person_toggles")) {
    Fail("AI Mask person toggles need the people task");
  }
  const auto& steps = json.at("prompt_steps");
  if (!steps.is_array()) {
    Fail("AI Mask prompt_steps must be an array");
  }
  AiMaskPromptGroup group;
  for (const auto& step : steps) {
    group.steps.push_back(PromptStepFromJson(step));
  }
  component.content = std::move(group);
  return component;
}

}  // namespace

void ValidateAiMaskRecipe(const AiMaskRecipe& recipe) {
  if (recipe.components.empty()) {
    Fail("AI Mask recipe must have at least one component");
  }
  for (const auto& component : recipe.components) {
    (void)OperationText(component.operation);
    if (const auto* task = std::get_if<AiMaskTaskComponent>(&component.content)) {
      ValidateTask(*task);
    } else {
      ValidatePromptGroup(std::get<AiMaskPromptGroup>(component.content));
    }
  }
}

void ValidateAiMaskEdgeAdjustment(const AiMaskEdgeAdjustment& edge) {
  if (!std::isfinite(edge.feather_px) || edge.feather_px < 0.0f ||
      edge.feather_px > kAiMaskMaximumFeatherPx) {
    Fail("AI Mask feather_px must be finite and in [0, 50]");
  }
  if (edge.expand_px < -kAiMaskMaximumExpandPx || edge.expand_px > kAiMaskMaximumExpandPx) {
    Fail("AI Mask expand_px must be in [-50, 50]");
  }
}

auto AiMaskRecipeToJson(const AiMaskRecipe& recipe) -> nlohmann::json {
  ValidateAiMaskRecipe(recipe);
  auto components = nlohmann::json::array();
  for (const auto& component : recipe.components) {
    components.push_back(ComponentToJson(component));
  }
  return components;
}

auto AiMaskRecipeFromJson(const nlohmann::json& json) -> AiMaskRecipe {
  if (!json.is_array()) {
    Fail("AI Mask components must be an array");
  }
  AiMaskRecipe recipe;
  for (const auto& component : json) {
    recipe.components.push_back(ComponentFromJson(component));
  }
  ValidateAiMaskRecipe(recipe);
  return recipe;
}

}  // namespace alcedo
