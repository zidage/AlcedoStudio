//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "json.hpp"

/**
 * @file
 * @brief Recipe of an AI Segmentation Mask: the ordered model steps that made its coverage.
 *
 * The AI Mask edit session and the stored Mask source use these types. The recipe does not
 * change the stored pixels; it lets the session replay and edit the steps. All coordinates are
 * normalized ReferenceSpace in [0, 1].
 */
namespace alcedo {

/// Maximum number of prompt points in one prompt group (the segmentation decoder limit).
inline constexpr std::uint32_t kAiMaskMaximumPromptGroupPoints = 64;
/// Maximum selected level of a single-point prompt step (three decoder masks).
inline constexpr std::uint32_t kAiMaskMaximumPromptLevel       = 2;
/// Inclusive range of the Feather sigma in analysis image pixels.
inline constexpr float         kAiMaskMaximumFeatherPx         = 50.0f;
/// Inclusive range of the Expand distance in analysis image pixels.
inline constexpr std::int32_t  kAiMaskMaximumExpandPx          = 50;

/// How a component combines with the components before it; also the label of a prompt point.
enum class AiMaskComponentOperation : std::uint8_t { Add, Subtract };

/// Task masks of the segmentation sidecar.
enum class AiMaskTaskKind : std::uint8_t { Subject, Background, Sky, People };

/// Normalized rectangle given by its corners; `x0 <= x1` and `y0 <= y1`.
struct AiMaskBox {
  float       x0                                                     = 0.0f;
  float       y0                                                     = 0.0f;
  float       x1                                                     = 0.0f;
  float       y1                                                     = 0.0f;

  friend auto operator==(const AiMaskBox&, const AiMaskBox&) -> bool = default;
};

/// One prompt point. @p label is the decoder point label (Add = foreground).
struct AiMaskPromptPoint {
  float                    x     = 0.0f;
  float                    y     = 0.0f;
  AiMaskComponentOperation label = AiMaskComponentOperation::Add;

  friend auto operator==(const AiMaskPromptPoint&, const AiMaskPromptPoint&) -> bool = default;
};

/**
 * @brief One prompt input (a stroke, a click, or a box) of a prompt group.
 *
 * @p level is the selected decoder mask: the decoder `selected_index`, or the level that the
 * user selected. Only a step with one point and no box has three levels.
 */
struct AiMaskPromptStep {
  std::vector<AiMaskPromptPoint> points;
  std::optional<AiMaskBox>       box;
  std::uint32_t                  level                                             = 0;

  friend auto operator==(const AiMaskPromptStep&, const AiMaskPromptStep&) -> bool = default;
};

/// Ordered prompt steps that the decoder chains with `mask_input`.
struct AiMaskPromptGroup {
  std::vector<AiMaskPromptStep> steps;

  friend auto operator==(const AiMaskPromptGroup&, const AiMaskPromptGroup&) -> bool = default;
};

/**
 * @brief One click on a person box of a People component.
 *
 * @p index is the person index of the People result; the replay asks for the person by it.
 * @p box is the normalized clipped box of the People list, for display only.
 */
struct AiMaskPersonToggle {
  std::uint32_t            index     = 0;
  AiMaskComponentOperation operation = AiMaskComponentOperation::Add;
  AiMaskBox                box;

  friend auto operator==(const AiMaskPersonToggle&, const AiMaskPersonToggle&) -> bool = default;
};

/// A task mask; @p person_toggles is used only by the People task.
struct AiMaskTaskComponent {
  AiMaskTaskKind                  task = AiMaskTaskKind::Subject;
  std::vector<AiMaskPersonToggle> person_toggles;

  friend auto operator==(const AiMaskTaskComponent&, const AiMaskTaskComponent&) -> bool = default;
};

/// One recipe component: a task mask or a prompt group, with its combine operation.
struct AiMaskComponent {
  AiMaskComponentOperation                             operation = AiMaskComponentOperation::Add;
  std::variant<AiMaskTaskComponent, AiMaskPromptGroup> content;

  friend auto operator==(const AiMaskComponent&, const AiMaskComponent&) -> bool = default;
};

/// Ordered components; the coverage folds them in this order.
struct AiMaskRecipe {
  std::vector<AiMaskComponent> components;

  friend auto operator==(const AiMaskRecipe&, const AiMaskRecipe&) -> bool = default;
};

/**
 * @brief Edge change that the coverage texture applies; the stored R8 does not contain it.
 *
 * @p feather_px is a Gaussian sigma in [0, 50]; @p expand_px dilates when > 0 and erodes when
 * < 0, in [-50, 50]. Both are in pixels of the analysis image.
 */
struct AiMaskEdgeAdjustment {
  float        feather_px = 0.0f;
  std::int32_t expand_px  = 0;

  friend auto  operator==(const AiMaskEdgeAdjustment&, const AiMaskEdgeAdjustment&)
      -> bool = default;
};

/**
 * @brief Check every rule of the recipe JSON specification.
 *
 * Rules: at least one component; People alone takes person toggles; a prompt group has at least
 * one step and at most 64 points; each step has points or a box; coordinates are finite and in
 * [0, 1]; box corners are ordered; a level above 0 needs a step with one point and no box, and
 * a level is at most 2.
 *
 * @throws std::runtime_error naming the first rule that fails. Does not change @p recipe.
 */
void               ValidateAiMaskRecipe(const AiMaskRecipe& recipe);

/**
 * @brief Check the Feather and Expand ranges.
 * @throws std::runtime_error when a value is outside its range or not finite.
 */
void               ValidateAiMaskEdgeAdjustment(const AiMaskEdgeAdjustment& edge);

/**
 * @brief Serialize the components as a JSON array.
 * @throws std::runtime_error when @ref ValidateAiMaskRecipe fails.
 */
[[nodiscard]] auto AiMaskRecipeToJson(const AiMaskRecipe& recipe) -> nlohmann::json;

/**
 * @brief Read a components array. Rejects unknown fields and missing fields at every level.
 * @throws std::runtime_error on a schema or validation error.
 */
[[nodiscard]] auto AiMaskRecipeFromJson(const nlohmann::json& json) -> AiMaskRecipe;

}  // namespace alcedo
