//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "app/ai_mask_coverage_composition.hpp"
#include "app/ai_mask_segmentation_service.hpp"
#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "sidecar_client/dto/segmentation.hpp"

namespace alcedo {

/// Outcome kind of one AI Mask session step.
enum class AiMaskStepStatus : std::uint8_t {
  /// The step made a new session state.
  Applied,
  /// The model found nothing (a task, People, or a person); the state did not change.
  NotFound,
  /// A model call failed or returned an invalid output; the state did not change.
  Failed,
  /// The step is not valid for the current state; no model call ran and the state did not change.
  Rejected,
};

/// Outcome of one AI Mask session step. @p message is empty for Applied, else the reason.
struct AiMaskStepResult {
  AiMaskStepStatus status = AiMaskStepStatus::Applied;
  std::string      message;
};

/**
 * @brief The model outputs of one AI Mask edit session, in their native grids (SM5 decision M7).
 *
 * Each output is keyed by the canonical JSON of the component prefix that produced it: a task
 * (`{"task": ...}`), a person (`{"person": i}`), or a prompt group prefix (all steps up to the
 * step; the level of the last step is not part of the key, because the decode does not depend on
 * it). An output is computed once with the runner and kept for the lifetime of the store, so
 * Undo, Redo, and a step that returns to an earlier state need no model call.
 *
 * Threading: not thread-safe; the owner calls every method from one thread. Runner calls block.
 */
class AiMaskModelOutputStore {
 public:
  /// @p runner must outlive the store. @p extent is the session image size. @p guide (null or of
  /// size @p extent) guides the upscale of the alpha grids (master plan D23).
  AiMaskModelOutputStore(IAiMaskSegmentationRunner& runner, Extent2D extent,
                         std::shared_ptr<const AiMaskCoverageGuide> guide = nullptr);

  /**
   * @brief Make every output of @p component exist; call the runner for each missing output.
   *
   * A task runs its task request; People also runs one Person request for each toggled person;
   * a prompt group decodes each step prefix in order, each with the selected logits of the step
   * before as `mask_input` (M4). Stops at the first call that fails or finds nothing; the outputs
   * computed before it stay.
   *
   * @return Applied when all outputs exist; NotFound or Failed with the sidecar message.
   */
  auto               Ensure(const AiMaskComponent& component) -> AiMaskStepResult;

  /// True when Ensure() would need no runner call for @p component.
  [[nodiscard]] auto Has(const AiMaskComponent& component) const -> bool;

  /**
   * @brief The coverage of @p component at the session size: a task output, the People base with
   * its person toggles folded in order (M5), or the selected level of the last group step.
   *
   * @pre Has(@p component).
   * @throws std::logic_error when an output is missing.
   */
  [[nodiscard]] auto ComponentCoverage(const AiMaskComponent& component) const
      -> std::vector<std::uint8_t>;

  /**
   * @brief The decoder `selected_index` of the last step of @p group.
   * @pre The output of the full group exists.
   * @throws std::logic_error when the output is missing.
   */
  [[nodiscard]] auto SelectedLevel(const AiMaskPromptGroup& group) const -> std::uint32_t;

  /// True when the People task output exists.
  [[nodiscard]] auto HasPeople() const -> bool;
  /// The People list of the People task output; empty before it exists.
  [[nodiscard]] auto People() const -> std::span<const SegmentationPerson> { return people_; }

  /// Number of decoder levels of the last step of @p group: 3 for a group prefix of one point
  /// without a box (a click), else 1.
  [[nodiscard]] static auto LevelCount(const AiMaskPromptGroup& group) -> std::uint32_t;

 private:
  struct DecodeOutput {
    std::uint32_t                   logit_size = 0;
    std::vector<std::vector<float>> levels;
    std::uint32_t                   selected_index = 0;
  };

  auto EnsureTask(AiMaskTaskKind task) -> AiMaskStepResult;
  auto EnsurePerson(std::uint32_t person_index) -> AiMaskStepResult;
  auto EnsurePromptGroup(const AiMaskPromptGroup& group) -> AiMaskStepResult;
  auto Decode(const AiMaskPromptGroup& prefix, std::vector<float> mask_input) -> AiMaskStepResult;
  /// Converts an Applied task result to its model output; Failed on an invalid result. An alpha
  /// grid gets @p alpha_gain.
  auto StoreTaskResult(const std::string& key, const SegmentationTaskResult& result,
                       AiMaskAlphaGain alpha_gain) -> AiMaskStepResult;

  [[nodiscard]] auto FindDecode(const AiMaskPromptGroup& prefix) const -> const DecodeOutput*;
  [[nodiscard]] auto SelectedLogits(const AiMaskPromptGroup& prefix) const
      -> const std::vector<float>&;

  [[nodiscard]] static auto  TaskKey(AiMaskTaskKind task) -> std::string;
  [[nodiscard]] static auto  PersonKey(std::uint32_t person_index) -> std::string;
  [[nodiscard]] static auto  PromptKey(const AiMaskPromptGroup& prefix) -> std::string;

  IAiMaskSegmentationRunner& runner_;
  Extent2D                   extent_;
  std::shared_ptr<const AiMaskCoverageGuide> guide_;
  std::map<std::string, AiMaskModelOutput>   task_outputs_;
  std::map<std::string, DecodeOutput>        decode_outputs_;
  std::vector<SegmentationPerson>            people_;
};

}  // namespace alcedo
